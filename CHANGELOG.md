# Changelog

[English](CHANGELOG.en.md)

## [Unreleased]

### Fixed

- 换过输出设备之后，任务栏手势会一直调「上一个设备」：一个只是不再是默认输出的设备
  并不会失效，它仍然接受 `SetMasterVolumeLevelScalar`，读回来也一致，于是浮层照常
  显示新数值，声音和静音却毫无变化，要重启插件进程才恢复。现在每次操作前都把绑定的
  端点和当前默认输出比较，不同就重新绑定；`plugins.log` 里每个设备只记一行
  `following output device …`，一眼能看出插件在跟哪个设备。
- 会话只在默认设备上枚举：应用在「设置 → 系统 → 声音 → 音量混合器」里被指到别的输出
  设备时，插件要么报「没有音频会话」，要么把数值写进默认设备上同名的僵尸会话（数字
  在变、声音不动）。现在枚举所有活动输出设备上的会话。
- 同一个应用有多个会话时只读列表里的第一个，而那个会话可能早已不发声（同一进程可以
  同时留下 100% 和 92% 两个非活动会话），滚轮会从旧值起跳。现在优先「正在发声」的
  会话，其次才是默认设备上的会话。
- 主音量只尝试 `eConsole` 一个 role，该 role 没有默认设备时直接失败；现在依次尝试
  `eConsole`、`eMultimedia`、`eCommunications`。
- WebView2 宿主（如新版 Teams）的音频调不了：会话属于 `msedgewebview2.exe`，而按钮
  解析到宿主进程，过去只认同 pid 或同 exe 路径。现在这两条都不中时沿父链向上走
  （最多三级）；系统宿主（explorer、svchost、SearchHost、RuntimeBroker、
  ShellExperienceHost）的后代一律不认——那条链说明不了音频属于谁。
- 清理 `CHANGELOG.md` / `CHANGELOG.en.md` 里遗留的合并冲突标记（0.1.1 段落此前被夹在
  冲突块中间）。

### Changed

- `vol` 列出的会话如果不在默认设备上，副标题会补上设备名。

### Added

- 自动化测试分层：73 个纯逻辑单测——用假后端直测 worker 的排队操作（步进、0/100
  钳制、静音切换、失败与「没有音频会话」的中英文文案，这部分此前完全没有覆盖），
  以及应用匹配规则（pid、exe 路径、父链 + 深度上限）。
- `tools/e2e_protocol.py`：以宿主的方式（initialize/query/action/shutdown，JSON
  行协议）驱动真实的 `volume.exe`，核对列表结构、单个会话按 `percentPerStep` 步进
  并还原、主静音切换并还原、退出码与日志无异常。机器没有音频端点时以 77 退出，
  ctest 记为 Not Run 而不是失败，CI 无声卡也不会红。
- `tools/e2e_target.py`：与 `tools/VolumeTestTarget` 的闭环——受控进程播放静音，
  插件的写入必须能在目标进程自己的 `ISimpleAudioVolume` 读到，目标的会话也必须
  出现在插件的列表里；用目标进程的真实状态校验插件的说法。三个脚本都已接入
  ctest（Windows 上运行，无音频自动跳过）。
- `tools/e2e_device_switch.py`：换设备回归测试——用 Settings 同款的
  `IPolicyConfig` 接口在插件运行时切换系统默认输出，断言插件记录了重绑、读到了
  新设备的主音量、恢复后回到原值。需要设 `TVB_E2E_DEVICE_SWITCH=1` 才运行
  （它切换的是全局默认设备），声明时必须显式 `InterfaceIsIUnknown`——默认的
  IDispatch 布局会把每个方法排到错误的 vtable 槽位，调用即崩溃。

### Notes

- 更新后需重新启动插件进程。

## [0.1.3] - 2026-11-10

### Fixed

- Ctrl+左键点任务栏音量图标偶发失效：托盘音量按钮的命中区只有 24 px 宽，左侧还有
  4 px 死区，远程会话的指针取整、高 DPI 或触摸板只要偏两三个像素就完全无反应。
  现在按按钮尺寸把命中区向四周扩到相邻按钮为止（实测 24 px → 34 px），既覆盖死区
  也不会侵占网络/时钟按钮。
- 音量按钮改为按 Explorer 的类名 `SystemTray.OmniButtonRight` + AutomationId
  `SystemTrayIcon` 识别，不再依赖「音量/Volume」本地化名称，换显示语言也能用；名称
  白名单只作兜底。
- UI Automation 初始化失败不再静默：现在写入 `plugins.log`（含 HRESULT 文字），
  之前只会表现为「插件装了但完全没反应」。
- 解析线程自动恢复：`SetWinEventHook` 丢失（Explorer 重启、远程会话重连）每 5 秒
  重装一次；UIA 初始化失败也在同一周期重试，不再需要重启插件。
- `WH_MOUSE_LL` 安装失败不再重试 3 次后永久放弃，改为 300 ms/1 s/5 s/30 s 退避无限
  重试，并在恢复后记录日志。
- `plugin.json` 补上顶层 `icon`，插件列表页不再显示为占位图标。
- 任务栏每秒都会因为时钟的可访问名称含时间而触发 `EVENT_OBJECT_NAMECHANGE`，过去这会
  让应用按钮在约 40% 的时间里处于 stale（穿透）状态。现在只接受 `OBJID_WINDOW` /
  `OBJID_CLIENT` 的任务栏名称变化，实测 stale 采样从 41–49% 降到 2–8%，音量按钮在
  10 秒采样中 100% 可用。

### Notes

- 本次仅生成本地标签与安装包。更新后需重新启动插件进程。
- 远程会话实测：RDP 连接时默认输出设备是 `Remote Audio`（`GetDefaultAudioEndpoint`
  成功，主音量可读写）；RDP 断开后该端点消失，`GetDefaultAudioEndpoint` 返回
  `E_NOTFOUND`、活动端点数为 0。此时任务栏手势仍能被识别（UIA 正常），但音量操作
  会显示「无法读取系统音量/静音状态」。音频后端**不缓存**这次失败，每次操作都会重新
  枚举端点，所以重连后无需重启插件即可恢复。

## [0.1.2] - 2026-11-09

### Fixed

- 修复可执行文件图标，补齐打包中的 ICO 和资源脚本。 包含工作区中现有的 UI Automation 头文件顺序修复。 MSVC 构建使用静态运行库。

### Notes

- 本次仅生成本地标签与安装包。实际验证范围见 docs/icon-validation.md；未进行 GitHub 发布。更新后需重新启动插件进程。

## [0.1.1]

- Preserve the current plugin behavior and add a reproducible tag/manual build, test and package workflow.
