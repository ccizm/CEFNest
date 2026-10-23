# 真实页面巡检批次

## 1. 它测什么

组件到这一刻的自动门禁大多跑探针页（`test/*.html`）。这份 batch 用五个**外部真实站点**，
每站一个独立的 `test/ci-session.sh`（nested headless mutter），记录：

- 是否发出 `load-end` 以及 `HTTP` 码；
- `document.readyState`、标题、正文长度、DOM 探针；
- 3 秒 `requestAnimationFrame` 计数；
- 一张 1000×653 的 widget dump（以及 client texture）。

它**不是** release gate：站点会改版、会限流、会要求登录/同意，CDN 也会抖。它的产物是一份
可复查的巡检报告，不是一个“所有站点永远绿”的承诺。

## 2. 跑法

```bash
./test/real-sites.sh                 # test/real-sites.tsv 里的全部
./test/real-sites.sh docs video      # 只跑指定站点
REAL_SITES_OUT=/tmp/real ./test/real-sites.sh
CI_ALL_REAL=1 ./test/ci-all.sh real  # 经 ci-all 的 opt-in 入口
```

`test/real-sites.sh` 直接为每站调用 `test/ci-session.sh`，所以每站都有自己的 nested
compositor、隔离 HOME/XDG/D-Bus 和超时；`test/ci-all.sh` 只需把 `real` 这一行交给它，
不要再套一层 ci-session。

产物在 `builddir/real-sites/<timestamp>/`：

| 文件 | 内容 |
| --- | --- |
| `<site>.session` | 实际执行的脚本 |
| `<site>.out` | demo/helper 的完整转录 |
| `<site>.png` / `.client.png` | widget dump 与客户端 texture |
| `<site>.readback.json` | `js-expr` 读回的 JSON（字段见下） |
| `<site>.harness.log` | ci-session 的外层记录 |
| `summary.md` | 本批表格与 findings |

## 3. 2026-10-21 首轮结果

最终干净一轮：`builddir/real-sites/20261021-203403/summary.md`。五个站点都完成
`load + screenshot + JS`：

| 站点 | HTTP | ready | rAF / 3s | 关键读回 | 结论 |
| --- | --- | --- | --- | --- | --- |
| Bing 搜索 | 200 | complete | 272 | `input=true`, text 1820 | 搜索页渲染正常 |
| MDN 文档 | 200 | complete | 269 | `article=true`, `main=true`, text 4852 | 文档站正常；无 canvas/WebGL |
| YouTube 视频 | 200 | complete | 269 | `videos=1`, `videoReady=4`, `vp9=probably` | 视频元素就绪；**H.264 `mp4=""`，VP9 可用** |
| Google Maps | 200 | complete | 271 | `canvas=3`, `webgl=True`, text 38 | WebGL 地图正常（见下） |
| Excalidraw | 200 | complete | 271 | `canvas=2`, buttons 26, text 141 | 重 JS 应用正常；2D canvas，非 WebGL |

所有截图都是非纯色（颜色数 898–244k），rAF 在 3 秒内 269–272 次（约 90 Hz，headless 的
节拍，不是 60 Hz 目标）。搜索/Maps/Excalidraw 的转录里各有 2 条
`*** stack smashing detected ***: terminated`：**那是本轮量出来的东西，本表记录时它还在**。
第六轮把它归因完并消掉了——abort 的是 helper 自己 `main()` 的栈保护检查，发生在 zygote 的
fork-without-exec 子进程正常返回的路上（`docs/internals/stack-canary-after-fork.md`）；
它不影响页面：这三站照样加载、回答 JS、出图，所以当初没有把它和用户页面崩溃混为一谈。
用改后的 helper 重跑这三站，这两行不该再出现；`test/real-sites.sh` 仍然数它们，
`ci-session` 的 proof 现在见一条就失败。

### 3.1 视频编解码

YouTube 页在 headless CEF 里：

```
videos=1  videoReady=4  mp4=""  vp9="probably"
```

`videoReady=4` 是 `HAVE_ENOUGH_DATA`，说明播放器拿到了可播流；`canPlayType` 明确拒绝
`video/mp4; codecs="avc1.42E01E"`，接受 VP9 WebM。也就是说这棵 CEF 分发的 **H.264 解码
能力缺失，VP9 可用**；YouTube 仍可能走 VP9。若产品要 MP4/H.264，得在 CEF 分发/编解码
供给上做决定，`js-expr` 只是把这件事量出来。

### 3.2 Maps 那次 WebGL 的两次结果

同一 URL 前一轮读回 `canvas=0, webgl=False`，最终一轮是 `canvas=3, webgl=True`。说明
Google Maps 会在不同的渲染器/加载阶段之间选择不同路径（或冷启动时还没建 canvas）；
巡检必须等页面稳定后再读，不能拿 12 秒前的一次 DOM 快照当结论。最终报告记录的是稳定
后的那次，截图也对应 WebGL 地图。

## 4. CSP 页面为什么需要 `js-expr`

MDN 的 CSP 是 `script-src ...`（不含 `unsafe-eval`）。原来的 `js` 命令在页面侧包了一层
`eval(code)`，被 CSP 直接拒绝：

```
Evaluating a string as JavaScript violates ... 'unsafe-eval' is not an allowed source
```

所以协议加了一条 `js-expr`：代码作为**表达式直接嵌入** wrapper，不经过页面 `eval`；它仍把
结果走同一个 `js-result`。`js-expr` 只接受表达式/IIFE，`js` 保留给语句形状。真实站点巡检
全部用 `js-expr`。

## 5. 已知边界

- **一站点一会话，不做登录、不做交互**；只验证首屏、JS 读回和短帧节奏。
- **headless** 没有输出设备，popup 映射等显示断言不在这里；帧数是 3 秒 rAF 采样，不等同于
  真实 vsync 或长时间稳定性。
- **网络外部性**：CDN/SSL/同意页会让某次失败；先看 `<site>.out` 的 `load-end`/`SSL`，
  再决定是否重跑，不要把一次外部失败写成组件回归。
- **视频未点播放**：`videoReady`/`canPlayType` 是首屏可播性，不是解码 4K/60fps 的吞吐测量。
- **DOM 深水区不保证**：shadow DOM 里的内容（Maps 的 text=38）不代表页面没画出来；以截图
  和可见区域为准。
