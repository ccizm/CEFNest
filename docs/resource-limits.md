# 资源上限：给客户端可发的请求设界

日期：2026-10-20，§2 补于 2026-10-21
问题：任务 #23、#83。合成器跑在宿主进程里，客户端能请求它分配的东西没有上限：一个
超大的 shm pool 就能让宿主每帧拷贝几 GB；一次失败的 dmabuf 导入会把平面描述符
留在原地；`wl_resource_create` 返回 NULL 时下一行就是
`wl_resource_set_implementation(NULL, …)`。

## 1. shm pool 上限

`NESTED_SHM_MAX_POOL = 256 MiB`（`src/nested-internal.h`）。`create_pool` 和
`resize` 都查，超了发 `WL_SHM_ERROR_INVALID_FD` 并关掉刚收到的 fd——与 Weston 的
`max_size` 检查同一做法。

为什么必须有：shm 缓冲在**首次 commit/尺寸变化**时从池里整块 `pread`/`mmap` 出来；稳定后按
`damage` 只复制一块矩形（`buffer_create_shm_texture` + `copy_pool_rect`），池子大小仍决定
首次分配和 mmap 的地址空间。
一帧 4K ARGB 约 33 MB，256 MB 还能放得下好几块，正常客户端碰不到。

这条只量单个池，量不到总量——见 §2。

顺带修掉一个整数溢出：`stride < width * 4` 用 int32 算，width 很大时会回绕成负数
从而通过校验。现在两边都提升到 int64，并且给宽高加了 65535 的界——16 位尺寸是
任何窗口都用不到的天花板，同时让下面所有 `stride * height` 的乘法天然安全。
dmabuf 侧的 `create` 加了同样的宽高界（`INVALID_DIMENSIONS`），因为
`buffer->stride` 是从 uint32 的平面 stride 截到 int32 的。

## 2. 每条连接的 shm 总额

`NESTED_SHM_MAX_CLIENT = 4 × NESTED_SHM_MAX_POOL = 1 GiB`。记账在
`NestedCompositor.shm_budget`（`struct wl_client *` → 字节数）：
`create_pool` 记整个池，`resize` 记增量，`pool_unref` 归还。归还走的是
`Pool.client`/`Pool.compositor` 两个新字段，所以表项的寿命和池一样，不需要
在连接销毁时单独清；dispose 里这张表**在 `wl_display_destroy_clients()` 之后**
销毁，因为归还本身要往表里写。

被拒时发的还是 §1 那条 `WL_SHM_ERROR_INVALID_FD`，话里带上已持有的量：

```
wl_shm#5: error 2: this client holds 1048576000 bytes, and 104857600 more is
over the 1073741824 byte pool limit for one client
```

为什么是这个数：先看真实客户端用多少。一次真 shm 会话（`G_MESSAGES_DEBUG=all`，
每条 `create_pool` 打一行累计量）——`CHROMIUM_HELPER_DISABLE_GPU=1`，窗口
1000x653，页面加载后 dump 两帧——峰值只有 **5 224 000 字节（4.98 MB）**，出现在
送画面的那条连接上：它在两块 2.6 MB（1000×653×4）的池之间来回，第三块永远在前一
块销毁之后才出现。另一条连接全程 2 304 字节，正好一块 24×24。把同样的两块缓冲换
成 4K 是 66 MB，与 1 GiB 差 16 倍——上限要挡的是"开十个 250 MB 池"的客户端，不是
正常页面。

上限管的是"一条连接能让宿主读多少"：首次整块读取/mmap 的成本按池算，一个客户端
开十个 250 MB 的池就是每帧 2.5 GB。

`shm-client --pool-budget` 是它的门禁：连续提 16 个各自合法的 100 MB 池、都不销毁
（销毁就是把字节还回去，那不是这条要测的），要求宿主**接下一拒一**——全接是漏，
连第一个都不接同样判失败，客户端总得画得出一帧。实测接 10 个、拒第 11 个，
与 1 GiB / 100 MB 相符。它作为 `seat-selftest-pool-budget` 注册在
`test/meson.build` 的 mode 列表里。

对照组：把 `NESTED_SHM_MAX_CLIENT` 临时改成 4096 倍再跑同一道题，

```
client: the compositor took all 16 pools it was sent (1600 MB) and refused none
client: …: the shm budget gate FAILED: the compositor granted 16 of the 16 pools
it was sent
```

也就是说这道题不是"总能过"的那种——它过是因为有人拒了。同一道题在真上限下的输出：

```
client: the compositor took 10 pools of 100 MB and refused the next one, so
1000 MB is what one connection may hold
```

## 3. 导入失败的平面描述符

`zwp_linux_buffer_params_v1.create` 的原本顺序是：把平面从 params 转交给
Buffer → **建 wl_buffer 资源** → 导入纹理 → 失败就 `send_failed` 返回。

失败时那个资源已经建好了，但客户端从不知道它的存在（非 immediate 的 `create`
用的是 id 0），于是资源和它持有的平面 fd 一起被搁到连接结束。客户端探测格式
（挨个试 fourcc/modifier 组合，CEF 和 Chromium 都会这么干）时，每失败一次就留下
最多 4 个描述符。

改法是把顺序倒过来：**先导入，成功了才建资源**。失败路径变成纯粹的本地清理
（`buffer_discard` 关掉平面、释放 Buffer），不再需要一个"销毁一个客户端不知道的
资源"的分支——那种销毁还会朝客户端发一个它认不出的 `delete_id`。

## 4. `wl_resource_create` 的 NULL

32 处调用（拆文件后各模块都有）此前一律不检查。新增
`resource_create()`（`src/nested-internal.h`）：失败即 `wl_client_post_no_memory`
并返回 NULL，调用点统一 `if (!x) return;`。

在需要交接所有权的几处，bail 之前把已经拿到的东西还回去：pool 建不出来就
`pool_unref`（它负责关 fd），buffer 建不出来就 `pool_unref + g_free`，
params 建不出来就 `g_free (params)`。`buffer_resource_new()` 现在会返回 NULL，
唯一的调用方（dmabuf）按上面的失败路径处理。

## 5. 验证：`shm-client --refused` 与 `--pool-budget`

新增一个"专门提非法请求"的客户端模式，跑两件事然后退出：

1. 40 次用 `fourcc=0x12345678` 的 dmabuf 导入（平面是真的 gbm bo 导出的 fd）；
2. 一个 300 MB 的 shm pool——放在最后，因为它的答复是杀掉这条连接。

`seat-selftest` 结束时打印自己的描述符数量（合成器就跑在这个进程里，客户端递过来
的 fd 都记在它账上）。2026-10-21 重测，脚本已经有 45 步：

| 运行 | 结束时相对开始 |
|---|---|
| 普通客户端 | +31 |
| `--refused` | +26 |
| `--pool-budget` | +26 |
| `--refused`（修好后，当时） | +13 |
| `--refused`（临时把 `buffer_discard` 的 close 去掉，当时） | **+53** |

40 次失败导入的净成本因此是 0——那 40 个描述符确实被关掉了；对照组说明计数器不是
噪声。上限那一半的证据在日志里：

```
wl_shm#5: error 2: pool of 314572800 bytes is over the 268435456 byte limit
error in client communication (pid …)
client: 40 of 40 impossible imports were refused
client: the compositor refused the 300 MB pool and dropped us
```

被拒之后 selftest 的 45 步照样跑完，说明杀掉一条越界的连接不会影响合成器本身。
`--pool-budget` 走的是同一条路：客户端在第 11 个池被杀，之后 selftest 照样跑到最
后一步，退出码 0。

## 6. 还没做的

- shm 的 mmap + damage shadow 已落地（`docs/frame-metrics.md` §2）；纹理仍是完整
  `GdkMemoryTexture`，所以“零拷贝”还没有，damage 只省 pool 读取；
- 无头 selftest 里 GDK 未初始化，所以 40 次失败都停在"没有 display"这一步，
  并没有真正走到 EGL 导入失败；等 #29 让 selftest 真的导入 dmabuf 之后，
  这条路径才会被完整覆盖（描述符泄漏的验证不受影响：两条失败分支都在
  `buffer_create_dmabuf_texture` 返回 NULL 之后）。
