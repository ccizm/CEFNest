# 运行前提、三种跑法，和报出来之后怎么查

日期：2026-10-23
问题：README 讲怎么把树构建出来、怎么装；这一篇讲的是另一头——一个跑起来的部件需要宿主给什么，
以及它没跑起来时那句错误对应哪一条决定。构建参数与安装布局不在这里重复
（`README.md` 的 Requirements/Build/Install、`docs/guides/packaging.md` §1）。

## 1. 运行前提

| 条件 | 要求 | 为什么 |
| --- | --- | --- |
| 桌面与显示 | 原生 Wayland 会话，GDK 走 Wayland 后端 | X11 / XWayland 下没有可交给 client 的 Wayland surface，部件不会工作 |
| Wayland 库 | 1.21+，含 `axis_value120` | 指针高分辨率滚轮与合成器实现都要这一代协议 |
| GTK | 4.14+ | 部件、帧时钟与输入法接口的下限 |
| Chromium 沙箱 | 可用的非特权用户命名空间 | 禁 userns 的宿主以 `No usable sandbox!` 拒绝启动，不会静默降级 |
| CEF 运行时 | 由 `cefnest-helper` 携带 | 包内已含 `libcef.so`、locales、icudtl.dat 等，安装后约 1.5 GB |

前两条是设计而不是疏漏：这个部件拿像素的唯一途径是让它自己的嵌套合成器接收 client 提交的
surface，所以宿主必须真有一个 Wayland 显示。沙箱那一条的完整决策（为什么不需要 setuid helper、
没有用户命名空间的宿主到底能跑到什么程度）在 `docs/internals/sandbox-policy.md`。

## 2. 三种跑法

```bash
# 1. 开发树 + 真 Chromium（cmake 构建过 helper）
CHROMIUM_URL=https://cnb.cool/yanzhiming/CEFNest ./builddir/src/cefnest-demo

# 2. 安装树 —— helper 在安装路径里，不用再指路径
CHROMIUM_URL=https://cnb.cool/yanzhiming/CEFNest cefnest-demo

# 3. 不带 Chromium：指一个不存在的 helper，demo 退回 shm-client
CEFNEST_HELPER=/nonexistent-helper ./builddir/src/cefnest-demo
```

第三种不是取巧：`shm-client` 是一个只走 Wayland 协议的客户端，用它就能把「合成器与部件这一侧
对不对」和「Chromium 那一侧对不对」分开判。全量环境变量表在 `docs/testing/overview.md`。

## 3. 排查

每一条都被某次真实运行验证过，不是猜的处置。

| 症状 | 处置 |
| --- | --- |
| helper 启动即报 `No usable sandbox!` | 宿主限制了非特权用户命名空间。给 `/usr/libexec/cefnest/<arch>/cefnest-helper` 加一条允许 userns 的 AppArmor profile（推荐），或显式设 `CHROMIUM_HELPER_NO_SANDBOX=1`。写法见 `docs/internals/sandbox-policy.md` |
| `start()` 报 `no helper binary at …` | 错误里那串就是本次选中的路径。装好 `cefnest-helper`，或用 `CEFNEST_HELPER` 指一个绝对路径；开发树则先跑 cmake。五步查找规则在 `docs/guides/packaging.md` §2 |
| 窗口起来了，页面区域一直空白 | 先确认 helper 在跑、日志里没有沙箱或连接错误；再确认是原生 Wayland 会话（`GDK_BACKEND=wayland`），X11 下不会渲染 |
| 设了 `G_MESSAGES_DEBUG` 却没有日志 | 域列表要用空格分隔。想看全部就用 `all`；可用的具体域是 `nested`、`chromium-view`、`chromium-session` |
| 关掉最后一个标签，整个页面都没了 | 一个 session 只有一个 helper，最后一个 browser 关闭即结束 helper。要保留页面就先开新 browser，或接受 `::closed`（`docs/features/session-lifecycle.md`） |
| 先看到 `Lost connection to Wayland compositor`，才收到 `::helper-exited` | 这是拆除阶段的正常顺序：信号触发的 `CefShutdown()` 还在用 Wayland 连接。等 `::helper-exited` 再销毁共享合成器 |
| 自定义前缀编译出的宿主找不到 `libcefnest.so` | 为自定义前缀加 `LD_LIBRARY_PATH` 或 ldconfig 条目。安装的 demo 不需要——它的 `DT_RUNPATH` 是 `$ORIGIN` 相对 |
