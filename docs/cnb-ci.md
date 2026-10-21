# CNB（cnb.cool）接入

仓库根目录的 `.cnb.yml` 只做一件事：`main` 分支 push 时在自托管 runner 上跑
`./test/ci-all.sh`，也就是和本地发布闸门同一条命令。

## 1. 为什么是自托管 runner

`test/ci-all.sh` 需要：

- GTK 4 / Wayland / xkbcommon / json-glib / gbm / DRM / Vulkan 的开发包；
- `gnome-shell --headless`（CI 会话用的 nested mutter）；
- ImageMagick（`pixel-gate.sh`）；
- 一份已解压的 CEF 分发（`third_party/cef/dist` 不进 git，约 1.5 GB）。

CNB 的平台镜像不保证这些；把安装步骤塞进每次 push 的流水线会又慢又不可控。所以
`.cnb.yml` 用 `runner.tags: [cefnest-gate]` 指定一台已经准备好的 runner。

## 2. runner 一次性准备

```bash
sudo apt install build-essential meson ninja-build pkg-config \
  libgtk-4-dev libwayland-dev libxkbcommon-dev libjson-glib-dev \
  libgbm-dev libdrm-dev libvulkan-dev imagemagick gnome-shell
./scripts/fetch-cef.sh
meson setup builddir
ninja -C builddir
```

`gnome-shell --headless` 需要能启动 D-Bus 会话；`test/ci-session.sh` 会自己建隔离的
HOME、XDG、bus 和 nested compositor，不会碰 runner 的真实桌面。

## 3. 触发

push 到 `main` 后 CNB 读取 `.cnb.yml`，只有标签为 `cefnest-gate` 的节点会被调度。
没有该标签时流水线不会自己找节点——这是刻意的，避免在没装依赖的公共节点上跑一半再失败。

本地等价命令：

```bash
./test/ci-all.sh
```

如需把真实站点巡检也挂上，runner 还要有网络，并改成：

```bash
CI_ALL_REAL=1 ./test/ci-all.sh
```

真实站点是外部输入，默认不进发布闸门。
