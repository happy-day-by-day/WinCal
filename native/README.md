# WinCal Native

WinCal 的原生 Win32/C++ 迁移工程。当前阶段提供可运行的低内存 UI 壳，用来冻结原生窗口、托盘、DPI、任务栏定位与输入交互方案；原 WPF 项目暂时保留用于功能对照。

## 已实现

- Win32 托盘图标与左键开关、右键菜单
- 点击 Windows 任务栏日期/时钟区域时拦截系统日历并显示 WinCal
- 再次点击托盘关闭面板，点击面板外部自动收起
- 无边框 DWM 圆角日历弹窗，不使用 WPF 分层透明窗口
- Direct2D/DirectWrite 日历绘制
- Direct2D 软件渲染目标：本机实测弹窗私有内存约 16–19 MB；硬件优先目标约 53 MB，因此不作为默认路径
- 单击日期、悬停反馈和轻量淡入动画
- 方向键切换月份/年份，鼠标滚轮切换月份
- 每次打开恢复当前月份
- 读取现有设置并从磁盘 ICS 缓存即时展示事件点及“休/班”角标
- 使用 Windows 中国农历在日期格显示农历日期与常见传统节日，不引入额外常驻组件
- 选中日期后展示当天全部日程；日程区内滚动只滚列表，其他区域滚轮仍切换月份
- 点击日程列表项显示原生日程详情，避免长标题在面板内被截断
- 原生设置窗口：主题、实时文字大小预览、数据源、ICS 链接与刷新频率、周起始日
- 设置保存后即时刷新日历外观和数据源；保留既有 JSON 设置与 ICS 别名的 Unicode 转义兼容
- 可选开机自启动，使用当前用户 Run 键并指向单体 EXE；已启用时自动从旧版可执行文件迁移
- 启动后按刷新间隔在后台更新 ICS，验证成功后原子替换缓存并重绘界面
- 展开常用 ICS 重复规则，支持 `COUNT` / `UNTIL` / `INTERVAL` / `BYDAY` / `BYMONTHDAY` / `BYMONTH`、`EXDATE` 与 `RDATE`
- 支持 `BYSETPOS`、`WKST` 和常见内嵌 `VTIMEZONE` 夏令时规则
- 将 UTC 及 IANA/Windows/内嵌 `TZID` 事件转换为系统本地时间
- 对多份 ICS 及系统日历结果做保守去重，节假日优先保留“休/班”标记
- 通过 C++/WinRT 按显示月份增量读取并缓存 Windows 系统日历，支持与 ICS 合并
- Per-Monitor DPI V2 与多显示器任务栏定位
- Explorer 重启后恢复托盘图标
- MSVC 静态运行库单体 EXE

## 待发布

- 发布前视觉与交互回归：托盘、任务栏四方向、双屏、DPI、主题、ICS/系统日历合并、日程详情面板
- 发布目录和版本信息整理

## 构建

需要 Visual Studio 2022 C++ Build Tools（Desktop development with C++）。

```powershell
.\native\build.ps1 -Configuration Release
```

输出：`native/out/WinCal.exe`

用 `WinCal.exe --settings` 可以直接打开设置窗口；若已有 WinCal 实例，会复用该进程。

设置页使用「外观 / 日历来源 / 常规」页签，不内嵌模拟日历。打开设置时同步打开真实日历并并排定位；主题、字号、周起始日立即更新真实日历。设置打开期间日历不会因点击设置而收起，保存后保留修改，取消或关闭设置会恢复原来的外观。开机启动和 ICS 订阅在保存后生效。字体和控件统一使用 DIP，切换显示器 DPI 时重新布局，工作区不足时提供滚动条。设置窗口仍然按需创建，关闭时释放字体、画刷和控件。

设置页离屏检查（不启动托盘、不读写用户配置、不访问日历服务）：

```powershell
cmake --build native/build --config Release --target WinCalSettingsPreview
# 参数：DPI、页签 0/1/2、深色 0/1、字号偏移、BMP 输出路径
.\native\build\Release\WinCalSettingsPreview.exe 120 0 0 7 native/out/settings-appearance.bmp
```

此目标仅在显式构建时生成，不包含在单体 EXE 中；同时检查字号边界、页签、主题、开关、订阅禁用，以及实时外观更新、取消恢复和保存保留逻辑。

## 冒烟验证

```powershell
cmake --build native/build --config Release --target WinCalNativeSmoke
.\native\build\Release\WinCalNativeSmoke.exe
```

验证中国农历不为空，并确认当前用户的设置及 ICS 别名能够读取。
