# 查看网页源代码：组件原生子窗口方案

状态：设计提案，未实施。适用 CEF 154 / GTK 4 / cefnest 当前 main。

> **修订（2026-10-25，代码复核）**：方案的事实基础逐条对照过代码，均成立——
> `MENU_ID_VIEW_SOURCE = 132`（`cef_types.h:2037`）、`ViewSource()` 的
> 头文件引文逐字一致（`cef_frame.h`）、helper `OnContextMenuCommand()` 的
> `default: return false` 属实、“宿主自绘菜单/默认菜单/`menu-pick` 三条路径
> 都汇到 `OnContextMenuCommand`”也属实（`context-menu-select` 走
> `CefRunContextMenuCallback::Continue`，helper 自己的注释即写明"let
> OnContextMenuCommand execute them"）。`CONTROL_LINE_MAX` 确为 4 MiB
> （`chromium-session.c:30`，只在 session 侧读入时生效；helper 读宿主命令
> 无上限，分块仍必要——源码 + JSON 转义轻易超限）。修正三处：真实门禁
> 第 3 条改为可断言的写法（§12.2）、fake-helper 门禁的环境描述（§12.1）、
> `auto-source-window` 是类上第一个属性的工时提醒（§6.2）。

## 0. 结论速览

右键菜单里的“查看网页源代码”现在会拉起系统文本编辑器，因为 helper 把
`MENU_ID_VIEW_SOURCE`（132）交回 CEF 默认实现，而 CEF 默认实现是
`CefFrame::ViewSource()`：把页面 HTML 写进临时文件，再交给桌面默认文本查看器。

本方案把这条命令收进 helper，由组件自己完成：

1. helper 用 `CefFrame::GetSource()` 取源码，按块经控制通道发给宿主；
2. `ChromiumSession` 累积源码，打开/复用一个组件自有的 GTK 子窗口；
3. 窗口用 `GtkSourceView` 显示，带行号、语法高亮、查找、自动换行和复制；
4. 源码只存在内存里，不再落临时文件。

关键点是**在 helper 里拦截 132**：无论宿主用组件默认菜单、自己画菜单，还是
测试脚本直接 `menu-pick 132`，都会走同一条路径，宿主不需要额外写代码。

## 1. 问题与根因

### 1.1 现象

在 demo 中右键页面空白处，选择“查看网页源代码”：

- 浏览器视图不变化；
- 系统拉起 `gnome-text-editor`；
- `/tmp` 下出现 `CEFSource*.txt`。

### 1.2 代码路径

`helper/src/main.cpp` 的 `OnContextMenuCommand()` 只处理链接/图片/媒体等自定义
id，其余 id 走 `default: return false`。132 因此回到 CEF：

- `MENU_ID_VIEW_SOURCE = 132` 定义在 `third_party/cef/dist/include/internal/cef_types.h`；
- CEF 对它的默认实现是 `CefFrame::ViewSource()`；
- `third_party/cef/dist/include/cef_frame.h` 原文：

  > Save this frame's HTML source to a temporary file and open it in the
  > default text viewing application.

### 1.3 实测记录

CEF 154，Debian 13，GTK 4：

- 选中 132 后日志出现 `gio open`，随后是 `gnome-text-editor` 的 GIO/dconf 日志；
- 生成 `/tmp/CEFSourceD1hmKQ.txt`（0600，3116 字节）；
- 该文件内容是**当前 DOM 的序列化结果**：页面 JS 已经把标题改成 `MENU-2`，
  也把右键日志写进了 `<pre id="log">`，文件里都能看到；
- 此前几次人工操作留下的 4 个 `CEFSource*.txt` 仍留在 `/tmp`。

结论：不是 demo 或宿主打开了编辑器，是 CEF Alloy 的默认行为；“源码”实际是
`GetSource()` 语义下的 DOM 序列化，不是网络返回的原始字节。

## 2. 目标与非目标

### 2.1 目标

- **嵌入即生效**：任何使用 `ChromiumSession` + `ChromiumView` 的宿主，右键
  “查看网页源代码”都能得到同一个子窗口，不需要自己实现。
- **真正的子窗口**：`transient-for` 宿主窗口、非模态、随父窗口销毁；同一
  session 复用同一个窗口。
- **可读可复制**：行号、语法高亮、等宽字体、选择/Ctrl+C/Ctrl+A、查找、
  自动换行、复制全部。
- **不落盘**：源码只在 helper → session → 窗口的内存里流转。
- **不破坏现有行为**：默认菜单、宿主自绘菜单、多 browser、菜单应答流程都不变。

### 2.2 非目标

- 编辑源码、保存源码、另存为文件；
- 原始网络字节（与 Chrome view-source 完全一致需要重新抓取，见备选方案 B）；
- 外部 CSS/JS 资源、网络面板、DevTools；
- 查看 PDF 等非 HTML 文档的原始字节；
- 一个 URL 一个窗口；v1 按 session 复用单窗口。

## 3. 方案总览

```
右键 132
   │
   ▼
helper: OnContextMenuCommand
   ├─ 132? ── 是 ──▶ frame->GetSource(visitor)
   │                    │
   │                    ▼
   │             view-source-begin
   │             view-source-chunk × N
   │             view-source-end
   │                    │  （NDJSON 控制通道）
   ▼                    ▼
其他 id 保持原路径   ChromiumSession
                       ├─ 按 id 累积、校验上限
                       ├─ 发 ::view-source(browser, url, source)
                       └─ 默认打开/复用 GTK 子窗口（GtkSourceView）
```

为什么拦截点必须在 helper：宿主的菜单是“helper 给模型、宿主画、宿主回 id”。
只要宿主把 132 回给 helper，helper 就能执行；宿主自绘菜单也一样。把逻辑放在
session 反而要求宿主先接菜单信号、再自己取源码，违背“组件原生支持”。

## 4. helper 侧设计

### 4.1 菜单命令

在 `OnContextMenuCommand()` 的 switch 中新增：

```cpp
case MENU_ID_VIEW_SOURCE: {
  CefString page = frame ? frame->GetURL() : CefString();
  if (page.empty())
    page = params->GetPageUrl();
  const std::string target = page.ToString();

  // 已经是查看器页面：不再套一层，避免递归和没有意义的重取。
  if (target.empty() || target.rfind("view-source:", 0) == 0)
    return true;

  CefRefPtr<CefFrame> source_frame = frame ? frame : browser->GetMainFrame();
  if (!source_frame || !source_frame->IsValid()) {
    EmitViewSourceError(NewSourceId(), target, "frame is gone");
    return true;
  }

  source_frame->GetSource(new SourceVisitor(NewSourceId(), target,
                                            SlotTitle(browser)));
  return true;
}
```

要点：

- 用**被点击的 frame**，不是永远用主 frame；iframe 右键应看到 iframe 的源码。
- 132 被自己处理，必须 `return true`，不能落到 CEF 默认的临时文件路径。
- `title` 取 `BrowserSlot` 里 `OnTitleChange()` 缓存的标题；没有就留空，由
  session 用 URL 补。
- 源码取回是 UI 线程上的异步回调；菜单应答已经结束，不会卡住菜单。

### 4.2 取源码与分块

`CefStringVisitor::Visit(const CefString&)` 回调里：

1. 校验 UTF-8（CEF 正常给的就是 UTF-8；异常时按原样发送，宿主校验）；
2. 若 `source.empty()`，发 `view-source-error`；
3. 否则发 `view-source-begin`，按 **64 KiB 原始字节**切块，每块发
   `view-source-chunk`，最后 `view-source-end`；
4. 切块必须落在 UTF-8 字符边界（回退到非 `10xxxxxx` 字节），不能把多字节
   字符劈成两半；
5. 总大小上限建议 16 MiB。超过时只发前 16 MiB，并在 `end` 里带
   `truncated: true`。

为什么必须分块：控制通道一侧的单行上限是 4 MiB（`src/chromium-session.c` 的
`CONTROL_LINE_MAX`）。整页源码很容易超过，且 JSON 转义还会再膨胀。

### 4.3 可选的显式命令

为了让宿主和测试不依赖右键，增加一个命令：

```json
{"cmd":"view-source"}
```

helper 对当前激活 browser 的主 frame 执行同一套取源/分块逻辑。右键 132 与
这个命令共用 `EmitSourceForFrame()`，行为一致。

### 4.4 事件命名

事件统一用 `view-source-*`，避免和导航/其他源代码概念混淆：

| 事件 | 载荷 |
| --- | --- |
| `view-source-begin` | `browser`、`id`、`url`、`title`、`bytes` |
| `view-source-chunk` | `browser`、`id`、`data` |
| `view-source-end` | `browser`、`id`、`truncated` |
| `view-source-error` | `browser`、`id`、`message` |

`browser` 必须显式写被右键或命令指定的 browser；`NewEvent()` 里的“当前
browser”在多窗口宿主下不一定正确。

`id` 由 helper 为每次请求分配，从 1 递增。一次请求的事件顺序固定为
`begin → chunk* → end`，失败时为 `begin → error` 或直接 `error`。

## 5. 控制通道规则

1. 单条事件仍是一行 JSON，长度不超过现有 4 MiB 上限；64 KiB 的 chunk 加上
   JSON 转义后远低于上限。
2. 宿主只接受 `begin` 之后、`end/error` 之前的 chunk；未知或已放弃的 `id`
   直接丢弃。
3. 同一 session 同时只累积一个待打开的源码。新的 `begin` 到来时丢弃旧的
   累积并记一条 debug 日志。
4. 总计超过宿主上限（与 helper 同为 16 MiB）时停止追加，等待 `end` 时按
   “已截断”显示；若 helper 已经截断，以 `truncated` 为准。
5. `view-source` 命令没有回执；结果只通过上面四个事件返回。

协议文档同步更新 `docs/control-channel.md` 与
`docs/control-channel.zh-CN.md`。

## 6. session 侧设计

### 6.1 状态

`ChromiumSession` 新增私有状态：

```c
typedef struct {
  guint  id;          /* helper 分配的请求 id */
  int    browser;     /* 来源 browser */
  char  *url;
  char  *title;
  GString *text;
  gboolean truncated;
  gboolean active;
} SourceTransfer;

SourceTransfer source;      /* 当前传输，同一时刻最多一个 */
GtkWidget *source_window;   /* 弱引用；窗口销毁时置 NULL */
```

事件处理：

- `begin`：若已有 `active`，丢弃旧累积；记录 id/url/title，清空 GString；
- `chunk`：id 匹配且 active 时追加；
- `end`：id 匹配时停止累积，发 `::view-source`，然后按属性决定是否开窗；
- `error`：id 匹配时停止累积，发 `::view-source-error`。

### 6.2 默认行为与逃生口

- 默认打开/复用组件自有的源码窗口。
- 新属性 `auto-source-window`（`gboolean`，默认 `TRUE`）：
  - `TRUE`：session 自己开窗；
  - `FALSE`：只发信号，由宿主决定窗口形态和父窗口。

  注意：`ChromiumSession` 目前**没有任何 GObject 属性**（24 个信号、0 个
  `install_property`），这会是类上第一个属性——属性表、`set_property`/
  `get_property` 都要从零搭，估工时别按“往列表里加一行”算。
- 新信号：

```c
/* browser 是来源 browser id；source 只在信号发射期间有效，宿主需要保留请自行复制 */
::view-source       (int browser, const char *url, const char *source)
::view-source-error (int browser, const char *message)
```

- 新方法（可选，建议一起做）：

```c
/* 请求当前激活 browser 的主 frame 源码；等价于菜单里的 132。 */
void chromium_session_view_source (ChromiumSession *self);
```

多窗口宿主为什么需要逃生口：session 只持有一个 `set_view()` 的视图，窗口化
browser 的 GTK 窗口由宿主管理；session 无法知道右键发生在哪个宿主窗口。
`auto-source-window=FALSE` + `::view-source` 让宿主把窗口开到正确的父窗口上。

### 6.3 子窗口

- 类型：普通 `GtkWindow`，不注册进 `GtkApplication` 的窗口列表；
- 父窗口：`gtk_widget_get_root(GTK_WIDGET(session->view))`，若是 `GtkWindow`
  就 `gtk_window_set_transient_for()`；
- `gtk_window_set_destroy_with_parent(TRUE)`，非模态；
- 默认尺寸：父窗口的约 75%，下限 720×480；
- 标题：`查看源代码 — <title 或 url>`；
- 同一 session 只保留一个窗口：再次收到源码时替换 buffer 内容、更新标题并
  `gtk_window_present()`；
- 窗口销毁时用 `g_object_add_weak_pointer()` 清理 `source_window`；
- session `stop()` / dispose 时销毁窗口并放弃未完成的传输；
- 视图还没有根窗口时：创建无 transient 的普通窗口，并记一条 debug 日志；
  宿主若在意父窗口，应使用 `auto-source-window=FALSE`。

不要在菜单 popover 尚未收起时抢焦点；取源码本来就有一次异步往返，窗口通常
在菜单关闭之后才创建。若实测仍有焦点竞争，把 `gtk_window_present()` 放进
`g_idle_add()`。

### 6.4 打开时机

建议在 `view-source-end` 后一次性创建/刷新窗口，而不是边收边显示：

- 避免窗口里短暂出现半页源码；
- `GetSource()` 是本地渲染进程回调，等待时间通常不可感知；
- 实现简单，不用处理“chunk 到来时窗口已关”的 UI 分支。

如果实测大页面等待明显，再改成 `begin` 时开窗、显示“正在读取…”，chunk
追加到 buffer。

## 7. 窗口内容与交互

### 7.1 布局

```
GtkWindow
└─ GtkBox (vertical)
   ├─ GtkHeaderBar
   │  ├─ title widget: “查看源代码” + 省略的 URL/页面标题
   │  └─ end: [查找] [自动换行] [复制全部]
   ├─ GtkSearchBar (默认隐藏)
   │  └─ GtkSearchEntry + 匹配计数
   └─ GtkScrolledWindow
      └─ GtkSourceView
```

### 7.2 GtkSourceView

- `gtk_source_view_set_show_line_numbers(TRUE)`；
- `gtk_source_view_set_highlight_current_line(TRUE)`；
- `gtk_source_view_set_wrap_mode()`：默认 `GTK_WRAP_WORD_CHAR`，按钮切到
  `GTK_WRAP_NONE`；
- 等宽字体（CSS `font-family: monospace`）；
- `GtkSourceBuffer` + `gtk_source_buffer_set_highlight_syntax(TRUE)`；
- 语言选择：
  - 用 URL 的 path basename 调
    `gtk_source_language_manager_guess_language()`；
  - 猜不到（例如 `https://example.com/`）默认 `html`；
  - `.xml` / `.svg` / `.js` / `.css` 等按扩展名走；
- 样式方案按系统明暗主题选 `Adwaita` / `Adwaita-dark`；取不到就退回默认。

### 7.3 查找

- `Ctrl+F` 展开/聚焦查找栏，`Esc` 只关查找栏；
- `GtkSourceSearchSettings` 开启 `wrap-around`；
- `GtkSourceSearchContext` 用异步接口（`forward_async` / `backward_async`），
  避免大文件同步查找卡住主循环；
- 回车下一个，Shift+回车上一个，查找栏显示当前匹配序号/总数；
- 关窗或换源码时释放 search context。

### 7.4 复制

- 选择、Ctrl+C、Ctrl+A、鼠标主选区全部由 `GtkTextView` 原生提供；
- “复制全部”用 `gdk_clipboard_set_text()`，不依赖当前选区；
- 窗口是只读的，不提供编辑、保存。

### 7.5 可访问性

- 窗口标题、按钮、查找框都给 accessible label；
- 打开窗口后焦点落在源码视图；
- 行号等纯装饰不进入可访问性树。

## 8. 源码语义

v1 使用 `CefFrame::GetSource()`，内容是**当前文档的 HTML 序列化**：

- 与现在 CEF 交给文本编辑器的内容一致；
- 包含页面脚本运行后的 DOM 变化；
- 不重新请求网络，POST 结果页、带登录态的页面不会因查看源码而改变；
- 不是 Chrome “查看网页源代码”展示的原始响应字节。

这一点必须写进 `chromium-session.h`、控制通道文档和用户文档。若产品要求与
Chrome 完全一致的原始源码，只能走备选方案 B（重新加载 `view-source:`）。

## 9. 边界情况

| 情况 | 行为 |
| --- | --- |
| 源码超过 64 KiB | 分块发送，宿主按顺序拼接 |
| 多字节 UTF-8 跨块 | helper 在字符边界切，宿主校验后拼接 |
| 源码超过 16 MiB | 截断，窗口顶部显示“源码过大，已截断” |
| 连续查看两次 | 丢弃第一次未完成的传输，复用同一个窗口 |
| 已经在 `view-source:` 页面 | 132 直接 no-op，不发请求 |
| 子 frame 右键 | 查看被点击 frame 的源码 |
| `GetSource()` 返回空 | `view-source-error`，不开空窗口 |
| frame 在取源前失效 | `view-source-error` |
| 窗口打开后用户关闭 | 只清窗口引用；session 和页面不受影响 |
| 传输中关闭窗口 | 继续接收但丢弃，或直接放弃当前 id |
| session 停止/销毁 | 关闭源码窗口，放弃未完成传输 |
| 父窗口销毁 | 源码窗口随 `destroy-with-parent` 销毁 |
| 视图没有父 GtkWindow | 开无 transient 窗口并记日志 |
| 多窗口宿主 | 默认父窗口可能不对；用 `auto-source-window=FALSE` + 信号 |
| 大量源码 | 16 MiB 上限；buffer 一次性写入，查找异步 |
| 页面是 PDF/插件 | `GetSource()` 可能给查看器 HTML 或空；按实际结果显示或报错 |

## 10. 备选方案

### 10.1 方案 B：新开 CEF browser 加载 `view-source:`

helper 创建 windowed browser，加载 `view-source:<url>`；组件创建 GTK 窗口 +
共享 compositor 的 `ChromiumView`，把 CEF window 的 surface 映射进去。

优点：

- 无新依赖；
- Chromium 原生源码查看器：高亮、行号、自动换行都现成；
- 展示的是重新抓取的原始源码，语义最接近 Chrome；
- 复制/选择沿用现有输入链。

代价：

- 多一个 WebContents（通常多一个 renderer），为一个只读窗口偏重；
- session 要接管窗口、surface 映射、焦点、销毁，和现有“宿主管多窗口”的
  设计冲突；
- 宿主的 `::browser-created` 会看到这个 source browser，需要新增 purpose
  标记并让宿主过滤；
- 重新抓取会改变 POST/登录页的语义，甚至抓不到；
- Ctrl+F 仍需要宿主的查找 UI，不是白送。

### 10.2 方案 C：GtkTextView + 自写简单高亮

用 `GtkTextView` + `GtkTextTag` 做 HTML 的标签/属性/字符串/注释着色。

优点：零新依赖、实现最小。

代价：JS/CSS 高亮质量差；标签匹配、转义、长行性能都要自己维护；后续加
XML/JSON 支持等于自己写一个小的 GtkSourceView。仅适合“绝对不新增依赖”的
硬约束。

### 10.3 对比

| | A（推荐） | B | C |
| --- | --- | --- | --- |
| 新增依赖 | GtkSourceView 5 | 无 | 无 |
| 额外 renderer | 无 | 有 | 无 |
| 源码语义 | DOM 序列化 | 原始源码 | DOM 序列化 |
| 高亮质量 | 好 | Chromium 原生 | 基础 |
| 查找 | 组件内建 | 需要宿主 | 需要自己写 |
| 多窗口集成 | 独立 GTK 窗口 | 需接管 surface | 独立 GTK 窗口 |
| 实现风险 | 低 | 高 | 中 |

## 11. 需要确认的决策

| # | 决策 | 推荐 |
| --- | --- | --- |
| 1 | 是否接受 `GetSource()` 的 DOM 序列化语义 | 接受，写进文档；原始源码需求出现时再做 B |
| 2 | 是否新增 `gtksourceview-5` 依赖 | 接受；运行包加 `libgtksourceview-5-0`，构建装 `libgtksourceview-5-dev` |
| 3 | 多窗口宿主如何拿正确父窗口 | v1 用 `auto-source-window` + 信号；需要时再公开 `ChromiumSourceWindow` |
| 4 | 是否同时提供 `chromium_session_view_source()` | 建议提供，方便宿主工具栏和门禁 |
| 5 | 是否复用单窗口 | 建议复用；多次查看不叠窗口 |

若第 1 项必须改成“原始源码”，整体方案改走 B，第 4、5 项仍然适用。

## 12. 测试计划

### 12.1 fake helper

扩展 `test/fake-helper.py`，URL 带 `source=` 时发源码事件，覆盖：

- `source=small`：单块 HTML；
- `source=multibyte`：块边界落在中文/emoji 中间，验证宿主拼接；
- `source=huge`：超过 16 MiB，验证截断和提示；
- `source=error`：直接 `view-source-error`；
- `source=slow`：chunk 之间延迟，验证关窗、换请求、session 停止。

同时记录宿主发出的 `view-source` 命令，验证可选 API。

注意：fake helper 门禁跑在 `ci-session.sh` 的**无输出嵌套会话**里——那不是
“无显示环境”：普通 GTK 窗口在该会话可正常 map（pixel-gate 即证），源码窗口
也不用 popover，menu-gate 的 headless 限制（popover 不 map）不适用于此。

### 12.2 新增 `test/source-gate.sh`

针对 fake helper：

1. `view-source` 命令 → 窗口创建日志、字节数、语言；
2. `source=multibyte` → 文本与预期完全一致，无 U+FFFD；
3. `source=huge` → 截断标记出现；
4. `source=error` → 不开窗，`::view-source-error` 被记录；
5. 连续两次请求 → 只有一个窗口，第二次是 refresh；
6. 关窗后再请求 → 新窗口；
7. session 停止 → 窗口销毁、无未完成传输；
8. `auto-source-window=FALSE` → 不开窗，只发信号。

针对真实 CEF helper（手动门禁，沿用 `menu-gate.sh` 的方式）：

1. 加载 `test/menu-page.html`，右键空白处，`menu-pick 132`；
2. 断言源码窗口日志出现、字节数非 0、语言为 html；
3. 断言选中后 `/tmp/CEFSource*` 数量不变（这是默认路径被拦截的直接证据），
   如需额外保险再查无文本编辑器进程（`pgrep -f gnome-text-editor` 为空）；
4. 再选一次 132，断言复用窗口而不是新开；
5. 页面先执行 JS 修改 DOM，再查看源码，确认内容符合第 8 节的语义并记录。

（修正说明：原第 3 条“断言日志中没有 `gio open` / `gnome-text-editor`”不可
作为断言——demo/session 的 stderr 从不打印这类字样，grep 它恒真、没有证明力；
`gio open` 是调研时从系统侧日志观察到的，不属于被测进程的输出。）

### 12.3 手工 UI 检查

- 高亮、行号、当前行；
- 鼠标选择、Ctrl+C、Ctrl+A、主选区中键粘贴到别的应用；
- Ctrl+F 查找、回车/Shift+回车、Esc；
- 自动换行开关；
- 复制全部；
- 大文件滚动和查找不卡主循环；
- 父窗口关闭、源码窗口先关、session 停止三种顺序；
- 深色/浅色主题；
- 多窗口宿主下用信号把窗口开到正确父窗口。

### 12.4 接入

- `test/ci-all.sh` 增加 fake helper 部分（在 ci-session 的无输出嵌套会话中
  可跑，普通 GTK 窗口在该环境可 map）；
- 真实菜单路径沿用现有手动门禁约定，headless 下用 `view-source` 命令触发；
- 更新 `test/README` 或对应门禁注释中的运行方式。

## 13. 依赖与发布面

若采用方案 A，需要同步：

- `meson.build`：`dependency('gtksourceview-5', version: '>= 5.4')`，接入
  `src/meson.build` 的编译依赖；
- `deb-cefnest/DEBIAN/control`：`Depends` 增加 `libgtksourceview-5-0`；
- RPM spec：`Requires: gtksourceview5`；
- CI 构建镜像/脚本：安装 `libgtksourceview-5-dev`；
- `README.md`、`README.zh-CN.md`：功能与限制；
- `site/`：功能介绍；
- `docs/control-channel.md`、`docs/control-channel.zh-CN.md`：新命令和事件；
- `src/chromium-session.h`：新信号、属性、方法的行为与生命周期说明。

如果第 2 项决策否决新依赖，则改走方案 B 或 C，本节相应删除。

## 14. 实施拆解

1. **协议与 helper**
   - 新增 `view-source` 命令、四个事件、分块/截断逻辑；
   - 132 拦截；
   - fake helper 支持 source 模式；
   - 更新控制通道文档。
2. **session 接收与窗口**
   - 传输状态机；
   - `GtkWindow` + `GtkHeaderBar` + `GtkSourceView`；
   - 行号/高亮/查找/换行/复制；
   - 生命周期与复用。
3. **公开接口**
   - `::view-source`、`::view-source-error`；
   - `auto-source-window` 属性；
   - 可选 `chromium_session_view_source()`；
   - demo 增加 `view-source` 脚本步骤（用于门禁）。
4. **门禁**
   - `test/source-gate.sh`；
   - 真实 CEF 菜单门禁断言；
   - `ci-all.sh` 接入。
5. **发布面**
   - 依赖、打包、CI、README、site。
6. **收尾**
   - 全量 CI；
   - 手工 UI 清单；
   - 在 `README` 的限制列表里补一句源码语义（DOM 序列化）。

## 15. 验证记录（本次调研）

环境：Debian 13，CEF 154.0.34，GTK 4，nested Wayland。

复现脚本（临时）：

```
load file:///…/test/menu-page.html
wait 3000
click 600 400 273        # 右键页面空白处
wait 1500
menu-pick 132            # 查看网页源代码
wait 3000
js location.href
shot /tmp/viewsrc.png
close
```

结果：

- 菜单 5 项，132 为“查看网页源代码(&V)”；
- 选中后 `gio open` 启动 `gnome-text-editor`；
- 页面本身没有导航，截图仍是 `menu-page.html`；
- `/tmp/CEFSourceD1hmKQ.txt` 内容为 DOM 序列化；
- 另测 `load view-source:…` 在现有视图中可以正常显示 Chromium 源码页
  （行号、高亮），说明“在页面里看源码”本身可行，但本方案按需求选择独立
  子窗口。

调研期间对 `helper/src/main.cpp` 做过的临时验证改动已全部还原；本方案本身
没有留下代码改动。工作区里若同时存在其他主题的改动（例如焦点链探针），与
本文无关。

## 16. 相关文件

- `helper/src/main.cpp` —— `OnContextMenuCommand()`、事件发送；
- `third_party/cef/dist/include/cef_frame.h` —— `GetSource()` / `ViewSource()`；
- `third_party/cef/dist/include/internal/cef_types.h` —— `MENU_ID_VIEW_SOURCE`；
- `src/chromium-session.c` —— 事件分发、信号注册、窗口生命周期；
- `src/chromium-session.h` —— 公开 API/信号文档；
- `src/chromium-view.c` / `src/nested.c` —— 视图和 compositor（方案 B 才涉及）；
- `test/fake-helper.py`、`test/menu-gate.sh`、`test/ci-all.sh` —— 测试；
- `docs/control-channel.md`、`docs/control-channel.zh-CN.md` —— 协议文档；
- `examples/windows.c` —— 多窗口/surface 映射参考（方案 B 才需要）。
