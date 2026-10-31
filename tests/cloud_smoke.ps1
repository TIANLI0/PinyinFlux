param([string]$CorePath = (Join-Path $PSScriptRoot '..\out\core\Release\PinyinFluxCore.exe'))
$ErrorActionPreference = 'Stop'
$CorePath = [IO.Path]::GetFullPath($CorePath)
$proxyPath = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Internet Settings'
$proxyBefore = (Get-ItemProperty $proxyPath).AutoConfigURL
$rootsBefore = @(Get-ChildItem Cert:\CurrentUser\Root | ForEach-Object Thumbprint) -join ','
$listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
$listener.Start()
$port = $listener.LocalEndpoint.Port
$listener.Stop()
$start = [Diagnostics.ProcessStartInfo]::new($CorePath, "--dev $port")
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$start.RedirectStandardOutput = $true
$process = [Diagnostics.Process]::Start($start)
$output = $process.StandardOutput.ReadToEndAsync()
try {
    $base = "http://127.0.0.1:$port"
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    do {
        try { $status = Invoke-RestMethod "$base/status" -TimeoutSec 1; break }
        catch { Start-Sleep -Milliseconds 100 }
    } while ([DateTime]::UtcNow -lt $deadline -and -not $process.HasExited)
    if (-not $status) { throw 'Isolated loopback core did not start.' }
    if ($status.nativeReady -ne 0 -or $status.proxyApplied) { throw 'Dev mode unexpectedly activated integration.' }
    $envelope = '{_t_:1,_cl_:_w_,_q_:_juequling_,_app_:_PinyinFluxTest.exe_,_c_:5}'
    $result = Invoke-RestMethod "$base/qsonhs.aspx?q=$([Uri]::EscapeDataString($envelope))" -TimeoutSec 6
    $suggestions = @($result.AS.Results.Suggests)
    $expected = 'Cand=' + [string]::new([char[]](0x7EDD, 0x533A, 0x96F6)) # U+7EDD U+533A U+96F6; ASCII-only so Windows PowerShell 5.1 parses this file
    if (-not $suggestions.Count -or $expected -notin $suggestions.Txt) { throw 'Live cloud did not return a native conversion record for the expected candidate.' }
    if (@($suggestions | Where-Object Type -ne 'KA').Count) { throw 'Suggestions do not use the native conversion protocol.' }
    $null = Invoke-RestMethod "$base/qsonhs.aspx?q=juequling" -TimeoutSec 3
    $status = Invoke-RestMethod "$base/status" -TimeoutSec 3
    if ($status.lastQuery -ne 'juequling' -or $status.cacheHits -lt 1) { throw 'Envelope spelling/cache key is incorrect.' }
    Write-Output 'PASS real cloud candidate, native envelope decoding, KA conversion response and cache reuse'
} finally {
    try {
        $event = [Threading.EventWaitHandle]::OpenExisting("Local\PinyinFlux.Exit.Dev.$port")
        $null = $event.Set(); $event.Dispose()
    } catch { }
    if (-not $process.WaitForExit(5000)) { $process.Kill(); $process.WaitForExit() }
    $log = $output.GetAwaiter().GetResult()
    $process.Dispose()
    $proxyAfter = (Get-ItemProperty $proxyPath).AutoConfigURL
    $rootsAfter = @(Get-ChildItem Cert:\CurrentUser\Root | ForEach-Object Thumbprint) -join ','
    if ($proxyBefore -ne $proxyAfter -or $rootsBefore -ne $rootsAfter) { throw 'Dev test changed proxy/trust configuration.' }
    Write-Output 'PASS isolated test leaves system proxy and trusted certificates unchanged'
}
