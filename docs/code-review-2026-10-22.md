# cefnest 代码审查报告

- 日期：2026-10-22
- 范围：全仓库（src/、helper/、test/、examples/、scripts/、构建与 CI 配置、docs/ 交叉核对）
- 方式：只读审查，未做任何编辑。所有【严重】项均已逐条人工核实到源码行
- 基线：main 分支 96de08c（领先 origin/main 16 个提交），工作区含 11 个文件未提交改动（多浏览器 + 下载路径）

## 目录

1. [项目概况](#项目概况)
2. [总体评价](#总体评价)
3. [严重问题](#严重问题已核实)
4. [中等问题](#中等问题)
5. [轻微问题](#轻微问题)
6. [存疑项](#存疑项需进一步验证)
7. [修复优先级建议](#修复优先级建议)
8. [附录：模块审查明细](#附录模块审查明细)

---

## 项目概况

**cefnest** — GTK4 组件库，在宿主进程内运行 libwayland-server 合成器，独立 CEF/Chromium 进程（helper）作为 Wayland client 连接，把真实 Chromium 渲染合成进 GTK widget。非 OSR、非 XEmbed、纯原生 Wayland。

三大组件：

| 组件 | 文件 | 职责 |
| --- | --- | --- |
| NestedCompositor | `src/nested*.c` | 合成器：globals、surface 栈、damage、输入、剪贴板、双向拖放 |
| ChromiumView | `src/chromium-view.c` | widget：输入路由、GdkTexture 绘制 |
| ChromiumSession | `src/chromium-session.c` | 控制平面：helper 进程管理、NDJSON 控制通道、页面状态 |

规模：src/ 约 500KB C 代码，helper/ C++ CEF 应用（cmake 构建），test/ 含 shm-client.c（110KB）、seat-selftest.c（54KB）、11 个 bash gate 脚本。文档 31 篇（docs/）与实现逐项对应。

## 总体评价

代码纪律和文档同步度在同类项目中属上游水平。经专项核查，核心主干（会话生命周期、剪贴板传输、CEF 线程模型）没有发现经典 use-after-free：

- `mark_closed()` 幂等设计、子进程收尸与 channel 挂断的双路径竞态处理正确；
- shm 的 64 位偏移校验与双预算上限、frame callback 的 ref 与 destroy listener 严格配对；
- helper 的 `--no-sandbox` 单一出口、读线程只解析并 `CefPostTask(TID_UI)`、`Shutdown()` 先于 `CefShutdown()` 的顺序；
- 测试基建诚意高（broken_run 假绿覆盖、dmabuf 计数器防静默回退、fd 计数）。

风险集中在三处交集：

1. **异常/敌意客户端路径** — SIGBUS、类型混淆、无界递归；
2. **多浏览器新扩展**（部分为未提交工作）— fullscreen 广播、悬垂键、幽灵 slot、孤儿 helper；
3. **控制通道两端协议层自我保护** — 部分写、无行上限。

---

## 严重问题（已核实）

### S1. data_device.set_selection 类型混淆 → UAF

`src/nested-data-device.c:2359-2372`

`data_device_set_selection` 直接 `wl_resource_get_user_data (source_resource)`，不校验接口类型。对比 primary 侧（同文件 1146 行）有 `wl_resource_instance_of` 校验。

后果：客户端把 `zwp_primary_selection_source_v1` 的对象 id 传进来，`selection_source` 指向另一种结构体（PrimarySource）被当 `DataSource` 解引用；该 source 销毁时走 primary 分支只 detach `primary_offers` 就 `g_free`，`selection_source` 指针与已挂入 `source->offers` 的 data offer 全部悬垂；之后 `compositor_replay_selection`（660 行）、`data_offer_receive`（2006 行）读已释放内存。2368 行还会对 primary 资源发 opcode 2（`wl_data_source.cancelled`）打死客户端。

### S2. dispose 遗漏 primary_replay_id idle 源 → UAF

`src/nested.c:2179-2182`（只清 `selection_replay_id`）；`primary_replay_id` 全仓库仅 nested-internal.h:253 与 nested-data-device.c:971/987/990 引用，销毁路径无任何清除（已 grep 证实）。

后果：合成器 dispose 时若 `primary_replay_selection` idle 挂起（unmap/销毁途中 primary 剪贴板变更即可触发），idle 在对象 finalize 后运行，写 `self->primary_replay_id` 并遍历 `self->primary_devices` → use-after-free。与相邻代码对 `selection_replay_id` 的正确处理形成明显不对称，属可立即修复的低成本问题。

### S3. shm pool 事后 ftruncate → SIGBUS 宿主崩溃

`src/nested.c:1205`（mmap 一次）、`fstat` 仅在 pool 创建（1174）与 resize（1393）时调用；commit 路径的 `read_pool`/`copy_pool_rect` mmap 快路径直接 `memcpy (dst, pool->map + offset, length)`。

后果：客户端通过 create_pool/resize 校验后对自己 fd `ftruncate` 缩小文件，宿主下次 commit 读回时 SIGBUS，整个宿主进程崩溃。pread 慢路径（549-551 行）已能优雅处理短文件，说明作者意识到该场景，但 mmap 快路径绕过了它；也未安装 SIGBUS handler（wlroots 等的常规对策）。威胁模型已包含敌意客户端（per-client shm 预算），此口子与之矛盾。

### S4. 控制通道部分写 → NDJSON 永久失步

宿主侧 `src/chromium-session.c:762-768`：单次 `g_output_stream_write` 只判 `< 0`，不循环补写。写入量超过 socket 发送缓冲（大段 `js <code>`、超长 URL）时行被静默截断，helper 的 NDJSON 流从此错位，无重同步路径。

helper 侧对称缺陷：`helper/src/main.cpp:279-291` 的 `Send()` 靠 SO_SNDTIMEO 200ms 限停，超时/出错发生在部分写入之后时直接 return，残留半条 JSON，下一条事件拼接其上。

### S5. helper 孤儿进程 + 无信号处理

`helper/src/main.cpp:200-211`：`HostGoneTask` 只关当前 `g_browser`（`CloseBrowser(true)`），而 `OnBeforeClose` 在还有其他 slot 时会选中下一个浏览器继续跑消息循环 → 多浏览器场景下宿主崩溃/断开后 Chromium 成为孤儿进程，违背文件头 16-17 行"宿主关闭 socket 则 helper 退出"的承诺。应遍历 `g_browser_slots` 全部关闭或直接 `QuitOnce()`。

另外 helper 全程无 SIGTERM/SIGINT 处理（main.cpp 仅 2243 行注释提到信号）→ 默认处置直接杀死进程，`control.Shutdown()`/`CefShutdown()` 均不执行，缓存目录 SingletonLock 可能残留。

### S6. demo assigned_surfaces 悬垂键 → 新窗口黑屏

`src/main.c:300-320`：`assign_browser_surfaces` 以裸 `NestedSurface*` 原始指针为键插入（307、320 行），全文件无删除路径（`browser_window_destroyed` 332-341 行只清 `browser_windows`/`browser_views`）。

后果：surface 随 helper 关闭浏览器销毁后键残留；compositor 复用同一堆地址创建新 toplevel 时被 `g_hash_table_contains`（300 行）误判"已分配"→ 新窗口永不获得 surface 或绑到错误 browser id，表现为黑屏/画面错配。

---

## 中等问题

### 合成器（nested*）

- **M1** `src/nested.c:1059-1073` — 销毁 wl_surface 不清理 `nested->cursor_pending`；`nested-cursor.c:332` 的强引用使 zombie surface 连同 texture/shm_shadow（可达百 MB 级）滞留到下次 set_cursor。
- **M2** `src/nested.c:1922-1923`、`src/nested-xdg.c:332-336` — popup 链无深度限制递归，恶意客户端几万个嵌套 popup 可栈溢出。
- **M3** `src/nested.c:863` — `set_buffer_scale` 立即写入 `self->scale`，未按协议 commit 时生效 → set_scale 与 commit 之间 partial-copy 读错区域（花屏/尺寸错乱）。
- **M4** `src/nested-dmabuf.c:621-626` — buffer 创建失败路径 `dmabuf_fds_close` 后 `buffer_discard`（513-523，不查 `fds_transferred`）再次 close → 双重 close，fd 号复用时误关无关描述符。
- **M5** `src/nested-data-device.c:722-724、940-942` — `compositor_paste_clipboard`/`primary_paste_clipboard` 调 `gdk_clipboard_read_text_async` 不持引用 → dispose 后回调收到悬垂 `self`。
- **M6** `src/nested-data-device.c:404-437` — `clipboard_transfer_start` 不检查 `g_unix_fd_add_full` 返回值：失败时读端 fd 永不关闭、transfer 永久滞留、源客户端写端被阻塞。
- **M7** `src/nested-data-device.c:1446-1455、1600-1607` — drag_enter 中 offer 创建失败仍带 NULL offer 发 enter，`drag_drop` 不视为拒绝照发 → drag 永停在 dropped-unconcluded。
- **M8** serial 校验普遍缺失：`data_device_set_selection`（2359）、`primary_device_set_selection`（1134）、`start_drag`（2276，serial 仅留日志）、`data_offer.accept`（1977）。`set_cursor`/`set_shape` 已正确校验，其余是明确缺口。
- **M9** `src/nested.c:2204` — dispose 中 `wl_display_destroy_clients` 触发的销毁回调链会向宿主发 `::toplevels-changed`/`::frame`，宿主在析构中途收到回调，依赖宿主自律。

### 会话 / widget / demo

- **M10** `src/chromium-session.c:696` — `g_data_input_stream_read_line_async` 无行长上限，异常 helper 可令宿主内存无界增长；行内 `\0` 被 `json_from_string` 静默截断。
- **M11** `src/chromium-view.c:332-342` — `chromium_view_watch_drops` 每次 map 重复添加 GtkDropTarget，unmap 不移除 → 一次拖放触发 N 次合成 drag；`window-fullscreen-request` 连接（370-372）同样只增不减。
- **M12** `src/chromium-view.c:370-372` — 共享 compositor 时 fullscreen 请求无目标参数，广播到所有已映射窗口。
- **M13** `src/chromium-session.c:730` — `accepted()` 在非取消性失败（如 EMFILE）后无条件重挂 `accept_one`，无退避 → 主循环忙旋。
- **M14** `src/chromium-session.c` 多处（313-314、335、352-354、398、486、1410-1411、1571、1610）— 事件字段直接 `json_object_get_*_member`，缺字段仅靠 critical 兜底，NULL 流入信号参数。

### helper / 测试 / 构建

- **M15** `helper/src/main.cpp:315` vs `295-307` — 读线程锁外读 `fd_`，`Shutdown()` 锁内 swap 并 close → 竞态窗口内 fd 复用，`read()` 读无关描述符。
- **M16** `helper/src/main.cpp:2145` — `g_browser_slots[slot].view = view` 用请求侧 slot 回写，与 `OnAfterCreated` 错位时生成幽灵 slot，`OnBeforeClose` 清不掉 → 全部浏览器关闭后消息循环不退出。
- **M17** `helper/src/main.cpp` — 控制通道无 `SO_PEERCRED` 校验，能连上 socket 的本地进程可让 helper 导航 `file://`、执行任意 JS、向任意路径写文件（依赖 socket 文件权限，代码层零防护）。
- **M18** `helper/build/Release` 四处硬编码（meson.options:12-14、test/ci-session.sh:31、test/download-gate.sh:24、helper/CMakeLists.txt:32-33）→ Debug/多配置生成器即失效。
- **M19** `test/pixel-gate.sh:89-93` — 缺 ImageMagick 时整门 `exit 0` 放行，`ci-all.sh:95-97` 按 rc 判 PASS → 像素校验完全未执行仍绿，违背 ci-all.sh:22-23"绿行不得隐藏 skip"的自述。
- **M20** `test/shm-client.c:3371-3397` — 合成器发协议错误时客户端打印后 `return 0`，plain 模式无帧导入/存活断言 → `meson test` 假绿。
- **M21** `scripts/fetch-cef.sh:37` — 百 MB CEF 发行包无 sha256 校验，与"pinned"承诺不匹配。
- **M22** `helper/src/main.cpp:1272-1287` — `close-browser` 对**标签模式**的 browser 是静默空操作：命令分发到了（同一个 id 换成不存在的 99 会回 `no-such-browser`），但 `CloseBrowser(true)` 之后没有 `OnBeforeClose`，既没有 `browser-closed` 事件，标签也留在宿主上。windowed 那条路正常。见「仍在处理」的实测记录。

## 轻微问题

| # | 位置 | 问题 |
| --- | --- | --- |
| L1 | src/nested.c:823 | damage 换算 int32 乘法可溢出（UB） |
| L2 | src/nested-xdg.c:181、296-319 | 位置器 int32 算术溢出（UB） |
| L3 | src/nested-xdg.c:752-766 | toplevel 销毁后允许同 xdg_surface 再次 get_toplevel（协议偏差） |
| L4 | src/nested-xdg.c:620-625 | 拖拽图标 surface 可被赋予 xdg 角色（角色冲突） |
| L5 | src/nested-xdg.c:477-480 | xdg_wm_base.destroy 不按规范 post error |
| L6 | src/nested.c:2538 | `set_input_surface` 无 `NESTED_IS_SURFACE` 校验 |
| L7 | src/nested.c:1332-1335 | 错误消息把 `needed - offset` 说成 "buffer needs N bytes"（诊断误导） |
| L8 | src/nested-seat.c:117-119 | `keyboard_impl` v4 的 `grab_keyboard` 槽位为空 → libwayland post error 杀死客户端 |
| L9 | src/nested-seat.c:811-820 | `stop` 对两轴无条件各发 axis_stop（未滚动轴收到假停止） |
| L10 | src/nested-cursor.c:332 | 光标 surface 销毁后旧光标图滞留显示 |
| L11 | src/chromium-session.c:1351-1357 | 空菜单路径 popover 未 set_parent 即 unparent → 泄漏 |
| L12 | src/chromium-session.c:2422-2426 | dispose 丢 `self->child` 后立即退出主循环时 helper 短暂僵尸 |
| L13 | src/main.c:698-772 | 下载名无长度上限；ext 空串时尾分隔符；存在性检查与写盘 TOCTOU |
| L14 | src/main.c:3162 | `helper_is_available()` 同步 D-Bus 探测阻塞启动 |
| L15 | src/main.c:2019 | 前一个 out-drag 未结束时 `demo_out_temp_path` 被覆盖 → 临时文件泄漏 |
| L16 | src/main.c:1720-1738 | `capture_view` 无 surface 时读取未初始化栈内存并打印 |
| L17 | src/chromium-view.c:948-951 | `chromium_view_new_shared` 先自建 compositor 再被换掉（socket 残留风险） |
| L18 | src/meson.build:18-21、85-88 | c_args 路径拼接，含 `"`/`\` 时产生非法/被注入的 C 字符串字面量 |
| L19 | src/meson.build:54 | pkg-config `requires` 漏 `threads`/`m`（应入 requires_private） |
| L20 | scripts/fetch-cef.sh:27 | URL 编码只处理 `+` |
| L21 | test/download-gate.sh:219 vs 223 | 注释 1 MiB vs 实际 2 MiB（文档漂移） |
| L22 | test/ci-session.sh:225-227 | `GS` 为空时收尾检查假绿通道 |
| L23 | test/seat-selftest.c:1370-1374 | 无显示时 return 0 计 PASS 而非 skip |
| L24 | .gitignore:15-16 | `third_party/` 重复两行；`builddir2` 类命名漏掉 |
| L25 | .cnb.yml | 无流水线级超时、无产物上传、runner 手工预置 CEF 包且不校验与 pin 一致 |

## 存疑项（需进一步验证）

- `json_from_string` 对超深嵌套 JSON 的深度限制（取决于链接的 json-glib 版本）。
- GLib < 2.66 时 `GSubprocess` 无人 wait（main.c:526、session.c:2425）是否留僵尸。
- compositor 创建失败后 view 仍 map 时 `on_map` 向 NULL instance 调 API 的行为。
- view `dispose` 直达（不先 unmap）是否可达；dispose 未补 `clear_input_surface`（chromium-view.c:808-840）。
- `primary_offer_receive` 仅按 `mime_types[0]` 判能力，多类型 primary source 可能被误拒。
- demo rpath 在 `libdir` 为绝对路径时 `fs.relative_to` 的行为。
- .cnb.yml 并发：两台 push 同打同一 self-hosted runner 的互斥取决于 runner 配置。

## 修复优先级建议

1. **立即（改动小、收益直接）**：S1（补接口类型校验）、S2（dispose 补清 `primary_replay_id`）、S4 宿主侧（写循环补写）。
2. **当前未提交工作合入前收口（多浏览器）**：S5（HostGone 遍历 slots/QuitOnce + 信号处理）、S6（assigned_surfaces 生命周期）、M11、M12、M16。
3. **敌意客户端封堵**：S3（commit 时 fstat 复查或 SIGBUS handler）、M2（popup 深度限制）、M8（serial 校验）、M3。
4. **测试可信度**：M19、M20、L23（skip 语义）、M21（sha256）。
5. **其余中等/轻微按需跟进**。

## 复核与处置（同一份报告，逐条回到当前树核实）

处置基线：`meson test` 22 项全绿（19 个 seat-selftest 模式 + 3 个静态/单元）。每条收录的判定都按
项目规矩在线上或像素上评分，新增门禁都做过双向变异。

### 已闭环（修复 + 门禁）

| 条目 | 处置 | 门禁 |
| --- | --- | --- |
| S1 | 补 `wl_resource_instance_of` 校验 | `--wrong-object`（裸 frame 绕过 libwayland 客户端的类型检查，见 shm-client.c:3746-3753 的实测记录） |
| S2 | dispose 补清 `primary_replay_id` | `test/audit-loop-sources.py`（静态：每个 idle/timeout 要么持引用要么被 dispose 取消） |
| S3 | 事后缩小的 pool 直接拒绝读取 | `--pool-shrink`（两种 pool 生命周期各一遍） |
| S4 | 宿主与 helper 两侧都补写循环 | `test/framing-gate.sh` |
| S5 | `CloseEveryBrowser()` + 信号处理 | `test/sandbox-soak.sh`、`test/multi-browser-gate.sh`、`test/helper-signal-gate.sh`（信号那半，见下方订正） |
| S6 | `assigned_surfaces` 生命周期 | `test/multi-browser-gate.sh` |
| M2 | popup 深度上限 `NESTED_POPUP_MAX_DEPTH` | `--popup-depth`（第 33 层被拒） |
| M8 | `start_drag` 必须引一次还按着的按下 | `--drag-fake-serial`（`docs/drag-loop.md` §6.7 附两次变异） |
| M3 / L1 | scale 与 damage 都改 commit 时生效，换算走 gint64 并 CLAMP | `--damage-scale`（三个采样点，`docs/frame-metrics.md` §2.1-2.2 附两次变异） |
| M7 | `drag_drop` 里给「永远等不到 finish 的那条 drag」就地退休 | `--drag-no-source`（`docs/drag-loop.md` §6.8 附变异） |
| M10 | 读侧自己切行 + 8 KiB chunk + 4 MiB 上限；行内 NUL 丢整行不截断 | `test/framing-gate.sh` 的 `flood` 段（一次跑三个方向：1 MiB 合法行必须整条读到、NUL 行必须不半信、无换行必须撞线；`docs/control-channel.md` "通道读这一侧" §4 附两次变异） |
| M11 | 两处挂接改回 `init`，共享 view 不再重连；`dispose` 断开设下来的 handler | `map-cycle`（运行，数 GLib 的槽位数，含反空跑的映射计数）+ `map-arms-audit`（静态，读 `*_on_map` 里挂出去又拆不回来的东西）；`docs/map-cycle.md` 附四次变异 |
| M13 | accept 失败改 200 ms 一次性重试，`dispose` 里 `g_clear_handle_id` | **没有门禁**：要造 EMFILE 得先把进程的 fd 塞满，而塞满之后先炸的是别的东西。忙旋这件事只能由源码形状拦 |
| M14 | 五个 `event_*` 包装：缺字段折成该类型的空值（`""`/`0`/`FALSE`/`0.0`/`-1`） | `test/event-shape-gate.sh`（28 条只有名字的事件，零 critical、零 `(null)`；变异把包装退回直接 getter → 18 critical + SIGSEGV） |
| 其余 S/M 严重度相邻项 | 空 selection payload、dnd v3 请求、每连接 pool 预算 | `--empty-copy`、`--dnd-requests`、`--pool-budget` |

### 复核后对报告的订正

- **M3 的方向说反了**：`set_buffer_scale` 与 damage 同为双缓冲，真正的分歧只有「同一轮里 damage
  先写、scale 后写」，错法是**少覆盖**（新 buffer 只读一角，其余继续显示上一帧），不是报告说的花屏
  或尺寸错乱。weston 与 mutter 都在 commit 时用刚生效的 scale 换算。已由 `--damage-scale` 钉住。
- **S1 的可达性**：经 libwayland-client 包装的调用到不了合成器（库自己报 `invalid object (8),
  type (zwp_primary_selection_source_v1), message set_selection(?ou)`），只有裸 socket 写帧能
  触发。结论不变（合成器必须自己能说不），威胁模型从「任何客户端」收窄为「任何写得出帧的客户端」。
- **M1 是有界延迟持有**，不是无界泄漏：`cursor_pending` 的强引用在下一次 `set_cursor` 或 surface
  销毁时释放。已补销毁分支。
- **M4 只在 ENOMEM 路径**：`fds_transferred` 之后二次 close 需要分配失败才会走到。已加判据。
- **M6 的 fd 泄漏不可达**：`g_unix_fd_add_full` 失败要求主循环本身拒绝挂载。
- **M7 的 NULL offer 本身合法**（wayland.xml 明说 source 为 NULL 时只给发起者发 enter/leave/motion），
  但那条 drag 因此永远无法被 `finish` 终结 — 这一半是真问题，另半（offer 创建失败）只在 OOM 时发生。
  后果也不是报告说的「停在 dropped-unconcluded」而是画不出来：图标 surface 的 `is_drag_icon` 只在
  `drag_destroy` 清，之后同一块 surface 被客户端拿来当 toplevel 会永远不进绘制栈。
- **L8 不成立**：`wl_keyboard_interface` 只有 `release` 一个槽，`grab_keyboard` 是合成器→客户端事件；
  报告还把后果方向也写反了（空请求槽打死的是整个合成器，不是客户端）。
- **L10 不成立**：光标 surface 销毁后保留旧图是正常客户端流程（换图标时先销毁上一张），
  wayland.xml 对 `set_cursor` 的 surface 死掉没有额外要求；pending 那一半已单独修好。
- **L19 不成立**：`libcefnest.so` 的 `NEEDED` 已有 `libm`，`gtk4 → glib-2.0` 已带 `-pthread`。
- **L20 不成立**：CEF 版本串里只有 `+` 需要 URL 编码。
- **M17 不成立**：socket 路径含随机 uuid，位于 0700 的 `$XDG_RUNTIME_DIR`， inode 权限 0600；
  `SO_PEERCRED` 在这个前提下只能证明同 uid，而同 uid 的攻击者与合法 peer 逐字节相同。
- **M15 已修**：`Shutdown()` 在锁内 swap `fd_`、`shutdown()` 唤醒阻塞的 `read`、`join` 读线程，
  之后才 `close`，没有描述符复用的窗口（残留的只是无同步的 `fd_` 读，最坏 `EBADF`）。
- **M14 的严重度报低了**：报告写的是「NULL 流入信号参数」，实测把五个包装退回直接 getter 之后，
  一条只说事件名的流不止打印 18 条 critical，**还把宿主打死（SIGSEGV）**。按后果它够 S 级，按
  可达性（对端是版本不同的 helper 就算，不需要敌意）也够。已按 M 级修好并进门禁。
- **M10 只中了宿主这一半**：报告那句「写路径部分写与读路径无上限假设理想流」里，无上限那半在宿主
  成立，helper 那边 `ReadLoop()` 早就按 4 KiB chunk 自己切行、1 MiB 撞线就丢弃 buffer
  （`helper/src/main.cpp:390`）。两端现在各自有上限，但**不对称**：helper 1 MiB 配丢弃继续服务，
  宿主 4 MiB 配拆连接；而且 helper 那一半没有门禁。
- **L11 成立，而且比报告说的更安静**：`gtk_widget_unparent()` 作用在**没有 parent** 的 widget 上
  （GTK 4.18 实测）什么都不做、浮游引用还在，所以空菜单那条路每次都留下一个 popover 加它的 box，
  且没有任何告警。已 `g_object_ref_sink` + `unref`。
- **S5 的信号那一半是虚报**：本节原先把「`CloseEveryBrowser()` + 信号处理」记成已闭环，但
  2026-10-23 那份复核在 `helper/src/main.cpp` 里 `grep` 不到 `signal`/`sigaction`/`SIGTERM`——
  报告那条「helper 全程无 SIGTERM/SIGINT 处理」当时只修了 `CloseEveryBrowser()`，信号处理从未写下去。
  现在补上了，并且是按量出来的形状补的：`CefInitialize()` 一路走出去会把 SIGHUP/SIGINT/SIGTERM
  改回 `SIG_DFL`（strace：装好之后约 2 ms 被重置），所以 handler 必须装在它**之后**；装在前面的一版
  门禁跑出来就是退出码 143/130、转录里没有 `caught signal`。门禁 `test/helper-signal-gate.sh`
  会先读 `/proc/PID/status` 的 `SigCgt` 再动手，将来 CEF 再把这三个收回去会当场响，而不是只少一行日志。
  量的过程里还翻出两个原有缺陷，见 `docs/features/session-lifecycle.md` §7：
  `HelperWindowDelegate` 多持有一份 browser view 的所有权，导致**第一个 tab 永远关不掉**
  （`CloseBrowser(true)` 被 Alloy 读成「关窗口」，`OnBeforeClose` 不来）——不修它，信号处理只能
  走到「开始关浏览器」；以及 helper 自报 `{"event":"closed"}` 之后，宿主的 dispose 仍会
  `g_subprocess_force_exit()`，正好把 `CefShutdown()` 砍在半路（profile lock 已经放了，但
  `main()` 的尾巴再也不跑）。两处都已修。

### 仍在处理

M12、M18、M21、M22、L2-L7、L9、L12-L17、L25 与存疑项：逐条核实中，
结论按上面的格式补进本节。M10/M11/M13/M14 与 L11 见上面「已闭环」（M13 无门禁，理由也在那里）。

- **M22 成立，是写 `examples/host.c` 时撞出来的**（2026-10-22，`test/ci-session.sh` 里的真
  helper，标签 `file://…/test/dnd-page.html?second`）。三条对照，逐字：
  `close-browser 99` → `** Message: …: session: helper status: no-such-browser`；
  `close-browser 2`（tab，slot 确实在）→ 之后**什么也没有**，没有 `helper: browser 2 closing`，
  没有 `browser-closed`，4 秒后 `quit`；
  `close-browser 3`（windowed）→ `helper: browser 3 closing`、`demo: browser 3 closed`、
  `demo: browser 1 selected`。
  所以不是 id 错位，也不是宿主没接事件。猜测（**未验证**）：`1281-1283` 先 `RemoveChildView`
  把 view 从 `CefWindow` 摘掉，CEF 在一个没有窗口父级的 `CefBrowserView` 上把 `CloseBrowser`
  当成无事可做。要证它，得试「不摘 view 直接 Close」和「摘了 view 之后等一帧再 Close」两种
  顺序，看哪条能出 `OnBeforeClose`。标签关不掉意味着多浏览器的 tab 生命周期只进不出，
  这是宿主层面无解的：`::browser-closed` 收不到，宿主就不能拆自己的 tab。

### 二轮审查（2026-10-22，提交 `1a5ee7f` 之后对同一批改动的复核）

对 M10/M11/M13/M14、L9、L11 这一轮改动整体复核：自切行读循环的每条边界（吐行后
`line_pending` 的存活检查、逐 chunk 的上限判断、严格大于语义）逐路径核对无洞；空菜单
popover 的 `ref_sink + unref` 对浮引用创建路径成立；`examples/host.c` 的"窗口先死"处理
逐 handler 核对无 NULL 漏网；`map-cycle` 的 block/unblock 计数法成立。25 个可自动运行的
测试全绿。**未发现正确性问题**，以下为新增记录（编号接 R 起，区别于首轮 M/L）：

- **R1（中）`test/audit-map-arms.py` 没有反空跑保护。** `MAP_BODY` / `ARM` 正则一旦与
  源码形状失配（`*_on_map` 改名、签名写成多行），`main()` 的循环跑 0 次直接打印 ok、
  rc=0——静默绿。`event-shape-gate.sh` 有"少于 20 个事件名就红"的下限，这条没有；
  而 `map-cycle` 在无 display 机器上 skip 时，它就是这条规则唯一的防线。另外
  `FUNC_HEADER` 只匹配单行函数签名，多行签名的函数体进不了 `functions` 索引，同为一层
  静默盲区。建议：加"至少找到 N 个 map 处理器，否则红"的下限。
- **R2（轻）999 次同名碰撞，示例与 demo 给了两种答案。** `examples/host.c:323` 的
  `download_path()` 循环耗尽后返回原名（理论上可覆盖已有文件），而 `src/main.c` 的
  `download_target_path()` 同场景用时间戳落名（本轮刚修的）。示例是被人照抄的代码，
  两条路应当一致——照抄 demo 的时间戳做法。
- **R3（琐碎）`test/event-shape-gate.sh:53` 注释与代码不符。** 注释说事件名
  "in the order the source lists them"，下一行 `sort -u` 排了序。删半句即可。
- **R4（记录取舍，非 bug）accept 重试不区分错误种类。** EMFILE 之外**永久性**的失败
  （如 socket 目录被删）会以 200 ms 一条的 warning 无限循环——比改前的热循环好得多，
  `src/chromium-session.c` 的注释也承认这是折衷，但"对错误不分类"意味着某类永久失败
  将来会以 5 Hz 刷日志的方式暴露。
- **R5（可不修）`scroll_axes` 清了、对端的账没清。** `src/nested-seat.c` 在
  `pointer_send_leave` 清 `scroll_axes`，但 `test/shm-client.c` 的 `axes_moved` 不随之清；
  "滚动 → 离开 surface → 重进 → stop"的序列会让客户端拿上一次手势的旧账比这一次的
  stop。selftest 不覆盖该序列，真实影响接近零——真要较真，是测试客户端自己欠的对称。

R1–R5 均未修：R1/R2/R3 留下一轮，R4/R5 记录在此以免重查。


---

## 附录：模块审查明细

### A. 合成器核心（nested.c / nested-xdg.c / nested-internal.h / nested.h）

协议健壮性意识明显高于平均水平：resource_create 的 no_memory 包装、逐资源类型校验、shm 64 位偏移校验与双预算上限、frame callback ref/destroy 配对、BufferUser 链表对称清理均经得起推敲，未发现经典 UAF。dispose"先杀 client、再摘 surface、最后拆 display"的顺序配合先移除 GSource 避免了事件循环重入。剩余风险：敌意 fd 篡改（S3）与客户端可控无界递归（M2）。

详见：S3、M1、M2、M3、M9、L1、L2、L5-L7。

### B. 输入 / 剪贴板 / DnD / dmabuf（nested-seat/data-device/dmabuf/cursor/text-input）

Transfer/offer/source 的 fd 归属与 slot 清除设计周密；DnD 对 offer 独立于 source 存活、icon/origin 强引用、surface 销毁中途 unmap 的处理核实无悬挂；tablet 与 README 声明一致。主要问题：set_selection 缺类型校验（S1，唯一客户端可触发 UAF）、销毁路径三处漏网（S2、M5、M4）。

详见：S1、S2、M4-M8、L8-L10。

### C. 会话 / widget / demo（chromium-session / chromium-view / main.c）

生命周期纪律好：mark_closed 幂等、竞态双路径覆盖、permission id 簿记、下载"宿主命名路径"模型成立；脚本解释器 sscanf 均带宽度，无溢出点。薄弱处：写路径部分写与读路径无上限（S4、M10）假设理想流；多窗口扩展暴露"信号无目标参数 + 表无清理路径"的结构性遗漏（M11、M12、S6）。

详见：S4、S6、M10-M14、L11-L17。

### D. helper / 测试 / 构建

CEF 线程模型主干干净。薄弱点集中在"多浏览器 + 异常路径"交集：孤儿 helper（S5）、幽灵 slot（M16）、部分写破坏分帧（S4 helper 侧）。测试基建诚意高，但 plain 模式对早退 0 的宽容和 pixel-gate 依赖缺失即整门跳过，恰在自己最强调的地方开了口子（M19、M20）。构建债：Release 硬编码四处（M18）、无校验和下载（M21）。

详见：S5、M15-M21、L18-L25。

---

*审查方式：四个并行深度审查代理分模块通读全部源码，所有【严重】项与关键【中等】项由主审逐一回到源码行核实后收录；存疑项单独列出，未与确认问题混排。*

---

## 第三轮：2026-10-22 修复记录

在本轮审查（二轮）之后，以下问题已修复，并在当前树上重新跑过
`meson test -C builddir`（26 项全绿）、`test/multi-browser-gate.sh` 和
`test/framing-gate.sh`。新增门禁都做过变异：把修复退回原状必须变红。

### 新发现并已闭环

| 条目 | 处置 | 门禁 |
| --- | --- | --- |
| popup 父链可成环（`nested-xdg.c:831`） | `get_popup` 沿父链走时拒绝 `ancestor == self`；深度循环带步数上限 | `seat-selftest-popup-cycle`；去掉环检查后红 |
| 控制通道 EOF 后重挂读循环（`chromium-session.c:751`） | EOF 与读错误都 `drop_connection` + `mark_closed` | `framing-gate.sh` 新增 eof 段（测 CPU tick）；退回重挂后 201 ticks 红 |
| `OnDismissPermissionPrompt` erase 后读迭代器（`helper/src/main.cpp:1133`） | 先取 `wire->second` 再 erase | `permission-gate` |
| CI 不编译 helper | `ci-all.sh` 开头 `cmake --build helper/build`，失败即整轮失败；`docs/testing/cnb-ci.md` 补 cmake 准备 | 构建失败即红 |
| M22 tab 关闭无效 | `RemoveChildView` 后清掉 slot 里的 view 引用，再 `CloseBrowser`；`OnBeforeClose` 只 erase | `multi-browser-gate` 新增 `close-browser 2`、`browser 1 selected`、关闭后 js 断言 |
| `set_size`/`set_window_fullscreen` 在 surfaces 遍历内 flush | 先收集带引用的 `GList`，再 configure | 现有 selftest |
| surface 销毁不清理 `frame_callbacks` | `surface_discard_frame_callbacks()` | 现有 selftest |
| text-input 偏移越界读 | 先 `g_utf8_strlen` 夹 offset；start/end 用 gint64 | `seat-selftest-text-input` |
| `close-request` 返回 void | 改回 `gboolean` 并返回 TRUE | 手工窗口关闭 |
| `send_json` 失败不拆通道 | 写/flush 失败都 `drop_connection` + `mark_closed` | `framing-gate` 现有段 |
| `::closed` 后 `start()` 返回 BUSY | 旧 child 未 reap 时允许 restart；`child_waited` 只处理当前 child | 未加门禁（需要构造重启宿主） |
| `OnBeforePopup` 用全局 `g_browser` | 改用参数 `browser`，并判 `GetMainFrame()` | 无门禁 |
| helper slot/id 混用、fullscreen 用全局窗口 | 先比较请求 slot 再 re-key；按 browser id 找 window | `multi-browser-gate` 的三浏览器路径 |
| `pending_window_ids` 死 id、`menu_popover` 覆盖 | 跳过无 view 的 pending id；新菜单先放旧菜单 | 现有 multi/menu gate |
| `wl_output` v4 不发 name/description；dmabuf v4 仍发 format | 按版本发送 | 现有 seat-selftest |
| 低危项 | cursor 热点溢出、dispose 半幂等、`GdkFileList` 泄漏、`event_string` 显式 null、`open_listener` socket 残留；L4 drag icon 角色、L6 input surface 类型校验、L7 错误文案 | 静态/现有测试 |
| L2 positioner 溢出 | `anchor_point` / `gravity_shift` / `place_with` / `overflow` 全部走 gint64，输出处才 clamp 回 int32 | `seat-selftest-popup-overflow`（anchor/offset 取 `INT32_MAX`，四种 constraint 全开） |
| L5 `xdg_wm_base.destroy` | 发现同 client 的 `xdg_surface` 还活着时 post `INVALID_METHOD`，不再静默销毁 | 现有 selftest |
| L15 demo out-drag 临时文件 | 前一个 drag 未结束（`demo_out_drag != NULL`）时拒绝新的 handover，不再覆盖 `demo_out_temp_path` | 无门禁（需要真实拖放） |
| R4 accept 重试不分类 | 只对 `G_IO_ERROR_TOO_MANY_OPEN_FILES` 重试；其他非取消错误 `mark_closed("control listener failed")`，不再 5 Hz 刷日志。旧 listener 的 accept 结果（stop/restart 后到达）直接丢弃 | 无门禁（需要构造 EMFILE/永久失败） |
| R5 测试客户端轴记账 | `shm-client` 的 `pointer_leave` 清 `axes_moved`/`axes_stopped`/`axis_in_frame`，与合成器的 `scroll_axes` 对称 | 现有 `seat-selftest`（不覆盖 leave/re-enter 序列，真实影响为零） |
| L25 CNB 产物 | 默认流水线改为托管 Ubuntu 24.04 的 build-only：安装编译依赖、构建库/demo/示例/test 客户端，打包后由 `cnbcool/attachments` 上传到 commit 附件；完整 `ci-all` 仍走本地/自托管，`CI_LOG_DIR` 与 `check-cef-pin.sh` 保留给完整门禁 | 本地 `check-cef-pin.sh`、`CI_LOG_DIR=ci-logs ./test/ci-all.sh unit`；CNB build-only 待托管节点跑一次 |
| 门禁基建 | `ci-all` 的 0 断言判 FAIL；内层 `timeout 90` 之和纳入外层预算；`audit-map-arms.py` / `audit-loop-sources.py` 加空跑下限；`ci-session.sh` 把 bus 地址落盘 | 直接运行 |
| `fetch-cef.sh` 无校验和 | 下载并校验 CEF 构建服务发布的 `.sha256` | 手工核对 CDN 提供该文件 |

### 仍未修/记录

M18 已闭环到可接受程度：9 个 gate 脚本改为 `source test/helper-path.sh`，
按 `Release/` → 单配置 → `Debug/` 的顺序找 helper，`CHROMIUM_HELPER` 仍然优先；
`meson.options` 的默认值仍是 `helper/build/Release`，但那只是可配置默认值。
代码侧没有仍未修的条目。L15、R4、R5 没有门禁，因为分别需要真实拖放、
构造 EMFILE/永久失败、以及覆盖 leave/re-enter 的轴序列；L25 的附件上传只能在
CNB runner 上验证。

### 验证状态

- `meson test -C builddir`：27/27 通过（新增 `seat-selftest-popup-cycle`、
  `seat-selftest-popup-overflow`）。
- `./test/ci-all.sh unit menu`：`unit` PASS 27 断言；`menu` 在 headless 下
  `SKIP`（1s，`exit 77`），总退出 0。
- `./test/ci-all.sh proof primary pixel`：PASS 7 / 2 / 12。
- `./test/ci-all.sh framing multi`：PASS 13 / 10。
- `./test/ci-all.sh download`：PASS 54。
- `./test/ci-session.sh ./test/permission-gate.sh`、`./test/dnd-gate.sh`：通过
  （permission 的 dismiss 路径、dnd 的拖入拖出都覆盖到了）。
- `test/menu-gate.sh`：当前 headless mutter 上 `gtk_popover_popup()` 不返回（菜单
  9 行已经打印后卡住），因此在 `CEFNEST_HEADLESS=1` 下整体 `exit 77`，
  `ci-all` 报 SKIP（1s）；`run_demo` 另有 90s 看门狗作为真桌面兜底。
- 同一份 `test/menu-gate.sh` 在真实 Wayland 桌面（`WAYLAND_DISPLAY=wayland-0`）
  复跑：8 跑、**52 条 ok、0 skip、0 fail**。这也验证了 `menu_popover` 的旧菜单
  清理改动没有破坏真实弹层。
- 变异：`popup-cycle` 去掉环检查 → FAIL；`framing` 的 eof 段退回“只 mark_closed、
  不 drop” → 201 CPU ticks → FAIL。

## 第四轮：2026-10-23 报告与处置

- 报告基线 `358dcfa`，当时 HEAD `b011d39`；本轮的改动都在工作树上，**尚未提交**。
- 报告的两处落点都成立：旧记录“已闭环”的 14 项里 **S5 是虚报**（helper 全文与全部 git
  历史里没有任何 `signal`/`sigaction`），**M22 只剩文档残留**（代码与门禁都对，头文件还在
  宣称 tab 关不掉）。新风险集中在发布管线与示例。
- 范围按“全量”处置：P1–P4、M-a…M-g、M22 与轻微项逐条见表。量不到的写在“本轮量不到的”
  一节里，不含糊带过。

### 严重项

| 条目 | 处置 | 判据 |
| --- | --- | --- |
| **P1** `cefnest-build.tar.gz` 只有库、版本还是 `0.0.1`，Release 说明却写“库 + demo + 全部示例 + helper，可整体解包运行”；同一个 Release 里 deb 是 `154.0.34-4`（`scripts/ci-build.sh`） | 构建树配置补 `-Dcefnest-version=$PACKAGE_VERSION`；产物树按 `src/`、`examples/`、`test/` 三层打包，正好对上二进制里的 `$ORIGIN` 推法；示例个数由脚本数出来填进说明；中英文两版说明都删掉“helper 也在这个包里”的暗示，明写**不含 helper、不含 CEF 运行时**，要渲染页面就装 `cefnest-helper` 或用 `CHROMIUM_HELPER` | 各配置一次对比 `build.ninja` 里的产物名：`-Dcefnest-version=154.0.34` → `libcefnest.so.154.0.34`，不设 → `libcefnest.so.0.0.1`。这一条就是报告里“两个版本串”的成因 |
| **P2** helper 包在 Ubuntu 24.04 默认策略下大概率起不来：`chrome-sandbox` 以 0755 打包、无 postinst、无 AppArmor profile，而沙箱默认开；流水线只构建不安装 | 不走 `chmod 4755`（没量过，而且是给全系统加一个 root SUID 目标）。文档五处写清后果与两条出路（`README.md`、`README.zh-CN.md`、`docs/guides/packaging.md` §1、`docs/internals/sandbox-policy.md` §6、`docs/known-limitations.md`），Ubuntu 那句按发行说明对齐成 **23.10 引入但默认关、24.04 默认开**；`.cnb.yml` 加第三个 stage `install and smoke the packages` 跑 `scripts/ci-smoke-packages.sh`：真 `apt install` 三个 deb → 对装出来的 `libcefnest.so.0` 与安装路径上的 helper 跑 `ldd` → 不带显示起一次 helper 读横幅，`sandbox=on` 必须在，出现 `No usable sandbox!` 即红 | 本机以 `CI_SMOKE_SKIP_INSTALL=1` 跑通 inspect 半边：**19 条 ok、0 FAIL**；安装半边需要 root，本机不是，容器也复现不了别人机器上的 AppArmor 策略——所以文档写的是“后果 + 两条出路”，不是“已测通” |
| **P3-1** `examples/browser.c:53-57` 把 zoom level 当线性百分比 | 显示改 `(int)(100.0 * pow(1.2, level) + 0.5)`，`examples/meson.build` 给示例补 `m_dep`；`docs/features/zoom-path.md` 新增换算一节并标明是人工测量 | 嵌套会话里读 `window.devicePixelRatio`：level `0` → `1`，`0.5` → `1.0954451560974121`，`1.0` → `1.2000000476837158`，即因子 `1.2^level` |
| **P3-2** `examples/menus.c:121-133` 把 popover 挂到 GtkWindow，而 `::context-menu` 的 x/y 是 view 内坐标 | parent 改成本次事件的 view（新增 `menus.view`，窗口销毁时置 NULL） | `gtk_popover_set_pointing_to()` 的矩形按父控件自身坐标系读（`/usr/share/gir-1.0/Gtk-4.0.gir` 原文）；仓库内 `examples/windows.c` 早已 parent 到 active view，是同树范本 |
| **P4** S5 信号处理从未实现（报告虚报） | `helper/src/main.cpp`：`sigaction` 装 TERM/INT/HUP；处理函数只往 `pipe2` 写一字节（async-signal-safe，`CefPostTask` 会取锁，不能在信号里调）；watcher 线程把字节 hop 到 UI 线程后 `CloseEveryBrowser()`，由 `OnBeforeClose` 收尾 → `CefShutdown()` → main 尾部 | 新门禁 `test/helper-signal-gate.sh`（`ci-all` 新增 `signal` 行，**12 条断言**）：`/proc/<pid>/status` 的 `SigCgt` 证明装上了、日志有 `caught signal 15`、走到 `helper: exited`、profile 的 `SingletonLock` 被释放、宿主侧没看到信号死。**第一版门禁当场抓到第二个 bug**：helper 自己宣布 closed 后，宿主结束运行、dispose 里 SIGKILL 孩子，于是死在 `CefShutdown()` 中途——`src/chromium-session.c` 现在不对“自己说要走”的 helper 动手 |
| **M22** `src/chromium-session.h:48-53` 还在宣称 tab 关闭不发 `::browser-closed` | 重写：tab 与独立窗口都会发（helper 释放 slot 的 view 引用），未知 id 回 `status: no-such-browser`，指向 `docs/features/multi-browser.md` 与 `test/multi-browser-gate.sh` | `multi` 门禁 10 条断言含 `close-browser 2`；`docs/guides/minimal-host.md` §5 把“曾经关不掉”的三方式测量留着并标注已修 |

### 中等项

| 条目 | 处置 | 判据 |
| --- | --- | --- |
| **M-a** `check-cef-pin.sh` 比的是同一次 fetch 写出的两个文件，漂移检不出来 | 改成三方比：`fetch-cef.sh` 里的 `PINNED`、`dist/.cef-version`、`dist/include/cef_version.h`；`PINNED` 行抓不到就直接失败（检查丢了对象不算通过）。故意 `CEF_VERSION=latest` 的走 `CI_SKIP_HELPER_BUILD=1`，不在这里加开关 | 造 `/tmp/pinroot`：只把 `PINNED` 改成 153、`third_party` 软链真目录 → 红，且三行 drift 一起打出来；真树绿 |
| **M-b** RELEASE-NOTES 说“只调库可不装 helper”，同篇末尾和 deb 的 `Depends: cefnest-helper (= …)` 又说必装 | 说明与 control/RPM 描述统一成分层硬依赖（helper 在最底层，库依赖它，dev/demo 依赖库），删掉“只调库”那句；四个包的 control 由同一个 `deb_control()` 写，字段不会各说各话 | 本轮重跑 deb 半边后 `dpkg-deb -f` 逐个看 Version/Depends |
| **M-c** `examples/downloads.c:100-103` 的 999 次碰撞 fallback：内联 `g_strdup_printf` 交给不接所有权的 `g_build_filename`，每次泄漏；落名 `photo.jpg-1698…` 丢扩展名 | 先生成拥有权的字符串再拼路径；改 `stem-<时间戳>.ext`（仓库范本 `src/main.c:857`） | 现有 `download` 门禁 54 条 |
| **M-d** 10 个新示例用“ref 且永不释放”，`minimal`/`useragent` 用“转移所有权”，无解释，混抄即 use-after-free | 11 个示例各加同一段四行注释（窗口一份负责窗口销毁，文件全局一份负责处理器永远对着活的 session；`minimal.c` 没有全局，所以把唯一那份交给窗口），`docs/guides/minimal-host.md` 加同一条规则段 | 代码审查 + `ninja` 全量重建 |
| **M-e** 摘要求来自同一 CDN 同一信道，只防损坏不防源被替换 | `fetch-cef.sh` 按架构把 `154.0.34` 两个归档的 sha256 钉进脚本；请求版本等于 `PINNED` 时必须等于仓库里的值才 `tar -xj`，否则删档退出。注释里写明这仍是 trust-on-first-use——两个初值本身是从构建服务取的 | 造一棵 stub `curl` 服务“自洽但非批准”的归档+摘要：CDN 那关通过、pinned 那关拦下，rc=1 且不落 `dist/` |
| **M-f** `rpmbuild` 存在性检查排在 1.5 GB deb 全部构建完之后 | 并入开头 `for tool in meson ninja dpkg-deb rpmbuild` 前置检查（本机无 rpm 时第一步就退出，不再白干） | 本轮本地跑就是靠这个顺序：deb 段跑完才在 RPM 段停住 |
| **M-g** `SHA256SUMS` 自称“全部产物”却不含同批上传的 tarball | `CEFNEST_EXTRA_ARTIFACTS` 把 `cefnest-build.tar.gz` 移进 `dist/` 再一起 `sha256sum`；名字不存在直接失败，不静默跳过 | 冒烟 job 的 `the manifest covers what the pipeline uploads`：清单既要能验，也必须列有 `cefnest-build.tar.gz` |

### 轻微项

| 条目 | 处置 | 判据 |
| --- | --- | --- |
| `examples/windows.c:171-174` paint stack 未释放 | `primary` 与 `stack` 两条列表都 `g_list_free_full(…, g_object_unref)`，长度取自 `primary` | `primary` 门禁 2 条 |
| `examples/findbar.c:107-110` Escape 无条件吞掉 | 只在 revealer 打开时吞，关掉后交回页面 | 现有 `find` 门禁 34 条 |
| `examples/browser.c:215-221` mute 图标从不随状态变 | `refresh()` 按 `chromium_session_get_muted()` 换图标 + tooltip + active；`on_mute_toggled()` 加回声守卫，避免 setter 再触发一次 toggled | 代码审查（无门禁） |
| `examples/menus.c:26` 注释写 0=none，实际置 -1 | 该字段换成 `gboolean pending`，注释与取值一致 | 代码审查 |
| 任意 tag 都会建 Release 并 `latest: true` | 第一个 stage `check the release strings` 比 `PINNED` / `CEFNEST_PACKAGE_RELEASE` / `CEFNEST_EXPECTED_TAG` / 附件清单里 9 个带版本的名字（`.cnb.yml` 里共 **13 个位置**要一起动），并顺带比 `git tag --points-at HEAD`（runner 里有 tag ref 才比） | 本地跑绿；改一个附件名为 `-3` → 红；`CEFNEST_EXPECTED_TAG` 写错 → 红。`docs/testing/cnb-ci.md` §5 同时写明残留边界：`git:release` 用的是**实际推上来的那个 tag**，ref 不在 runner checkout 里时这一步量不到 |
| RPM 手写依赖只有 glibc，`%global __os_install_post %{nil}` 会不会连 `find-requires` 一起关掉，未验证 | 不改行为，只把不确定性写进 `docs/testing/cnb-ci.md` §2；冒烟 job 打印 `cefnest-helper rpm --requires` 而**不做断言**（红绿都不该由猜决定） | 本机无 `rpmbuild`，仍为零测量——已在文档里点明第一次 CI 跑完要补回这一段 |
| 四个 deb 没有 copyright | `deb_control()` 为四个包各写 machine-readable copyright，落在**数据层** `/usr/share/doc/<包名>/copyright`；helper 额外一段 `Files: usr/libexec/cefnest/*` = `BSD-3-clause`，并把 CEF 分发的 `LICENSE.txt` 装到 `/usr/share/doc/cefnest-helper/LICENSE.cef.txt`（缺文件就报错退出）。RPM 侧用 spec 的 `License:` 字段，helper 子包覆写成 `LGPL-2.1-only and BSD-3-Clause` 并打包同一份 CEF 文本 | `lintian`：改前 `E: no-copyright-file` + `W: unknown-control-file [copyright]`，改后这几个标签全清；冒烟 job 新增 4 条 + 1 条 helper BSD 段断言，变异（退回 `DEBIAN/copyright`）→ 红。**踩过的坑**：第一版就是写在 `DEBIAN/copyright`，看着像有了，dpkg 根本不安装这个成员 |

### 本轮量不到的（如实记）

- `meson test` 里 `protocol / impl-slot-audit` 是**红的**，且与本轮无关：`src/nested.c`
  的 `shm_impl` 只填 `.create_pool`，本机 `wayland-server` 是 1.23.1，`wl_shm` 在版本 2
  多了 `release`，于是审计报 `MISSING … slot 1 = release since 2`；CI 的 Ubuntu 24.04 带
  1.22，那个槽位还不存在，所以这条只在 Debian 13 这类新头文件宿主上红。`shm_impl` 与
  审计脚本都不在本轮改动里（`git diff` 可查），报告的原样留着：客户端按 v2 绑定后发
  `release`，libwayland-server 会当场 abort 合成器。修它要在 compositor 核心动 6 行，
  不在本轮范围，等决定。
- helper 有一条**先前就存在的偶发 `*** stack smashing detected ***`**：在改动前的旧二进制上
  同样能复现，本轮未定位。
- RPM 分支本机零测量（无 `rpmbuild`）；`License:` 覆写、`%files` 新行、`Requires` 自动发现
  都要等 CI。
- 装包 + `ldd` + 起 helper 那半边只会在 CI/root 容器里真跑；AppArmor 限制宿主复现不了。

### 验证状态

- `meson test -C builddir`：**26 ok / 1 fail**（唯一失败是上面那条 `impl-slot-audit`）。
- `./test/ci-all.sh`：`proof 7`、`framing 13`、`pixel 12`、`find 34`、`permission 58`、
  `download 54`、`dnd 59`、`primary 2`、`multi 10`、`signal 12`、`soak 12` 全 PASS；
  `menu` SKIP（headless mutter 不映射 popover，`exit 77`，同第三轮）；`unit` FAIL，
  失败项就是同一条 `impl-slot-audit`。总退出码非 0。
- `scripts/build-packages.sh`：deb 半边本地重跑产出四个包（RPM 半边被 stub 的 meson 停在
  配置步，本机无 `rpmbuild`）。`dpkg-deb -e`/`-c` 复核：四个包都有
  `usr/share/doc/<包>/copyright`，helper 带 `LICENSE.cef.txt`，`chrome-sandbox` 仍是
  `-rwxr-xr-x`。
- `scripts/ci-smoke-packages.sh`（`CI_SMOKE_SKIP_INSTALL=1`）：19 ok / 0 fail / 1 note。
- `scripts/check-release-tag.sh`：绿；两处变异（附件名改 `-3`、`CEFNEST_EXPECTED_TAG` 写错）都红。
- `scripts/check-cef-pin.sh`：真树绿，`/tmp/pinroot` 变异红。
- `scripts/fetch-cef.sh`：stub CDN 变异红（不解包）。
- P1 的版本串：两次 `meson setup` 对比 `build.ninja`（`154.0.34` vs `0.0.1`）。
- P3-1 的换算：三个 dpr 值，人工测量。
- 本轮所有改动仍未提交。
