# WinCal

WinCal 是一个面向 Windows 的轻量日历托盘工具。当前项目主线是原生 Win32/C++ 版本，源码、构建脚本和发布入口都位于 [`native/`](native/)。旧版 WPF 实现已经冻结并归档，不再作为默认开发或发布入口。

## 功能

- 托盘图标和任务栏日期/时间区域唤起，自动根据任务栏所在方向定位弹窗
- 月份、年份键盘切换，鼠标滚轮切换月份，打开时回到当前月份
- 农历、传统节日，以及节假日“休”和补班“班”角标
- Windows 系统日历、ICS 订阅或两者合并；支持本地缓存、别名、自动刷新和常见重复规则
- 日期悬停、选中反馈和淡入动画；选中日期后查看日程列表与详情
- 原生设置窗口：主题、字号、数据源、ICS 链接、刷新频率和周起始日
- Per-Monitor DPI、多显示器任务栏定位、Explorer 重启恢复托盘图标
- Direct2D 软件渲染和 MSVC 静态运行库，保持单体 EXE 和较低常驻开销

## 目录结构

```text
native/           原生 Win32/C++ 主工程、构建脚本和测试
native/src/       日历数据、农历、设置和窗口实现
native/tests/     原生冒烟测试与设置页离屏检查
native/resources/ Win32 manifest 和图标资源配置
archive/wpf/      已冻结的旧版 WPF 工程，仅供历史对照
Assets/           与两个工程共用的 WinCal.ico
```

## 环境要求

- Windows x64
- Visual Studio 2022 C++ Build Tools（工作负载 **Desktop development with C++**）
- CMake 3.25 或更高版本；构建脚本会优先使用 Visual Studio 自带的 CMake

## 构建与运行

在仓库根目录执行：

```powershell
.\native\build.ps1 -Configuration Release
```

脚本会配置、编译并安装原生工程，产物为 [`native/out/WinCal.exe`](native/out/WinCal.exe)。运行程序或直接打开设置：

```powershell
.\native\out\WinCal.exe
.\native\out\WinCal.exe --settings
```

如果已有 WinCal 实例，`--settings` 会复用该实例并打开设置窗口。

## 验证

原生冒烟测试覆盖农历、设置读取和 ICS 别名读取：

```powershell
cmake --build native/build --config Release --target WinCalNativeSmoke
.\native\build\Release\WinCalNativeSmoke.exe
```

设置页离屏检查不会启动托盘、读取用户配置或访问日历服务：

```powershell
cmake --build native/build --config Release --target WinCalSettingsPreview
# 参数依次为：DPI、页签（0/1/2）、深色（0/1）、字号偏移、BMP 输出路径
.\native\build\Release\WinCalSettingsPreview.exe 120 0 0 7 native/out/settings-appearance.bmp
```

更完整的原生功能说明、设置页行为和发布前检查项见 [`native/README.md`](native/README.md)。

## 配置与数据

- 设置文件：`%LOCALAPPDATA%\miniCal\settings.json`
- ICS 缓存：`%LOCALAPPDATA%\WinCal\cache\`
- 数据源可选“系统日历”“ICS 订阅”或“系统日历 + ICS”
- ICS 自动刷新支持 10 分钟、30 分钟、60 分钟、120 分钟和 1 天

设置页关闭后才释放其临时资源；保存设置后日历会即时刷新，取消或关闭则恢复原来的外观设置。

## 项目状态

原生 Win32/C++ 已作为主工程使用，WPF 工程已移入 [`archive/wpf/`](archive/wpf/)。当前剩余工作主要是发布前的多显示器、DPI、主题、双数据源和日程详情回归，以及发布目录和版本信息整理。

## 归档工程

旧版 WPF 工程只用于历史对照和必要的数据兼容参考，说明见 [`archive/wpf/README.md`](archive/wpf/README.md)。
