# 开发说明

## 目录结构

| 路径 | 内容 |
| --- | --- |
| `src/PinyinFlux.Core/` | 核心 `PinyinFluxCore.exe`（C++20）：本地服务、词库引擎、注入器、符号解析 |
| `src/PinyinFlux.Core/providers/` | 百度、搜狗词库接口 |
| `src/PinyinFlux.Inject/` | 注入输入法宿主的扩展 `PinyinFluxInject.dll`，及与核心共享内存的协议 |
| `src/PinyinFlux.App/` | 设置界面 `PinyinFlux.exe`（WinUI 3，.NET 10） |
| `tests/` | CTest 测试、联网冒烟测试、手工输入测试窗口 |
| `installer/`、`build/` | 安装包定义（[FluentSetup](https://github.com/TIANLI0/FluentSetup) 的 `fluentsetup.json`）与打包脚本 |
| `docs/` | 设计文档 |

## 构建

需要 Visual Studio 2022 及以上（“使用 C++ 的桌面开发”，含 CMake 和 Ninja）以及 .NET 10 SDK。首次构建会从上游获取固定提交的 [MinHook](https://github.com/TsudaKageyu/minhook)。打包安装程序还需要 [FluentSetup](https://github.com/TIANLI0/FluentSetup/releases) 的 `fluentsetup` 命令行（在 PATH 中，或用环境变量 `FLUENTSETUP` 指向 `fluentsetup.exe`）；CI 通过 `TIANLI0/FluentSetup` Action 获取。

```powershell
.\build.ps1                       # 核心、扩展与测试，输出到 out\core\Release
dotnet build src/PinyinFlux.App -c Release -r win-x64 -p:Platform=x64
.\build\build-installer.ps1       # 完整安装包，输出到 artifacts\installer
```

`build.ps1` 通过 `vswhere` 定位 Visual Studio 并导入 x64 开发环境，不需要在开发者命令行中运行。

## 测试

```powershell
ctest --test-dir out/core/Release --output-on-failure
.\tests\cloud_smoke.ps1           # 联网：真实查询百度、搜狗
```

| 测试 | 内容 |
| --- | --- |
| `cloud_priority_deadline_cache` | 词库合并规则、读音对齐（全拼/简拼/混拼）、多云词打分与自适应排序、等待截止、缓存与回填 |
| `native_layout_from_symbols` | 从微软公开符号与指令解析本机组件布局；在最初校验的版本上必须与手工核对值完全一致 |
| `native_no_empty_placeholder` | 真实 COM 工厂在扩展下生成零元素占位列表 |
| `native_response_parser` | 微软真实解析器接受 PinyinFlux 生成的全拼与简拼响应 |
| `native_redirect_restore` 等 | 在隔离进程中加载真实组件：请求重定向、复用连接、停止恢复、核心重启、未知版本拒绝 |
| `native_preflight` / `native_preflight_rejects_bad_extension` | 核心注入前的自检（`PinyinFluxCore.exe --preflight <扩展>`）在本机组件上通过，对无效扩展失败 |
| `native_preflight_double_pinyin` | 进程的 HKCU 换成开着双拼的临时键，自检仍然通过 |

依赖微软拼音组件的测试在未安装中文输入法的系统（如 Windows Server CI 镜像）上会报告跳过。程序级测试不能代替真实输入验收：

```powershell
cmake --build out/core/Release --target PinyinFluxImeTest
.\out\core\Release\PinyinFluxImeTest.exe
```

在测试窗口中用微软拼音输入 `ysqd`、`ldsd` 等简拼，停顿后应在第 2 位看到带云图标的云词；再试 `nihao`、`zhongguoren` 等常用词（候选应不变）、连续输入、退格和停止核心。测试窗口加 `--candidates <文件>` 会记录输入法通过 TSF 公开的候选列表（新版微软拼音不经此接口公开）。

自适应排序的验收：选中或忽略（上屏本地词）一个显示中的云词后，`/status` 的 `ranking` 应立即变化；按 Esc 取消输入不应变化。选中的云词会被微软拼音学进本地词库，之后作为本地候选出现，所以同一个拼音只能验证一次。用 `/qsonhs.aspx?q=<拼音>` 查看学习后的提交顺序。

## 运行与诊断

```powershell
.\out\core\Release\PinyinFluxCore.exe --console     # 前台运行并输出日志
.\out\core\Release\PinyinFluxCore.exe --dev 18999   # 独立端口的开发实例，不注入、不改系统设置
.\out\core\Release\PinyinFluxCore.exe --test yyds   # 查询一次词库
.\out\core\Release\PinyinFluxCore.exe --stop
```

注入输入法宿主需要管理员权限。配置位于 `%LOCALAPPDATA%\PinyinFlux\config.ini`，`[Core] NativeInjection=1` 为原生模式，`0` 为兼容模式（PAC 代理 + 本机证书）；核心发现模式变化会自行退出，设置界面随后以新模式重新启动它。

`http://127.0.0.1:18765/qsonhs.aspx?q=<拼音>` 返回微软拼音会收到的响应，`/status` 字段：

| 字段 | 含义 |
| --- | --- |
| `nativeLayout` | 各组件地址的来源：`symbols`（公开符号）、`built-in`（内置已校验版本）、`no symbols` / `incomplete`（不安装钩子） |
| `nativeCompat` | 这个 Windows 版本能否注入：`state`（`resolving` 解析中、`waitingSymbols` 等待符号下载、`checking` 自检中、`verified`、`unsupported`、`crashed` 注入后宿主崩溃已停用）、`detail`（自检结果）、`windows`（InputService 版本）。见 [云候选设计](cloud-candidates.md#windows-更新后的适配) |
| `nativeRestartPending` | 仍在运行更新前组件、等待重启的宿主数量 |
| `nativeDoublePinyin` | 微软拼音处于双拼模式，PinyinFlux 不补充云词（不适配双拼） |
| `nativeTiming` | 显示过的云词从最后一次按键到出现的平均耗时（ms）：`pauseMs` 微软的停顿等待、`answerMs` 核心与词库、`replayMs` 重放；`cloudDelayMs` 微软配置的停顿；`missed` 请求未应答就上屏的次数；`totals` 合计耗时分布（<300、<600、<1000、<2000、更长） |
| `nativeReady` / `nativePending` | 钩子已就绪 / 等待 ChsProxyDS 加载的宿主数量 |
| `nativeDenied` / `nativeUnsupported` | 权限不足 / 无法确认地址的宿主数量 |
| `nativeNotifications` / `nativeTriggers` | 收到的原生通知 / 补充的云查询触发 |
| `nativeRequests` / `cloudRequests` | 重定向的请求 / 本地服务收到的请求 |
| `nativeArrivals` / `nativeReplays` | 微软收到的云结果 / 因有可显示的新词而重放占位 |
| `nativeDebounceSkips` | 重放查询跳过输入停顿等待的次数 |
| `nativeResponseRecords` → `nativeFilterBefore` / `nativeFilterAfter` → `nativeExcerptStored` | 最近一次响应的云词数：解析出的转换记录 → 本地去重前 / 后 → 微软实际存下并显示的云摘录（目前最多 1 个） |
| `queries` / `answered` / `cacheHits` / `backfills` | 引擎查询 / 有结果 / 命中缓存 / 迟到结果回填 |
| `typingEnabled` / `typing` | 打字统计开关 / 当天的上屏次数、汉字数、拼音字母数、选中云词次数、打字秒数、云词出现次数 `shown`（含尚未写入 `stats.csv` 的部分） |
| `ranking` | 自适应排序的学习状态，全拼（`full`）与简拼（`abbr`）分开：提交备选的门槛、各词库的命中率（见 [云候选设计](cloud-candidates.md)） |

打字统计由扩展挂钩 `ChxIME::ProductItemPool::SetPrevPrimitive`（微软拼音每次上屏时以上屏文字和读音调用，用作联想上下文；不记录输入历史的场合返回 `S_FALSE`，此时不计数）。扩展只在共享内存中累加计数，核心汇总各宿主的增量，每分钟把当天一行写入 `%LOCALAPPDATA%\PinyinFlux\stats.csv`（`date,commits,chars,letters,cloud,activeSeconds,shown`；0.3 以前的行没有 `shown`，读作 0），设置界面读该文件显示历史。云词出现次数在重放出的结果填入占位时计数，每次输入最多一次。清除统计通过命名事件 `Local\PinyinFlux.ClearTyping` 通知核心。

停止核心会恢复所有内存钩子。为避免正在执行的回调访问已卸载的代码，扩展 DLL 会以停用状态留在宿主中，直到输入法进程退出；升级时 DLL 复制到按内容哈希命名的目录，不会覆盖已加载的文件。

## 适配新的 Windows 版本

核心启动时由 `native_symbols.cpp` 为本机组件解析布局，过程见 [云候选设计](cloud-candidates.md#原生流程)。在新版本上出现 `incomplete` 时：

1. 运行 `PinyinFluxLayoutTest.exe`，它会打印解析出的每个函数地址和字段偏移，缺失项为 0。
2. 用 `dumpbin /disasm /range:<起始>,<结束>` 查看对应函数，确认指令形态的变化。
3. 调整 `native_symbols.cpp` 中对应的匹配规则，并保证在已校验版本上 `native_layout_from_symbols` 仍然通过。

没有那台机器时，可以从微软符号服务器取到别的版本的组件：在 [Winbindex](https://winbindex.m417z.com/) 查到文件的时间戳与 `SizeOfImage`，下载 `https://msdl.microsoft.com/download/symbols/InputService.dll/<时间戳8位大写十六进制><SizeOfImage小写十六进制>/InputService.dll`（ChsProxyDS.dll、ChsEM.dll 同理），放进同一个目录，然后：

```powershell
.\out\core\Release\PinyinFluxLayoutTest.exe <目录>                                   # 离线解析该版本的布局
.\out\core\Release\PinyinFluxNativeTest.exe out\core\Release\PinyinFluxInject.dll --images <目录>
```

后者在隔离进程中装上钩子并测试重定向与恢复；扩展只接受系统目录中的组件，本地验证时需临时放开 `native_hook.cpp` 的 `SystemModule` 检查（不要提交）。

Windows 11 22H2/23H2（22621/22631）的 InputService 以 XFG 编译：虚函数调用前多一条 `movabs r10, <类型哈希>`，寄存器分配也与 24H2 不同，匹配规则需容许这些差异。

## 发布

版本由 `CHANGELOG.md` 驱动，与 EncoDeck 相同：

1. 修改 `Directory.Build.props` 中的 `VersionPrefix`，以及同样写着版本号的 `src/PinyinFlux.Core/common.h`（`kVersion`，`/status` 显示的版本）和 `src/PinyinFlux.Core/PinyinFluxCore.rc`（核心的文件版本）。安装包的版本由 `build\build-installer.ps1` 以 `-D Version=<版本>` 传给 FluentSetup，无需修改 `installer\fluentsetup.json`。
2. 在 `CHANGELOG.md` 最上方加入 `## [0.2.0] - 2026-11-02 · 可选标题` 及更新内容（预发布写作 `0.2.0-beta.1`）。
3. 推送到 main。CI 的 `version` 任务发现该版本还没有 Release，就会构建测试、打包，并以该段内容为说明发布 `v0.2.0`。版本号与 `VersionPrefix` 不一致时构建失败。

最上方为 `## [Unreleased]` 时只构建不发布；每次构建都会上传 `<版本>-ci.<序号>` 的安装包和压缩包作为 Actions 产物。手动推送 `v*` 标签同样会发布。