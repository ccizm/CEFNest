# 资源上限：给客户端可发的请求设界

日期：2026-10-20
问题：任务 #23。合成器跑在宿主进程里，客户端能请求它分配的东西没有上限：一个
超大的 shm pool 就能让宿主每帧拷贝几 GB；一次失败的 dmabuf 导入会把平面描述符
留在原地；`wl_resource_create` 返回 NULL 时下一行就是
`wl_resource_set_implementation(NULL, …)`。

## 1. shm pool 上限

`NESTED_SHM_MAX_POOL = 256 MiB`（`src/nested-internal.h`）。`create_pool` 和
`resize` 都查，超了发 `WL_SHM_ERROR_INVALID_FD` 并关掉刚收到的 fd——与 Weston 的
`max_size` 检查同一做法。

为什么必须有：shm 缓冲在**每次 commit** 时从池里整块 `pread` 出来
（`buffer_create_shm_texture`），所以池子大小直接决定宿主每帧分配和读取多少。
一帧 4K ARGB 约 33 MB，256 MB 还能放得下好几块，正常客户端碰不到。

顺带修掉一个整数溢出：`stride < width * 4` 用 int32 算，width 很大时会回绕成负数
从而通过校验。现在两边都提升到 int64，并且给宽高加了 65535 的界——16 位尺寸是
任何窗口都用不到的天花板，同时让下面所有 `stride * height` 的乘法天然安全。
dmabuf 侧的 `create` 加了同样的宽高界（`INVALID_DIMENSIONS`），因为
`buffer->stride` 是从 uint32 的平面 stride 截到 int32 的。

## 2. 导入失败的平面描述符

`zwp_linux_buffer_params_v1.create` 的原本顺序是：把平面从 params 转交给
Buffer → **建 wl_buffer 资源** → 导入纹理 → 失败就 `send_failed` 返回。

失败时那个资源已经建好了，但客户端从不知道它的存在（非 immediate 的 `create`
用的是 id 0），于是资源和它持有的平面 fd 一起被搁到连接结束。客户端探测格式
（挨个试 fourcc/modifier 组合，CEF 和 Chromium 都会这么干）时，每失败一次就留下
最多 4 个描述符。

改法是把顺序倒过来：**先导入，成功了才建资源**。失败路径变成纯粹的本地清理
（`buffer_discard` 关掉平面、释放 Buffer），不再需要一个"销毁一个客户端不知道的
资源"的分支——那种销毁还会朝客户端发一个它认不出的 `delete_id`。

## 3. `wl_resource_create` 的 NULL

32 处调用（拆文件后各模块都有）此前一律不检查。新增
`resource_create()`（`src/nested-internal.h`）：失败即 `wl_client_post_no_memory`
并返回 NULL，调用点统一 `if (!x) return;`。

在需要交接所有权的几处，bail 之前把已经拿到的东西还回去：pool 建不出来就
`pool_unref`（它负责关 fd），buffer 建不出来就 `pool_unref + g_free`，
params 建不出来就 `g_free (params)`。`buffer_resource_new()` 现在会返回 NULL，
唯一的调用方（dmabuf）按上面的失败路径处理。

## 4. 验证：`shm-client --refused`

新增一个"专门提非法请求"的客户端模式，跑两件事然后退出：

1. 40 次用 `fourcc=0x12345678` 的 dmabuf 导入（平面是真的 gbm bo 导出的 fd）；
2. 一个 300 MB 的 shm pool——放在最后，因为它的答复是杀掉这条连接。

`seat-selftest` 结束时打印自己的描述符数量（合成器就跑在这个进程里，客户端递过来
的 fd 都记在它账上）：

| 运行 | 结束时相对开始 |
|---|---|
| 普通客户端 | +18 |
| `--refused`（修好后） | **+13** |
| `--refused`（临时把 `buffer_discard` 的 close 去掉） | **+53** |

40 次失败导入的净成本因此是 0——那 40 个描述符确实被关掉了；对照组说明计数器不是
噪声。上限那一半的证据在日志里：

```
wl_shm#5: error 2: pool of 314572800 bytes is over the 268435456 byte limit
error in client communication (pid …)
client: 40 of 40 impossible imports were refused
client: the compositor refused the 300 MB pool and dropped us
```

被拒之后 selftest 的 24 步照样跑完，说明杀掉一条越界的连接不会影响合成器本身。

## 5. 还没做的

- shm 每帧整块 `pread`（约 4 MB/帧）仍是主要成本，上限只是止血；按 damage 拷贝
  或改 mmap 常驻是正解，记在 `docs/frame-clock-pacing.md` 的后续里；
- 没有"每客户端总内存"配额：一个客户端开很多个 250 MB 的池仍然可以撑爆宿主；
- `--refused` 目前只能手工跑，等 #26 把它接进 `meson test`；
- 无头 selftest 里 GDK 未初始化，所以 40 次失败都停在"没有 display"这一步，
  并没有真正走到 EGL 导入失败；等 #29 让 selftest 真的导入 dmabuf 之后，
  这条路径才会被完整覆盖（描述符泄漏的验证不受影响：两条失败分支都在
  `buffer_create_dmabuf_texture` 返回 NULL 之后）。
