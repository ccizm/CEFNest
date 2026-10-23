# 协议天花板：把没广告的协议和版本上限逐个判掉

日期：2026-10-20
问题：任务 #28。嵌套 compositor 只广告 11 个全局对象（有渲染节点时再加
`zwp_linux_dmabuf_v1`，共 12 个），版本各自封顶（`src/nested-internal.h`）。
这份记录回答两个问题：**客户端在我们要的东西，我们给了没有？客户端在别的
compositor 上还要的东西，我们不给行不行？**
每一项都有量出来的证据，不是看协议文档猜的。

> 2026-10-23 重量一遍：`wl_shm` 的广告版本从 1 抬到 2（`docs/internals/empty-request-slot.md` §9）。
> 本文 §1 的归属判据和 §2 的两张表都按新测量重写，顺带补上旧表漏掉的两个
> 真实 bind。客户端**要**的版本没变，变的只是它能要的上限。

## 1. 量法

两跑，各回答一半：

```bash
# 跑 A：真 Chromium（helper）连我们 + demo 的 GDK 连 mutter，同一份日志
timeout 30 env GDK_BACKEND=wayland CHROMIUM_HELPER=1 WAYLAND_DEBUG=1 \
  CHROMIUM_SESSION_SCRIPT=/tmp/probe.session ./builddir/src/cefnest-demo > /tmp/probe.log 2>&1

# 跑 B：seat-selftest 本体的 GDK 连的是**外层** display（它要把合成结果画进
# GdkTexture，见 test/seat-selftest.c:1693 的 gtk_init_check）；只有 --gtk-client
# 派出的第二个进程被 start_gtk_client() 把 WAYLAND_DISPLAY 改成我们的 socket。
# 所以这份日志里有两份 GDK 连接，得按下面的指纹分。
WAYLAND_DEBUG=1 ./builddir/test/seat-selftest /tmp/x.png \
  "$PWD/builddir/test/shm-client" --gtk-client > /tmp/gtk-client-bind.log 2>&1
```

归属判据：我们的 compositor 按固定顺序建全局，名字恰好是 1–12，版本也是
我们广告的版本，其中最靠得住的指纹是 `wl_compositor`——我们封顶 **4**，mutter 48.7
给的是 **6**，两个注册表一眼分开。名字位置不能当判据：`gtk_shell1` 在宿主机那份
注册表里是 11 号，在 headless 嵌套 mutter 里是 10 号，因为它前面的全局有多少
`wl_output` 会变。

> 这里原来还有第二条判据："跑 B 里 `gtk_shell1`、`wp_presentation` 等 mutter 独有
> 协议的 bind 行数为 **0**，证明这份日志是纯'客户端对我们'的测量"。**重量时它不
> 成立**：重跑上面那条命令，日志前半段就有这么一行
> `-> wl_registry#2.bind(10, "gtk_shell1", 6, new id [unknown]#11)`，
> 那是 selftest 本体连外层 mutter 的那份 GDK，不是脏数据。旧结论只是恰好被"两份
> 连接的 `wl_compositor` 版本号不同"救了。判据换成上面那条指纹，或者只看我们
> compositor 服务端打印的、不带 `->` 的行——§2 两张表这次就是这么摘的。

## 2. 客户端对我们：要的 exactly 是我们有的

2026-10-23 在 `./test/ci-session.sh` 的 headless 嵌套 mutter 里重跑两遍量出来的。
两张表都只取我们 compositor **服务端**打印的请求（不带 `->` 的那些），按
`wl_compositor 4` 认领连接。

跑 B 里那个被强制连我们的 GTK4/GDK 客户端，绑了 12 个全局中的 11 个：

```
wl_compositor 4, wl_shm 1, wl_output 3, xdg_wm_base 3, wp_viewporter 1,
wp_fractional_scale_manager_v1 1, wl_seat 8, wl_data_device_manager 3,
zwp_primary_selection_device_manager_v1 1, wp_cursor_shape_manager_v1 1,
zwp_linux_dmabuf_v1 5
```

没绑的只有 `zwp_text_input_manager_v3`——它要等一个有焦点的输入区，而这个客户端
不映射窗口。旧表把它列进去了、却漏了 `wl_output` 和 `zwp_primary_selection_...`，
那两条正是本次重量的差别（`--as-gtk-client` 现在压根不建窗口）。

跑 A 里真 Chromium 起了**两条**连接（browser 进程和它的子进程），旧表把两条混成了
一句"同上"：

```
连接 1：上面 11 个全绑，再加 zwp_text_input_manager_v3 1，共 12 个；
        wl_output 绑 4（我们封顶 4），dmabuf 绑 4（我们封顶 5，它只要 4）
连接 2：绑 11 个，同样不碰 text_input；wl_output 绑 3、dmabuf 绑 5；
        xdg_wm_base 要到 create_pool 之后 330 ms 才补 bind
```

dmabuf 两边都只收到 `modifier` 事件、没有那条已废弃的 `format`——`version < 4` 才
发它（`src/nested-dmabuf.c:304`），所以绑 4 和绑 5 拿到的格式信息是一样的。

两条连接的 `wl_shm` 都**只要版本 1**，尽管全局现在广告 2。这条对本次改动要紧：
抬广告版本并没有让真实客户端开始发 `release`（它们连要都没要），所以
`docs/internals/empty-request-slot.md` §9.1 说的"日常可达的那条路是 1.22 表头的
`wl_shm_destroy()`"没有被这次测量推翻——槽 1 该填还是得填。

结论：

- 两个真实客户端（CEF Chromium、GDK4）对我们的 bind **一个不多、一个不少**，
  全部落在我们广告的全局里（有渲染节点时 12 个，没有时 11 个——dmabuf 是条件广告，
  `src/nested.c:2645`）。
- 两边都没有任何 missing-global 抱怨；跑 B exit 0（GDK 客户端跑完整个脚本
  序列没崩），跑 A 的 Chromium 页面渲染、滚轮、剪贴板都是已验证的黄金路径
  （`docs/features/wheel-resolution-and-key-repeat.md`、`docs/features/clipboard-bridge.md`）。
- GDK 的设计是"有就绑、没有就不用"：它在 mutter 上绑的那一打协议（见 §3）
  到我们这里一行 bind 都没有发过——因为没广告，它连问都没问。缺协议不会
  让客户端失败，只会让它少一个增强。

## 3. 没广告的协议：逐个判掉

跑 A 里 demo 的 GDK 连 mutter 时还绑了下面这些（我们从来没广告过它们）。
按"广告了就要实现每个请求、发每个事件"的纪律（`docs/internals/empty-request-slot.md`），
多广告一个就是要多养一份服务端实现，所以每项都要有理由：

| 协议 | 判决 | 理由 |
|---|---|---|
| `gtk_shell1` 6 | 永不 | mutter 私有，规范上就不该出现在别的 compositor |
| `wl_subcompositor` 1 | 不给 | 要整个 subsurface 树合成；Chromium 对我们从未绑它，弹窗需求已由 popup 合成路径（`docs/internals/popup-compositing.md`）覆盖 |
| `wp_presentation` 1 | 不给 | 帧时序反馈；我们自己控帧（`docs/internals/frame-clock-pacing.md`），客户端没有可反馈的对象 |
| `wp_single_pixel_buffer_manager_v1` 1 | 不给 | GDK 用来画 1×1 纯色面；对我们没有任何客户端要过 |
| `xdg_activation_v1` | 不给 | 跨窗口焦点传递令牌；我们只有一个 toplevel |
| `xdg_system_bell_v1` / `xdg_dialog_v1` | 不给 | 桌面装饰类，嵌套 compositor 无从谈起 |
| `zxdg_output_manager_v1` 3 | 不给 | 逻辑输出布局/缩放信息；`wl_output` 4 的 geometry/name/description 已覆盖客户端所需 |
| `zwp_idle_inhibit_manager_v1` | 不给 | 我们根本没有"空闲"可抑制——嵌套 compositor 不做任何屏保/锁屏管理，抑制一个不存在的东西没有意义 |
| `zwp_keyboard_shortcuts_inhibit_manager_v1` | 不给 | 同上：我们没有快捷键系统可抑制 |
| `zwp_pointer_gestures_v1` 3 | 暂不给 | 触控板捏合缩放。真 Chromium 会绑它，损失是页面里的捏合缩放不可用；补齐要捏合手势→Chromium 的整条链路，等有触控板需求再议 |
| `zwp_tablet_manager_v2` | 暂不给 | 数位板。损失同上，等需求 |
| `zwp_primary_selection_device_manager_v1` 1 | **给**（2026-10-21） | 中键主选区：独立的 source/device/offer，文本双向桥到 GTK primary clipboard。普通 `wl_data_device` 剪贴板仍是另一条通道；两者不互相替代 |
| `zxdg_exporter_v1/v2`、`zxdg_importer_v1/v2` | 不给 | xdg-foreign，GDK 在 mutter 上做窗口定位/激活握手用；单一 toplevel 用不上 |
| `wl_drm` 2 | 永不 | 遗留 dmabuf 协议，`zwp_linux_dmabuf_v1` 5 已是正身 |

"暂不给"三项是真实的已知损失，都记在 `docs/known-limitations.md`；
判据写死：**有客户端对我们发出第一次 bind 请求（即我们先广告了才可能发生），
或者用户报了对应场景不能用，再回来补。**

## 4. 版本封顶：三个候选，全部不升

升版本同样受纪律约束——新版本带来的每个请求都要实现、每个事件都要发。
对照 `/usr/share/wayland/wayland.xml` 和 `xdg-shell.xml` 的 since 标注逐个看过：

- **`wl_compositor` 4 → 6？** 不升。compositor 4 下 `wl_surface` 已到 4，
  **`damage_buffer`（since 4）客户端现在就能用**——升到 5/6 新增的只有
  `wl_surface.offset`（请求）和 `preferred_buffer_scale` /
  `preferred_buffer_transform`（事件）。前两个是 subsurface 定位用的，
  我们没有 subsurface；后两个要求我们表态"偏好缩放"，而这件事已经由
  `wp_fractional_scale_manager_v1` 的 `preferred_scale` 承担，再发一份
  等于引入第二个真相来源。Chromium 在真机上绑 6 只是"能多拿就多拿"，
  对我们绑 4 跑完整黄金路径，没有量出任何缺失。
- **`xdg_wm_base` 3 → 5？** 不升。3→4 加 `xdg_toplevel.configure_bounds`
  事件、4→5 加 `wm_capabilities` 事件，都是服务端必须会发的新事件；
  `wm_capabilities` 要求我们声明最大化/最小化/全屏能力，而我们的 toplevel
  状态机只到"能配尺寸"这一层。Chromium 绑 3 工作正常，等状态机真的支持
  maximize/fullscreen 时一起升。
- **`wl_seat` 8 → 9？** 不升。9 新增 `wl_pointer.axis_relative_direction`
  事件（自然滚动方向逐事件标记）。我们封顶 8，Chromium 绑 8，
  `axis_value120` 的高分辨率滚轮已验证（`docs/features/wheel-resolution-and-key-repeat.md`）。
  发这个事件需要逐事件判断滚动方向，做错比不发更糟。

## 5. 怎么复核

```bash
WAYLAND_DEBUG=1 ./builddir/test/seat-selftest /tmp/x.png \
  "$PWD/builddir/test/shm-client" --gtk-client 2>&1 \
  | grep -o 'bind([0-9]*, "[a-z_0-9]*", [0-9]*' | sort -u
```

期望输出只有 §2 那 10 个（GTK 不绑 `wl_output`）。出现第 12 个接口名 =
有人先广告了没实现的东西，停，回来查。
