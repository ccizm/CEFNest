# 帧计量与 software-path shm 拷贝

## 1. 可选帧导出

`chromium_session_get_frame_count()` 返回 session 启动以来合成器呈现的顶层帧数。
它是计数器，不是采样率；宿主可以自己按固定间隔读：

```c
uint64_t frames = chromium_session_get_frame_count (session);
```

demo 的两条无头入口：

- 脚本步骤 `frames`：打印 `demo: frames <n>`；
- 环境变量 `CHROMIUM_FRAME_METRICS=1`：每秒打印 `demo: frame-count <n>`。

真实站点巡检的 `readback.json` 用页面自己的 `requestAnimationFrame` 计数，和这里的
compositor 帧计数是两个不同层次；需要区分“页面在跑”与“合成器在出帧”时同时看两边。

## 2. shm 软件路径

CEF 通常走 dmabuf，shm 是软件回退，也是 `shm-client` 的路径。原实现每次 commit 对整块
pool `pread()`，README 曾把一次 1000×653 窗口约 4 MB/帧列为主要残余成本。

现在：

- pool 在创建/`resize` 时 `mmap(MAP_SHARED, PROT_READ)`，不再每帧系统调用；
- `wl_surface.damage` / `damage_buffer` 累积成一个矩形；
- surface 保留一块 `shm_shadow`；尺寸/stride/format 不变时只把 damage 矩形从 mmap
  复制进 shadow；
- `wl_surface.attach` 后仍用 shadow 生成一张完整 `GdkTexture`（GTK 没有公开的可变
  memory texture），所以**省下的是 pool 读取，不是纹理上传**；第一条 commit、尺寸/
  stride/format 改变时仍是全量读；
- 非 dmabuf 的 shm 帧不会因为 damage 为空就跳过呈现：shadow 保留上一帧，这符合
  “no damage = no pixel change”。

`tests/seat-selftest` / `shm-client` 的 shm 模式会覆盖这条路径；CEF 的 dmabuf 路径不受影响。

### 2.1 damage 用什么单位说话，取决于哪一次 commit

`wl_surface.damage` 是 surface 坐标，`wl_surface.damage_buffer` 是 buffer 像素，而 shadow
只认 buffer 像素。把前者变成后者的乘数是 `wl_surface.set_buffer_scale` —— 但它和 damage
一样是双缓冲的：请求属于「下一次 commit 生效」那一半状态。于是同一个 commit 里两条请求的
先后顺序并不决定结果，决定结果的是 commit 时刻刚生效的那个 scale。

`libwayland` 把两种单位各存进一个 pending 矩形（`pending_damage_surface` /
`pending_damage_buffer`），`surface_commit()` 顶部先 `self->scale = self->pending_scale`，
再由 `surface_apply_pending_damage()` 用刚生效的 scale 换算并求并集，最后才是
`buffer_create_shm_texture()` 的局部复制。乘法全部走 `gint64` 再 `CLAMP`
到 `DAMAGE_COORD_LIMIT`：矩形是客户端挑的，`2^30 × 4` 在 C 里不是「一个错的数」，而是编译
器随便读的权限。

weston 与此一致：`libweston/compositor.c` 把 damage 原样存进 `pending.damage_surface`，
`surface-state.c` 在 commit 时用刚应用的 scale 换算；mutter 的
`meta-wayland-surface.c` 同样在 surface 坐标里累加、在 commit 时转换。也就是说，被 2026-10-22
那份 review 当成缺陷的顺序（「set_buffer_scale 先于 damage 就错」）方向是反的：真正会出错的
只有「同一轮里 damage 先写、scale 后写」，而且错法是**少覆盖**（新 buffer 只读了一角，其余
继续显示上一帧），不是它说的花屏或尺寸错乱。Chromium Ozone 先发 scale 再发 damage，所以真实
流量掩盖了这个差别。

### 2.2 门禁：`seat-selftest-damage-scale`

`shm-client --damage-scale` 在窗口画了几帧之后接管这个 surface：先提交一张 200×200 全红
buffer（scale 1，全量 damage），再提交一张同样 200×200 的全绿 buffer，这一轮 damage 写的是
`50x50` 的 **surface** 像素，紧接着 `set_buffer_scale(2)`。此后 `draw()` 被
`damage_scale_holding` 挡住，普通绘制不会再补一帧把证据盖掉。

正确的读法是 buffer 的 `(0,0,100,100)`，于是 selftest 在导入的纹理上读三个点：

| 像素 | 期望 | 读到别的值时说明什么 |
| --- | --- | --- |
| (25,25) | 绿 | 这一帧根本没到屏幕 |
| (75,75) | 绿 | 到达即乘 scale：只读了 `(0,0,50,50)` |
| (150,150) | 红 | 整块重读：这条 run 没走局部复制，两个 scale 哪个错都看不出来 |

第三行是这条门禁能咬人的关键：只检查受损区域的话，「永远全量读」这个昂贵的实现反而永远绿。

两个方向都验过。把 `surface_damage()` 改回到达即乘 `self->scale`：

```
selftest: damage-scale gate FAILED: pixel (75,75), inside it only once the scale this
commit applied is used, is ff0000, wanted 00ff00: the damage was multiplied by the
scale the surface still had when the request arrived, so a quarter of the new buffer
was read out of it
```

把 `buffer_create_shm_texture()` 的 shadow 复用条件短路成「总是重新分配」：

```
selftest: damage-scale gate FAILED: pixel (150,150), outside it, where the frame before
has to still be showing, is 00ff00, wanted ff0000: the whole buffer was re-read, so
this run exercised no partial copy at all and cannot tell either scale was wrong
```

