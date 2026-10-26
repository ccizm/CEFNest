# 剩余缺口计划（上报集合 + 咨询式接口 + frame 生命周期，2026-10-26）

本文立项 `docs/cef-features.zh-CN.md` 第十一节缺口清单里**剩余适用**的项。
承接 [work-plan-2026-10-26.md](work-plan-2026-10-26.md)（已历史化）与
[epiphany-host-plan-2026-10-26.md](epiphany-host-plan-2026-10-26.md)（16 项
全部落地）；[response-filter-plan-2026-10-26.md](response-filter-plan-2026-10-26.md)
独立推进，互不阻塞。分类判据、四交互形态、工程约束全部沿用总计划第一/四
部分，本文只写缺口与增量，不重复裁定。

**遴选判据**（2026-10-26 已按代码与 CEF 156 头文件核实）：CEF 有面、helper
确实没接（`grep` 核对 `helper/src/main.cpp` 的 override 集）、且没有任何
在册计划覆盖。"缺口清单"里的其余项各有裁定：音频流/拖放介入/无障碍树/
桌面捕获/媒体会话/下载续传/RunModal/Widevine 维持不做（第六部分），PDF
查看器降为宿主配方（第四部分）。

## 第零部分 · 现状锚点（2026-10-26 已核实）

| 事实 | 锚点 |
| --- | --- |
| HelperClient 挂着 LifeSpan/Load/Display/Focus/Keyboard/Request 六个 handler；Display 已覆写 OnAddressChange/OnTitleChange/OnFaviconURLChange/OnFullscreenModeChange/OnAutoResize/OnLoadingProgressChange | `helper/src/main.cpp:6817` |
| `OnConsoleMessage`/`OnTooltip`/`OnStatusMessage`/`OnMediaAccessChange`/`OnCursorChange` 未覆写；`CefFrameHandler`/`CefDragHandler`/`CefAudioHandler`/`CefAccessibilityHandler` 整类缺席 | 同上 + `cef-features` 附录 A 差集 |
| fire-and-forget 事件先例：`NewEvent()` 填名 + `Emit()` 单行 JSON；devtools/background 一律不上通道 | `helper/src/main.cpp:965`、`OnLoadingProgressChange:7463` |
| parked 弃单先例：`DropDialogsForBrowser` 在 OnBeforeClose 对每种 parked 单据回 `*-closed` | `helper/src/main.cpp:4007`、`:7384` |
| spawn 期开关先例：env → `OnBeforeCommandLineProcessing` 并入命令行（proxy/accept-lang/spellcheck 同型）；session 端 `g_subprocess_launcher_setenv` + 无值 `unsetenv` 防污染 | `helper/src/main.cpp:6682`、`src/chromium-session.c:5494` |
| 自动播放现状：helper 全文零 `autoplay` 命中，审计文档自注"该开关还未接" | `cef-features` §八 |
| 打印现状：`HelperPrintHandler` 全 stub，`OnPrintDialog` 返回 false——`window.print()` 静默 no-op；`print_pdf` 命令（CDP PrintToPDF 流式回传）已在 | `helper/src/main.cpp:4286` |
| 指纹豁免表 `g_cert_authorized`（helper 生命周期）已由 `certificate-authorize` 维护，无撤销入口 | `helper/src/main.cpp` HandleCommand |
| 崩溃/挂起上报先例：`renderer-exited` 事件；`OnRenderProcessUnresponsive` 未覆写（CEF 默认 = Alloy 静默继续等待） | `helper/src/main.cpp:7653`、`cef_request_handler.h:247` |
| CEF 面核实：`OnConsoleMessage`（cef_display_handler.h:116）、`OnTooltip`（:99）、`OnStatusMessage`（:108）、`OnMediaAccessChange`（:163）、`OnSetFocus`（cef_focus_handler.h:70）、frame 五事件（cef_frame_handler.h:126–186）、`CefUnresponsiveProcessCallback::Wait/Terminate`（cef_unresponsive_process_callback.h） | 全部在 |
| 协议版本 `kHelperProtocolVersion = 2`、`SESSION_CHANNEL_MAJOR = 2`，大版本以 100 分档 | `helper/src/main.cpp:162`、`src/chromium-session.c:139` |
| demo 脚本步进先例（gate 的驱动方式）：`load`/`js`/`wait`/`quit`/`ua-set`…，fire-and-forget 断言走 helper 日志行 + demo 信号日志行双面 | `src/main.c:3490`、`test/favicon-gate.sh` |

---

# 第一部分 · 批一 · 上报集合（fire-and-forget + 一个属性，宿主零应答）

共性：全部是"组件把 Chromium 已经知道的事搬到宿主"——无策略、无落盘、
无往返。事件命名沿用现有小写连字符惯例；`NewEvent()` 已带活动 browser，
点名浏览器时显式覆盖（`OnTitleChange` 的先例）。devtools/background 的
过滤沿用 Display 系先例（标题/地址/进度同款）：**inspector 的输出不是页面
的输出**。media-access 一条例外：后台页同样能占住麦克风，指示器必须亮
（下文 1.5 单独裁定）。

## 1.1 自动播放策略（组件集成，属性）

- **CEF 面**：Chromium `--autoplay-policy` 命令行开关（CEF 无运行时 API，
  与 proxy 同型——只能在 `CefInitialize` 前定型）。
- **形状**：session 属性 `autoplay-policy`（string；空 = Chromium 默认，
  不追加开关；非空值原样透传，推荐 `"user-gesture-required"` /
  `"document-user-activation-required"`）。start() 前设置生效、start 后
  设置仅下次 start（proxy 语义）。env `CHROMIUM_HELPER_AUTOPLAY_POLICY`
  → `OnBeforeCommandLineProcessing` 追加；无值 `unsetenv`（DATA_DIR 防污染
  先例）。per-site 不可行（CEF 无面），文档写明"全局一刀切"。
- **Session API**：`chromium_session_get/set_autoplay_policy()`。
- **门禁**：`test/autoplay-gate.sh`——探针页在 document 载入即建
  `AudioContext` 并回读 `state`：默认策略 `running`；设
  `user-gesture-required` 后 `suspended`。AudioContext 是确定性探针（不依
  赖媒体文件与真实声卡，门禁环境无音频设备也能断言）。

## 1.2 控制台消息（上报）

- **CEF 面**：`CefDisplayHandler::OnConsoleMessage`（cef_display_handler.h:116）。
- **形状**：`console {browser, level, message, source, line}`——level 映射
  名字（debug/info/warning/error），message 4 MiB 行限内的截断沿用通用
  约束（超长 message 掐到 1 MiB 并在尾部加 "…"，页面 console 不值得为它
  开流式三段）。
- **用途**：壳子的页面报错采集、开发者面板地基（缺口#2 的正面回答）。
- **门禁**：`test/console-gate.sh`——探针页 `console.error/info` 各一条，
  断言 helper 日志与 `::console` 信号行各两条、level 对。

## 1.3 悬停文案（上报：tooltip + status message）

- **CEF 面**：`OnTooltip`（:99）/`OnStatusMessage`（:108）。
- **形状**：`tooltip-changed {browser, text}`（text 为空 = 收起，鼠标
  移出或悬停结束即来一条空）；`status-message {browser, text}`（悬停链接
  时是目标 URL）。**两条都返回 true**——抑制 CEF views 自己的 tooltip
  弹层，悬停提示的画法归宿主（状态栏/地址栏尾部）。
- **门禁**：并入 `console-gate.sh` 同页断言：悬停带 `title` 的链接
  （脚本步进 `hover` 已有）后 `tooltip-changed` 携带 title 文案。

## 1.4 焦点请求（上报）

- **CEF 面**：`CefFocusHandler::OnSetFocus`（cef_focus_handler.h:70，
  FocusSource：navigation / popup）。
- **形状**：`focus-requested {browser, source}`，返回 false（不拦——页面
  要焦点照给，宿主只是知情；窗口激活联动是宿主的事）。
- **门禁**：源码级 wiring 断言并入 `console-gate.sh`（自然触发要真弹窗，
  不值得在门禁里造；拦截本身零分支）。

## 1.5 媒体占用指示（上报）

- **CEF 面**：`OnMediaAccessChange`（cef_display_handler.h:163）。
- **形状**：`media-access-changed {browser, video, audio}`。
  **过滤裁定**：只滤 devtools，**不滤 background**——后台页占住麦克风时
  硬件灯是亮的，指示器不亮就是谎报；这与 Display 系"后台不上通道"的
  惯例是一处有意例外，注释写明理由。
- **门禁**：`getUserMedia` 探针页 + fake 设备（`--use-fake-device-for-
  media-stream` 需要注入……CEF 侧无此开关通道）——**降级为双例**：权限
  弹出后 `permission-gate` 已证采集路径；本门禁只做源码级 wiring 断言 +
  `permission_respond(ACCEPT)` 后断言事件到达（假设备注入由门禁脚本经
  `CHROMIUM_HELPER_EXTRA_SWITCHES` 不做——不新增开关通道；若实测无假设备
  则采集失败、事件不带 true，同样能断言 wiring 通）。

## 1.6 打印请求（上报，接住 window.print()）

- **CEF 面**：`OnPrintDialog` 返回 false 的现状 = `window.print()` 静默
  no-op（实测 stub）。打印是宿主的 GTK 事务——但"页面想打印"这个事实
  现在宿主看不见。
- **形状**：`print-requested {browser, has_selection}`（OnPrintDialog
  位置发出），返回 false 维持现状（CEF 的打印机路径照旧不启动）。宿主要
  打印就调**已有**的 `print-pdf` 拿到字节，喂自己的 GTK 打印对话框——
  组件零新增管道，闭环全在既有面上。
- **门禁**：`test/print-gate.sh`——探针页 `window.print()` 一发，断言
  helper 日志 + `::print-requested` 信号；紧接 `print-pdf` 流式三段回归
  （pdf-gate 的断言子集）证明闭环成立。

# 第二部分 · 批二 · 咨询式接口（有回调语义；默认值是"继续等"与"照旧"）

## 2.1 渲染进程无响应（must-report、optional-answer）

- **CEF 面**：`OnRenderProcessUnresponsive` + `CefUnresponsiveProcessCallback`
  （Wait=重置计时，Terminate=杀 renderer，随后 OnRenderProcessTerminated）；
  `OnRenderProcessResponsive`。CEF 默认（不覆写）= Alloy 静默继续等待。
  依赖 hang monitor（默认开）。
- **形状**：parked 表 `g_hang_dialogs`（每 browser 至多一条；重复触发只
  return true 续等，不叠单）。
  - `render-unresponsive {browser, id}` parked → 宿主
    `render-unresponsive-respond {id, action: "wait"|"terminate"}`；
    wait → `callback->Wait()`，terminate → `callback->Terminate()`；
  - `render-responsive {browser, id}`：页面缓过来，宿主面板收起，弃单；
  - `render-unresponsive-closed {browser, id}`：浏览器关闭弃单
    （DropDialogsForBrowser 加一段）。
  - **无人应答 = CEF 默认**（return true 不碰 callback = 静默继续等），
    行为与今天完全一致——这是最安全的默认：杀进程的决定不能由超时代答。
- **Session API**：`::render-unresponsive (id, browser)` +
  `chromium_session_render_unresponsive_respond (self, id, action)` +
  `::render-responsive` / `::render-unresponsive-closed`。
- **门禁**：`test/hang-gate.sh`——探针页 JS 死循环 ≥20 秒（hang monitor
  阈值 15 秒），断言：`render-unresponsive` 到达 → 答 `wait` → 页面仍挂
  → `render-responsive` 到达（循环改可控：脚本先死循环 N 秒后恢复）→
  第二轮答 `terminate` → `renderer-exited` 到达 → reload 恢复。超时预算
  600 秒。

## 2.2 证书豁免撤销（能力接口）

- **CEF 面**：`CefRequestContext::ClearCertificateExceptions`（:212）**不用**
  ——它清的是 Chromium 自己的 HSTS 例外存储，而本组件的豁免长在 helper 的
  `g_cert_authorized` 内存表（`certificate-authorize` 写入）。撤销 = 清
  这张表，一行命令。
- **形状**：命令 `certificate-clear-authorized`（无参，清全部——指纹级
  撤销没有宿主需求，表本来就小）；Session 方法
  `chromium_session_certificate_clear_authorized()`。
- **门禁**：`certificate-gate.sh` 扩一段：authorize → 重载自动放行 →
  clear → 重载再次 `certificate-error`。原三例不动。

# 第三部分 · 批三 · frame 生命周期（缺口#1 的正面回答）

- **CEF 面**：`CefFrameHandler` 五事件（cef_frame_handler.h:126–186）：
  OnFrameCreated/Destroyed/Attached/Detached、OnMainFrameChanged。
- **形状**：全部 fire-and-forget，frame 标识用 `CefFrame::GetIdentifier()`
  的字符串原样（CEF 156 无数字 frame id；`GetFrameIdentifiers` 同一形状，
  代码已核实）：
  - `frame-created {browser, frame, is_main, url}`
  - `frame-destroyed {browser, frame}`
  - `frame-attached {browser, frame}` / `frame-detached {browser, frame}`
    （跨站换进程时同一逻辑 frame 的走与回）
  - `main-frame-changed {browser, old_frame, new_frame}`
- **边界裁定**：**不**回改既有事件——`load-started` 已带 is_main_frame，
  `load-end`/`load-error` 主框架过滤是既有门禁锁定的行为；往里塞 frame id
  换不来新能力（宿主要逐 frame 加载状态时用 frame-created + 脚本统计）。
  挂载 GetFrameHandler 后 CEF 不再需要 polling frame 表，`GetFrameIdentifiers`
  的两处调用点不动。
- **过滤**：devtools/background 不上通道（与 Load/Display 系同一惯例）。
- **Session API**：五个信号 `::frame-created (browser, frame, is_main,
  url)` 等，GType string/int 直传。
- **门禁**：`test/frame-gate.sh`——探针页含同源 iframe + 动态增删 iframe
  的脚本：断言 created/destroyed 成对、iframe 的 is_main=false、主框架
  created 的 is_main=true、导航后 attached/detached 或新 created 出现
  （哪种形态以实测为准，门禁断言写成两者皆可）。

# 第四部分 · 配方 · PDF 查看器（缺口#9，宿主侧实现，组件零改动）

实测结论（cef-features §三）：发行包没有 PDF viewer 扩展，`application/pdf`
提交后页面空白，PDF 链接只能当下载。CEF 无查看器面——但已有能力拼得出
完整闭环：

1. `intercept_add("*://*/*.pdf", …)` → redirect 到宿主 scheme（如
   `viewer://pdf?src=<原 URL>`）；
2. `scheme_register("viewer", …)` 服务查看器壳页（pdf.js 发行文件由宿主
   打包）；
3. 壳页 fetch 原始 PDF URL（`cors` flag 或同 scheme 代理）渲染。

写成 `docs/pdf-viewer.md` + `.zh-CN.md`（配方文档，双语成对，沿
session-restore 的体裁：步骤 + 可跑的最小示例片段，不放组件代码）。
`examples/` 不动——pdf.js 体积（~2 MiB）不适合进仓库，配方只写取得与
挂载方式。

# 第五部分 · 实施顺序与工程约束

| 步 | 内容 | 完成标志 |
| --- | --- | --- |
| 1 | 批一全部六项（helper 六事件 + autoplay 属性 + session 信号/属性 + demo 日志） | autoplay/console/print gate 绿 |
| 2 | 批二两项（hang parked 表 + 豁免撤销） | hang/certificate 扩例绿 |
| 3 | 批三 CefFrameHandler | frame gate 绿 |
| 4 | PDF 配方双语文档 + 双语协议/API 表同步 + cef-features 状态表更新 | 文档齐 |

工程约束沿用总计划第四部分：新事件默认活动浏览器、可点名；4 MiB 行限；
parked 弃单回 `*-closed`；UI 线程收发；每特性一个 gate 进 ci-all；组件层
不画任何 UI（tooltip/status/print 全是上报，画法归宿主）。本批全部是
**加法**：协议 minor 内新增命令/事件不改大版本（握手 2 不动），旧 session
遇到新事件走 `unknown event` 告警路径，无害。

# 第六部分 · 明确不做（本文范围）

`CefAudioHandler`（管道重、无需求拉动）；`CefDragHandler`（dnd-gate 实测
页面拖放已通，宿主介入无需求）；`CefAccessibilityHandler`（AT-SPI 系统级
已通，宿主读树无需求）；桌面捕获（Alloy 无源选择公开面，头文件零命中）；
媒体会话/系统媒体键（CEF 无公开面，CDP Media 域 metadata 面存疑）；下载
跨重启续传；`OnCursorChange`（嵌套合成器光标链路已通）；`GetRootWindow-
ScreenRect`/`OnContentsBoundsChange`（无宿主需求）；`OnBeforePopupAborted`
（无消费方）；RunModal/CefUrlRequest/CefServer/CefMediaRouter/Widevine
（裁定沿用）。

---

# 第七部分 · 落地记录（2026-10-26）

三批全部落地，门禁进 `ci-all`。计划留待实测的点，结论逐条记在这里（计划
正文不改）：

| 条目 | 结论 |
| --- | --- |
| 1.1 autoplay | 属性/env/开关链路照计划落地。**实测修订门禁组合**：本 Chromium（156）默认策略下无手势 `AudioContext` 就是 `suspended`（Chrome 71 起 Web Audio 受默认策略约束）；`user-gesture-required` **不**管 Web Audio（测得 running）；真正双向可断言的对是 `document-user-activation-required`（suspended）↔ `no-user-gesture-required`（running）。`autoplay-gate` 断言这一对 |
| 1.2 console | 照计划；门禁双面断言（helper 日志行 + `::console` 信号行） |
| 1.3 tooltip/status | **实测分叉**：`::status-message` 通（悬停链接时到达、移开时空串，门禁断言这一对）；`::tooltip-changed` 的面接了、helper 返回 true 压弹层，但 CEF 156 的 views 层在本形态从不调用 `OnTooltip`（多段 hover + 2.5 秒停留实测无回调）——面保留、无门禁断言，协议文档记实测 |
| 1.4 focus | 照计划；`FOCUS_SOURCE` 枚举实测是 `navigation`/`system`（无 popup 值），信号文档随之。真实导航中观察到事件到达（revoke 门禁的日志） |
| 1.5 media-access | 照计划；wiring 通（无假设备注入通道，门禁不 asserting true 态——计划已预留该降级） |
| 1.6 print | **实测修订上报点**：`OnPrintDialog` 只在环境里有打印机时才被调用——本环境无打印机，`window.print()` 直接走 Chromium 的打印错误对话框（print_error_dialog，Chrome-views UI，实测不崩）。上报点移到 `OnPrintStart`（必触发），信号签名去掉 `has_selection`；无打印机环境下仍能闭环（print-gate：上报 + `print-pdf` 回归） |
| 2.1 hang | **实测降级**：CEF 文档明说检测靠"输入事件 15 秒无应答"；裸 JS 死循环不触发（无输入可失应答），补点击后观察 35 秒仍无事件（三组实验，挂起最长 90 秒）——本 CEF 的 Alloy/嵌套 Wayland 形态下 `OnRenderProcessUnresponsive` 不触发。处理沿 client-cert 的先例：五件套（helper handler、parked 表、事件、session 信号、respond 命令）与浏览器关闭弃单全部落地，`hang-gate` 做 wiring 断言 + 健康页静默反例，协议文档记实测 |
| 2.2 证书撤销 | **实测发现两张表**：`OnCertificateError` 继续（Continue）之后 Chromium 自己按 (host, 证书) 记忆放行，只清 helper 指纹表撤销不掉——补 `ClearCertificateExceptions`。CEF 建议的 `CloseAllConnections` **实测在浏览器活着时调它段错误（signal 11）**，且不清连接重连照样再询问——只清例外表，协议文档记实测。`certificate-gate` 加 revoke 段（authorize → 通 → clear → 再问 → deny → 失败） |
| 批三 frame | 照计划；实测 created 时刻 `GetURL()` 为空（导航提交前 CEF 不给 URL），门禁不匹配 URL 只匹配 is_main 与 id 配对；"frame id 贯穿既有事件"按计划裁定不做 |

新接口落点：`chromium_session_set/get_autoplay_policy()`、
`chromium_session_certificate_clear_authorized()`、
`chromium_session_render_unresponsive_respond()`（`ChromiumHangAction`）；
信号 `::console`、`::tooltip-changed`、`::status-message`、
`::focus-requested`、`::media-access-changed`、`::print-requested`、
`::render-unresponsive/responsive/-closed`、`::frame-created/destroyed/
attached/detached`、`::main-frame-changed`。demo 新脚本步：
`certificate-clear`、`hang-respond <wait|terminate>`，新探针页：
`autoplay-page.html`、`console-page.html`、`print-page.html`、
`hang-page.html`、`click-probe.html`、`frame-page.html`、`frame-child.html`。
