Birdie {version}
MSFS 2024 的 HUD 叠加层 · A HUD overlay for Microsoft Flight Simulator 2024
{repo}

(English below)

======================================================================
中文
======================================================================

需要
  - Microsoft Flight Simulator 2024，全屏、无边框、窗口化都可以（VR 下看不到 HUD）
  - ChasePlane：HUD 跟着它的驾驶舱镜头摆位置
  - Windows 10 / 11（64 位）。设置窗口用 Microsoft Edge WebView2，Windows 11 自带；
    没有的话设置会在浏览器里打开

安装
  1. 把整个压缩包解压到一个固定的文件夹，例如 D:\Birdie。不要在压缩包里直接双击运行。
  2. 双击 Birdie.exe。第一次运行会打开设置窗口，可以顺便装上游戏内面板（可选）。
  3. 如果 Windows 提示"Windows 已保护你的电脑"：点"更多信息"→"仍要运行"。
     Birdie 是个人开源项目，没有买代码签名证书。

使用
  - 详细说明（每个符号、设置、快捷键）：同一文件夹里的 Birdie-Manual-zh-CN.pdf。
  - 托盘里的小鸟：单击打开设置，右键是菜单。
  - 显示 / 隐藏 HUD：Ctrl+Alt+Insert（设置 → 细调 → 快捷键 里可以改）。
  - 进驾驶舱视角；起飞滑跑超过 40 kt 时 HUD 自动出现（设置 → 细调 → 自动显示 / 隐藏）。
  - 测试版的 HUD 在高度带上方显示"Birdie 测试版"，正式版没有。

游戏内面板（可选）
  - 装上后，模拟器顶部工具栏里有小鸟，飞行中不用切出去也能调设置。
  - 设置 → 细调 → 游戏内面板：安装 / 更新 / 移除 / 打开文件夹。
    Birdie 从 UserCfg.opt 自动找 Community 文件夹（商店版和 Steam 版都行）；找不到时可以自己选。
  - 装好后要重启一次模拟器。
  - Birdie 升级后，启动时会自动把已装的面板换成新版本。
  - 用插件管理工具（链接到 Community）的话：设置里"导出到别处…"，
    或者命令行 Birdie.exe --export-panel <文件夹>。

升级
  退出 Birdie（托盘右键 → 退出），用新版本的文件覆盖原来的文件夹，再启动。设置会保留。

卸载
  1. 装过游戏内面板的话：设置 → 细调 → 游戏内面板 → 移除。
  2. 开过"随 Windows 启动"的话先关掉，然后退出 Birdie。
  3. 删除 Birdie 所在的文件夹。设置和日志在 %APPDATA%\Birdie，也可以一起删掉。

出问题时
  - 日志：%APPDATA%\Birdie\birdie.log（设置 → 细调 → 日志 可以直接打开）。反馈问题时请附上它。
  - 反馈：{repo}/issues

作者
  Naux，与 Claude Opus 5.5（Anthropic）协作完成。

许可
  Birdie 是自由软件，按 GNU 通用公共许可证第 3 版（GPL-3.0）发布，见 LICENSE.txt；不提供任何担保。
  源代码：{repo}
  第三方组件的许可见 THIRD_PARTY_NOTICES.md 和 licenses 文件夹。
  Microsoft Flight Simulator、ChasePlane、FlyByWire、FSLabs、Fenix、Airbus 是各自所有者的商标；
  Birdie 与它们没有关联，也没有得到它们的认可。

======================================================================
English
======================================================================

Requirements
  - Microsoft Flight Simulator 2024: full screen, borderless or windowed (the HUD does not show in VR)
  - ChasePlane: the HUD follows its cockpit camera
  - Windows 10 / 11, 64-bit. The settings window uses Microsoft Edge WebView2 (part of Windows 11);
    without it the settings open in your browser

Install
  1. Extract the whole zip to a folder of its own, e.g. D:\Birdie. Do not run it from inside the zip.
  2. Run Birdie.exe. The first time, its settings window opens and offers the in-sim panel (optional).
  3. If Windows says "Windows protected your PC": click "More info", then "Run anyway".
     Birdie is a personal open-source project without a code-signing certificate.

Use
  - The full manual (every symbol, setting and hotkey): Birdie-Manual-en.pdf in this folder.
  - The bird in the tray: click for the settings, right-click for the menu.
  - Show / hide the HUD: Ctrl+Alt+Insert (change it under Settings > Fine tuning > Hotkeys).
  - Use the cockpit view; the HUD comes up by itself once the take-off roll passes 40 kt
    (Settings > Fine tuning > Show / hide by itself).
  - Test versions show "Birdie beta" above the altitude tape; release versions do not.

In-sim panel (optional)
  - Puts the bird in the sim's top toolbar, to adjust things without leaving the flight.
  - Settings > Fine tuning > In-sim panel: install / update / remove / open folder.
    Birdie finds the Community folder from UserCfg.opt (Microsoft Store and Steam); if it cannot,
    choose it yourself.
  - Restart the sim once after installing.
  - After an upgrade, Birdie replaces an installed panel with the new version when it starts.
  - Add-on managers (linked into Community): use "Export elsewhere..." in the settings,
    or run Birdie.exe --export-panel <folder>.

Upgrade
  Quit Birdie (tray > right-click > Quit), copy the new files over the old folder, start it again.
  Your settings stay.

Uninstall
  1. If you installed the in-sim panel: Settings > Fine tuning > In-sim panel > Remove.
  2. Turn off "Start with Windows" if you turned it on, then quit Birdie.
  3. Delete Birdie's folder. Settings and the log live in %APPDATA%\Birdie; delete that too if you like.

When something goes wrong
  - Log: %APPDATA%\Birdie\birdie.log (Settings > Fine tuning > Log opens it). Please attach it to reports.
  - Reports: {repo}/issues

Credits
  Birdie by Naux, built together with Claude Opus 5.5 (Anthropic).

License
  Birdie is free software under the GNU General Public License, version 3 (GPL-3.0), see LICENSE.txt;
  it comes with no warranty. Source code: {repo}
  Third-party licenses: THIRD_PARTY_NOTICES.md and the licenses folder.
  Microsoft Flight Simulator, ChasePlane, FlyByWire, FSLabs, Fenix and Airbus are trademarks of their
  owners. Birdie is not affiliated with or endorsed by any of them.
