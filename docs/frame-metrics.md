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
