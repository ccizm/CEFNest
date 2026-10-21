# 剪贴板桥：空镜像比没有镜像更糟，以及 Wayland 的焦点门槛

日期：2026-10-20
问题：任务 #18。拆分之后要重新确认外层（GTK 宿主 clipboard）→ 内层（Chromium 客户端）
的粘贴还能走通。跑起来发现两件事：一件是桥的真 bug，一件是 Wayland 平台的规矩。

## 1. 桥只有两条数据来源

`src/nested-data-device.c` 里内层客户端 `wl_data_offer.receive` 能拿到的文本，来源只有
两个：

| 来源 | 什么时候有 | 代码 |
| --- | --- | --- |
| 某个内层客户端自己 copy 出来的 `wl_data_source` | `self->selection_source` 非空 | `wl_data_source_send_send()` 直接把 fd 交给 owner |
| 外层 GTK clipboard 的镜像 `clipboard_bytes` / `clipboard_mime` | 外层有文本时 | `clipboard_send_bytes()`，或者镜像为空时 `clipboard_paste_later()` 现读一次 |

镜像由 `notify::content` 驱动：`compositor_paste_clipboard()` 看外层有没有文本 MIME，
有就异步读进 `clipboard_bytes`，再 `compositor_advertise_selection()` 把 offer 播给所有
`wl_data_device`。读不到就整个清空。

## 2. 真 bug：读到空字符串也当成"有选区"

`compositor_clipboard_read_done()` 原来只判 `!text`。可 `gdk_clipboard_read_text_finish()`
对一份 0 字节的选区返回的是**指向 `""` 的指针，不是 NULL**——这一点是量出来的，不是猜的：

```
$ GDK_BACKEND=x11 /tmp/clipprobe          # 一次性小程序：set_text ("") 然后立刻 read_text_async
probe: set an empty string, 2 formats
probe: non-NULL, length 0: []
```

外层 clipboard 只要还挂着文本 MIME（哪怕内容是空的），`compositor_paste_clipboard()` 就会
走进异步读，回来的是 `""`，于是桥把 `clipboard_mime` 设成 `text/plain;charset=utf-8`、
`clipboard_bytes` 设成 0 字节，然后播出去。

危害不在于客户端粘到一个空串，而在于 `data_offer_receive()` 的分派：

```c
if (!self->clipboard_bytes)
  {
    /* 镜像为空才会现读一次 clipboard */
    clipboard_paste_later (self, fd);
    return;
  }
```

一份 0 字节的镜像是**非空**的 `GBytes`，所以这个"按需现读"的补救分支再也不会进。晚一步
到的粘贴请求本来能读到已经恢复好的真文本，现在只能拿到空。换句话说：空镜像比没有镜像更
糟。

改法是把"空"统一归到"没有"（`!text || !*text`），清掉 `clipboard_bytes` / `clipboard_mime`
并且在没有 owner 的时候播 NULL，让客户端明确看到"选区被清了"而不是"选区是空的"：

```c
  /* Nothing to mirror — and an empty mirror is worse than none, because a
   * client that is offered one never reaches the read-again-on-demand path
   * that would have got the real text a moment later. */
```

同一个不变量也补到了反方向：`clipboard_transfer_finish()` 里客户端 copy 空选区时，外层
clipboard 照样设成 `""`（那是客户端的意愿），但**不再**留下 0 字节的镜像。现在
`clipboard_bytes` 只有两种状态：NULL，或者有内容。

## 3. 平台规矩：Wayland 上 owning 选区要有键盘焦点

`gdk_clipboard_set_text()` 之后 `notify::content` 会连着来两次：先是有文本，紧接着
0 formats。这是宿主（mutter）把选区收回去了，因为 Wayland 只把 `wl_data_device.set_selection`
授予**当前持有键盘焦点**的客户端。脚本化跑起来的 demo 窗口从来没被激活过：

```
demo: seeded the clipboard with 'pasted-from-outside' (at startup, window active=0)
nested: the widget clipboard holds 19 bytes of text        <-- 镜像确实建立起来了
nested: the widget clipboard holds no text (0 formats)     <-- 100ms 后宿主收回
client: selection cleared
```

所以这不是桥的问题，但它会让"验证"看起来像坏了：外层空 clipboard 和内层桥不工作，在日志
里一模一样。为此 demo 侧做了两件事：把 seed 抽成 `clipboard_seed (why)`，启动时来一次、
窗口第一次被激活时再来一次（`notify::active`），并且把 `window active=%d` 一起打出来，
这样一眼能看出是"没焦点"还是"没桥"。

真实产品里这条不构成限制：用户要用 Ctrl+V 粘贴，浏览器窗口本来就必须是焦点，`notify::active`
那一次 seed 也就是用户点进窗口的那一下。

## 4. 验证

内层客户端用 `test/shm-client` 的 `--paste` / `--copy`，外层用 `CHROMIUM_CLIPSET` 播种、
`CHROMIUM_CLIPWATCH` 回读。

| 方向 | 宿主后端 | 结果 |
| --- | --- | --- |
| 外 → 内 | `GDK_BACKEND=x11` | `client: pasted 19 bytes of text/plain;charset=utf-8: 'pasted-from-outside'` |
| 内 → 外 | `GDK_BACKEND=x11` | `nested: a client copied ... (27 bytes)` + `demo: clipboard is 'text from the nested client'` |
| 内 → 外 | `GDK_BACKEND=wayland` | 同上，文本进到了 widget clipboard |
| 外 → 内（镜像建立） | `GDK_BACKEND=wayland` | 19 字节确实进了 `clipboard_bytes` |
| 外 → 内（端到端） | `GDK_BACKEND=wayland` | 未过：窗口没有键盘焦点，宿主在 ~100ms 内收回选区 |

桥的代码路径与宿主后端无关（都走同一个 `GdkClipboard`），x11 那一行证明的是同一段逻辑。
反向的自激也确认挡住了：客户端 copy 完之后 `clipboard_transfer_finish()` 会 `gdk_clipboard_set_text()`，
这一步 `notify::content` 是同步发的，`clipboard_echo` 让 `compositor_paste_clipboard()` 直接返回，
镜像改由 `clipboard_bytes = g_bytes_new (text, ...)` 就地写——所以 copy 之后立刻 paste 不依赖宿主
有没有把选区发回来。

回归：`seat-selftest` 六种模式（plain / `--dmabuf` / `--text-input` / `--cursor-shape` /
`--popup` / `--refused`）全部跑到 step 24 且退出码 0，fd 增量与改前一致——客户端起得来的
四种是 +18，`--refused` 是 +13（绝对数随起始环境漂移，看增量）。两个方向在改完之后又各跑
了一遍：`pasting 19 bytes of the widget clipboard` 和 `a client copied ... (27 bytes)`。

## 5. 中键主选区（已接）与还没做的

* 原生 Wayland 宿主下、窗口真的拿到键盘焦点时的那一次端到端确认——需要一个真人点进窗口，
  或者一个能给窗口 focus 的无头宿主（机器上没有 weston/cage，没法自己起一个会话）。
* 图片、URI 列表这类非文本剪贴板仍然不桥：`mime_is_text()` 之外的一律当作"没有选区"播出去，
  避免客户端粘到一份过期文本。
* `zwp_primary_selection_device_manager_v1` 已在 2026-10-21 接上：`nested-data-device.c` 里
  有独立的 primary source/device/offer，host→page 实测把 274 字节的 host primary 粘进
  textarea。page→host 的 source/offer 路径也已实现，但本轮 CEF 只创建 primary device，
  页面选中文本**没有创建 primary source**；这是 CEF/Chromium 的写 primary 行为，不是协议
  桥缺失。验证用 `CHROMIUM_PRIMARYSET` / `CHROMIUM_PRIMARYWATCH`，fixture 是
  `test/primary-page.html`。
