# docs/ 索引

这里的文档都是记录，不是教程：每一篇回答一个问题——某个功能实测下来是什么形状、某条
设计为什么这样定、某个门禁怎么跑——并带着测量的前后证据。除从英文 README 搬入的
`docs/testing/overview.md` 与 `docs/known-limitations.md` 外均为中文。

## 先读哪篇

- **想把 demo 跑起来**：仓库 `README.md`（构建/安装/运行），然后
  `docs/testing/overview.md`（怎么跑测试）。
- **想把这个库接进自己的应用**：`docs/guides/minimal-host.md`（三个对象、必需顺序、
  API 对照表）→ `docs/guides/control-channel.md`（通道上每条命令与事件）→
  `docs/guides/packaging.md`（装出来什么、session 怎么找 helper）。边角行为看
  `docs/known-limitations.md`。
- **想知道某个页面功能实际什么行为**：`docs/features/` 里对号入座。
- **想知道某条设计为什么这样定**：`docs/internals/`。从 `docs/internals/nested-split.md`
  （怎么按协议拆模块）和 `docs/internals/frame-clock-pacing.md`（帧时钟与看门狗的整个
  往返）开始最能建立全貌。
- **想知道测试基建怎么跑**：`docs/testing/overview.md` → `docs/testing/headless-ci.md`。

## guides/ —— 宿主接线（用库的人）

| 文档 | 关于 |
| --- | --- |
| `docs/guides/minimal-host.md` | 三个对象的宿主、必需的顺序、导航/JS/下载/菜单/主选区/无障碍该加哪个 API/信号的对照表，以及 §5：中等示例、它的脚本化运行测到了什么、那个曾经关不掉的标签是怎么修好的 |
| `docs/guides/control-channel.md` | socket 上的每条命令与事件、失败的加载和死掉的 renderer 长什么样、被决定不做的命令 |
| `docs/guides/packaging.md` | 什么装到哪、session 找 helper 的五步规则、为什么 `get_option('libexecdir')` 必须先拼进 prefix 再编进去、两个方向故意相反的回退 |

## internals/ —— 设计记录（为什么长这样）

| 文档 | 关于 |
| --- | --- |
| `docs/internals/nested-split.md` | `nested.c` 怎么按协议拆成模块 |
| `docs/internals/frame-clock-pacing.md` | 按 GTK 帧时钟而不是定时器绘制、给永远不来的显示周期配的看门狗，以及发现看门狗悄悄把时钟路径从帧上拿掉的那一轮 |
| `docs/internals/frame-metrics.md` | compositor 帧计数器 / 脚本与环境导出，以及软件路径的 pool mmap + damage 影子拷贝 |
| `docs/internals/popup-compositing.md` | 页面弹窗的 surface 栈、摆放与关闭 |
| `docs/internals/resource-limits.md` | 每 pool 和每连接的 shm 上限、分配检查、dmabuf fd 泄漏 |
| `docs/internals/empty-request-slot.md` | 一个 NULL 请求槽位杀死整个桌面 |
| `docs/internals/clipboard-offer-during-client-init.md` | 一个合法却早一次 dispatch 发出的 `data_offer` 杀死一个 GTK client |
| `docs/internals/sandbox-policy.md` | 沙箱决策：不需要 setuid helper、没有用户命名空间的宿主对运行意味着什么、让进程树和 fd 数在反复导航与两次 `kill -9` renderer 之间保持平稳的 soak（即 `test/sandbox-soak.sh`），以及为什么这里没有崩溃处理器可修可关 |
| `docs/internals/protocol-ceilings.md` | 真实 client 对嵌套 compositor 绑定什么，以及每个未通告的协议与版本上限的实测判定 |
| `docs/internals/dmabuf-formats.md` | feedback 格式表由本进程能导入什么——GDK 通告的集合——决定，而不是静态四格式通告，附带对 CEF 全线性 buffer 的实测后果 |
| `docs/internals/map-cycle.md` | 每次 map 挂的东西都得有拆下的路（M11 发现：N 次 map 周期，N 份 handler）；守住这条规则的两个门禁及各自被验证过的变异 |
| `docs/internals/stack-canary-after-fork.md` | 那条"偶发 `*** stack smashing detected ***`"的归因：zygote 的 fork-without-exec 子进程继承 `main` 的帧却换掉了 `%fs:0x28`，于是 glibc 在我们自己的尾言里报警；修法是不走尾言，附三条未量到的边界 |

## features/ —— 功能行为记录（实测形状）

| 文档 | 关于 |
| --- | --- |
| `docs/features/session-lifecycle.md` | 与 helper 一起启动、停止和死亡，含 helper 收 SIGTERM/SIGHUP/SIGINT 的那一条（handler 只能装在 `CefInitialize()` 之后）与它的 12 条断言门禁 |
| `docs/features/multi-browser.md` | 单 helper 的 browser 注册表、标签条、独立宿主窗口、协议 id、按窗口的输入交还规则及其门禁，以及剩下的弹窗警告 |
| `docs/features/clipboard-bridge.md` | `wl_data_device` ↔ `GdkClipboard` 双向 |
| `docs/features/drag-loop.md` | 页面内拖拽的编排、"wl_data_device.enter 到得了页面"的实测证明、目的地真正接受什么、为什么 fd 中继不是同页 payload 的载体、跨越窗口边缘的拖拽怎么从页面的 source 上读出来交给宿主（图片字节落成以文件列表提供的临时文件）、反方向的同一跨越怎么让 compositor 当 source——包括"页面在被 enter 的一刻就读宿主 drag"那条测量，所以只有 `finish` 能定判定 |
| `docs/features/wheel-resolution-and-key-repeat.md` | `axis_value120`、每个手势一帧、自动重复的记账 |
| `docs/features/download-path.md` | 停靠的 `OnBeforeDownload` 门、谁定路径，以及只有测量才能告诉你的四种 CEF 下载行为 |
| `docs/features/find-path.md` | 一条不回答的命令、只有测量才给的回复形状（每请求一个 identifier、每请求一到三次更新），以及真正让页面滚动的东西 |
| `docs/features/context-menu.md` | 决定整个设计的唯一约束（Chromium 只握一个未回答的菜单请求）、CEF Alloy 默认模型里到底有什么、helper 加的链接/图片/媒体行、以及 `Continue(id)` 在页面内被证明 |
| `docs/features/permission-prompt.md` | 哪些请求根本到不了宿主、让一个不回答的请求连累页面下一个问题的两条队列，以及为什么 `accept` 只能是"整个掩码" |
| `docs/features/zoom-path.md` | CEF 缩放级别、半级步长、按来源持久化、`load-end` 为什么重报级别、demo 的键位 |
| `docs/features/audio-path.md` | 页面的声音实际走哪：一个不带沙箱的 audio 服务直通宿主扬声器，以及静音测到什么 |
| `docs/features/pdf-path.md` | 实测的 PDF 查看结果：widget dump 里的缩略图/翻页控件，以及没测的东西（表单、注释、打印） |
| `docs/features/a11y.md` | 读屏器能否够到嵌套架构里的页面：能，因为 Alloy 的 windowed 浏览器在 `STATE_ENABLED` 就是 Chromium 自己的 Complete AT-SPI 模式；切换它的 session-bus 监听器，以及 ORCA 看到了什么 |
| `docs/features/fullscreen.md` | Alloy 在 `requestFullscreen()` 上只*通知*——触发迁移的是 client；从 `OnFullscreenModeChange` 到 `xdg_toplevel.set_fullscreen` 的接线，以及实测的改前 |

## testing/ —— 测试与 CI

| 文档 | 关于 |
| --- | --- |
| `docs/testing/overview.md` | 全部测试怎么跑、每个门禁证明什么、断言背后的实测数字与被验证过的变异，以及 demo 的完整环境变量表 |
| `docs/testing/headless-ci.md` | 对着没人看得见的嵌套 mutter 跑门禁：为什么 session 要自己的 HOME 和 bus、一个信号怎么收走整棵进程树、没有输出设备的显示器能证明什么不能证明什么 |
| `docs/testing/cnb-ci.md` | 仅在版本 tag 上构建的发版流水线（分层依赖的四个包、Release 资产、修订号）；完整门禁的本地/自托管准备 |
| `docs/testing/real-sites.md` | 五站点真实页面巡检批次：每站一个嵌套 session、截图、JS 读回、3 秒 rAF 计数，以及实测的 H.264/VP9 结论 |

## 根目录的元记录

| 文档 | 关于 |
| --- | --- |
| `docs/known-limitations.md` | 已知限制的完整记录：每条带着它的测量（README 里是每条一行的摘要） |
| `docs/structural-decisions.md` | 有意留给产品决策的两件事：标签/窗口的浏览器路由，以及 arm64/loong64 的 CEF 供给 |
| `code-review-2026-10-22.md` | 带日期的评审存档（原在仓库根）——里面的路径引用保留搬家前的原样，是历史记录 |
