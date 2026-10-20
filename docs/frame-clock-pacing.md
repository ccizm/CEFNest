# 帧调度改造：frame 回调挂接 GdkFrameClock

日期：2026-10-20
问题：嵌入的浏览器内容操作"有延迟、不跟手"。
结果：客户端等待 frame 回调的时间 p50 从 **20.5ms 降到 15.3ms**，p99 从 **29.8ms 降到
22.0ms**（同桌面条件背靠背实测）；更重要的是回调发射点从"随机相位"变为"与控件
上屏周期对齐"，典型输入到上屏少等 1 帧左右。

## 1. 问题定位

用户反馈组件操作不跟手。用 `WAYLAND_DEBUG=1` 抓客户端侧日志逐段测量输入→上屏链路：

| 链路段 | 实测 | 结论 |
|---|---|---|
| GDK 事件 → `notify_*` → 客户端收到 | < 2ms | 无辜：注入是同步 flush 的（nested-seat.c） |
| 客户端等 frame 回调（frame 请求 → done） | **p50 ≈ 20ms** | 主凶 |
| 客户端 commit → 控件上屏 | 0~16.7ms（随机相位） | 帮凶 |

### 原实现

frame 回调由一个 `g_timeout_add(16)` 软件定时器发射（`frame_clock_tick`）：

1. **固定 16ms 税**：客户端每帧"commit → 请求回调 → 等 done → 才开始画下一帧"，
   done 永远在 ~16ms + 主循环抖动之后到达，实测 p50 20.5ms、p99 29.8ms，帧率被
   压到 ~52fps；
2. **与上屏不对齐**：定时器相位和 GTK frame clock 的 paint 相位完全无关，客户端
   commit 落在控件绘制周期的随机位置，平均再白等 ~8ms、最坏 16.7ms；
3. **两层 16ms 循环串联拍频**：周期性出现连续错过上屏的两帧卡顿（实测 commit
   间隔 p99 = 34~36ms）；
4. **隐藏空转**：定时器不随控件可见性暂停，窗口被遮挡时客户端仍以 ~50fps 重绘。

典型输入到上屏 ≈ 事件压缩(8) + 等回调(9~20) + 客户端绘制(4~8) + 等上屏(8~17)
≈ **35~55ms**，原生 GTK 控件为 16~25ms——正是"慢一拍半"的体感。

## 2. 改动

### API（nested.h）

```c
void nested_compositor_set_frame_clock (NestedCompositor *self,
                                        GdkFrameClock *clock);
```

与 `set_clipboard` 同一套接法：控件 map 时把 `gtk_widget_get_frame_clock()` 交给
合成器，unmap 时传 NULL。

### 核心逻辑（nested.c）

- 抽出 `fire_pending_frame_callbacks()`：向所有等待中的客户端发 done 并 flush，
  返回是否仍有人等待。原 `frame_clock_tick`（定时器路径）与新 `frame_clock_update`
  （时钟路径）共用它。
- `frame_clock_update`：挂在时钟的 **update 相位**。一个显示周期开始时告诉客户端
  "可以画了"，客户端的产出正好赶上下一个周期的 paint——合成器与客户端形成标准的
  一帧深流水线。仍有人等待就 `gdk_frame_clock_request_phase(UPDATE)` 让时钟继续
  转；没人等待时时钟自然空闲，直到下一次 commit 或 frame 请求。
- `nested_start_frame_clock`：有时钟 → 请求 UPDATE 相位；无时钟 → 原样武装 16ms
  定时器（后备路径，供 selftest 等无界面场景）。
- `set_frame_clock()`：挂载时停掉后备定时器、立即把等待中的客户端并入显示周期；
  卸载时恢复定时器，行为与改前一致。
- dispose 增加时钟解挂。

### 控件接线（chromium-view.c）

`chromium_view_on_map` / `on_unmap` 各加一行挂载/卸载调用。

副产品：窗口被遮挡或最小化时 GDK frame clock 停摆 → 客户端回调随之暂停 → 不再
隐藏空转（与原生应用行为一致）。

### 不变的部分

`nested.h` 其余 API、协议处理、绘制路径（commit → `frame` 信号 → `queue_draw` →
snapshot 读当前纹理）全部未动；selftest 等无控件场景继续走定时器后备。

## 3. 实测对比

方法：把改前代码恢复到 /tmp 副本单独构建，两个版本背靠背各跑 4 秒
`cefnest-demo + shm-client`，从客户端侧 `WAYLAND_DEBUG` 日志统计
frame 请求 → done 的延迟与 commit 节奏（n≈400 帧/组）。

| 指标 | 改前（16ms 定时器） | 改后（GdkFrameClock） | 变化 |
|---|---|---|---|
| 回调延迟 p50 | 20.5 ms | **15.3 ms** | −25% |
| 回调延迟 p90 | 25.2 ms | **20.6 ms** | −18% |
| 回调延迟 p99 | 29.8 ms | **22.0 ms** | −26% |
| 回调延迟 mean | 19.6 ms | **14.9 ms** | −24% |
| commit 间隔 p90 | 20.4 ms | **13.3 ms** | 节奏更稳 |
| commit 最大断档 | 125.8 ms | **81.4 ms** | 卡顿幅度减半 |

比数字更重要的是**对齐**：改前回调相位与控件 paint 无关，commit 平均要多等半个
绘制周期；改后回调在周期起点发射，commit 赶得上下一个 paint。回调延迟本身只反映
一半收益，输入→上屏的整体体感改善大于表中数字。

剩余的 p50 ≈ 15ms 是一帧深流水线的固有成本（请求落在周期内偏后的位置时，要等到
下一周期起点），与原生 Wayland 客户端一致，不再是"额外税"。

## 4. 验证

- `ninja` 全量零新增告警（`-Wcast-function-type` 一处为拆分前旧代码，未触碰）；
- `seat-selftest` 17 步全过（走定时器后备路径），两张 PNG 正常导出；
- demo + shm-client 渲染正确（1.25 分数缩放、输入十字准线均正常）；
- 改前基线以相同方法测得，具备可比性（基线构建目录已清理，
  改前 `nested.c` 备份仍在 `/tmp/nested.c.pre-split.bak`）。

## 5. 复现测量

```bash
# 客户端侧日志单独落盘（wrapper 隔离 demo 自身的 wayland 流量）
CHROMIUM_DUMP=/tmp/d.png CHROMIUM_DUMP_DELAY=4000 \
  builddir/src/cefnest-demo \
  sh -c 'WAYLAND_DEBUG=1 exec builddir/test/shm-client 2>/tmp/client-wl.log'
# 统计 frame 请求 → done 的延迟分布（解析 {Default Queue} -> 行，
# 回调 id 会复用，需按 FIFO 配对）
```

## 6. 后续可选

- shm 路径的每帧全量 `pread` 回读（~4MB/帧）仍会阻塞主循环 1~3ms，是残余抖动的
  来源之一；CEF 走 dmabuf 不受影响。可改为 mmap 常驻或按 damage 拷贝。
- 长尾（max ~80ms）来自桌面侧偶发的时钟停顿（遮挡/负载），属合成器环境噪声，
  组件侧已无可做。

## 7. 第二轮：发射点从周期头（update）挪到周期尾（after-paint）

日期：2026-10-20 晚。上一轮把回调从软件定时器挂到了 frame clock，但挂的是
**update 相位**——周期的起点。这留了一个结构性浪费：

GTK 的一个显示周期在**同一次主循环回调里顺序跑完 update → layout → paint**，
中途不回事件源。于是客户端在收到 done 之后 ~4ms 的 commit，会坐在 socket 里
错过**同一个周期**的 paint，到下一个周期才被处理、再下个周期才被显示——
**done → 上屏固定隔 2 个周期**。

把发射点挪到 **after-paint 相位**（`GDK_FRAME_CLOCK_PHASE_AFTER_PAINT`，
信号 `"after-paint"`）之后：

```
周期 N:   … paint(显示旧纹理) → after-paint(发 done)      ← 周期结尾
                  └→ 客户端拥有周期间空隙，画 3~5ms，commit
周期 N+1: 主循环处理 commit → queue_draw → paint_N+1 显示   ← 只隔 1 个周期
```

这正是真实合成器的形状（在自己上屏之后发回调，让客户端利用消隐期生产下一帧）。
GTK 4.18 源码（`gdkframeclockidle.c`）确认：单独请求 `AFTER_PAINT` 会让周期
照常按 vsync 运行、只发 after-paint（update/layout/paint 按请求位跳过），
请求位一次一消费——"发完仍有人等就再请求"的既有模式不变。

### 实测（同机同法背靠背，各 ~1200 帧）

| 指标 | update 相位 | after-paint 相位 | 解读 |
|---|---|---|---|
| 请求→done p50 | 13.7 ms | 16.6 ms | **基本持平**——它度量的是节拍，不是对齐 |
| commit 间隔 p50 | 10.9 ms | 8.7 ms | 客户端生产节奏更跟手 |
| commit 间隔 p99 | 17.0 ms | 20.3 ms | 同量级 |

回调延迟持平是符合推导的：两种相位下，客户端的请求都落在"上一个 done 之后
不久"，到下一次发射都要等约一个周期。**真正的收益是结构性的**：done 的发射
点从周期头挪到周期尾后，客户端的 commit 落进周期间空隙、必然赶上下一个
paint——**done → 上屏从 2 个周期变为 1 个周期**。对应到输入→上屏：最坏情况
从"约 2 周期 + 绘制"（~37ms）收敛到"约 1 周期 + 绘制"（~21ms），均值小幅
改善、分布明显收紧。

### 剩余的延迟地板

输入投递（≤1 周期，GTK 事件压缩）+ 客户端绘制（CEF 决定）+ 1 次 vsync 上屏。
这就是嵌套合成架构的理论下限，组件侧已无大的可挤空间。

## 8. 第三轮：时钟会沉默，客户端不能跟着停（2026-10-21）

搭 headless CI 会话（`test/ci-session.sh`，nested mutter `--headless`）之后，同一个
`test/find-gate.sh` 在真桌面 6/6 过，在 headless 里 6 次只过 2 次，失败的都是
`scrollTop was 0 after the step`。第一反应是"步进滚动是二峰的、有时慢"，于是把一次
固定延时的快照换成 15 次采样（每 600ms 一次，覆盖 9 秒）——**结果 4/5 次全程是 0**，
1 次在步进后 400ms 就已经是 2749。不是慢，是有和没有。

### 测量：页面到底有没有在出帧

在页内挂一个 rAF 计数器，同时给合成器的 frame 路径打临时 PROBE 日志：

| 指标 | 真桌面 | headless（改前） |
|---|---|---|
| 1 秒内的 rAF 次数 | 62 | **2**（此后不再增长） |
| 客户端 `wl_surface.frame` 请求 | 每次都在 1~15ms 内等到 after-paint | 全程**只有 1 次**，且从未被回答 |
| `after-paint` 发射次数 | 持续 | 全程 **1** 次（挂上时钟后 2.3s 的首个周期） |
| 客户端 commit | 持续（#1…#6+） | **2 次**之后再无 |
| `shot` 截图 | 页面 + 命中高亮 | 纯色空白 |
| find-next 后 scrollTop | 2749 | 0（等 9 秒仍是 0） |
| `wheel` 脚本步 | 有效 | 无效 |
| 页内 `scrollIntoView()` | 有效 | **有效**（2749） |

最后一行是定性的关键：主线程的即时滚动照样生效，说明 scroller、合成、提交链路都是
好的，冻结的只有**需要 BeginFrame 节拍的那一半**——查找命中点的 reveal 和滚轮。所以
这不是查找路径的问题，`docs/find-path.md` 的结论不动。

### 根因

`nested_start_frame_clock()` 只在"根本没有时钟"时才走 16ms 定时器兜底。headless
mutter 没有输出设备，GDK 此后再不产生显示周期：我们请求的 `AFTER_PAINT` 相位永远
不会有人兑现，客户端那一次 frame 请求就永远挂在队列里，Chromium 靠自己的 vsync
回退挤出十几帧后彻底停住。§7 里"单独请求 AFTER_PAINT 会让周期照常按 vsync 运行"
这句 GTK 行为的引用，前提正是**有 vsync 可等**。

### 改动：给时钟配看门狗

- `clock_alive`：挂上时钟时为真；一次 `after-paint` 回答即为真；到点没回答则为假。
- `nested_start_frame_clock()`：有时钟**且**它还活着 → 请求相位并武装一个
  `FRAME_CLOCK_WAIT_MS`（100ms，6 个名义帧间隔）的一次性看门狗；否则 → 16ms 定时器。
- `frame_clock_after_paint()`：撤销看门狗与定时器，回到时钟节拍。
- `frame_clock_tick()`：接管期间每一跳仍然照旧 `request_phase`，这样显示一旦恢复，
  一个 after-paint 就能把节拍交还给时钟，不会永远停在兜底路径上。

改完 headless 侧：rAF 回到 62/秒，步进后 scrollTop=2749，`shot` 出图与真桌面一致
（含 `belowthefold` 的橙色高亮），`test/find-gate.sh` 在 headless 会话里连过 3 次。

### 代价

§2 那条副产品——"窗口被遮挡或最小化时 GDK frame clock 停摆 → 客户端回调随之暂停"
——现在只维持 100ms，之后客户端会以兜底节拍继续画。两种"时钟沉默"（没有 vsync 的
显示 / 不在任何输出上的窗口）在组件内部无法区分：一个让整页冻结，一个只是空转，
取舍不需要犹豫。真正要暂停客户端的信号是 unmap，`chromium_view_on_unmap()` 照旧
把时钟摘掉。

