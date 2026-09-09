# WinCal WPF 归档版

这是 WinCal 的旧版 WPF 工程，已冻结，不再作为项目主工程。主工程和发布入口为仓库根目录下的 `native/` 原生 Win32/C++ 项目。

归档工程不再接收功能开发或 UI 调整；只有在需要历史对照、迁移设置格式或定位旧版行为时才应打开它。构建需要 .NET 8 SDK 和 Windows Desktop/WPF 支持，日常开发请使用原生工程的构建脚本。

如需历史对照，可在仓库根目录执行：

```powershell
dotnet build .\archive\wpf\WinCal.csproj
```

该工程引用根目录的共用图标资源 `Assets/WinCal.ico`。
