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
