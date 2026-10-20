# 弹出层合成：xdg_popup 的定位、命中与关闭

日期：2026-10-20
目标：任务 #21。控件此前只画 `primary` 那一个 toplevel，客户端弹出的
`xdg_popup` 表面（右键菜单、`<select>` 下拉、日期选择器、tooltip）虽然被协议接受，
但既不上屏、也收不到输入。现在它们按 positioner 规则定位、叠在页面上绘制、
能被命中、能抢键盘、能被外部点击关掉。

## 1. 三件事

一个 popup 要走通，需要同时解决三件互相牵连的事：

1. **定位**：客户端把规则写进 `xdg_positioner`，合成器算出位置并 `configure` 回去；
2. **绘制与命中**：控件要按 z 序画出一棵树，指针要知道自己压在哪个表面上，
   并且事件坐标要换算到那个表面自己的坐标系；
3. **模态**：`xdg_popup.grab` 是客户端自己声明的"这是模态"，键盘要归它，
   点在它外面要回 `popup_done`。

## 2. 定位（nested-xdg.c）

`xdg_positioner` 不再是空实现：每个请求把值写进 `PositionerRules`（`nested-internal.h`）。
`get_popup` 和每次 `reposition` 都把这份规则**拷贝**到 `NestedSurface.positioner`——
协议允许客户端在发出请求后立刻 destroy positioner，之后的重排只能靠副本。

`place_with()` 是规则的一次求值：

| 步骤 | 做法 |
|---|---|
| anchor | 由 `anchor` 枚举取锚矩形的点；不带边的枚举在该轴取中点 |
| offset | 加到**锚点**上（`set_offset` 的原文语义），不是加到结果上 |
| gravity | 决定子表面往哪个方向展开：`bottom_right` → 位移 0，`top_left` → −size，单边则另一轴居中 |
| 约束 | 优先级按协议：1) flip 2) slide。flip 只在镜像结果"露出更少"时才采纳；**resize 忽略**——只有客户端能按别的尺寸重画 |

`anchor`/`gravity` 是 0..8 的**枚举**，不是位域，所以全部用 `switch` 展开；只有
`constraint_adjustment` 是位域，用位与。

约束盒取**父表面的 window geometry**（与 wlroots 一致）。嵌套合成器没有"屏幕"，
父窗口就是那个 popup 世界的全部边界。

### 一个真实的例子

测试客户端开两个 popup：菜单挂在 toplevel 的锚矩形 `(150,100,30,20)` 上，
`ANCHOR_BOTTOM_LEFT` + `GRAVITY_BOTTOM_RIGHT`；子菜单挂在菜单的 `(200,10,10,20)` 上，
`ANCHOR_TOP_RIGHT` + `GRAVITY_BOTTOM_RIGHT`，两者都允许 slide。

- 菜单：锚点 (150,120)，gravity 位移 (0,0) → **(150,120)**，在 640×480 内不用滑动；
- 子菜单：理想位 (210,10)，但约束盒是菜单自己的 200×120，右侧超出 150 →
  slide 回 **(60,10)**。子菜单被"塞回"父菜单内部，这正是靠屏幕边缘时菜单反向展开的行为。

`--popup-reposition` 再验证一次动态重排：新规则 anchor rect `(600,400,30,20)`、
`ANCHOR_TOP_LEFT`、`GRAVITY_TOP_RIGHT`、offset `(4,−8)` → 点 (604,392)，
位移 (0,−120) → (604,272)，slide_x 退回 164 → **configure (440,272)**，
`repositioned token=77` 一并送回；子菜单跟着父菜单走（它的坐标本来就是父相对的）。

## 3. 表面树（nested.c）

`NestedSurface` 多了 `popup_parent` / `popup_children` / `popup_link` 和 `popup_x/popup_y`。
引用方向是**子持有父**：父被销毁时子还活着（子表面的生命周期由客户端决定），
反过来不行。`nested_surface_get_position()` 沿链累加 `popup_x/popup_y`，
给出控件自己的坐标系里的位置——子菜单因此不需要知道 toplevel 的存在。

树被摊平成一个**绘制栈**（自底向上 DFS，`popup_children` 按加入顺序即 z 序）：

```c
GList *nested_compositor_get_paint_stack (NestedCompositor *self);
```

只有 `surface_is_painted()`（已 map、有纹理、且最终挂在 `primary` 上）的表面进栈，
所以调用方拿到的就是屏幕上有的东西。命中测试和键盘归属都从栈顶往下找：

- `nested_compositor_surface_at(x, y)` → 指针焦点；
- `nested_compositor_keyboard_target()` → 最顶上的**已 grab** popup，否则 `primary`。

任何树变化（popup 上屏、消失、停止绘制）都走 `nested_surfaces_changed()`：
重定向座位 + 通知控件重绘。少了这一步，一个被删掉的菜单会在指针焦点里留一个洞。

## 4. 输入（nested-seat.c）

`seat_retarget()` 从"一个目标"拆成"指针目标 + 键盘目标"，两者可以不同表面。
`pointer_local()` 把控件坐标减去表面位置——事件必须落在客户端自己的坐标系里，
实测子菜单收到的 enter 是 `90.0,70.0`（控件的 300,200 减去栈上两级偏移）。

按下时先重定向（按下可能是移动后的第一个事件），发出按钮事件**之后**再判断：
如果命中的表面不是键盘目标，就 `xdg_popups_dismiss(self, pointer_focus)`——
给所有不在 `keep` 祖先链上的 grabbed popup 发 `popup_done` 并清掉 grab，
然后再次重定向，键盘立刻回到页面。先发后关，页面才能既看到这次点击、
又看到菜单关掉。

## 5. 绘制（chromium-view.c）

snapshot 遍历绘制栈，逐个 `nested_surface_get_paint_rect()` 再平移：
1:1 且源矩形从原点开始时直接 `gtk_snapshot_append_texture()`，
否则先 `push_clip` 到目标矩形再按 src→dst 缩放平移（viewporter 裁剪的情况）。

## 6. 顺手修掉的一个热点

`surface_commit()` 原本只要父表面有 popup 子表面就重排整棵树——即每帧一次。
本例客户端跑了约 180 帧，调试日志因此每帧两条。改成只在**尺寸变化**时重排
（window geometry 的变化由 `xdg_surface.set_window_geometry` 自己负责，
它已经调了 `xdg_popups_replace`），整轮 placement 日志从每帧两条降到 5 条，
首次 map 从 1×1 修正到真实位置的那次重排仍然保留。

## 7. 验证

`test/shm-client --popup --popup-reposition` 开一个菜单和一个子菜单，画出边框、
grab 色带和本地十字准线；`test/seat-selftest` 的 24 步脚本驱动座位并把栈打出来，
再用 cairo 合一张图（与 GSK 无关的独立实现，用来交叉核对位置）：

```
stack[0] nested-shm-test at 0,0 size 640x480
stack[1] (popup) at 150,120 size 200x120
stack[2] (popup) at 210,130 size 140x80
3 surfaces painted
client: popup menu grabbed the seat with serial 6
client: keyboard enter on menu
client: key down evdev=30 keysym=a on menu mods=0x0   # 键盘进了菜单
client: pointer enter popup submenu serial=8 at 90.0,70.0   # 本地坐标
# 在 (60,60) 按下之后：
client: popup menu dismissed by the compositor
client: popup menu torn down / popup submenu torn down
stack[0] nested-shm-test at 0,0 size 640x480 → 1 surfaces painted
```

真机 GSK 路径另跑一遍（窗口在 1.25 分数缩放的 GNOME 会话里）：

```bash
GDK_BACKEND=wayland CHROMIUM_SESSION_SCRIPT=/tmp/popup-gsk.script \
  builddir/src/chromium-demo builddir/test/shm-client --popup
# 脚本：wait 1200 / hover 300 200 / shot 带菜单 / click 60 60 / shot 关菜单后 / quit
```

两张截图证明合成的是控件真实渲染结果：菜单与子菜单叠在页面上、子菜单里
`90,70` 的准线位置正确、点击之后只剩页面。

## 8. 已知取舍

- **grab 的 serial 不校验**：协议要求 grab 引用一次 enter 的 serial，但真实菜单
  会在 reposition 后重新 grab。这里一律接受，因为拒绝的代价是 Chromium 的右键
  菜单直接消失；
- **reactive positioner 忽略**（`set_reactive`/`set_parent_size`/`set_parent_configure`
  都是空实现）：客户端要响应式重排就得自己发 `reposition`；
- **每个 pointer motion 都重定向**：会构造一次绘制栈（若干 `g_object_ref` + GList）。
  菜单只有几个表面，暂时不做缓存；如果后面发现热路径太贵，可以把栈缓存在
  `nested_surfaces_changed()` 上；
- **约束盒是父几何**，不是控件边界：一个超出 toplevel 的 popup 会被 slide 回来，
  这与 wlroots 的嵌套 popup 行为一致；
- **`xdg_toplevel.close` 仍是空实现槽**（impl 里没这一项），留给 #28 一起处理。
