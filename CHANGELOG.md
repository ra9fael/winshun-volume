# Changelog

<<<<<<< HEAD
## [0.1.1]

- Preserve the current plugin behavior and add a reproducible tag/manual build, test and package workflow.
=======
[English](CHANGELOG.en.md)

## [Unreleased]

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
>>>>>>> 08067e4 (fix(volume): package and assign plugin icons)
