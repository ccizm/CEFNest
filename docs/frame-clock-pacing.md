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
