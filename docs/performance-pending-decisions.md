# 性能工作待决事项(2026-10-25,第二轮调研后重写)

> **状态(2026-10-25 执行完毕)**:第 0/1/2 节已全部落地,提交分别为
> `1cb9540`(回归修复)、`e84cec3`(dmabuf release)、`2c372c5`(遮挡心跳),
> 各节标题已标注;仍开放的只有第 3 节(显式同步,暂缓)、第 4 节(注入脚本,
> 实测否决维持)、第 5 节(P0-B shm 回退,维持降级)。本文正文保留调研证据
> 原文,执行数字见文末决策记录最后一行。

P0-A 零风险批与 P1 已收官(背景与逐条核实见仓库根的
`performance-plan-2026-10-24.md`,测量结论见 `.memory/measured-facts.md`)。
本文第一版(2026-10-25 早)按四项待决列出方向;随后按「先测再修」做了
一轮代码调研 + 实测,结论推翻了其中一项的"暂缓"前提、坐实了另一项的
设计,并**发现一个 P0-A 引入的输入路由回归**。本文是重写后的版本,
每项给出实测证据、定稿方案与回归门禁。

## 本轮结论速览(答案)

一句话:发现并定位了一个 P0-A 引入的输入路由回归(一行修复已验证、未
提交),dmabuf release 项方案定稿,遮挡窗口项从"暂缓"翻案为"可立项"
——全部有实测数字支撑。

1. **P0 回归(新发现,排第一)**:selftest `second-window` 确定性失败,
   bisect 坐实是 P0-A#7(`21e7919`)的 seat_retarget 快路径漏比较
   `input_surface`,多窗口输入改道被吞。一行修复在 worktree 验证后
   全套 24 模式转绿。
2. **dmabuf release 项定稿**:新测生产缓冲回收——8s 动画页 471 commits
   (≈56/s)全程只有 **3 个 dmabuf buffer**(三缓冲,启动 70ms 导入完),
   推迟 release 的风险上界 = +1 个 buffer(≈3.7MB)。方案定为「顶替时
   release」(swapchain 语义,不碰 after-paint),挂点与门禁见第 1 节。
3. **遮挡窗口翻案**:实测 mutter 对被完全盖住的窗口停发帧回调(时钟
   116→12→144 ticks/段),遮挡信号存在;而我们的看门狗把它抵消掉,
   被盖住的窗口页面照跑 56 commits/s。立项,带四条硬约束(活过→安静
   latch、单视图范围、心跳防死锁、看门狗一行不动),排在 release 之后
   (第 2 节)。
4. **维持原判**:注入脚本两项维持实测否决(第 4 节);P0-B 维持"仅当
   生产掉回 shm 才提前"(第 5 节);新增显式同步条目从 release 项拆出
   暂缓(第 3 节)。

**建议执行顺序**:① 提交回归修复(独立小提交)→ ② dmabuf release
(带新增 release 顺序自测断言)→ ③ 遮挡挂起单独立项排后。
(**已按此顺序执行完毕**:①→`1cb9540` ②→`e84cec3` ③→`2c372c5`,
详见各节标题与文末决策记录。)

已落地部分(不再待决):

- P0-A 十项:`65bd80e`(第 0 步测量)→ `acfbf50`(缓存扫描后置),10 个独立提交。
- P1 第六节隐藏窗口停帧:`8612aed`(per-toplevel 挂起 + `test/frame-stop-gate.sh`),
  附带 `b8fd1e6`(menu-gate 断言行改事件驱动,P0-A#6 的遗留)。
- P1 第五节两项被实测否决,未改(见第 4 节)。

本轮实测基线(上面四条结论的证据;2026-10-25,真机 GNOME/Wayland,
i915,本机构建):

| 量 | 数值 |
|---|---|
| 生产 commit 速率(动画页 8s) | 471 commits ≈ 56/s,dmabuf 100% |
| 生产 dmabuf buffer 总数 | **3 个**,启动 70ms 内导入完(三缓冲,AB24 双平面,4-tiled) |
| selftest `--perf` shm | 120/120 帧,copied=wrapped=147MB(脚本客户端全平面 damage,预期) |
| selftest `--perf` dmabuf | 60/60 帧,**全程仅 2 个 buffer**(客户端双缓冲换链) |
| 遮挡窗口的 GDK 时钟 | 可见 116 ticks/2s → **被盖住 12/4s** → 揭开 144/2s(见第 2 节) |
| selftest 全套 24 模式 | 23 过,**1 挂**(second-window,见第 0 节) |

---

## 0. 【已落地 `1cb9540`】P0-A#7 引入的输入改道回归(本轮新发现,先于一切)

- **现象**:`meson test --suite selftest` 的 `seat-selftest-second-window`
  确定性失败(3/3 重现):改道到第一个窗口后,键盘/指针焦点仍留在第二个
  窗口,且两窗口自绘的焦点色互相矛盾。
- **定位**:`git bisect` 在 worktree 里二分 d8cceac(测试引入时,过)→
  b8fd1e6(挂),第一个坏提交 = **`21e7919`(P0-A#7 "Skip seat_retarget's
  full walk",2026-10-24)**。与 8612aed 无关(在 8612aed 的父提交上同样挂)。
- **根因**:快路径(`src/nested-seat.c:307-327`)检查了 `input_surface`
  **可用**(mapped 且有 wl_surface),但从不比较它和上次全量 walk 记录的
  `retarget_input_surface`。`nested_compositor_set_input_surface()`/
  `clear_input_surface()`(多浏览器架构的输入改道,`src/nested.c:2809/:2822`)
  只改字段 + 调 `seat_retarget`,**不 bump `tree_generation`**——于是改道调用
  自己触发的 retarget 被自己的快路径吞掉。
- **生产影响**:两个共享 compositor 的浏览器窗口重叠、指针在重叠区内时,
  `set_input_surface` 的改道不生效,输入(打字/点击路由)留在错误的窗口,
  直到指针移出旧焦点边界或树变化;`clear_input_surface` 同样被吞。这是
  用户可见的多窗口输入错误,不只是测试问题。
- **修法(一行,已验证)**:快路径条件补 `self->input_surface ==
  self->retarget_input_surface &&`(`retarget_input_surface` 本来就在
  walk 结尾记录,字段已存在)。在 b8fd1e6 的 worktree 上打补丁后
  `--second-window` 转绿,全套 24 模式 0 挂。
- **门禁**:`seat-selftest-second-window` 本身就是抓到它的测试;修复随
  全套 selftest 回归即可,不需要新门禁。
- **为什么排第一**:它不是性能项但是上一轮的欠账;dmabuf release 项
  (第 1 节)与它同在 nested 代码域,门禁必须先回到全绿才有意义。

## 1. 【已落地 `e84cec3`】dmabuf release-on-commit(正确性项,方案定稿,下一提交)

- **问题**(第一版已记):`src/nested.c:1136` 在 commit 里同步
  `wl_buffer_send_release`;dmabuf 纹理直接引用客户端显存,release 之后
  客户端可改写正在显示的存储 → 撕裂/陈旧像素风险。生产 dmabuf 100%,
  每一帧都走这条路。
- **本轮新增证据**:生产 8s 动画页 471 commits(≈56/s),全程只有 **3 个
  dmabuf buffer**,全部在启动 70ms 内导入——Chromium 用固定三缓冲池,
  稳态零导入。这意味着"推迟 release 会撑深缓冲池"的风险上界就是
  **+1 个 buffer(1250×762 双平面 ≈ 3.7MB VRAM),一次性**。
- **定稿方案:「顶替时 release」(swapchain 语义),不碰 after-paint**:
  1. `NestedSurface` 增加 `BufferUser current_user`(模式抄
     `pending_user`,`src/nested.c:440`),记录当前被显示纹理所属的 buffer;
  2. commit 换上新纹理后:若 `current_user.buffer` 存在、是 dmabuf、且
     **不是**本次的 buffer → 此刻才 `wl_buffer_send_release`(旧 buffer 的
     release 推迟了恰好一拍);
  3. 本次 buffer 进入 `current_user`,**不**立即 release;
  4. `surface_clear_content`(attach NULL)与 surface 销毁时:release
     `current_user` 并清槽(buffer_resource_destroyed 对 client 侧销毁
     已经通过 `users` 链表清理,两个方向都要处理);
  5. 纹理创建失败路径(`src/nested.c:1124`)维持**立即** release(buffer
     从未上屏,没有推迟的理由);shm 路径完全不动(内容已拷贝,立即
     release 对吞吐有利)。
- **为什么选「下一帧 commit」而不是「after-paint 后 release」**:不与
  frame pacing/看门狗发生任何耦合(禁改清单),release 时点纯由 commit
  序列决定;每 buffer 仍恰好一次 release(同 buffer 重复 attach 幂等,
  不重复发)。after-paint 版本只早 ~1 帧,却把正确性挂到时钟上,不值。
- **时序安全性**:release 到达时,`self->texture` 已是新技术理;旧纹理
  只剩 GSK 上一帧节点缓存引用,而每次 commit 都经 `::frame` →
  `gtk_widget_queue_draw`(`src/chromium-view.c:479-481`)标记损伤,单线程
  模型下 commit 之后任何 paint 都会重快照、不再采样旧纹理。残余竞争
  (客户端新写 vs 我们上一帧已在途的 GPU 读)由 i915 dma-buf 隐式同步
  覆盖——与所有未实现显式同步的 compositor 同一假设;显式同步
  (linux-dmabuf v4/syncobj)记为未来加固项,不阻塞本项。
- **与第六节挂起的交互**:隐藏窗口停止 commit 后,其最后 1-3 个 buffer
  保持未 release——这正是协议语义("compositor 还在用"),Chromium 池里
  多挂几块无副作用;map 恢复后 release 随下一批 commit 流走。无需特判。
- **Chromium 侧推演**:三缓冲池下,commit(B) 归还 A,恰在 Chromium 需要
  为帧 N+2 取 buffer 之前——管线不饿。selftest 的脚本客户端双缓冲
  (`--perf` 实测 2 buffer/60 帧)同样不饿:release(A) 与 commit(B) 同时
  到达,取 A 渲染下一帧时 release 已在。若实测出现池变深,也只是 +1。
- **门禁**:
  1. `--perf` 两模式照跑,断言不变(120/120、60/60);
  2. **新增 release 顺序断言**:dmabuf 客户端已有 per-buffer release
     监听(`shm-client.c` `buffer_release`/`second_after_release` 模式),
     给 `--dmabuf` 客户端记录"attach 时的 commit 序号 vs release 到达时的
     commit 序号",断言 dmabuf buffer 的 release 严格晚于其 attach commit、
     且每个 buffer 至多一次;
  3. pixel-gate 真桌面 + headless 双侧;
  4. 真桌面动画页人工比对(撕裂无法自动断言)+ 一轮 CHROMIUM_FRAME_
     METRICS 会话对比 commit 速率(应仍 ≈56/s)与 import 总数(应仍 =3,
     至多 4)。

## 2. 【已落地 `2c372c5`】遮挡窗口:信号已实测存在,立项(独立设计,排在第 1 项后)

第一版判定「数字可观再立项」;本轮实测把两个前提都钉死了:

- **成本可观**(第一版已知):被完全盖住但仍 mapped 的窗口,页面照常
  56 commits/s——因为桌面停止调度我们的窗口时,看门狗把节奏交给
  16ms 兜底定时器,继续回答 Chromium 的帧请求。mutter 想替我们省的,
  被自己的兜底抵消了。
- **信号存在(本轮实测)**:GTK 两窗口探针(受害窗口挂 tick callback,
  全屏不透明窗口盖住 4s):受害窗口的帧时钟
  **116 ticks/2s(可见)→ 12 ticks/4s(被盖住)→ 144 ticks/2s(揭开)**。
  mutter 对完全遮挡的窗口确实停发帧回调(与 SDL #4335、GNOME frame
  clock 文档一致),"mapped 且时钟突然安静"就是遮挡信号,且解除遮挡
  后自动恢复。探针:`/tmp/occlusion-probe.c`(一次性,未入库)。

**方案约束(为什么不能直接挂上去)**:

1. headless/无 vsync 显示器上时钟**从来不响**(§8 病理)——与遮挡信号
   同形。直接"安静即挂起"会在 headless 门禁里冻死页面,正是 bcbebc8
   事故的形状。必须 latch:**"时钟曾经活过(见过 after-paint)→ 之后
   安静"才算遮挡**;headless 从不满足前半句,特性天然钝化——**全套
   既有 headless 门禁因此就是误冻结守卫**。
2. 时钟是 per-compositor 的(第一个 view 的),挂起是 per-toplevel 的:
   主窗口被盖但独立窗口可见时,不能连独立窗口的页面一起挂。第一版
   范围:**只对"compositor 恰有一个 mapped view"的会话启用**(单窗口
   是绝大多数场景);多窗口维持现状。
3. 挂起后无 commit → 无 queue_draw → 时钟可能永远不再响,存在"揭开后
   恢复不了"的死锁形状。需要**心跳**:遮挡态下每 ~500ms 应答一拍帧
   回调并 queue_draw 一次,给时钟一个醒来的机会;任何 after-paint 到达
   立即退出遮挡态。误判代价 = 可见页面掉到 2 fps 直到时钟恢复(自愈),
   而不是永久冻死。
4. 看门狗逻辑一行不动:遮挡态是**消费**看门狗已有的"时钟安静"信号,
   不是改它。

**门禁**:全套 headless 门禁(时钟从未活过 → 特性钝化 → 页面满速,即
误冻结断言)+ 真桌面人工遮挡验证(盖住后 commit 计数应掉到心跳速率、
揭开立即恢复,记录恢复延迟)+ `frame-stop-gate.sh` 照跑(它 assert 的
是 unmap 路径,不得被遮挡路径影响)。

**收益**:被遮挡/其他工作区窗口从 56 commits/s 降到 ~2/s;这是本清单里
唯一覆盖"用户拿别的窗口盖住浏览器"这一最常见遮挡的项(unmap 挂起只
覆盖最小化/隐藏)。

## 3. dmabuf 显式同步(从第 1 项拆出的未来加固项,暂缓)

第 1 节的时序安全性建立在 i915 隐式同步上(生产实测 i915 4-tiled)。
若未来出现不给隐式同步的 GPU/驱动,正确的加固是接入 linux-dmabuf v4
feedback + 显式同步,而不是把 release 推得更晚。**暂缓**,仅在真实硬件
上出现撕裂报告时重提。

## 4. 第五节注入脚本两项(实测否决,维持不变)

结论同第一版,已记入 `.memory/measured-facts.md`:MutationObserver 全
文档扫描 0.40µs/次、selectionchange 已被 Blink 按帧合并,收益噪声级,
注入脚本全体页面共用,不值得冒回归风险。重议条件不变:万节点级编辑器
页实测出可测成本再说。

## 5. P0-B shm 帧路径(维持"回退路径优化",不变)

生产稳定走 dmabuf(本轮再次实测 56/s、dmabuf 100%),整节维持降级。
触发条件、两阶段顺序、忙碌守卫要求均同第一版;补充一点:第 0 步的
"GDK 渲染器是 GL 还是 cairo"确认项仍然只在 shm 回退真正发生时才有
意义,不需要提前跑 sysprof。

---

## 决策记录

| 日期 | 决定 |
|---|---|
| 2026-10-24 | 第 0 步测量落定:生产 dmabuf 100%,P0-B 降级、第七节升为待办首位 |
| 2026-10-24 | P0-A 十项全部落地并提交 |
| 2026-10-25 | P1 第六节落地(实测修正了方案对 rAF/定时器关系的误判) |
| 2026-10-25 | P1 第五节两项实测否决,暂不改 |
| 2026-10-25 | **二轮调研:发现 P0-A#7 输入改道回归(bisect→21e7919),一行修复已验证;定为立即执行项** |
| 2026-10-25 | **二轮实测:生产三缓冲池(3 buffer/471 commits),release 推迟风险上界 = +1 buffer;release 项定稿为「顶替时 release」** |
| 2026-10-25 | **二轮实测:遮挡信号存在(时钟 116→12→144 ticks),第 2 项立项,约束 = 活过→安静 latch + 单视图范围 + 心跳,排在 release 之后** |
| 2026-10-25 | **执行完成:①回归修复已提交(独立小提交,全套 24 模式转绿);②「顶替时 release」已提交(selftest 新增 release 顺序断言,负向验证有牙,perf/pixel/frame-stop/帧指标全过,池未加深);③遮挡心跳已提交(实测 59/s→2/s→揭开即时恢复,`test/occlusion-gate.sh` 为真桌面门禁,headless 由 latch 钝化、全套 ci 电池回归通过;看门狗逻辑未动)** |
| 2026-10-25 | 复测备注:重测 import 总数时注意**按窗口尺寸分批**——窗口初建一次(1250x807)、定型再一次(1250x762),每批 3 块;上轮门禁写"应仍 =3"指稳态单尺寸,resize 后的会话总共 6 次属正常,不是池加深 |
