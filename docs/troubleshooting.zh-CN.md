# 常见问题

[English](troubleshooting.md) | 简体中文

| 症状 | 处置 |
| --- | --- |
| helper 启动时报 `No usable sandbox!` | 系统限制了非特权用户命名空间。为 `cefnest-helper` 安装一个允许 userns 的 AppArmor profile（推荐），或设置 `CHROMIUM_HELPER_NO_SANDBOX=1` |
| `start()` 报 `no helper binary at …` | 报错里的路径就是实际尝试的路径：安装 `cefnest-helper`，或用 `CEFNEST_HELPER` 指向绝对路径；在开发树里先运行 cmake |
| 窗口出现，页面区域一片空白 | 确认 helper 正在运行，且当前是原生 Wayland 会话（`GDK_BACKEND=wayland`）；X11 下不会渲染 |
| 设置了 `G_MESSAGES_DEBUG` 却没有任何日志 | 域列表必须用**空格**分隔；用逗号分隔的列表匹配不到任何域 |
| 关闭最后一个标签页，页面就没了 | 一个 session 对应一个 helper；先新建一个 browser，或正确处理 `::closed` |
| 在 `::helper-exited` 之前先看到 `Lost connection to Wayland compositor` | 这是正常的销毁顺序——等收到 `::helper-exited` 之后再销毁自己的 display |
| helper 在 stderr 上几乎不出声 | 刻意为之：Chromium 日志级别为 WARNING，逐事件的 INFO 输出已去掉；警告与错误仍会打印 |
