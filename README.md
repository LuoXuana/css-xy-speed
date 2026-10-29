# CSS XY Speed

用于 **Counter-Strike: Source x64** 的客户端水平速度显示工具，面向连接高延迟服务器时，服务器速度 HUD 更新滞后的情况。

它直接读取本地角色速度并随游戏画面绘制，不等待服务器发送 HUD 文字。显示的是客户端观测值，不是独立的服务器测量；本工具不会降低网络延迟，也不保证消除服务器纠正引起的数值跳变。当前取样位于客户端输入回调，尚未完成高延迟环境下的预测时序验证。

## 下载与使用

从本仓库 **Releases** 下载完整 ZIP 并解压。先启动 64 位 CS:S、进入地图，再运行 `启动速度显示.cmd` 或 `Start-XYSpeed.cmd`。

默认显示 `1234.56 u/s`，使用平滑的系统字体。不会修改灵敏度或服务器参数。

| 按键 | 功能 |
| --- | --- |
| F8 | 打开位置调整；左键按住速度文字拖动，松开自动保存。再次按 F8 结束 |
| Esc | 结束位置调整并返回游戏 |
| Shift+F8 | 显示／隐藏 |
| F9 | 重新读取 `xy-speed.ini` |
| F11 | 卸载 DLL |

位置调整会打开游戏菜单释放鼠标，服务器仍继续运行，请先在安全位置站稳。切出游戏会结束调整并保留菜单。

在 `xy-speed.ini` 中可修改 `X`、`Y`（相对位置，0～1）、`FontSize`（12～96）和 `Decimals`（0～2）。修改后按 F9。

支持 Windows x64、CS:S build **11003710** 和 D3D9。不支持 CS2 或 32 位 CS:S。当前版本不保证适配其他客户端修改或渲染包装层，也不承诺 VAC 安全性。

## 构建

需要 Visual Studio 2022 的 C++ 桌面开发工具、Windows SDK、CMake 3.24 或更新版本。

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel 2
ctest --test-dir build -C Release --output-on-failure
powershell -NoProfile -ExecutionPolicy Bypass -File tools/Package.ps1
```

构建完全使用仓库内所需的依赖源码。发布脚本仅打包 DLL、专用加载器、默认设置、启动脚本、说明和依赖许可证；不包含日志、调试符号、个人配置或构建目录。

## 实现与验证范围

水平速度为 `sqrt(vx² + vy²)`，单位为游戏单位／秒，忽略垂直速度。客户端输入回调用于读取角色状态；D3D9 用于绘制。正常显示不修改移动输入。调整模式仅通过游戏菜单释放鼠标并处理编辑点击。

包含独立隐藏窗口测试，覆盖输入转发、编辑期间拦截、失焦恢复、卸载恢复、位置保存和字体加载。实际游戏显示及高延迟下的实时性仍需验证，测试通过不代表反作弊兼容性保证。

## 依赖与许可证

项目代码使用 MIT 许可证。仅包含所需的 Dear ImGui 1.91.9b（MIT）和 MinHook 1.3.4（BSD）源码，原始许可证保留在 `third_party` 目录。字体在运行时读取 Windows 系统文件，不随本项目分发。
