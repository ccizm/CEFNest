# 音频：页面出的声走哪条路，以及静音开关

日期：2026-10-20
问题：任务 #33。`docs/control-channel.md` 第 4 节把「静音 / 音量」停在"待测"：先得确认
这条路径到底会不会出声、音频在哪个进程里、嵌套合成器管不管得着。这一篇把三件事都量掉，
然后给静音一个实现。

## 1. 页面的声音会到扬声器：量到的路径

探针页面就是一个 440 Hz 的 `AudioContext`（`test/audio-probe.html`），它必须等一次
**真实点击**才开始——`ctx.resume()` 放在 click handler 里，autoplay 政策照常生效，
我们通过嵌套合成器注入的那次 `click 300 250` 就是那个手势。点完之后 `ctx.state`
变成 `"running"`（`js` 步骤读回来的）。

进程树和音频连接都取自一次 hosted 运行进行中的 `/proc`（`Seccomp` / `NoNewPrivs` 取自
`/proc/PID/status`，类型从 `/proc/PID/cmdline` 里切出来，见第 3 节）：

| pid | ppid | Seccomp | 类型 |
| --- | --- | --- | --- |
| 133976 | demo | 0 | 浏览器进程 |
| 133993 | 133976 | 0 | zygote（`--no-zygote-sandbox`，gpu 的父进程） |
| 133994 | 133976 | 0 | zygote |
| 133996 | 133994 | 0 | zygote |
| 134027 | 133993 | 0 | gpu-process |
| 134030 | 133976 | 0 | utility `network.mojom.NetworkService` |
| 134056 | 133996 | 2 | utility `storage.mojom.StorageService` |
| 134069 | 133996 | 2 | renderer |
| 134070 | 133996 | 2 | renderer |
| 134092 | 133976 | 0 | utility `audio.mojom.AudioService` |

三条量到的事实：

* **碰音频服务器的只有一个进程**：`/proc/134092/fd` 里持有
  `/run/user/1000/pipewire-0` 的连接（`ss -x` 里能看到这条 ESTAB）。它是浏览器进程的
  直接子进程、**`Seccomp=0`**——沙箱压根没罩到它身上（Chromium 给这个服务显式配的就是
  `--service-sandbox-type=none`）。renderer（`Seccomp=2`）手里一个音频 fd 都没有。
* **不是 PulseAudio 客户端，是 ALSA 客户端**：`ldd libcef.so` 里只有 `libasound.so.2`，
  没有 `libpulse`。这台宿主的 ALSA `default` 被
  `/usr/share/alsa/alsa.conf.d/99-pipewire-default.conf` 指到 `type pipewire`，所以
  Chromium 打开 `default` 就直接成了 PipeWire 的 ALSA 客户端。
* **流是活的、已经挂到真实声卡上**：`wpctl status` 里出现名为 `Chromium` 的客户端和
  一条 `output_FL/FR > ALC257 Analog:playback_FL/FR [active]` 的流，硬件 PCM
  （`/proc/asound/card0/pcm0p/sub0/status`）整个探测期间都是 `state: RUNNING`。哪怕
  页面的增益是 −80 dBFS（听不见），这条流也一直在往扬声器送数据。

所以答案是：**页面能出声，而且出声就直达宿主桌面**。嵌套合成器在这条路上完全缺席——
声音没有 Wayland 表面，不走我们的 socket，GNOME 音量条里它就是一个普通的 "Chromium"
应用流。组件自己不出面的话，唯一能拦住它的就是桌面混音器。

## 2. 决定：给静音一条命令，不给音量

CEF 那边是 `CefBrowserHost::SetAudioMuted(bool)` / `IsAudioMuted()`（只能在 UI 线程调，
`HandleCommand` 正好在）。它满足 `docs/control-channel.md` 的判据：一次请求、不需要
回答、没有进度、没有 id 空间。而 **音量不做**：那是桌面混音器已经管着的事，GNOME 里
对这条流就能调；组件再做一个音量滑条只是把同一个旋钮画第二遍。

通道上新增：

* host → helper：`{"cmd":"mute","on":true}`（`on` 缺省 = 翻转）；
* helper → host：`{"event":"muted","muted":true}`——**回读**而不是回执：helper 在
  `SetAudioMuted` 之后读 `IsAudioMuted()`，报的是 CEF 承认的状态。宿主侧
  `chromium_session_get_muted()` 跟着这个事件走（晚一个往返，随 `::changed` 落地），
  新起的 helper 永远从"不静音"开始。

`ChromiumSession` 新增 `chromium_session_set_muted ()` / `chromium_session_get_muted ()`，
demo 的会话脚本多两步 `mute` / `unmute`，`::changed` 的转录里多一个 `muted=` 字段。

## 3. 验证：mute 之后扬声器上是什么

这次测量有一段弯路，先记下来免得重蹈：

* **`pw-record --target <sink>.monitor` 在这套 WirePlumber 上不生效**——它静默地连到
  默认**麦克风**去了。头两轮"满刻度方波"的录音其实是 built-in mic 的环境声，发现后
  已经删掉。要对着 monitor 量，用 `parecord --device=<monitor>`，并在录之前核对
  `pactl list source-outputs` 里那条 `Source:` 是不是 monitor 的序号。
* 所以最终的量法是：临时 `pactl load-module module-null-sink
  sink_name=chromiumgtk-probe`，把 Chromium 那条流 `pactl move-sink-input` 到空 sink 上
  （声音从此进不了扬声器，桌面上别的声音也进不了这次测量），然后 `parecord --device=
  chromiumgtk-probe.monitor --format=s32le` 连续录下整个 mute/unmute 周期，量三个窗口。
  完了 `pactl unload-module`。

结果（页面增益 0.0001 ≈ −80 dBFS，Goertzel 单频能量，满刻度 dBFS）：

| 窗口 | rms | 440 Hz | 1 kHz |
| --- | --- | --- | --- |
| 播放中（mute 前） | −83.0 | **−86.0** | −233.6 |
| `mute` 之后 | **−inf** | **−inf** | −inf |
| `unmute` 之后 | −83.0 | **−86.0** | −241.8 |

播放段里 440 Hz 那个格子比 1 kHz 高出 150 dB——那就是页面发的那个音，原样出现在
audio server 的输出上。`mute` 之后三个格子全是精确的 −inf：**数字静音，一个比特
都不剩**；`unmute` 之后同一个 440 Hz 格子回到 −86.0。CEF 的回读（`IsAudioMuted()`）
和实际到达音频服务器的内容在这两次往返里逐点对上。

另外一个顺带量到的形状：**mute 不会拆流**。整个周期里 `wpctl status` 的 "Chromium"
流一直挂着、硬件 PCM 一直 `RUNNING`——静音发生在链路更上游，桌面混音器从外面看不出
任何变化。所以"mute 生效了吗"这个问题在 `wpctl`/`pactl` 里没有答案，只有采样级
测量（或 `IsAudioMuted()` 回读）能回答。

## 4. 顺带修正：子进程的类型标签其实读得出来

`docs/sandbox-policy.md` 第 2 节写过「类型标签给不出，`/proc/PID/cmdline` 只剩 1 项」。
前半句对，后半句错：Chromium 把子进程 argv 重写成了**一个**以空格分隔的长字符串
（`argc=1`，整条命令行都在 `argv[0]` 里）。按 NUL 切分确实什么也得不到，但按**空格**
切那一项，`--type=` 和 `--utility-sub-type=` 都在——第 1 节那张表就是这么来的。
沙箱篇里那张 9 行表的 pid 现在都能对上类型了。

## 5. 还没做的

* **「页面开始出声」没有事件**。CEF 154 没有暴露 audio-state 回调，宿主想知道
  "这一页正在响"只能自己拿 `js` 去数 `<audio>`/`AudioContext`。mute 是宿主主动的
  开关，反过来的一侧没有。
* **音量没做**（第 2 节的决定），`SetAudioMuted` 之外的 `browser.set_volume` 之类
  不存在也没必要。
* **audio utility 死了仍然没人报**。这次确认音频就住在那个 `Seccomp=0` 的 utility
  里，它死掉的形状和 `docs/control-channel.md` 第 6 节记的一样：宿主什么都收不到，
  只剩 `::closed` 兜底。
