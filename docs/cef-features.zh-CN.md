# CEF 特性实现一览

简体中文（本文暂无英文版；`docs/` 下其余文档均成对，本文是状态审计，随代码走）

本文按 **CEF 156 公开头文件**逐接口核对本仓库的实际实现，回答一个问题：CEF
给了什么、我们用了什么、哪些没接、哪些明确不做。判据全部来自代码本身
（`helper/src/main.cpp`、`src/chromium-session.[ch]`）与门禁脚本，不凭印象。
核对方法见文末"附录 A"。

| 标记 | 含义 |
| --- | --- |
| ✅ | 已实现；关键行为有门禁脚本覆盖（个别条目只在集成路径上被间接覆盖） |
| ◐ | 部分实现：接口有方法没接，或只走 CEF 默认行为，或已接但无门禁 |
| ✗ | 未实现（CEF 有面，本组件没接） |
| — | 明确不做（有计划或契约层面的理由，见文末） |

第二节的 ✅/◐ 针对"该 handler 的方法有没有全接"：方法全接才是 ✅，
接了常用的几个是 ◐。

---

## 一、运行时骨架

| 特性 | CEF 面 | 状态 | 实现 / 说明 |
| --- | --- | --- | --- |
| Alloy 运行时 | `cef_runtime_style_t` | ✅ | 全局 Alloy；DevTools 弹窗按 CEF 硬性要求单独声明 Chrome style（`browser_view_impl.cc` 会 CHECK，混用即崩） |
| Chrome 运行时 | `CEF_RUNTIME_STYLE_CHROME` | — | 不用：Chrome style 自带整套浏览器 UI，与"宿主画外壳"的定位冲突 |
| 窗口化渲染（原生 Wayland 面） | `CefBrowserView` + `CefWindow` | ✅ | 一个 frameless `CefWindow` 装 `CefBrowserView`，Chromium 直接渲染到 Wayland surface；无 OSR、无 XWayland |
| 离屏渲染（OSR） | `CefRenderHandler` | — | 不用：`windowless_rendering_enabled=false`。本项目的合成路径是"宿主自己就是合成器" |
| 多进程 | 默认 | ✅ | Chromium 标准进程树；sandbox 默认开（`CHROMIUM_HELPER_NO_SANDBOX=1` 才关） |
| sandbox 显式关闭 | `CefSettings.no_sandbox` | ✅ | 只有该环境变量等于 `1` 生效；没有"失败降级"路径 |
| 单线程消息循环（隔离宿主 UI 线程） | `multi_threaded_message_loop` / `external_message_pump` | ✅（关闭态） | 两者都关：CEF 跑在自己的主循环里，宿主是独立进程，天然隔离 |
| GPU 开关 | `CHROMIUM_HELPER_DISABLE_GPU=1` | ✅ | 只追加 `--disable-gpu`，不改渲染路径 |
| 日志级别 | `CefSettings.log_severity` | ✅ | 固定 `WARNING` |
| 用户代理（启动期） | `CefSettings.user_agent` | ✅ | 继承 `CHROMIUM_USER_AGENT`；运行期切换走属性，见 D.3 行 |
| Accept-Language | `CefSettings.accept_language_list` | ✅ | `CHROMIUM_HELPER_ACCEPT_LANG`，同时也成为 `navigator.language` |
| profile / 缓存目录 | `root_cache_path` / `cache_path` | ✅ | 无数据目录 = 内存 profile（Chromium incognito）；有 = 完整 profile，带 flock 独占锁 |
| 会话 cookie 持久化 | `persist_session_cookies` | ✅（关闭态） | 固定 false：会话 cookie 不落盘，与 incognito 语义一致 |
| 资源目录 | `resources_dir_path` / `locales_dir_path` | ✅ | `CEF_RESOURCES_PATH` |
| 远程调试端口 | `remote_debugging_port` | — | 不用：CDP 只走 `ExecuteDevToolsMethod` + 观察者，不开放端口 |
| 自定义 scheme 注册 | `CefSchemeRegistrar::AddCustomScheme` | ✅ | `cefnest-extension`（插件）+ 宿主声明的 scheme（`CHROMIUM_HELPER_SCHEMES`），每个进程一致注册 |
| 进程级命令行改写 | `CefApp::OnBeforeCommandLineProcessing` | ✅ | ozone=wayland、`--disable-chrome-login-prompt`、代理两开关、可选关 GPU / 拼写检查 / 强制 a11y |
| 子进程启动散播 | `CefBrowserProcessHandler::OnBeforeChildProcessLaunch` | ✅ | 插件表与宿主脚本表经 base64 命令行开关交给每个 renderer（document_start 的唯一可靠通道） |
| 外部消息泵 | `OnScheduleMessagePumpWork` | — | 不用：`external_message_pump=false` |
| 默认 client | `GetDefaultClient` | ✗ | 不接：每个浏览器都显式给 client，没有"裸浏览器"需求 |
| 进程间单实例迁移 | `OnAlreadyRunningAppRelaunch` | ✗ | 不接：helper 由宿主按 socket 名一对一拉起 |
| 自定义首选项 | `OnRegisterCustomPreferences` | ✗ | 不接：preference 体系是 Chrome runtime 的，Alloy 用命令行与属性 |
| 崩溃报告 | `CefCrashUtil`、breakpad | ✗ | 不接：崩溃表现为进程退出 + `::closed`/`::helper-exited`，核心转储交给系统 |
| CEF 版本上报 / 握手 | `CEF_VERSION` | ✅ | `ready` 事件带 `version`（控制通道大版本 2）+ CEF 版本串；大版本不符即停 |

## 二、CefClient 各 handler（逐接口）

CEF 156 的 `CefClient` 可返回 18 个 handler。下表覆盖这 18 个，以及它们背后的
子接口（资源请求、render/browser 进程、窗口与视图委托）和 CEF 其余常用手柄
（V8、scheme、resource、DevTools 观察者、无障碍、扩展等）。

| Handler | 状态 | 已实现的方法 | 未实现（走 CEF 默认） |
| --- | --- | --- | --- |
| `CefLifeSpanHandler` | ◐ | `OnAfterCreated`、`OnBeforeClose`、`OnBeforePopup` | `OnBeforePopupAborted`、`OnBeforeDevToolsPopup`（DevTools 弹窗由 CEF 自己链路，本项目不替换）、`DoClose`（Alloy 关闭顺序已够，见 multi-browser 门禁） |
| `CefLoadHandler` | ✅ | `OnLoadingStateChange`、`OnLoadStart`、`OnLoadEnd`、`OnLoadError` | —（全接） |
| `CefDisplayHandler` | ◐ | `OnAddressChange`、`OnTitleChange`、`OnFaviconURLChange`、`OnFullscreenModeChange`、`OnAutoResize`、`OnLoadingProgressChange`、`OnConsoleMessage`、`OnTooltip`、`OnStatusMessage`、`OnMediaAccessChange` | `OnCursorChange`、`OnContentsBoundsChange`、`GetRootWindowScreenRect` — 见下文"缺口"；OnTooltip/OnStatusMessage 有实测注（§三） |
| `CefFocusHandler` | ✅ | `OnTakeFocus`、`OnGotFocus`、`OnSetFocus`（`::focus-requested`，只上报不拦） | —（全接） |
| `CefKeyboardHandler` | ◐ | `OnPreKeyEvent`（F12 / Ctrl+Shift+I / Esc / Ctrl+Q） | `OnKeyEvent`（不拦截，全部交给页面） |
| `CefRequestHandler` | ◐ | `OnOpenURLFromTab`、`GetResourceRequestHandler`、`GetAuthCredentials`、`OnCertificateError`、`OnSelectClientCertificate`、`OnRenderProcessTerminated`、`OnRenderProcessUnresponsive`、`OnRenderProcessResponsive` | `OnBeforeBrowse`、`OnRenderViewReady`、`OnDocumentAvailableInMainFrame`；unresponsive/responsive 已接但实测本形态不触发（§三） |
| `CefDragHandler` | ✗ | — | `OnDragEnter`、`OnDraggableRegionsChanged`。页面 HTML5 拖放照常工作（Chromium 自己处理 Wayland data device，`dnd-gate.sh` 实测通过）；缺的是"宿主介入拖放"的能力 |
| `CefAudioHandler` | ✗ | — | `GetAudioParameters`、`OnAudioStreamStarted/Packet/Stopped/Error`。声音走 Chromium 自己的 audio service；宿主只有静音开关，拿不到音频流 |
| `CefFrameHandler` | ✅ | `OnFrameCreated/Destroyed/Attached/Detached`、`OnMainFrameChanged`（`frame-*`/`main-frame-changed` 事件族，"-4" 占位 frame 滤掉） | —（全接） |
| `CefCommandHandler` | — | — | Chrome runtime 的命令面，Alloy 不适用 |
| `CefResourceRequestHandler` | ◐ | `OnBeforeResourceLoad`（拦截的 parked / passive 两态） | `GetCookieAccessFilter`、`GetResourceHandler`、`OnResourceRedirect`、`OnResourceResponse`、`GetResourceResponseFilter`、`OnResourceLoadComplete`、`OnProtocolExecution`、`CanSendCookie`、`CanSaveCookie` |
| `CefRenderHandler` | — | — | OSR 专用，见第一节 |
| `CefDownloadHandler` | ✅ | `CanDownload`、`OnBeforeDownload`、`OnDownloadUpdated` | —（全接，含暂停/恢复/取消与进度） |
| `CefFindHandler` | ✅ | `OnFindResult` | — |
| `CefJSDialogHandler` | ✅ | `OnJSDialog`、`OnBeforeUnloadDialog`、`OnResetDialogState`、`OnDialogClosed` | —（全接，全部 parked 给宿主） |
| `CefDialogHandler` | ✅ | `OnFileDialog` | — |
| `CefPermissionHandler` | ✅ | `OnRequestMediaAccessPermission`、`OnShowPermissionPrompt`、`OnDismissPermissionPrompt` | 无遗漏（MIDI / 地理 / 剪贴板 / 通知都经 `OnShowPermissionPrompt`） |
| `CefContextMenuHandler` | ✅ | `RunContextMenu`、`OnContextMenuCommand`、`OnContextMenuDismissed` | `OnBeforeContextMenu` 不需要：整张模型交给宿主绘制，不修改 CEF 默认项 |
| `CefPrintHandler` | ✅ | `OnPrintStart` 上报 `::print-requested`（实测 `OnPrintStart` 必触发而 `OnPrintDialog` 只在有打印机时到）；其余方法保持不接管系统打印 | 见第七节——打印对话框归宿主 |
| `CefRenderProcessHandler` | ◐ | `OnWebKitInitialized`、`OnContextCreated`、`OnProcessMessageReceived` | `OnBrowserCreated/Destroyed`、`GetLoadHandler`、`OnContextReleased`、`OnUncaughtException`、`OnFocusedNodeChanged` |
| `CefBrowserProcessHandler` | ◐ | `OnBeforeChildProcessLaunch` | `OnContextInitialized`、`GetDefaultClient`、`OnScheduleMessagePumpWork`、`OnAlreadyRunningAppRelaunch` |
| `CefV8Handler` | ✅ | 四个桥函数：`__chromiumHelperReport`（js 结果）、`cefnestPostMessage`、`cefnestPrimarySelection`、`__cefnestPlugin` | — |
| `CefSchemeHandlerFactory` | ✅ | 插件 scheme 工厂、宿主 scheme 工厂 | — |
| `CefResourceHandler` | ✅ | 插件静态资源（流式 + 403/404 错误体）、宿主 scheme（parked + 分块流式） | — |
| `CefDevToolsMessageObserver` | ✅ | `OnDevToolsMethodResult`、`OnDevToolsEvent` | —（宿主管道与组件内部调用共用，按消息 id 分流） |
| `CefWindowDelegate` | ◐ | `OnWindowCreated/Destroyed/ActivationChanged/BoundsChanged/FullscreenTransition`、`GetInitialBounds`、`IsFrameless`、`CanResize` | `GetInitialShowState`、`GetParentWindow`、`IsWindowModalDialog`、`WithStandardWindowButtons`、`GetTitlebarHeight`、`CanMaximize/Minimize/Close`（均取默认值）、`OnAccelerator`、`OnKeyEvent`、`OnThemeColorsChanged`、`GetLinuxWindowProperties` |
| `CefBrowserViewDelegate` | ◐ | `GetBrowserRuntimeStyle`、`GetDelegateForPopupBrowserView`、`OnPopupBrowserViewCreated`（DevTools 无框窗） | `GetChromeToolbarType`（Chrome runtime 专用） |
| 访问器类的 visitor | ✅ | `CefStringVisitor`（源码 / 页文本）、`CefCookieVisitor`、`CefNavigationEntryVisitor`、`CefPdfPrintCallback` | — |
| `CefAccessibilityHandler` | ✗ | — | `OnAccessibilityTreeChange`、`OnAccessibilityLocationChange`。系统级无障碍走 Chromium 自己的 AT-SPI（`SetAccessibilityState(STATE_ENABLED)`），但宿主拿不到无障碍树 |
| `CefResourceBundleHandler` | ✗ | — | 字符串本地化覆盖；CEF 自带 locales 够用 |
| `CefRequestContextHandler` | ✗ | — | 单 request context（唯一 profile），没有 per-context 策略需求 |
| `CefMediaRouter` | — | — | 投屏 / Cast：不在范围 |
| `CefServer` | — | — | 组件不是 HTTP 服务 |
| `CefUrlRequest` | — | — | 独立 HTTP 客户端：宿主有自己的网络栈；组件内所有网络都走浏览器上下文 |
| `CefDomVisitor` / `CefImage` / `CefZipReader` / `CefXmlReader` | — | — | 需要时用 JS 或宿主侧库更直接；插件包解压走宿主侧 zip 实现 |

## 三、页面与导航

| 特性 | 状态 | 宿主接口 | 说明 / 门禁 |
| --- | --- | --- | --- |
| 加载 URL | ✅ | `load_uri(browser, url)` | 缺省作用于活动浏览器，可点名后台标签（A.1）；`history-gate` |
| 后退 / 前进 / 刷新（软硬） / 停止 | ✅ | `go_back` / `go_forward` / `reload(hard)` / `stop_loading` | 全部可点名 browser；`multi-browser-gate` |
| 导航历史读取 | ✅ | `history(browser, id)` → `::history-result` | `CefNavigationEntryVisitor`，带 `current_index` 与标题；`history-gate` |
| 导航历史跳转 | ✅ | `history_go(browser, index)` | CEF 公开 API **无**跳转接口，组件内部走 CDP `Page.getNavigationHistory` + `Page.navigateToHistoryEntry`；宿主不需要 `cdp-enable`；`history-gate` |
| 加载生命周期 | ✅ | `::load-started` / `::load-end` / `::load-error` | 每次导航 started/end 各一次；失败恰好一条 error（ERR_ABORTED 回声已按"同 URL + 5 秒内"丢弃）；`history-gate` 三例 |
| 加载进度 | ✅ | `::loading-progress` | 0–1，另有一条 `::changed` 派生的 loading 状态 |
| 地址 / 标题 / favicon | ✅ | `::address` / `::title` / `::favicon-changed`（URL 列表） | favicon 只给 URL，下载与选择归宿主；`favicon-gate` |
| 页文本 | ✅ | `page_text(browser, id)` → `::page-text-*` | 4 MiB 上限，UTF-8 边界切块；`page-text-gate` |
| 页面源码 | ✅ | `view_source(browser)` → `::view-source*` | DOM 序列化结果（非网络原始字节），16 MiB 上限；`source-gate` |
| 页面内查找 | ✅ | `find` / `stop_finding` → `::find-result` | 每次请求新 identifier；`find-gate` |
| 缩放 | ✅ | `zoom(browser, action)` / `set_zoom_level` → `::zoom-changed` | CEF zoom level（0.5 = 一档）；Chromium 按源持久化；`zoom-gate` |
| 静音 | ✅ | `set_muted` / `::changed` + 浏览器表 `muted` | 读回确认；后台标签静音不冒充活动标签 |
| 全屏 | ✅ | `::fullscreen-changed` | CEF 窗口跟着走，宿主可跟 |
| 自动调整尺寸 | ✅ | `set_auto_resize` / `set_auto_resize_bounds` → `::auto-resize-updated` | CEF `SetAutoResizeEnabled`；`auto-resize-gate` |
| 用户代理 | ✅ | `user-agent` 属性 / `browser_set_user_agent` | 组件内部 `Emulation.setUserAgentOverride`，每次 load start 重放（抗渲染进程崩溃）；`ua-gate` |
| 截图 | ✅ | `capture_screenshot(id, browser)` → `::screenshot-result` | `Page.captureScreenshot`，PNG；`screenshot-gate`（魔数 + 尺寸） |
| 打印为 PDF | ✅ | `print_pdf` / `print_pdf_with_settings` → `::pdf-*` | `PrintToPDF` 写临时文件再流回，16 MiB 上限；`pdf-gate` |
| 页面内 PDF 查看 | ✗ | — | **实测（2026-10-26）**：加载本地 PDF 得到 `document.contentType=application/pdf`，但页面里没有 viewer 的 `embed`、正文为空 —— 本发行包没有可用的 PDF 查看器组件扩展。PDF 链接目前只能当下载处理 |
| 控制台消息转发 | ✅ | `::console (browser, level, message, source, line)` | 逐条转发；`console-gate` 双面断言 |
| 状态栏 / tooltip 文案 | ◐ | `::status-message` / `::tooltip-changed` | status-message 实测通（悬停链接的 URL，`console-gate` 断言）；tooltip 的面接了，但实测（2026-10-26）本形态 CEF views 层从不调用 `OnTooltip`——面保留，画法归宿主 |
| 光标形状 | ◐ | — | `OnCursorChange` 未接：窗口化模式下 helper 作为 Wayland 客户端自己设光标（`wp_cursor_shape` / `wl_pointer.set_cursor`），嵌套合成器（`src/nested-cursor.c`）负责把它落到 GTK 光标上。宿主拿不到改光标的接口，但光标本身是通的 |
| 页面主动索要焦点 | ✅ | `::focus-requested (browser, source)` | 只上报不拦；窗口激活联动归宿主 |

## 四、信任面与凭据

| 特性 | 状态 | 宿主接口 | 说明 / 门禁 |
| --- | --- | --- | --- |
| TLS 证书错误 | ✅ | `::certificate-error` + `certificate_respond` / `certificate_authorize` | must-be-answered；默认拒绝（Chrome 语义）；指纹豁免只存 helper 生命周期；`certificate-gate` |
| HTTP 认证（401） | ✅ | `::auth-required` + `auth_respond` / `auth_cancel` | must-be-answered；默认取消。**必须加 `--disable-chrome-login-prompt`**，否则 CEF 156 把认证交给自己的 UI，回调根本不触发（上游 #3792）；`auth-gate` 正/误/取消三例 |
| 代理认证（407） | ✅ | 同上，`is_proxy=true` | `auth-gate` 代理例（需 `<-loopback>` 才让 loopback 目标走代理） |
| 客户端证书选择 | ◐ | `::client-cert-request` + `client_cert_respond(index)` | 已接 `OnSelectClientCertificate`，-1 = 不选；本环境起不了 mTLS，门禁为源码级 wiring 断言 |
| 正常页面的证书信息 | ✅ | `get_tls_info(id, browser)` → `::tls-info` | CEF 无直接取口；走 `Security.enable` + `visibleSecurityStateChanged`（156 后端没有 `getVisibleSecurityState`）；自签页状态字为 `insecure-broken`；`tls-info-gate` |
| 证书例外清理 | ✅ | `certificate_clear_authorized()` | helper 指纹表 + `ClearCertificateExceptions`（Chromium 自己的 per-(host, cert) 记忆）一起清；CEF 建议的 `CloseAllConnections` 实测段错误，不清连接实测照样再询问；`certificate-gate` revoke 段 |
| 权限提示（媒体/地理/剪贴板/MIDI/通知） | ✅ | `::permission-request` + `permission_respond` | 默认拒绝；`permission-gate` |
| 桌面捕获（getDisplayMedia） | ✗ | — | CEF Alloy 无桌面捕获源选择面；未接，未实测 |
| 站点隔离 / 进程模型 | ◐ | — | 走 Chromium 默认（每站点一 renderer）；未做 per-site 进程策略 |
| 响应级拦截 / 放行 / 重定向 | ✅ | `intercept_add` / `::request-intercept` / `request_respond` | `OnBeforeResourceLoad` 的 parked 与 passive 两态；`intercept-gate` |
| 响应头 / 响应体改写 | ◐ | `filter_add` / `filter_remove`（`replace`、`inject-css`） | 响应体改写已接：声明式规则长在 helper，replace 跨块拼接、inject-css 仅 `text/html`；`filter-gate`。响应头改写无面：`OnResourceResponse`、`GetResourceResponseFilter` 里的 response 均标注不可修改，实测（2026-10-26）两处改写都不上线——`drop-header` op 被点名拒绝（`filter-add-drop-header-unsupported`） |
| Cookie 访问过滤 | ✗ | — | `GetCookieAccessFilter` / `CanSendCookie` / `CanSaveCookie` 未接 |
| Content-Security-Policy 干预 | — | — | 不改页面 CSP；宿主脚本按"无隔离世界"跑在页面域内（文档写明） |
| 防护（点击劫持/混合内容等） | ◐ | — | 全交给 Chromium 默认 |

## 五、网络与协议

| 特性 | 状态 | 宿主接口 | 说明 / 门禁 |
| --- | --- | --- | --- |
| 代理 | ✅ | `set_proxy` / `set_proxy_bypass`（须在 `start()` 前） | 命令行开关，CEF 无运行时 API；`proxy-gate` |
| Cookie 读 / 写 / 删 | ✅ | `cookies_get` / `cookies_set` / `cookies_delete` | 操作运行中 helper 的 request context；`cookies-gate` |
| 分类型清数据 | ✅ | `clear_data_types(types, origins)` | CDP `Network.clearBrowserCache` / `clearBrowserCookies` / `clearHttpAuthCache`、`Storage.clearDataForOrigin`；`clear-data-gate` |
| 整目录清数据 | ✅ | `clear_data()` | 删数据目录（与上者并存：分类型走协议、全删走目录）；`data-dir-gate` |
| 自定义 scheme（宿主供数据） | ✅ | `scheme_register` / `::scheme-request` + `scheme_response/write/end/abort` | 两段式：**选项**只能启动期经 `CHROMIUM_HELPER_SCHEMES`，**数据工厂**运行期挂；`scheme-gate` 三例 |
| 插件 scheme（`cefnest-extension://`） | ✅ | 插件计划 | 只服务已加载插件的 root，含路径逃逸防护；`plugin-gate` |
| 宿主脚本注入 | ✅ | `script_add` / `script_remove` | `document_start` 先于页面内联脚本（`script-gate` 实测 yes:1）；无隔离世界 |
| 插件内容脚本 | ✅ | 插件计划 | 同上机制 + `chrome.*` shim；`plugin-gate` |
| CDP 管道（宿主面） | ✅ | `cdp_enable` / `cdp_execute` / `::cdp-event` | 安全开关，per-browser，默认关；`cdp-gate` |
| CDP 内部使用 | ✅ | — | 历史跳转、TLS、UA、截图、清数据都走组件自有管道，**不**要求宿主开开关 |
| DevTools 前端窗口 | ✅ | `devtools-enabled` 属性 | 无框独立窗口，`::browser-created` 带 `devtools=true`；`devtools-gate` |
| 下载 | ✅ | `::download-begin` + accept/reject/pause/resume | 落盘前必须宿主给路径，不存在"默认下载目录"；`download-gate` |
| 下载跨重启续传 | ✗ | — | 未接：进行中的暂停/恢复有（`pause_download`/`resume_download`），helper 重启后的续传没有 |
| HTTP 缓存控制 | ◐ | — | 只有"清缓存"；无 per-request cache 策略 |
| QUIC / HTTP3、DoH 等传输层开关 | — | — | 走 Chromium 默认 |
| 独立 HTTP 请求（`CefUrlRequest`） | — | — | 不用，见第二节 |

## 六、多浏览器与窗口

| 特性 | 状态 | 宿主接口 | 说明 / 门禁 |
| --- | --- | --- | --- |
| 多标签（共享一个 CefWindow） | ✅ | `new_browser(url, FALSE)` | `multi-browser-gate` |
| 独立窗口 | ✅ | `new_browser(url, TRUE)` | 同上 |
| 后台常驻页 | ✅ | `new_background_browser` | 无窗口、无 surface、永不激活；插件 `background.service_worker` 骑它 |
| 关闭单个浏览器 / 最后一个 | ✅ | `close_browser` | Alloy 的"关闭窗口 vs 关闭浏览器"陷阱已在 helper 侧处理（先摘 view 引用）；`multi-browser-gate` |
| 浏览器表 | ✅ | `browsers_list()` + `::browsers-changed` | 创建/关闭/选中/标题/地址/静音/缩放时整表重推；`browsers-gate` |
| 弹出窗口（window.open） | ✅ | `::popup-requested (url, disposition, gesture, browser)` | 前台请求在发起浏览器就地导航，后台只上报。**中键/ctrl+点击走 `OnOpenURLFromTab`**，不与 `OnBeforePopup` 混；`multi-browser-gate` |
| 弹窗几何 | ✅ | `new_browser_with_geometry` + `::window-properties` | 窗口无框，toolbar/menubar/status 恒 FALSE；`window-gate` |
| 弹窗 features 的工具栏/菜单栏可见性 | — | — | 本组件窗口本就无 chrome，没有可授予/拒绝的行 |
| surface 配对（id ↔ toplevel） | ✅ | `get_browser_surface` / `::browser-surface` | 按创建顺序配对，含 DevTools 窗口与关闭重开；`surface-pair-gate` |
| 模态对话框 | ✗ | — | `RunModal` 未接；没有模态浏览器需求 |

## 七、UI 与交互

| 特性 | 状态 | 宿主接口 | 说明 / 门禁 |
| --- | --- | --- | --- |
| 右键菜单（CEF 模型 → 宿主绘制） | ✅ | `::context-menu` + select/dismiss | 默认由 session 弹自己的 popover；宿主接信号可自绘；`menu-gate`、`menu-page.html` |
| 插件自定义菜单行 | ✅ | `menu-add-user-item` / slot 26600+ | 订阅模式的 `when` 过滤；`plugin-gate` |
| JS 对话框（alert/confirm/prompt/beforeunload） | ✅ | `::js-dialog` + `js_dialog_respond` | 全部 parked；无人应答时按 Chrome 默认答（alert/beforeunload=ok，confirm/prompt=cancel）；`js-dialog-gate` |
| 文件选择（打开/多选/目录/保存） | ✅ | `::file-dialog` + `file_dialog_respond` | 默认取消；`file-dialog-gate` |
| 系统打印对话框 | — | — | 明确不做：打印是宿主的 GTK 事务。`HelperPrintHandler` 的六个方法只为不让 CEF 挂在默认实现上；`window.print()` 得不到宿主对话框 |
| 页面拖放（拖入 / 拖出 / 图片拖出） | ✅ | — | 复合器层实现（`wl_data_device`），Chromium 自己处理；`dnd-gate`、`drag-page.html`、`image-dnd-page.html` |
| 拖放策略介入（改拖放目标/拖放图标） | ✗ | — | `CefDragHandler` 未接 |
| 键盘快捷键 | ◐ | — | 只有 Esc / Ctrl+Q / F12 / Ctrl+Shift+I；其余全给页面 |
| Tab 焦点链出界 | ✅ | `::take-focus` | 页面遍历走到文档外时交回 GTK；seat 层有单元测试 `seat-selftest`（`ci-all` 的 `unit` 行） |
| IME / 文本输入 | ✅ | — | 复合器侧 text-input 协议（`nested-text-input.c`），由 Chromium 消费 |
| 主选择区（primary selection） | ✅ | `::primary-selection` | CEF/Chromium 只读不写，渲染端注入 listener 把选区推给宿主写 GTK primary；`primary-gate` |
| 剪贴板（复制链接等） | ✅ | `::copy-text` / 右键菜单命令 | 复制走 GTK clipboard；粘贴/剪切由 CEF 自己执行 |
| 无障碍（系统级） | ✅ | `set_accessibility` + AT-SPI 自动侦测 | 映射 `STATE_ENABLED`（完整 AT-SPI 模式）；`a11y-on/off` 脚本步进 |
| 无障碍树访问（宿主读） | ✗ | — | `CefAccessibilityHandler` 未接 |
| 自绘窗口装饰 | — | — | 无框窗口，标题栏归宿主 |

## 八、媒体

| 特性 | 状态 | 说明 |
| --- | --- | --- |
| 音频播放 | ◐ | 走 Chromium 自己的 audio service，组件不介入；`test/audio.session` + `audio-probe.html` 是手动探针（点击后 mute/unmute），没有自动门禁 |
| 静音 | ✅ | `SetAudioMuted` + 读回；见第三节 |
| 麦克风 / 摄像头采集授权 | ✅ | `OnRequestMediaAccessPermission`（`permission-gate`） |
| 媒体流音频参数 / 包回调 | ✗ | `CefAudioHandler` 未接：宿主拿不到音频数据 |
| 桌面捕获（getDisplayMedia） | ✗ | 见第四节 |
| 投屏（MediaRouter / Cast） | — | 不在范围 |
| DRM（Widevine） | ✗ | 发行包不含 Widevine（`libwidevinecdm` 缺失），Netflix/Spotify 类站点不可用 |
| 自动播放策略 | ✅ | `autoplay-policy` 属性（spawn 期全局一刀切，per-site 在 CEF 无面） | env → `--autoplay-policy`；AudioContext 探针，`autoplay-gate`（实测：本 Chromium 默认已挂起无手势 AudioContext，`user-gesture-required` 不管 Web Audio，可双向断言的组合是 `document-user-activation-required` ↔ `no-user-gesture-required`） |
| 媒体会话（Media Session / 系统媒体键） | ✗ | 未接 |

## 九、插件与扩展体系

| 特性 | 状态 | 说明 |
| --- | --- | --- |
| Chrome 扩展 API（`CefExtensionHandler`） | — | 不用。自研插件模型：manifest 目录 + 可选 `.so`（native）或 content script（web） |
| content scripts（`chrome.*` 子集） | ✅ | `runtime` / `storage` / `contextMenus` / `tabs` / `action`；`plugin-gate` |
| 后台页 | ✅ | manifest `background.service_worker` → 隐藏浏览器 |
| 扩展自己的页面（popup/options） | ✅ | 走 `cefnest-extension://` scheme |
| 插件包安装（zip / crx） | ✅ | 宿主侧 `install_async` 解包到安装根 |
| 插件权限表 | ✅ | 宿主裁决，`::grant-request`；未应答 = 拒绝 |
| `chrome.*` 扩面（webRequest 等） | — | 插件计划裁定不变 |

## 十、宿主本侧（非 CEF，但决定上表很多行为）

| 能力 | 位置 | 说明 |
| --- | --- | --- |
| 嵌套 Wayland 合成器 | `src/nested*.c` | 宿主自己就是合成器；helper 的 `CefWindow` 是它上面的 toplevel，逐帧 commit 到 GTK |
| shm / dmabuf 帧传递 | `nested-data-device.c`、`nested-dmabuf.c` | 零拷贝路径 + shm 回退，`pixel-gate` 逐像素校验 |
| surface 配对 | `chromium-session.c` | 按创建顺序把 CEF 窗口配给浏览器 id |
| a11y 侦测 | `chromium-session.c` | 监听 `org.a11y.Status.ScreenReaderEnabled`，自动开 CEF 无障碍 |
| 认证/证书/对话框的默认应答 | `chromium-session.c` | 每个 must-be-answered 事件都有"宿主不接时"的 Chrome 语义默认值 |

---

## 十一、缺口清单（按影响排序）

只列 **CEF 有面、本组件确实没接** 的项，以及它们的实际后果：

1. ~~`CefFrameHandler`（frame 生命周期）~~ — **已接**（2026-10-26）：`frame-*` 事件族，`frame-gate`。
2. ~~`OnConsoleMessage`~~ — **已接**（2026-10-26）：`::console`，`console-gate`。
3. **`CefAudioHandler`** — 拿不到音频流。影响：可视化、转写、逐页音量归一都做不了；只有静音。
4. ~~`CefResponseFilter` / `OnResourceResponse`~~ — **响应体改写已接**（2026-10-26，`response-filter-plan` 落地）：`filter_add` 的 `replace`/`inject-css`，`filter-gate`。响应头改写**实测定案无面**：response 在两个回调里都标注不可修改，实测改写均不上线，`drop-header` 被点名拒绝。
5. **`CefDragHandler`** — 不能改拖放目标或拖放内容。维持不做：页面拖放已通，宿主介入无需求。
6. **`CefAccessibilityHandler`** — 宿主拿不到无障碍树（系统级 a11y 正常）。
7. **`OnStatusMessage` / `OnTooltip`** — status-message 已接（实测通）；tooltip 的面接了但本形态 CEF 不回调（2026-10-26 实测），面保留。
8. `GetRootWindowScreenRect`、`OnContentsBoundsChange` — 细节能力缺失，未见宿主需求。`OnSetFocus`（`::focus-requested`）与 `OnMediaAccessChange`（`::media-access-changed`）**已接**（2026-10-26）。
9. **PDF 查看器** — 实测：PDF 以 `application/pdf` 提交，但没有查看器 embed，页面空白。
   影响：壳子里的 PDF 链接只能当下载；打印为 PDF（`print_pdf`）不受影响。
10. **Widevine** — 发行包不带，受 DRM 保护的流媒体不可用。
11. **桌面捕获（getDisplayMedia）** — 无源选择面。
12. **系统打印对话框** — 明确不做（宿主 GTK 事务）；`window.print()` 现以 `::print-requested` 上报（2026-10-26），宿主拿 `print_pdf` 喂自己的 GTK 对话框即闭环。
13. **下载续传、`CefUrlRequest`、`CefServer`、`CefMediaRouter`** — 无需求/不在范围。证书例外清理**已接**（2026-10-26，`certificate_clear_authorized()`，`certificate-gate` 的 revoke 段），此处旧表未及时划线。

## 十二、明确不做（与计划的裁定一致）

| 项 | 理由 |
| --- | --- |
| 隔离世界（isolated world） | CEF 无原语；宿主脚本与页面同域，文档已写明 |
| 表单捕获 / 密码管理 / 自动填充钩子 | 完整移植的工程，壳子范围砍除；D.2 只留注入面 |
| ITP / 第三方 cookie 摘要 | 不在壳子范围 |
| per-site 自动播放 | CEF 无面，只能全局 |
| 拼写检查运行时开关 | CEF 无面，只能 spawn 期环境变量（重启生效） |
| JS 全局开关运行时切换 | `WebPreferences` 在浏览器创建时定型 |
| WebDriver | 自动化测试诉求由 CDP 覆盖 |
| 系统打印对话框 | 宿主侧 GTK 事务 |
| `chrome.*` 扩面 | 插件计划裁定不变 |
| Chrome runtime / OSR | 与"原生 Wayland 面 + 宿主画外壳"的架构冲突 |

---

## 附录 A · 核对方法

```bash
# 1. 本仓库接了哪些 CEF 接口
grep -nE "public Cef[A-Za-z]+" helper/src/main.cpp

# 2. 每个 handler 覆写了哪些方法
grep -nE "\)\s*(const\s*)?override" helper/src/main.cpp

# 3. CEF 156 给每个 handler 开了哪些方法（与 2 做差集）
for h in cef_life_span_handler cef_load_handler cef_display_handler \
         cef_focus_handler cef_keyboard_handler cef_request_handler \
         cef_drag_handler cef_audio_handler cef_frame_handler \
         cef_command_handler cef_resource_request_handler \
         cef_render_process_handler; do
  echo "--- $h"
  grep -oE "virtual [A-Za-z:<>\*& ]+ ([A-Za-z]+)\(" \
    third_party/cef/dist/include/$h.h | sed -E 's/.* ([A-Za-z]+)\($/\1/' | tr '\n' ' '
  echo
done

# 4. CefClient 可返回的全部 handler（对照未实现的 Get*Handler）
grep -oE "virtual CefRefPtr<Cef[A-Za-z]+> Get[A-Za-z]+" \
  third_party/cef/dist/include/cef_client.h
```

## 附录 B · 协议面（宿主可见的命令与事件）

命令 67 条、事件 65 条，清单与逐条载荷见
[control-channel.zh-CN.md](control-channel.zh-CN.md)；方法/信号速览见
[api.zh-CN.md](api.zh-CN.md)。辅助用的脚本步进（demo 的
`CHROMIUM_SESSION_SCRIPT`）见 `src/main.c` 顶部注释。

## 附录 C · 启动期开关与环境变量

| 开关 / 变量 | 值 | 作用 |
| --- | --- | --- |
| `--ozone-platform=wayland` | 强制 | 只跑原生 Wayland |
| `--disable-chrome-login-prompt` | 强制 | 把认证问句交回宿主（CEF 128+ 必需） |
| `--proxy-server` / `--proxy-bypass-list` | `CHROMIUM_HELPER_PROXY*` | 代理 |
| `--disable-spell-checking` | 默认追加 | 关拼写检查（`CHROMIUM_SPELLCHECK=1` 恢复） |
| `--disable-gpu` | `CHROMIUM_HELPER_DISABLE_GPU=1` | 关 GPU |
| `--force-renderer-accessibility` | `CHROMIUM_A11Y=1` | 强制无障碍 |
| `CHROMIUM_HELPER_SCHEMES` | `name=flag,flag;name2` | 宿主 scheme 的注册选项（每进程） |
| `CHROMIUM_USER_AGENT` | 字符串 | 启动期 UA（运行期用属性） |
| `CHROMIUM_HELPER_DATA_DIR` | 绝对路径 | 持久化 profile；缺省 = 内存 profile |
| `CHROMIUM_HELPER_NO_SANDBOX=1` | 唯一有效值 | 关 sandbox |
| `CHROMIUM_HELPER_AUTO_QUIT_MS` | 毫秒 | 自测用自动退出 |
