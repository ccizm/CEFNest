# dmabuf 格式表：以本进程可导入集为准

日期：2026-10-20
问题：feedback 表此前静态广告 `{INVALID, LINEAR} × 4 个 8888 格式`，理由是
"tiled 只对 scan-out 有意义"。这个判断是错的——tiled 影响 GPU 渲染目标和
采样的缓存友好度。后果实测：CEF 的每块 buffer 都是 `modifier=0x0` 线性内存。

## 1. 新的真相来源

合成器只做 **导入**，从不分配。所以正确的格式集是"本进程能导入什么"——
这正是 GDK 自己探测并公布的集合：`gdk_display_get_dmabuf_formats()`。
`nested_setup_dmabuf` 改为遍历该集合，过滤出单平面 8888 四格式
（M2 硬约束保留），写入 feedback。

gbm 设备保留但降级为**设备身份**：feedback 的 `main_device` 是渲染节点的
dev_t，客户端（CEF）用它决定在哪块设备上分配；格式可导入性以 GDK 为准，
`gbm_device_is_format_supported` 的探测（从不带 modifier，本来就答不了
这个问题）不再使用。

## 2. 实测（Intel，GDK 广告 13 对）

feedback 表从 8 对变为 13 对，新增
`X_TILED / Y_TILED / Yf_TILED / 4_TILED`。CEF 154 的分配随之切换：

```
改前: imported 1250x817 AB24 modifier=0x0        planes=1（线性）
改后: imported 1250x817 AB24 modifier=0x100000000000004 planes=2（4_TILED+元数据面）
```

即 Chromium 现在以原生 Chrome 的方式分配：4-tile + 渲染压缩元数据面。
导入零拷贝不变，渲染与采样的内存访问模式对齐驱动最优路径。

## 3. 保持不变

- 无渲染节点 / GDK 集合为空 → 不广告 dmabuf global（客户端回退 wl_shm）；
- Y-invert / interlaced 拒绝；单平面 8888 之外四色不收；
- feedback 的 memfd 格式表、main_device/target_device 机制；
- selftest `--dmabuf` 按合成器侧导入计分。
