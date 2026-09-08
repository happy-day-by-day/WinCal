# WinCal Native

WinCal 的原生 Win32/C++ 迁移工程。当前阶段提供可运行的低内存 UI 壳，用来冻结原生窗口、托盘、DPI、任务栏定位与输入交互方案；原 WPF 项目暂时保留用于功能对照。

## 已实现

- Win32 托盘图标与左键开关、右键菜单
- 点击 Windows 任务栏日期/时钟区域时拦截系统日历并显示 WinCal
- 再次点击托盘关闭面板，点击面板外部自动收起
- 无边框 DWM 圆角日历弹窗，不使用 WPF 分层透明窗口
- Direct2D/DirectWrite 日历绘制
- 单击日期、悬停反馈和轻量淡入动画
- 方向键切换月份/年份，鼠标滚轮切换月份
- 每次打开恢复当前月份
- Per-Monitor DPI V2 与多显示器任务栏定位
- Explorer 重启后恢复托盘图标
- MSVC 静态运行库单体 EXE

## 待迁移

- Windows 系统日历（C++/WinRT）
- ICS 下载、缓存、解析和聚合
- 农历、节假日“休/班”角标和事件点
- 近期日程及详情窗口
- 原生设置页、主题与开机启动

## 构建

需要 Visual Studio 2022 C++ Build Tools（Desktop development with C++）。

```powershell
.\native\build.ps1 -Configuration Release
```

输出：`native/out/WinCal.exe`
