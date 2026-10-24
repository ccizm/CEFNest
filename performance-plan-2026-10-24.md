# CEFNest 性能提升方案（两份 AI 诊断的核实与整合，2026-10-24）

本方案基于对两份 AI 诊断的逐条源码核实。所有条目都标注了核实结果（✅ 属实 /
⚠️ 部分属实或表述偏差 / ❌ 不成立），冲突处已裁决。执行时按"第 0 步测量 →
零风险批 → 按测量结论决策大改"的顺序推进，每步用现有门禁回归，全程不碰宿主桌面。

---

## 〇、第 0 步测量结果（2026-10-24，已实测，决策已定）

测量基础设施已落地并验证：

- 4 个计数器（shm/dmabuf commits、copied、wrapped）在 `NestedCompositor`，
  getter `nested_compositor_get_commit_stats()`，session 侧有镜像；
  `CHROMIUM_FRAME_METRICS=1` 每秒输出 `demo: commits shm= dmabuf= copied= wrapped=`。
- `test/seat-selftest.c` 新增 `--perf` 模式：`--perf-frames=N` 驱动客户端画
  N 帧（在收到最后一帧回调时退出，保证每个计入帧都被显示），对计数器做
  算术门禁。实测 shm 120/120、dmabuf 60/60，全零误差；默认 52 步回归通过。

**生产环境基线（真实 CEF 会话，Intel 核显，60fps 动画页）：**

| 场景 | 每秒 commits | 路径 | copied/帧 | wrapped/帧 |
|---|---|---|---|---|
| 正常（GPU 开） | ~59 | **dmabuf 100%，shm=0** | 0 | 0 |
| CHROMIUM_HELPER_DISABLE_GPU=1 | ~60 | shm | 2.16MB | 3.81MB |

**结论（方案的决策点就此落定）：**

1. **生产走 dmabuf** → P0-B（shm 双 shadow）整体降级为"回退路径优化"，
   排到 P0-A 与 P1 之后；只有当生产环境因驱动/节点问题掉回 shm 时才提前。
2. 回退路径若真被走到，第一阶段（消 wrapped）可省 **64% 的 shm CPU 流量**
   （wrapped 3.81MB 占 copied+wrapped 5.97MB 的比值），damage 路径现有省略
   占 43%（copied 2.16MB vs 全帧 3.81MB）。
3. **dmabuf release-on-commit（第七节，正确性）在生产热路径的每一帧上**，
   优先级应提到 P1 首位；隐藏窗口停帧（第六节）同理——遮挡窗口在 dmabuf
   路径同样以 60fps 出帧。
4. 测试负载本身（shm-client 全 surface damage）copied==wrapped 属预期；
   真实页面的 damage 收益要看上面第 2 条的实测比值，不要用 selftest 客户端外推。

---

## 一、两份诊断的核实结论

### 1.1 双方一致且完全属实（可直接执行）

| 论断 | 核实位置 | 结论 |
|---|---|---|
| shm 每帧新建 GBytes + GdkTexture | src/nested.c:721-723 | ✅ `g_bytes_new` 本身就是整帧 memcpy，且新纹理对象使 GSK 指针缓存必 miss → 整帧 CPU→GPU 上传。damage 只省了 pool→shadow 那段 CPU 拷贝 |
| XRGB8888 逐像素 OR 强制 alpha | src/nested.c:663-668（damage 路径）、:708-716（全量路径） | ✅ 两处整平面循环；`GDK_MEMORY_B8G8R8X8` 可替代（字节序 B,G,R,X 正好对上 wl_shm XRGB8888） |
| 控制通道行重组 O(n²) | src/chromium-session.c:721-756、helper/src/main.cpp:549-582 | ✅ 两端同一模式：每次从 byte 0 重扫 + `erase` 整体前移。`CONTROL_LINE_MAX = 4MiB`（src/chromium-session.c:30），满载单行最坏 ~1GiB memchr 流量 |
| send_json 每命令新建 generator + 拼接 + strlen | src/chromium-session.c:964-992 | ✅ 序列化一次、g_strconcat 整串复制一次、strlen 再扫一遍，只为加一个 `\n` |
| EmitJsResult 双序列化 | helper/src/main.cpp:1409-1423 | ✅ :1411 完整 `CefWriteJSON` 只为量长度（丢弃），`Emit()` 内再序列化一次 |
| 输入路径 g_debug 实参无守卫 | src/chromium-view.c:482（motion）、:766/:783（按键，`gdk_keyval_name()` 每键必调） | ✅ `g_debug` 是真实函数调用，实参先求值；需 `G_MESSAGES_DEBUG` 才打印，但调用成本照付 |
| 拼写检查 MutationObserver 全文档扫描 | helper/src/main.cpp:104 | ✅ `subtree:true` 观察整个 documentElement，每次 DOM 变更 `querySelectorAll` 全文档；默认开启 |
| 拼写外另一注入脚本 selectionchange 无节流 | helper/src/main.cpp:110-111 | ✅ 有 `t===last` 去重，但拖选期间内容每次都变 → 逐事件 IPC；`read()` 每事件都分配字符串 |
| helper 日志 LOGSEVERITY_INFO | helper/src/main.cpp:2589 | ✅ INFO 每条 stderr + debug.log 双写 |
| 遮挡/隐藏窗口全速渲染 | src/chromium-view.c:408 → src/nested.c:2939-2943 | ✅ **且有既有实测**：遮挡窗口 rAF 61.5/秒（.memory/measured-facts.md，2026-10-21）；unmap 摘时钟后 16ms 兜底定时器继续回答帧回调 |
| dmabuf commit 即 release 有撕裂风险 | src/nested.c:1087 | ✅ dmabuf 纹理引用客户端存储，立即 release 允许客户端改写正在显示的显存；shm 不受影响（内容已拷贝）。正确性问题，性能之外独立立项 |
| 帧路径骨架（预导入、after-paint+看门狗、单线程）健康 | src/nested-dmabuf.c:621-623、src/nested.c:1972-2042、:2658 | ✅ 属实，且 frame pacing 事故有实测记录（bcbebc8，整页冻死，.memory/measured-facts.md §8/§9），列入禁改清单 |

### 1.2 有偏差或错误的论断（执行时修正）

| 论断 | 核实结果 |
|---|---|
| A："deb/rpm 都是 --buildtype=release，构建没有便宜可捡" vs B："ci-build.sh 未指定 buildtype，继承 debugoptimized" | **各对一半**。打包树确实是 release（scripts/build-packages.sh:104、:295）；但 scripts/ci-build.sh:64 的构建树（即 cefnest-build.tar.gz 发布附件里的二进制、CI 门禁跑的树）**未指定 buildtype，是 debugoptimized（-O2 -g、断言开启）**。这是一个真实缺口：发布 tarball 里的二进制当前不是 release 配置 |
| A："300ms 定时器只做 g_debug，生产里是死重" | ⚠️ 表述误导。src/chromium-session.c:1660 是 `g_timeout_add_once` —— 每次开右键菜单**一次性**触发，非周期死重；但回调 `menu_report_size`（:1497-1509）确实纯 debug。结论：顺手删掉或并入 debug 守卫即可，不算性能项 |
| A："每次 shm 拷贝两次 fstat（:602/:642）" | ⚠️ 实为每 commit 一次（`pool_map_is_whole`，src/nested.c:564 起；copy_pool_rect :641 与 read_pool :601 是互斥分支）。~1µs 级，全项目最低优先 |
| A/B："每个 motion 事件建完整 paint stack + 上千次 flush 系统调用" | ⚠️ 高估但方向正确。hit-test 走的是**当前视图的树**（toplevel + popups + 拖拽图标，通常 1-4 个节点），不是全部 surface；`wl_display_flush_clients` 对空缓冲不产生 write 系统调用。真实成本：每 motion 两次小 GList 构建/释放（keyboard_target 一次 + surface_at 一次）+ seat_retarget 无条件 flush（通常空转）+ motion 一次真实 write。1000Hz 下是数千次小分配 + 1000 次 write/s，值得修，但收益是微秒级不是毫秒级 |
| B："双 shadow + `g_bytes_new_with_free_func`，零额外拷贝" | ⚠️ 方案可行但**缺关键规格**：GDK 纹理会持有 GBytes（GSK 缓存、dump 路径 `nested_surface_get_texture` 都可能延长其生命），直接轮换两块 shadow 会在纹理仍存活时改写其背ing 内存 → 陈旧像素/撕裂。必须加**忙碌守卫**：free_func 里标记 shadow 可复用；两块都忙时回退到现在的 `g_bytes_new` 拷贝路径。另注意它只消灭 memcpy 半边，整帧 GPU 上传仍在 |
| B："内核 udmabuf 终极零拷贝" | ⚠️ 技术上成立但过度工程：依赖 `/dev/udmabuf`、运行时探测、与 GTK 纹理生命周期的复合。仅在"生产确实主走 shm 且前两阶段不够"时再议。暂缓 |

### 1.3 两份报告都没说对的补充（本次核实新增）

- `seat_retarget`（src/nested-seat.c:365-465）在**每次 motion** 都被调用，且结尾无条件 `nested_flush`（:464）；`nested_compositor_notify_pointer_motion`（:676-716）随后再 flush 一次（:715）。早退条件（坐标仍在当前 focus bounds 内且 popup 集合未变）能同时省掉两次栈构建和一次多余 flush。
- 测量基础设施现状：只有 `CHROMIUM_FRAME_METRICS`（src/main.c:3430）和 `nested_compositor_get_frame_count`；**没有 shm/dmabuf commit 计数器**，无法证明生产环境走哪条路径。两份报告的"先补测量"一致且必要。
- `.memory/measured-facts.md` 已有现成测量方法可复用：`CHROMIUM_HELPER_DISABLE_GPU=1` 可强制走 shm 路径对照；`G_MESSAGES_DEBUG` 挂 `nested` 域读合成器日志；rAF 计数脚本在 test/real-site-run.sh。

---

## 二、第 0 步：补测量（先于一切优化，一次会话可完成）

1. **四个计数器**加进 `NestedCompositor`（shm commits / dmabuf commits / shm 损伤拷贝字节累计 / 全帧拷贝+上传字节累计），暴露 getter，并入 `CHROMIUM_FRAME_METRICS=1` 的周期输出。不加新环境变量（符合项目惯例）。
2. **test/seat-selftest.c 加 `--perf` 步骤**：进程内 compositor + 脚本化客户端跑固定帧数，输出每帧均值（拷贝、上传、commit 分类）。selftest 不碰宿主桌面，人在用机器时安全可跑。
3. **sysprof 抓一段滚动动画**：GTK4 自带 marks，直接看 snapshot 与上传占比；同时确认生产环境的 GTK 渲染器是 GL 还是 cairo 软件回退（决定 P0-B 第二阶段的收益上限——cairo 回退下 dmabuf 纹理会触发每帧 CPU readback，是灾难级）。
4. **拿到基线数字**：真实 CEF 会话（不设 DISABLE_GPU）的 shm/dmabuf commit 比。这个数字直接决定 P0-B 的优先级：
   - 稳定走 dmabuf → P0-B 整体降级为"回退路径优化"，只做第一阶段或不做；
   - 掉回 shm（无 render node、GDK 格式表缺项、驱动问题）→ P0-B 升为最高收益项。

---

## 三、P0-A：零风险批（无需等测量，每项独立可回归）

按收益/风险排序，建议拆成独立提交：

| # | 项目 | 位置 | 修法 | 回归门禁 |
|---|---|---|---|---|
| 1 | 发布构建配置 | scripts/ci-build.sh:64 | `meson setup builddir` 补 `--buildtype=release`。当前发布 tarball（cefnest-build.tar.gz）里是 debugoptimized 二进制 | ci 构建树跑全套门禁 |
| 2 | XRGB8888 去 alpha OR | src/nested.c:684（format 判定）、:663-668、:708-716 | XRGB8888 改用 `GDK_MEMORY_B8G8R8X8`，删两处 OR 循环（meson.build:10 已要求 gtk4 >= 4.14，编译期即可确认枚举存在） | pixel-gate **真桌面 + headless 双侧**，另验 X8 在 GL 渲染器下不被着色器转换 |
| 3 | 控制通道行重组 | src/chromium-session.c:721-756、helper/src/main.cpp:549-582 | 记住 `scan_offset` 只扫新增字节；head/tail 双索引替代 `g_string_erase`/`buffer.erase`，一批 chunk 切完只压缩一次。两端同改 | framing-gate（必跑，它就是打这条路径的）+ event-shape-gate |
| 4 | send_json 两段写 | src/chromium-session.c:964-992 | 生成 JSON 后直接写 `json`，再写单字节 `"\n"`（或 writev 一次），删 g_strconcat + strlen | framing-gate |
| 5 | EmitJsResult 单序列化 | helper/src/main.cpp:1389-1423 | 序列化 value 一次、用结果长度判 64KiB，`Emit` 复用该文本 | js-result 相关 selftest + menu-gate |
| 6 | g_debug 守卫 | src/chromium-view.c:482、:766、:783；src/nested.c:1084 | 进程启动时缓存一次 `static const gboolean dbg = !!g_getenv("G_MESSAGES_DEBUG")` 之类的布尔，守卫掉热路径 g_debug；`menu_report_size`（src/chromium-session.c:1497）纯 debug 一次性定时器，直接删 | seat-selftest + pixel-gate |
| 7 | seat_retarget 早退 | src/nested-seat.c:365-465 | 坐标仍在当前 pointer_focus 的 bounds 内且 popup 集合未变 → 跳过两次栈构建；无变化时不再走结尾的 flush | seat-selftest（45 步）+ dnd-gate + menu-gate |
| 8 | helper 日志降级 | helper/src/main.cpp:2589 | `LOGSEVERITY_INFO` → `LOGSEVERITY_WARNING`。注意这是行为变化，需同步中英文 README（如保留 CHROMIUM 环境变量表的话加一行开关说明） | sandbox-soak / ci-session 观察 stderr 减少 |
| 9 | a11y D-Bus 改 async | src/chromium-session.c:200-222、:2075-2080 | `g_dbus_proxy_new_for_bus_sync` → `*_async`。注意时序：async 回来时 helper 可能已连接，`session_send_a11y` 反而能真正送达；`a11y_forced` 语义不变 | ci-session a11y 步骤 |
| 10 | 缓存扫描后置 | helper/src/main.cpp:2568-2576 | `PruneStaleCacheDirs` 移到 `CefInitialize` 之后（Start() 之前/之后皆可，别在首条命令前阻塞太久） | ci-session 启动序列 |

预期收益：#1 让发布物和测量基线都回到 release 配置；#2 消掉 XRGB 回退路径每帧一遍全平面写；#3/#4/#5 把大 js-result 的 CPU 从 O(n²) 降到 O(n)；#6/#7 削 1000Hz 输入下的主循环占用。**每项都不改变可观察行为（除 #8 日志量）。**

---

## 四、P0-B：shm 帧路径（等第 0 步结论再动工）

现状每 commit 的完整成本链（已核实）：

```
damage 矩形 pool→shadow 拷贝（局部，已优化）
  + g_bytes_new 整帧 memcpy        ← 1080p ARGB 8.3MB/帧
  + 新 GdkTexture → GSK 缓存 miss → 整帧 CPU→GPU 上传   ← 60fps 约 500MB/s
  + 每秒 60 次纹理对象分配/销毁
```

若生产走 dmabuf：**整节降级为回退路径优化，只做第一阶段或跳过**。

若生产走 shm，分两阶段：

**第一阶段（局部改动，消整帧 memcpy）**
- 双 shadow 交替 + `g_bytes_new_with_free_func` 包住后台 shadow 交给 `gdk_memory_texture_new`。
- **必须带忙碌守卫**（B 方案缺失的一环）：free_func 触发时标记该 shadow 可复用；两块都仍被存活纹理持有（GSK 缓存 / dump 路径延寿）→ 本帧回退现在的 `g_bytes_new` 拷贝，保证正确性永不牺牲。
- 代价：2× shadow 内存；收益：稳态下每帧省一次整帧 memcpy。

**第二阶段（大改，消整帧上传）**
- 持久 GL 纹理，每 commit 只对 damage 矩形 `glTexSubImage2D`，用 `gdk_gl_texture_new()` 包装进 GTK 渲染器。
- 风险与回退：需拿到与 GTK 渲染器兼容的 GL context；**GTK 退到 cairo 软件渲染器时必须自动回落现路径**（这也是第 0 步要确认渲染器的原因）。
- 回归：pixel-gate 双侧 + 新增 damage 正确性用例（部分更新后整图校验）。

**udmabuf 零拷贝**：暂缓。仅当"生产主走 shm 且两阶段后带宽仍是瓶颈"再评估（需 /dev/udmabuf 探测 + 能力 + 与纹理生命周期的复合验证）。

---

## 五、P1：helper 注入脚本的页面运行时开销

1. **MutationObserver（helper/src/main.cpp:104）**——注入脚本里最实在的一处：只处理 `MutationRecords` 的 `addedNodes`（对新增子树内部查询），或对 `fix()` 做 100ms 合并。聊天流/无限滚动/编辑器类页面从"每次变更大半个文档遍历"降到常数级。门禁：spellcheck-page + pixel-gate。
2. **selectionchange 节流（helper/src/main.cpp:110-111）**：renderer 侧加 30-50ms 合并定时器，**首事件立即发**保证响应感；保留现有 `t===last` 去重与 64KiB 截断。门禁：primary-gate。

---

## 六、P1：隐藏/遮挡窗口停帧

这是仓库里**证据最强**的一项：遮挡窗口 rAF 61.5/秒已有实测（.memory/measured-facts.md，2026-10-21）；代码路径 `chromium_view_on_unmap`（src/chromium-view.c:408 摘时钟）→ `nested_compositor_set_frame_clock(NULL)`（src/nested.c:2939-2943 立即武装 16ms 兜底定时器）→ 被隐藏页面只要有 rAF 就继续 60fps 出帧：shm 拷贝、GPU 渲染、电池全部照付。

- **修法**：unmap 时完全停止应答帧回调（兜底定时器也不开），map 时恢复。
- **与 §8/§9 事故不冲突**：那次（bcbebc8）是"窗口可见但时钟不回答"导致整页冻死；本次改的是"窗口不可见时不回答"。看门狗与 after-paint 逻辑本身一行不动。
- **Chromium 侧行为**：宿主从不告知 CEF 可见性，页面 rAF 不会自停——这正是需要我们停的原因（Chromium 对真正隐藏的 tab 本来就停 rAF，此改动只是补上等价语义）。
- **回归**：pixel-gate 真桌面 + headless 双侧（含"重映射后继续动"）；复用 test/real-site-run.sh 的 `__raf` 计数法断言"unmap 后 rAF 归零、map 后恢复"。

---

## 七、P1（正确性，独立立项）：dmabuf release 时机

src/nested.c:1087 在 commit 里同步 `wl_buffer_send_release`。shm 安全（内容已拷入 shadow）；dmabuf 纹理引用的**就是客户端那份显存**，立即 release 等于告诉 Chromium"可以画了"，而它可能还是当前正在显示的帧 → GPU 路径下可能撕裂。

- **修法**：dmabuf 的 release 延迟到该 buffer 被下一次 commit 顶替时（swapchain 语义）；shm 维持现状（对吞吐有利）。
- **回归**：滚动/动画门禁观察撕裂；seat-selftest 的 buffer 生命周期步骤补一条"release 不早于顶替"断言。
- 与第六节有交互（停帧后 buffer 何时 release），建议两者在同一轮设计、分开提交。

---

## 八、P2：微观与顺手清理

| 项 | 位置 | 说明 | 优先级理由 |
|---|---|---|---|
| paint stack 复用 | src/nested.c:2798-2835、src/chromium-view.c:112-150 | compositor 缓存扁平化栈，`nested_surfaces_changed`/映射变化时失效 | 每视图栈仅 1-4 节点，收益=免分配，非数量级 |
| fstat 缓存 | src/nested.c:564、:641 | pool 尺寸确认可缓存，仅触碰 pool 尾页或 resize 后重验 | 每 commit 一次 ~1µs，全项目最低 |
| b_lto | meson 配置 | 库很小，LTO 便宜；顺手在打包树开启验证 | 一次性 |
| 大负载异步写 | src/chromium-session.c:993 | >8KiB 的 js-result 主线程同步写 | 对端活跃时无问题，收益有限，不强求（同意 B） |

---

## 九、明确不碰的

1. **frame pacing**（src/nested.c:1972-2042）：after-paint 派发 + 看门狗 + 16ms 兜底。bcbebc8 事故实测把页面冻在 2fps/整页冻死，代码注释与 .memory §8/§9 都记了案。改这里=重演事故。
2. **dmabuf 预导入时机**（src/nested-dmabuf.c:621-623）：故意在 create 时导入，让 client 还能回退 wl_shm 时就拿到失败。注释即理由。
3. **单线程无锁模型**（src/nested.c:2658 GUnixFDSource 进主上下文）：无锁无竞争是优点，不为"多线程"引入锁。
4. **scale snap-back 快路径**（src/chromium-view.c:90-93）：1.25x 缩放下走 scale==1 不重采样路径，已优化好。

---

## 十、执行顺序

1. **第 0 步**：计数器 + selftest `--perf` + sysprof + 渲染器确认 → 产出基线报告（哪个环境走哪条路径、每帧真实字节数）。
2. **P0-A 零风险批**（上表 #1-#10，独立提交，一次或两轮做完）。
3. **按基线结论决策 P0-B**：shm 是主路径 → 做第一阶段（双 shadow + 忙碌守卫），测收益后再决定第二阶段（GL 持久纹理）；dmabuf 是主路径 → P0-B 降级跳过。
4. **P1 三项**（隐藏停帧 / MutationObserver / release 时机），每项单独提交 + 对应门禁。
5. **P2 随手清理**。

每步回归套餐：`seat-selftest`（45 步）+ `pixel-gate.sh` 真桌面与 headless 双侧 + 涉及路径的专项门禁（行重组→framing-gate；菜单→menu-gate；选区→primary-gate；拖拽→dnd-gate）。全程 selftest 进程内跑，不影响正在使用的桌面。

---

## 附：两份原诊断的可用性评价

- **报告 A**（第一份）：源码定位准确率高（12/14 核实属实），"明确不要碰"清单全部成立且有事故记录背书；两处偏差（300ms 定时器定性、fstat 次数）不影响其主体结论。行重组、双序列化、g_debug、XRGB8888 四项是它的独有贡献。
- **报告 B**（第二份）：独有贡献是 `g_bytes_new` 整帧 memcpy 的发现（A 漏了）、ci-build.sh 构建配置缺口（A 否认错了）、MutationObserver/selectionchange 两处注入脚本开销、隐藏窗口停帧。双 shadow 方案方向对但必须补忙碌守卫；udmabuf 过早。
- 两份报告在"先补测量、确认 dmabuf vs shm、别碰 frame pacing"上完全一致——这三点就是本方案的骨架。
