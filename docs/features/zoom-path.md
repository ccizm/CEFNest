# 页面缩放：CEF 的 level，不是 CSS 缩放

## 1. 一条命令、一个回读

`zoom-in` / `zoom-out` / `zoom-reset` 是三条**不发 ack**的 host → helper 命令：

| 命令 | CEF 动作 | 回读 |
| --- | --- | --- |
| `zoom-in` | `SetZoomLevel(current + 0.5)` | `GetZoomLevel()` |
| `zoom-out` | `SetZoomLevel(current - 0.5)` | `GetZoomLevel()` |
| `zoom-reset` | `SetZoomLevel(0.0)` | `GetZoomLevel()` |

0.5 是 Chrome UI 的一格；0.0 = 100%。答案统一走 `zoom` 事件（`level`），宿主不能靠本地猜测
“应该到了哪一级”。

### level 不是百分比：因子是 `1.2^level`

`GetZoomLevel()` / `SetZoomLevel()` 传的是 Blink 的 zoom level，换算到页面缩放因子是
`1.2^level`。本机量过（2026-10-23）：`./test/ci-session.sh` 里跑 demo，脚本在每两次
`zoom-in` 之间插一条 `js window.devicePixelRatio`——嵌套 surface 的 scale 是 1，所以 dpr
读出来就是缩放因子本身。

| level | `window.devicePixelRatio` | `1.2^level` |
| --- | --- | --- |
| 0.00 | `1` | 1 |
| 0.50 | `1.0954451560974121` | 1.0954451 |
| 1.00 | `1.2000000476837158` | 1.2 |

把 level 当百分比线性换算会读成 50 % 和 100 %，而页面其实是在 110 % 和 120 %——
`examples/browser.c` 工具栏上那个数字因此是 `100 * pow (1.2, level)`，不是 `level * 100`。

## 2. Chromium 自己按 origin 记

缩放 level 由 Chromium 持久化到 **origin**，不是整个 browser。换页/重新载入后同一 origin 可能
回来就是上次的值，所以 helper 在主 frame 的 `load-end` 之后**再发一次** `zoom`，把
`GetZoomLevel()` 的真实结果交给宿主。宿主把 `::zoom-changed` 当成“当前页面 level 的权威值”，
不要把它当成本次命令的 ack。

## 3. 宿主 API 与 demo

`src/chromium-session.h`：

- `chromium_session_zoom (session, "in" | "out" | "reset")`
- `chromium_session_get_zoom_level (session)`
- `::zoom-changed (double level)`

demo 绑定：

- `Ctrl+plus` → `in`
- `Ctrl+minus` → `out`
- `Ctrl+0` → `reset`

这三个组合在 **capture phase** 被窗口拿走，所以视图/页面看不到它们的 press。**修饰键的 release 必须
由同一个控制器补发**（`find_key_released()`）：如果 Ctrl 的 release 留在窗口而没有送进嵌套
compositor，Chromium 会以 Ctrl 仍按下的状态收到之后的滚轮，Ctrl+wheel 就会继续缩放。快捷键的
release 也在这里补发；普通 F/Esc 没有被 consume 时仍让视图收到 release，不能把页面按键卡住。

脚本步骤同名：`zoom-in` / `zoom-out` / `zoom-reset`；`test/menu-page.html`/`test/dnd-page.html`
都可用于手动观察。

## 4. 边界

- **不是 pinch zoom**：没有 `zwp_pointer_gestures`，触控板捏合不会进这条路径；这里只做键盘/API
  的页面缩放。
- **没有独立门禁**：现阶段靠 `ChromiumSession` 的 `zoom` 回读与 demo 手动验证；不要把它写成
  “门禁已覆盖”。§1.1 那组 dpr 是一次手动测量，不是断言——它读的是 demo 打印的脚本结果，跑法
  记在那里。真正的自动门禁需要一条可稳定读到的 CEF level 与一个页面侧的期望值，现有测试页没有
  为此加检查。
- **level 不等于布局宽度**：CEF 的 zoom 改的是页面缩放因子，不改变嵌套 surface 的尺寸；窗口
  resize 仍然走原来的 compositor 路径。
