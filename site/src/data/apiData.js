// 调用文档数据 —— 全部取自 src/chromium-session.h / chromium-view.h / nested.h 的内联文档
export const apiSections = [
  { key: 'quickstart', label: '最小宿主' },
  { key: 'examples', label: '常用示例' },
  { key: 'lifecycle', label: '会话生命周期' },
  { key: 'nav', label: '导航与状态' },
  { key: 'js', label: 'JavaScript 执行' },
  { key: 'download', label: '下载' },
  { key: 'find', label: '页内查找' },
  { key: 'menu', label: '右键菜单' },
  { key: 'permission', label: '权限请求' },
  { key: 'tabs', label: '多浏览器与标签' },
  { key: 'misc', label: '缩放 · 静音 · 无障碍' },
  { key: 'signals', label: '信号总表' },
  { key: 'view', label: 'View 与 Compositor' },
]

// 各节的 API 条目；quickstart 与 signals 是特殊渲染节，不在此列
export const entries = {
  lifecycle: [
    {
      sig: 'ChromiumSession *chromium_session_new (const char *helper_path)',
      desc: '创建会话。helper_path 为 NULL 时按五步规则自行解析（CEFNEST_HELPER → CHROMIUM_HELPER → 编入的安装路径 → 开发构建 → 配置路径报错）。',
    },
    {
      sig: 'void chromium_session_set_view (ChromiumSession *self, ChromiumView *view)',
      desc: '绑定视图。helper 要从视图的合成器取 WAYLAND_DISPLAY，所以 start() 之前必须完成绑定。',
    },
    {
      sig: 'gboolean chromium_session_start (ChromiumSession *self, const char *initial_url, GError **error)',
      desc: '启动 helper 并打开首页。失败返回 FALSE 并置 error。成功后 helper 连上控制通道时发 ::ready。',
    },
    {
      sig: 'void chromium_session_stop (ChromiumSession *self)',
      desc: '停掉 helper。关窗口时调用；对应的 ::closed 带理由 "stopped by the host"。',
    },
    {
      sig: 'signal ::closed (reason)',
      kind: 'signal',
      desc: '页面没了，本会话不会重来；要新页面须再次 start()。reason 四选一："stopped by the host" / "helper closed the browser" / "control channel closed" / "process exited"。后两者是同一次死亡的两个侧面，先到先报——除宿主自己的 stop 外，一律按"页面没了，且不是我要求的"处理。进程本身何时消失看 ::helper-exited。',
    },
    {
      sig: 'signal ::helper-exited ()',
      kind: 'signal',
      desc: 'helper 进程已被 wait 收走，::closed 之后至多一次；宿主拥有 helper 正在画的嵌套 compositor 时等它再拆，因为信号触发的 CefShutdown() 还在用那条 Wayland 连接。',
    },
    {
      sig: 'signal ::ready ()',
      kind: 'signal',
      desc: 'helper 已连上控制通道、无障碍状态已下发，startup URL 即将加载。要下发初始命令的宿主挂这里。',
    },
    {
      sig: 'const char *chromium_session_get_helper_path (ChromiumSession *self)',
      kind: 'getter',
      desc: '会话实际选中的 helper 路径。',
    },
    {
      sig: 'gboolean chromium_session_get_running (ChromiumSession *self)',
      kind: 'getter',
      desc: 'helper 是否在运行。',
    },
  ],
  nav: [
    { sig: 'void chromium_session_load_uri (ChromiumSession *self, const char *uri)', desc: '加载 URI。' },
    { sig: 'void chromium_session_go_back (ChromiumSession *self)', desc: '后退。' },
    { sig: 'void chromium_session_go_forward (ChromiumSession *self)', desc: '前进。' },
    { sig: 'void chromium_session_reload (ChromiumSession *self, gboolean hard)', desc: '刷新；hard=TRUE 强制绕缓存。' },
    { sig: 'void chromium_session_stop_loading (ChromiumSession *self)', desc: '停止加载。' },
    {
      sig: 'signal ::load-end (url, http_code)',
      kind: 'signal',
      desc: '一次导航的终局。失败的导航也发它，只是 http_code 为 0——要区分"加载了"与"没加载出来"须挂 ::load-error。',
    },
    {
      sig: 'signal ::load-error (error_code, url, message)',
      kind: 'signal',
      desc: '从未提交的导航：Chromium 自身的 net 错误码（负数）与名字，如 ERR_NAME_NOT_RESOLVED。一次失败报告两次——真实错误码在前，约一秒后再来一条 ERR_ABORTED (-3)。两者都原样透传，不过滤。',
    },
    {
      sig: 'signal ::renderer-exited (status, error_code, message)',
      kind: 'signal',
      desc: '渲染进程死亡：status 0 异常退出 / 1 被杀 / 2 崩溃（SIGSEGV）/ 3 内存耗尽。此后 surface 停在最后一帧，后续命令石沉大海。',
    },
    {
      sig: 'const char *chromium_session_get_uri (ChromiumSession *self)',
      kind: 'getter',
      desc: '当前 URI。另有 get_title ()（页面标题）、get_loading ()（加载中）、get_can_go_back ()、get_can_go_forward ()。',
    },
  ],
  js: [
    {
      sig: 'void chromium_session_run_javascript (ChromiumSession *self, const char *code, int id)',
      desc: '在页面里执行语句形代码。结果经 ::script-result 送回，带同一个 id。',
    },
    {
      sig: 'void chromium_session_run_javascript_expression (ChromiumSession *self, const char *code, int id)',
      desc: 'CSP 安全形：代码作为表达式嵌入、不经 eval() 包裹，适用于 script-src 禁止 unsafe-eval 的页面。语句形代码请用上面的 run_javascript。',
    },
    {
      sig: 'signal ::script-result (id, ok, value, message)',
      kind: 'signal',
      desc: 'value 是 JSON 文本形式的结果，无返回值的脚本是 "null"。message 解释失败、页面无法序列化的结果、或超出通道容量的结果。把页面导航走的脚本可能永远不回答。',
    },
    {
      sig: 'signal ::page-message (json)',
      kind: 'signal',
      desc: '页面 → 宿主：renderer 在每个 context 注册 cefnestPostMessage(jsonString)，宿主整串收回。内部 schema 由宿主自定。回程无专用命令——宿主用 run_javascript 派发一个 MessageEvent 即可。超大消息受宿主侧 4 MiB 行上限约束，超限会话直接关闭而非截断。',
    },
    {
      sig: 'signal ::popup-requested (url)',
      kind: 'signal',
      desc: 'target=_blank、window.open 或中键要开新窗口。信号里只有 URL：前台请求已被 helper 就地导航进本视图，后台标签只报告不动。disposition 与手势信息留在 socket 事件上，宿主无从区分。',
    },
  ],
  download: [
    {
      sig: 'signal ::download-begin (id, url, suggested_name, mime_type, total_bytes)',
      kind: 'signal',
      desc: '下载问题的提出。宿主不回答，下载就永远停靠、不落一字节。total_bytes 在服务器未声明长度时为 -1。',
    },
    {
      sig: 'void chromium_session_accept_download (ChromiumSession *self, int id, const char *path)',
      desc: '接受并指定完整落盘路径。目录——即页面的文件能落到哪——由宿主决定，页面无权置喙；helper 不创建父目录。',
    },
    {
      sig: 'void chromium_session_reject_download (ChromiumSession *self, int id)',
      desc: '拒绝一个停靠中的下载，或取消一个已在途的下载——同一个调用。',
    },
    { sig: 'void chromium_session_pause_download (ChromiumSession *self, int id)', desc: '暂停在途下载。尚未开始（尚无 item callback）的应答 status "download-not-in-flight"；终局信号照常来自 CEF。' },
    { sig: 'void chromium_session_resume_download (ChromiumSession *self, int id)', desc: '续传被暂停的下载。' },
    {
      sig: 'signal ::download-progress (id, received_bytes, total_bytes, percent, speed)',
      kind: 'signal',
      desc: '每个整百分比跟一条；speed 单位 bytes/s；percent 在未知长度时为 -1。',
    },
    {
      sig: 'signal ::download-end (id, state, path, received_bytes)',
      kind: 'signal',
      desc: '唯一的收尾处：state 为 "complete" / "cancelled" / "interrupted"；path 读自 Chromium——接受与拒绝两条路都终结于此，所以宿主的下载列表只需要盯这一个信号。注意 "complete" 并非文件出现在磁盘上的时刻（已知限制）。',
    },
  ],
  find: [
    {
      sig: 'void chromium_session_find (ChromiumSession *self, const char *pattern, gboolean forward, gboolean match_case, gboolean next)',
      desc: '开始一个查找，或在已有查找里走一步。pattern 每次都要给全（next=TRUE 表示"这一搜索的上/下一个匹配"）：helper 不保存查找状态，一步不可能关于宿主已经忘掉的搜索。',
    },
    {
      sig: 'void chromium_session_stop_finding (ChromiumSession *self, gboolean clear_selection)',
      desc: '结束查找，不回答任何东西；clear_selection=TRUE 时连 Chromium 画在匹配上的高亮一并清掉。',
    },
    {
      sig: 'signal ::find-result (identifier, count, active_ordinal, final)',
      kind: 'signal',
      desc: '实测形状（CEF 154）：identifier 每请求一新，用于归组；有匹配的请求答两次——先报计数，再以 final=TRUE 重报同一组数；无匹配的只答一次（count 0，final=TRUE），因为 CEF 没有单独的"无匹配"标志。新搜索报 active_ordinal 0（尚无活动匹配），步进则报落点是 count 中的第几个。',
    },
  ],
  menu: [
    {
      sig: 'signal ::context-menu (x, y, items) → gboolean',
      kind: 'signal',
      desc: '页面右键。x/y 是页面内逻辑像素坐标；items 是 GList（ChromiumMenuItem*），仅在本信号发射期间归会话所有，要留就拷。处理函数返回 TRUE 即自绘菜单并承担回答义务：下面两个调用必须恰好执行其一。返回 FALSE（或根本没连）则 session 在视图上弹出自带 popover——只想"右键能出菜单"的宿主白得一个。',
    },
    {
      sig: 'struct ChromiumMenuItem { int id; const char *label; const char *type; gboolean enabled; gboolean checked; }',
      kind: 'struct',
      desc: 'id 是 CEF 自己的编号（100 后退、117 全选、132 查看源码……）加上 helper 扩充的链接/图片/媒体行（26501–26507）。type 为 "command" / "check" / "radio" / "separator" / "submenu" / "none"；separator 无 label。label 保留 CEF 的 "(&B)" 助记拼法。',
    },
    { sig: 'void chromium_session_context_menu_select (ChromiumSession *self, int id)', desc: '选中一个菜单项——自绘宿主交还选择的两种方式之一。' },
    { sig: 'void chromium_session_context_menu_dismiss (ChromiumSession *self)', desc: '整个菜单作罢。helper 同时只持一个菜单请求，Chromium 在第一个被回答前不开第二个——不回答的菜单连累后面每一次右键。' },
    {
      sig: 'signal ::copy-text (text)',
      kind: 'signal',
      desc: '宿主策略：请宿主把复制下来的链接/图片地址放上系统剪贴板。留在 Chromium 内的动作 helper 自己执行；这两条是仅有的出墙请求。',
    },
    {
      sig: 'signal ::open-url (url)',
      kind: 'signal',
      desc: '宿主策略：请宿主开一个新窗口。',
    },
  ],
  permission: [
    {
      sig: 'signal ::permission-request (id, kind, origin, permissions, raw) → gboolean',
      kind: 'signal',
      desc: '页面问宿主要麦克风、定位、剪贴板……kind 为 "media"（getUserMedia）或 "prompt"（Chromium 权限提示）；permissions 是 GList（归会话所有的名称串，如 "device-audio"、"geolocation"、"clipboard"）；raw 是 CEF 被问到的原始掩码，想按位匹配的宿主用它。处理函数返回 TRUE 即自管提示并须以 chromium_session_permission_respond() 恰好应答一次；返回 FALSE（或不连）由 session 代答拒绝——今天"无宿主的页面"看到的就是这个。',
    },
    {
      sig: 'enum ChromiumPermissionResult { ACCEPT, DENY, DISMISS }',
      kind: 'struct',
      desc: '应答取值。',
    },
    { sig: 'void chromium_session_permission_respond (ChromiumSession *self, int id, ChromiumPermissionResult result)', desc: '发送应答。对宿主已不再持有的 id 应答等于没有效果。' },
    {
      sig: 'signal ::permission-dismissed (id, result)',
      kind: 'signal',
      desc: '页面问到下一个权限时，仍被扣着的提示会在宿主脚下被撤掉：result 为 "ignore"。media 类请求没有 dismiss 回调。不回答的代价超出沉默——页面的 promise 悬着，第二个 media 请求永远等不到。',
    },
  ],
  tabs: [
    {
      sig: 'void chromium_session_new_browser (ChromiumSession *self, const char *url, gboolean windowed)',
      desc: '一个 helper 可承载多个 browser。第一个由 start() 创建；这里再建。windowed=FALSE 进共享标签窗口，TRUE 建独立 CefWindow。',
    },
    { sig: 'void chromium_session_select_browser (ChromiumSession *self, int id)', desc: '切换焦点 browser。id 由 CEF 分配，宿主应以 ::browser-created 为键建立自己的标签/窗口映射。' },
    {
      sig: 'void chromium_session_close_browser (ChromiumSession *self, int id)',
      desc: '关闭一个 browser，标签与窗口式都生效，对应 ::browser-closed 一次。关掉最后一个 browser 会结束 helper。未知 id 会得到 status: no-such-browser 应答——宿主借此区分"已经没了"与"从未存在"。',
    },
    { sig: 'signal ::browser-created (id, url, windowed)', kind: 'signal', desc: '新 browser 上线。' },
    {
      sig: 'signal ::browser-selected (id)',
      kind: 'signal',
      desc: '焦点转移。它可能先于 ::browser-created 到达——helper 会对"创建事件还在路上"的 browser 报告选中，宿主必须能为还没见过的 id 先立一个标签。',
    },
    { sig: 'signal ::browser-closed (id)', kind: 'signal', desc: 'browser 消亡；标签与窗口都会发一次。关掉最后一个后 helper 结束，随后是 ::helper-exited。' },
  ],
  misc: [
    {
      sig: 'void chromium_session_zoom (ChromiumSession *self, const char *action)',
      desc: 'action 取 "in" / "out" / "reset"：in/out 各走一个 CEF 缩放级（0.5，即一个 Chrome UI 档），reset 回 100%。Chromium 按来源持久化缩放——导航到存过级别的来源时页面拿到的是存的值，不一定是 0。',
    },
    {
      sig: 'double chromium_session_get_zoom_level (ChromiumSession *self)',
      kind: 'getter',
      desc: '最近一次上报的级别，0.0 为 100%。helper 对 zoom 命令与每次主框加载都以 ::zoom-changed (level) 应答。',
    },
    {
      sig: 'void chromium_session_set_muted (ChromiumSession *self, gboolean muted)',
      desc: '页面声音经 helper 自带的 audio 进程直通宿主扬声器，绕开部件——这是嵌入应用唯一的开关。get_muted () 报的是 helper 读回的值，调用后隔一个往返才变，并随之移动 ::changed。',
    },
    {
      sig: 'void chromium_session_set_accessibility (ChromiumSession *self, gboolean enabled)',
      desc: '运行时无障碍开关。session 会自己盯 session bus 上 org.a11y.Status 的 ScreenReaderEnabled 并调用它，宿主也可强制。TRUE 映射 CEF 的 STATE_ENABLED——Alloy windowed 浏览器下即 Chromium 自带的 Complete AT-SPI 模式，ORCA 实测可达。helper 启动时的 CHROMIUM_A11Y=1 是显式覆盖，会压制自动监视。',
    },
    {
      sig: 'uint64_t chromium_session_get_frame_count (ChromiumSession *self)',
      kind: 'getter',
      desc: '合成器自会话开始呈现的顶层数帧数——计数而非速率；要画图请用定时器采样。另有 ::changed 信号在属性批变时发出。',
    },
  ],
  view: [
    {
      sig: 'ChromiumView *g_object_new (CHROMIUM_TYPE_VIEW, NULL)',
      desc: '部件自身创建嵌套合成器。放入窗口并 present 后，session 即可绑定它。',
    },
    { sig: 'const char *chromium_view_get_socket_name (ChromiumView *self)', kind: 'getter', desc: '内层 Chromium 进程要以 WAYLAND_DISPLAY= 启动的 socket 名。' },
    { sig: 'NestedCompositor *chromium_view_get_compositor (ChromiumView *self)', kind: 'getter', desc: '取出部件内的合成器，供共享视图或直接操作使用。' },
    {
      sig: 'ChromiumView *chromium_view_new_shared (NestedCompositor *compositor, NestedSurface *target)',
      desc: '在别的视图的合成器上再开一个视图：第一个视图保住键盘/剪贴板/帧时钟所有权，target 指定这一个视图画哪个嵌套 toplevel（NULL = 主 surface）。demo 用它给每个独立 CEF 窗口配一个 GTK 窗口。set_target_surface () 可后续换目标。',
    },
    {
      sig: 'NestedCompositor *nested_compositor_new (GError **error)',
      desc: '直接创建合成器（一般不必：ChromiumView 自带）。$XDG_RUNTIME_DIR 下自有 socket。',
    },
    {
      sig: 'void nested_compositor_set_clipboard (NestedCompositor *self, GdkClipboard *clipboard)',
      desc: 'wl_data_device ↔ GdkClipboard 双向桥：client 的拷贝落到 clipboard，clipboard 的内容作为 selection 提供给 client。NULL 断开（部件 unmap 时就是这么做的）。另有 set_primary_clipboard ()——只走文本的主选区桥。set_frame_clock () 把 client 的帧回调校到部件的显示周期；set_cursor_target () 让页面要的光标成为平台窗口的光标。',
    },
    {
      sig: 'void nested_compositor_set_input_surface (NestedCompositor *self, NestedSurface *surface)',
      desc: '把指针/键盘路由到指定 surface 而非主 surface；NULL 清除。两个 GTK 窗口共享一个嵌套合成器靠的就是它——各自聚焦时报出自己显示的那个 CEF toplevel。clear_input_surface () 在窗口消失时交还路由。拖拽出窗经 drag_take_out ()/::drag-out，入窗经 drag_from_host () 系列。',
    },
    {
      sig: 'void nested_compositor_notify_pointer_motion (NestedCompositor *self, double x, double y)',
      desc: '输入注入族之一：坐标是 surface 本地逻辑像素，GTK 部件自己的事件坐标可直接送入。同级还有 notify_pointer_button ()（BTN_* 码）、notify_pointer_axis ()（一次手势双轴，含 value120 刻度与 stop 帧）、notify_keyboard_key ()（evdev 码；已按下的键再 press 是宿主的自动重复，正是 client 期待合成器做的）。set_pointer_active ()/set_keyboard_active () 控制转发总闸。',
    },
    {
      sig: 'gboolean nested_compositor_text_input_active (NestedCompositor *self)',
      kind: 'getter',
      desc: '输入法组合进行中：此刻部件须把按键交给 GtkIMContext 并扣住 notify_keyboard_key——被组合的 client 要的是文字，不是文字背后的原始键。get_text_input_state () 取 client 报的字段信息（字符索引非字节），set_preedit_text ()/commit_text ()/delete_surrounding_text () 送回组合结果。',
    },
  ],
}

// 信号总表：覆盖 src/chromium-session.c 注册的全部 24 个信号，按主题排列；参数类型取自 g_signal_new
export const signalRows = [
  { name: 'changed', args: '—', desc: 'URI / 标题 / 加载态 / 静音回读等属性批变时发出' },
  { name: 'ready', args: '—', desc: 'helper 连上控制通道，初始 URL 即将加载' },
  { name: 'load-end', args: 'url: string, http_code: int', desc: '导航终局；失败导航亦发（http_code=0）' },
  { name: 'load-error', args: 'error_code: int, url: string, message: string', desc: '未提交的导航；一次失败报告两次（真实码 + ERR_ABORTED）' },
  { name: 'renderer-exited', args: 'status: int, error_code: int, message: string', desc: '渲染进程死亡：0 异常 / 1 被杀 / 2 崩溃 / 3 OOM' },
  { name: 'script-result', args: 'id: int, ok: gboolean, value: string, message: string', desc: 'JS 执行结果，value 为 JSON 文本' },
  { name: 'popup-requested', args: 'url: string', desc: '页面请求新窗口；前台已被就地导航，后台标签仅报告' },
  { name: 'page-message', args: 'json: string', desc: 'cefnestPostMessage() 整串回传' },
  { name: 'primary-selection', args: 'text: string', desc: '页面选择变化的主选区镜像；空串即清除' },
  { name: 'browser-created', args: 'id: int, url: string, windowed: gboolean', desc: '新 browser 上线' },
  { name: 'browser-selected', args: 'id: int', desc: '焦点转移；可能先于 created 到达' },
  { name: 'browser-closed', args: 'id: int', desc: 'browser 消亡；标签与窗口都会发一次' },
  { name: 'download-begin', args: 'id: int, url: string, suggested_name: string, mime_type: string, total_bytes: int64', desc: '下载之问；不回答则停靠不落盘' },
  { name: 'download-progress', args: 'id: int, received: int64, total: int64, percent: int, speed: int64', desc: '每整百分比一条；speed 为 bytes/s' },
  { name: 'download-end', args: 'id: int, state: string, path: string, received: int64', desc: '唯一收尾：complete / cancelled / interrupted' },
  { name: 'find-result', args: 'identifier: int, count: int, active_ordinal: int, final: gboolean', desc: '查找应答；有匹配答两次，无匹配一次' },
  { name: 'context-menu', args: 'x: int, y: int, items: GList* → gboolean', desc: '右键菜单；TRUE 自绘并承担回答义务' },
  { name: 'permission-request', args: 'id: int, kind: string, origin: string, permissions: GList*, raw: int → gboolean', desc: '权限之问；TRUE 自管并须恰好应答一次' },
  { name: 'permission-dismissed', args: 'id: int, result: string', desc: '提示被页面下一步撤掉时 result="ignore"' },
  { name: 'zoom-changed', args: 'level: double', desc: '缩放级上报；0.0 = 100%' },
  { name: 'copy-text', args: 'text: string', desc: '宿主策略：复制链接/图片地址上剪贴板' },
  { name: 'open-url', args: 'url: string', desc: '宿主策略：开新窗口' },
  { name: 'closed', args: 'reason: string', desc: '页面没了；每次 start 至多一次，进程可能还在 CefShutdown()' },
  { name: 'helper-exited', args: '—', desc: '子进程已被 wait 收走；拆与 helper 共用的合成器等这一条' },
]
