# nested 拆分记录

日期：2026-10-20
前置版本：`src/nested.c` 单文件 5030 行（备份：`/tmp/nested.c.pre-split.bak`，确认稳定后可删）
拆分后：7 个文件，共 5192 行，编译零新增告警，seat-selftest 全流程通过且渲染输出与拆分前一致。

## 1. 背景与动机

`nested.c` 最初是单文件合成器。Gate 1 通过后进入 Gate 2，文件在两天内从 4404 行
涨到 5030 行（text-input-v3 一项就 +626 行），并且还在快速增长（cursor-shape、
popup 定位都在排队）。单文件装了七个本可独立的协议域，审读剪贴板或 seat 逻辑时
其余四千多行全是噪音，合并冲突也会全部集中在一点。

拆分窗口选在 text-input 刚落地、cursor-shape/popup 未动工之前：再晚，新功能又会
堆进既有分区。

## 2. 文件布局

行数为当前值（frame-clock 与 cursor-shape 落地之后），职责列随每次新增协议更新：

| 文件 | 行数 | 职责 |
|---|---|---|
| `src/nested.c` | 1814 | 核心：NestedSurface（wl_surface/commit 语义）、wl_shm/pool/buffer、帧回调、wp_viewporter、wp_fractional_scale_v1、NestedCompositor 生命周期（socket/globals/frame clock/dispose/公开 API） |
| `src/nested-internal.h` | 278 | 私有共享头：跨模块类型 + 跨模块函数声明。不是公开 API——外部消费者仍然只用 `nested.h` |
| `src/nested-seat.c` | 711 | wl_seat/wl_pointer/wl_keyboard、xkb keymap 编译与下发、输入注入公开 API（`nested_compositor_notify_*`、`set_*_active`）、`seat_retarget`/`seat_clear_focus`、enter 序号记账（供 cursor 校验） |
| `src/nested-data-device.c` | 846 | wl_data_device_manager 剪贴板桥（仅文本、双向、4MB 上限）、`nested_compositor_set_clipboard`、`data_device_shutdown` |
| `src/nested-text-input.c` | 604 | zwp_text_input_v3 协议端（双缓冲状态、commit 计数序号、密码字段屏蔽）+ 控件侧公开 API（preedit/commit/delete-surrounding、字节↔字符偏移换算） |
| `src/nested-dmabuf.c` | 584 | gbm 渲染节点探测、zwp_linux_dmabuf_v1（v5 + feedback 格式表）、buffer params、dmabuf 纹理导入 |
| `src/nested-xdg.c` | 522 | xdg_wm_base/xdg_surface/xdg_toplevel/xdg_popup、wl_output、`xdg_surface_configure`、`output_send_state_all` |
| `src/nested-cursor.c` | 362 | wp_cursor_shape_manager_v1（CSS 形状 → GDK 光标名）、wl_pointer.set_cursor（客户端自带光标图 → `gdk_cursor_new_from_texture`，等 surface commit 生效）、`nested_compositor_set_cursor_target` |

模块依赖方向（单向，核心被依赖，从不反向）：

```
            nested-internal.h（类型 + 声明）
                     ▲
   ┌─────────┬───────┼───────┬──────────┬───────────┬──────────┐
nested.c   seat   data-device dmabuf  text-input    xdg      cursor
（核心）     │        │         │          │         │         │
   ▲        │        │         │          │         │         │
   └── surface_commit/destroy 会在关键路径上调用
       seat_clear_focus / text_input_forget_surface /
       cursor_surface_committed / xdg_surface_configure /
       nested_update_primary
   cursor ── seat_resource_is_pointer / seat_pointer_serial_is_current
```

## 3. nested-internal.h 里放了什么

**完整结构定义**（多个模块按字段访问，必须共享）：

- `DmabufFormat`、`Pool`、`Buffer`、`BufferUser`——buffer 被 surface（commit 路径）
  和 dmabuf（params 创建）两边操作；
- `struct _NestedSurface`、`struct _NestedCompositor`——所有模块都按字段访问。

**仅前向声明的 typedef**（compositor 结构里只是指针，完整定义留在各自文件）：

- `TextInput`（→ nested-text-input.c）、`DataSource`、`ClipboardTransfer`
  （→ nested-data-device.c）。

**留在各文件私有的类型**：`SeatClient`（seat）、`Params`（dmabuf）、
`OutputResource`（xdg）、`Viewport`/`FractionalScale`/`FrameCallback`（核心）、
`DataOffer`/`DataDevice`（data-device）、`TextInputState`（text-input）。

**宏**：版本上限全部集中到头文件——`NESTED_SEAT_VERSION`、`NESTED_DMABUF_VERSION`、
`NESTED_DMABUF_MAX_PLANES`（Buffer 数组尺寸，必须共享）、
`NESTED_DATA_DEVICE_MANAGER_VERSION`、`NESTED_TEXT_INPUT_VERSION`、
`NESTED_CURSOR_SHAPE_VERSION`。
`NESTED_SEAT_NAME`/`NESTED_POINTER_VERSION`/`NESTED_KEYBOARD_VERSION` 只有 seat 用，
留在 nested-seat.c。

## 4. 跨模块接缝（拆分引入的全部新代码）

除了搬移，只新增了六个小函数。行为与拆分前逐字等价，作用是把"不得不跨文件"的
引用收敛成有名字的接缝：

| 接缝 | 定义处 | 作用 |
|---|---|---|
| `buffer_resource_new(client, buffer, id)` | nested.c | wl_buffer 的 impl 和销毁回调留在核心；dmabuf 模块建资源时走这里（原 params_create_buffer 里的三行原地包装） |
| `nested_notify_text_input_state(self)` | nested.c | `text-input-state` 信号的 ID 注册在核心 class_init 里；text-input 模块借此发信号，不接触 `signals[]` 静态数组 |
| `seat_resource_is_seat(resource)` | nested-seat.c | `wl_resource_instance_of(…, &seat_impl)` 的封装；text-input 的 get_text_input 校验 seat 参数用，seat_impl 保持文件私有 |
| `data_device_shutdown(self)` | nested-data-device.c | dispose 路径：取消在飞的剪贴板传输并将其 compositor 指针置空（原 dispose 内联代码） |
| `nested_shutdown_dmabuf(self)` | nested-dmabuf.c | dispose 路径：销毁 gbm 设备（核心不再需要 gbm.h） |
| `output_send_state_all(self)` | nested-xdg.c | 遍历 outputs 列表逐个 `output_send_state`；`nested_compositor_set_size` 改为调用它，OutputResource 不必进头文件 |

小函数搬家（非新增）：`nested_next_serial`/`nested_now_msec` 从核心移到 seat 文件
（全项目只有输入注入用它们；核心的 frame clock 直接读 `g_get_monotonic_time()`）。

### 4.1 拆分之后新增的接缝（cursor-shape）

新协议按 §9 的约定单开 `nested-cursor.c`，沿用同一做法——跨文件引用一律收敛成
有名字的函数，`pointer_impl`、`CursorDevice` 之类继续留在各自文件里：

| 接缝 | 定义处 | 作用 |
|---|---|---|
| `seat_resource_is_pointer(resource)` | nested-seat.c | `get_pointer` 的参数必须真是本 compositor 发出的 wl_pointer（否则 `post_wrong_object`） |
| `seat_pointer_serial_is_current(pointer, serial)` | nested-seat.c | `set_shape`/`set_cursor` 只认当前那次 enter 的序号；抢在 leave 之后属正常竞态，静默忽略而非报错 |
| `cursor_surface_committed(self, surface)` | nested-cursor.c | 核心 commit 路径回调：`wl_pointer.set_cursor` 点名的 surface 要到 commit 才真正换光标 |
| `cursor_reset(self)` | nested-cursor.c | 指针焦点变化、或换 target surface 时收回客户端光标，回到控件自己的指针 |

## 5. 链接属性改动

- 头文件声明的跨模块函数全部非 static：
  核心的 `nested_flush`、`nested_update_primary`、`post_wrong_object`、
  `surface_from_resource`；各模块的 `*_bind`、`seat_retarget`、`seat_clear_focus`、
  `nested_setup_keymap`、`nested_setup_dmabuf`、`nested_shutdown_dmabuf`、
  `buffer_create_dmabuf_texture`、`xdg_surface_configure`、`output_send_state_all`、
  `text_input_retarget`、`text_input_forget_surface`、`data_device_shutdown`。
- 原文件头部"Interface implementations, declared up front"的整块前向声明按归属
  拆散：核心保留 compositor/region/surface/shm/shm_pool/buffer/viewporter/viewport/
  fractional 九个，seat 文件补 seat/pointer/keyboard 三个，xdg 文件补
  xdg_surface/xdg_toplevel/xdg_popup 三个。其余模块的 impl 前向声明原本就在各自
  分节内，随搬移自动就位。
- 头文件里的系统包含按模块裁剪：核心去掉了 `drm_fourcc.h`/`gbm.h`/`xkbcommon`/
  `sys/mman.h`/`fcntl.h`；dmabuf、seat、data-device 各自补齐了 `fcntl.h`、
  `sys/mman.h`、`glib-unix.h` 等。所有文件继续用 `G_LOG_DOMAIN "nested"`，
  日志格式不变。

## 6. 行为保持性

- 协议处理代码是**逐字搬移**（从拆分前备份按行提取），没有任何逻辑编辑；
- 上表六个接缝函数是仅有的新代码，且都是对既有内联代码的原地包装；
- dispose 的两处替换（`data_device_shutdown`、`nested_shutdown_dmabuf`）与原
  内联语句逐条对应；`nested_compositor_set_size` 的 outputs 循环原样移入
  `output_send_state_all`；
- 公开头文件 `nested.h` 与 `chromium-view.h` 零改动，`chromium-view.c`、
  `chromium-session.c`、demo、测试程序全部不需要任何修改。

## 7. 构建系统

`src/meson.build` 的 `libnested` 源列表从 `['nested.c', …]` 扩为
`['nested.c', 'nested-dmabuf.c', 'nested-seat.c', 'nested-data-device.c',
'nested-xdg.c', 'nested-text-input.c', …]`。依赖、生成的协议源、demo/测试目标
均不变。

cursor-shape 落地时新增的构建项：`staging/cursor-shape/cursor-shape-v1.xml` 的
server header / client header / private-code 三个 target（测试客户端用 client
header，libnested 用 server header + code）。另外该协议的 v2 请求
`get_tablet_tool_v2` 在生成的消息表里引用 `zwp_tablet_tool_v2_interface`，
所以还要把 `stable/tablet/tablet-v2.xml` 的 private-code 一起链入——只用它的
接口描述符，本项目没有任何平板实现，advertise 的版本是 1，这条请求根本到不了。

## 8. 验证

1. **编译**：`ninja` 全量通过。唯一告警是 `nested.c` 中
   `g_source_set_callback(…, (GSourceFunc) nested_dispatch, …)` 的
   `-Wcast-function-type`——拆分前就存在，本次未触碰，保持原样。
2. **行为**：`builddir/test/seat-selftest` 跑通全部 17 步脚本，包括
   pointer enter/motion、button、axis、key 和 text-input 全链路
   （surrounding 上报 → preedit `"nihao"` → commit `"你好"` 双字节用例 →
   `delete_surrounding(-2,2)`），两张导出 PNG 正常。
3. **渲染一致性**：`/tmp/after-split.png` 与拆分前 03:09 的
   `builddir/seat-selftest-current.png` 特征一致（十字准线位置、按键/滚动
   标记、边框颜色、棋盘相位）。

selftest 未覆盖的路径：剪贴板（data-device）与 CEF 嵌套。这两块是原样搬移，
建议下次跑 `clip-copy`/`clip-paste` 冒烟和 `cef-windowed-probe` 的
`run-nested.sh` 时顺带确认。

### 8.1 cursor-shape 落地后的回归

1. `seat-selftest` 17 步全通过（不带 cursor 参数），带 `--dmabuf --text-input`
   同过；`--cursor-shape` 时日志出现 `nested: pointer becomes text`。
2. `--cursor-surface` 走 `wl_pointer.set_cursor`：先 `default`（此刻还没有
   buffer），随后的 commit 再变 `a client image`——正好证明"等 commit 生效"
   这条语义成立。
3. 真 CEF：demo 脚本新增 `hover <x> <y>` 步骤（无头跑也能让页面命中测试），
   依次悬停 `<input>` / `<a>` / `cursor: grab` 的 div，日志依次为
   `text` / `pointer` / `grab`，全部来自 Chromium 的
   `wp_cursor_shape_device_v1.set_shape`。
4. 同批修掉 demo 脚本 `dump <path>` 少切前缀的 bug（把整行当路径写出，
   导致 PNG 从来没写成过）。

## 9. 后续

- 该拆分在无版本控制状态下完成，随后已 `git init` 并以拆分为早期 commit 之一入库。
- 后续新协议按本文件的约定各自开新文件：cursor-shape 已落地为
  `src/nested-cursor.c`（见 §2/§4.1/§8.1），popup positioner、subcompositor
  仍未动工；seat 侧输入类协议继续归 `nested-seat.c` 或仿照 text-input 单开文件，
  不再回到 `nested.c`。
