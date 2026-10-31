// SPDX-License-Identifier: MIT
using System.Diagnostics;
using System.Globalization;
using System.Net;

namespace PinyinFlux.App.Services.Updates;

public enum UpdateStage
{
    Idle,
    Checking,
    UpToDate,
    /// <summary>A newer release exists; nothing downloaded yet.</summary>
    Available,
    Downloading,
    /// <summary>The verified installer is on disk and can be started.</summary>
    Ready,
    /// <summary>The installer was started; the app is about to exit.</summary>
    Installing,
    Failed,
}

/// <summary>Why a check or download failed, in categories the UI can explain without exception text.</summary>
public enum UpdateFailure
{
    None,
    Offline,
    RateLimited,
    NotFound,
    /// <summary>Size or checksum mismatch.</summary>
    Verification,
    /// <summary>The download folder could not be written, or the installer could not be started.</summary>
    Storage,
    Other,
}

public sealed record UpdateStatus
{
    public UpdateStage Stage { get; init; }

    public UpdateRelease? Release { get; init; }

    public long Received { get; init; }

    public long Total { get; init; }

    public UpdateFailure Failure { get; init; }

    public string? InstallerPath { get; init; }

    public DateTimeOffset? LastChecked { get; init; }

    public bool IsBusy => Stage is UpdateStage.Checking or UpdateStage.Downloading or UpdateStage.Installing;
}

/// <summary>
/// Checks GitHub Releases shortly after the settings app opens (when enabled, at most once per
/// <c>checkInterval</c>) and again while it stays open, downloads a newer installer in the background and
/// starts it only when the user asks. All work runs off the UI thread; one check or download at a time.
/// </summary>
public sealed class UpdateManager : IAsyncDisposable
{
    /// <summary>Inno Setup switches for an in-place update; /RELAUNCH=1 makes the installer start PinyinFlux again (see installer/PinyinFlux.iss).</summary>
    public const string InstallerArguments = "/SILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS /RELAUNCH=1";

    /// <summary>Passed by the installer when it relaunches PinyinFlux after an update.</summary>
    public const string RelaunchArgument = "--updated";

    private static readonly TimeSpan Wake = TimeSpan.FromHours(1);

    private readonly GitHubUpdateClient _client;
    private readonly Func<bool> _autoCheck;
    private readonly Func<bool> _includePrerelease;
    private readonly string _stateFile;
    private readonly TimeSpan _firstCheckDelay;
    private readonly TimeSpan _checkInterval;
    private readonly CancellationTokenSource _shutdown = new();
    private readonly SemaphoreSlim _busy = new(1, 1);
    private readonly Lock _gate = new();
    private UpdateStatus _status;
    private Task? _loop;
    private bool _disposed;

    /// <param name="stateFile">Holds the time of the last successful check, so reopening the app does not hammer the API.</param>
    public UpdateManager(GitHubUpdateClient client, Func<bool> autoCheck, Func<bool> includePrerelease, string stateFile,
        TimeSpan? firstCheckDelay = null, TimeSpan? checkInterval = null)
    {
        _client = client;
        _autoCheck = autoCheck;
        _includePrerelease = includePrerelease;
        _stateFile = stateFile;
        _firstCheckDelay = firstCheckDelay ?? TimeSpan.FromSeconds(5);
        _checkInterval = checkInterval ?? TimeSpan.FromHours(1);
        _status = new UpdateStatus { LastChecked = ReadLastCheck() };
    }

    public SemanticVersion CurrentVersion => _client.Current;

    public UpdateStatus Status
    {
        get { lock (_gate) return _status; }
    }

    /// <summary>Raised on a background thread; handlers should read <see cref="Status"/> on their own thread.</summary>
    public event EventHandler<UpdateStatus>? StatusChanged;

    public void Start()
    {
        lock (_gate)
        {
            if (_loop is not null || _disposed) return;
            _loop = Task.Run(() => RunAsync(_shutdown.Token));
        }
    }

    private async Task RunAsync(CancellationToken ct)
    {
        try
        {
            // Installers of the version now running (or older) are no longer needed.
            _client.CleanUpInstalled();
            await Task.Delay(_firstCheckDelay, ct).ConfigureAwait(false);
            while (!ct.IsCancellationRequested)
            {
                if (_autoCheck() && IsDue(DateTimeOffset.UtcNow)) await CheckCoreAsync(automatic: true, ct).ConfigureAwait(false);
                await Task.Delay(Wake, ct).ConfigureAwait(false);
            }
        }
        catch (OperationCanceledException) when (ct.IsCancellationRequested) { }
        catch (Exception ex) { Debug.WriteLine($"Update loop stopped: {ex}"); }
    }

    internal bool IsDue(DateTimeOffset now) => Status.LastChecked is not { } last || now - last >= _checkInterval || last > now.AddDays(1);

    /// <summary>User-initiated check; never downloads by itself.</summary>
    public Task CheckNowAsync() => CheckCoreAsync(automatic: false, _shutdown.Token);

    /// <summary>Downloads the available release (user-initiated). Returns the verified installer, or null.</summary>
    public async Task<string?> DownloadAsync()
    {
        if (Status is not { Release: { } release } || _disposed) return null;
        if (!await _busy.WaitAsync(0).ConfigureAwait(false)) return null;
        try { return await DownloadCoreAsync(release, _shutdown.Token).ConfigureAwait(false); }
        finally { _busy.Release(); }
    }

    private async Task CheckCoreAsync(bool automatic, CancellationToken ct)
    {
        if (_disposed || !await _busy.WaitAsync(0, CancellationToken.None).ConfigureAwait(false)) return;
        try
        {
            var previous = Status;
            Publish(s => s with { Stage = UpdateStage.Checking, Failure = UpdateFailure.None });
            var result = await _client.CheckAsync(_includePrerelease(), ct).ConfigureAwait(false);
            if (result.Outcome is not (UpdateCheckOutcome.Offline or UpdateCheckOutcome.Failed))
            {
                // Rate-limited counts as a check too: retrying sooner would only extend the limit.
                var now = DateTimeOffset.UtcNow;
                WriteLastCheck(now);
                Publish(s => s with { LastChecked = now });
            }
            switch (result.Outcome)
            {
                case UpdateCheckOutcome.UpToDate:
                    Publish(s => s with { Stage = UpdateStage.UpToDate, Release = null, InstallerPath = null });
                    break;
                case UpdateCheckOutcome.Available:
                    var release = result.Release!;
                    if (previous is { Stage: UpdateStage.Ready, InstallerPath: { } path } && Equals(previous.Release?.Version, release.Version) && File.Exists(path))
                        Publish(s => s with { Stage = UpdateStage.Ready, Release = release });
                    else if (automatic)
                        await DownloadCoreAsync(release, ct).ConfigureAwait(false);
                    else
                        Publish(s => s with { Stage = UpdateStage.Available, Release = release, InstallerPath = null });
                    break;
                default:
                    Publish(s => s with
                    {
                        Stage = UpdateStage.Failed,
                        Failure = result.Outcome switch
                        {
                            UpdateCheckOutcome.Offline => UpdateFailure.Offline,
                            UpdateCheckOutcome.RateLimited => UpdateFailure.RateLimited,
                            UpdateCheckOutcome.NotFound => UpdateFailure.NotFound,
                            _ => UpdateFailure.Other,
                        },
                    });
                    break;
            }
        }
        catch (OperationCanceledException) when (ct.IsCancellationRequested) { }
        finally { _busy.Release(); }
    }

    private async Task<string?> DownloadCoreAsync(UpdateRelease release, CancellationToken ct)
    {
        Publish(s => s with { Stage = UpdateStage.Downloading, Release = release, Received = 0, Total = release.Installer.Size, Failure = UpdateFailure.None, InstallerPath = null });
        try
        {
            string path = await _client.DownloadAsync(release, new Callback(p => Publish(s => s with { Received = p.Received, Total = p.Total })), ct).ConfigureAwait(false);
            Publish(s => s with { Stage = UpdateStage.Ready, Release = release, Received = release.Installer.Size, InstallerPath = path });
            return path;
        }
        catch (OperationCanceledException) when (ct.IsCancellationRequested) { return null; }
        catch (Exception ex)
        {
            Debug.WriteLine($"Update download failed ({ex.GetType().Name}: {ex.Message})");
            Publish(s => s with { Stage = UpdateStage.Failed, Failure = Classify(ex), Received = 0 });
            return null;
        }
    }

    /// <summary>
    /// Starts the verified installer silently. The caller then closes the app; the installer stops the core,
    /// installs over the current version, starts the core again and relaunches PinyinFlux.
    /// </summary>
    public bool TryStartInstaller()
    {
        var status = Status;
        if (status is not { Stage: UpdateStage.Ready, InstallerPath: { } path } || !File.Exists(path)) return false;
        try
        {
            string log = Path.Combine(_client.DownloadDirectory, "install.log");
            Process.Start(new ProcessStartInfo(path)
            {
                Arguments = $"{InstallerArguments} /LOG=\"{log}\"",
                WorkingDirectory = _client.DownloadDirectory,
                // Shell execution so a UAC prompt can appear if Windows decides the installer needs one.
                UseShellExecute = true,
            })?.Dispose();
            Publish(s => s with { Stage = UpdateStage.Installing });
            return true;
        }
        catch (Exception ex) when (ex is System.ComponentModel.Win32Exception or IOException or InvalidOperationException)
        {
            Debug.WriteLine($"Could not start the installer ({ex.GetType().Name})");
            Publish(s => s with { Stage = UpdateStage.Failed, Failure = UpdateFailure.Storage });
            return false;
        }
    }

    internal static UpdateFailure Classify(Exception exception) => exception switch
    {
        UpdateVerificationException => UpdateFailure.Verification,
        HttpRequestException { StatusCode: null } or TimeoutException or OperationCanceledException => UpdateFailure.Offline,
        HttpRequestException { StatusCode: HttpStatusCode.NotFound } => UpdateFailure.NotFound,
        HttpRequestException { StatusCode: HttpStatusCode.Forbidden or HttpStatusCode.TooManyRequests } => UpdateFailure.RateLimited,
        IOException or UnauthorizedAccessException => UpdateFailure.Storage,
        _ => UpdateFailure.Other,
    };

    private void Publish(Func<UpdateStatus, UpdateStatus> change)
    {
        UpdateStatus next;
        lock (_gate)
        {
            if (_disposed) return;
            next = _status = change(_status);
        }
        try { StatusChanged?.Invoke(this, next); }
        catch (Exception ex) { Debug.WriteLine($"Update status handler failed: {ex}"); }
    }

    private DateTimeOffset? ReadLastCheck()
    {
        try
        {
            return File.Exists(_stateFile) && DateTimeOffset.TryParse(File.ReadAllText(_stateFile).Trim(), CultureInfo.InvariantCulture, DateTimeStyles.RoundtripKind, out var value)
                ? value : null;
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { return null; }
    }

    private void WriteLastCheck(DateTimeOffset value)
    {
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(_stateFile)!);
            File.WriteAllText(_stateFile, value.ToString("O", CultureInfo.InvariantCulture));
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { Debug.WriteLine($"Could not save the update check time ({ex.GetType().Name})"); }
    }

    private sealed class Callback(Action<UpdateDownloadProgress> report) : IProgress<UpdateDownloadProgress>
    {
        public void Report(UpdateDownloadProgress value) => report(value);
    }

    public async ValueTask DisposeAsync()
    {
        Task? loop;
        lock (_gate)
        {
            if (_disposed) return;
            _disposed = true;
            loop = _loop;
        }
        await _shutdown.CancelAsync();
        if (loop is not null)
        {
            try { await loop.ConfigureAwait(false); } catch (Exception) { /* Logged by the loop. */ }
        }
        // A user-started check or download observes the same token; wait for it to let go of the files.
        if (await _busy.WaitAsync(TimeSpan.FromSeconds(5)).ConfigureAwait(false)) _busy.Release();
        _client.Dispose();
        _shutdown.Dispose();
    }
}
