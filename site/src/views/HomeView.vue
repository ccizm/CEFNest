<script setup>
import CodeBlock from '../components/CodeBlock.vue'

const quickCode = `cc host.c -o host $(pkg-config --cflags --libs cefnest)

GtkWidget       *view    = g_object_new (CHROMIUM_TYPE_VIEW, NULL);
ChromiumSession *session = chromium_session_new (NULL);

chromium_session_set_view (session, CHROMIUM_VIEW (view));
g_signal_connect (session, "load-end", G_CALLBACK (on_load_end), NULL);
chromium_session_start (session, "https://cnb.cool/yanzhiming/CEFNest", &error);`

const debCode = `# Debian 系
apt install libcefnest0 libcefnest-dev cefnest-helper cefnest-demo
# RPM 系
dnf install libcefnest0 libcefnest-dev cefnest-helper cefnest-demo

# 宿主对 runtime 编译
cc host.c -o host $(pkg-config --cflags --libs cefnest)`

const buildCode = `# 1. 拉取固定版本的 CEF 二进制发行包
./scripts/fetch-cef.sh

# 2. Chromium 侧 helper
cmake -S helper -B helper/build -DCMAKE_BUILD_TYPE=Release
cmake --build helper/build -j

# 3. 部件 + helper + CEF 运行时，装出自含树
meson setup builddir -Dinstall-helper=true
ninja -C builddir install

# 运行：安装树不依赖构建树
CHROMIUM_URL=https://cnb.cool/yanzhiming/CEFNest \\
  $HOME/.local/cefnest-stage/bin/cefnest-demo`

const pieces = [
  {
    name: 'NestedCompositor',
    file: 'src/nested*.c',
    desc: '嵌套 Wayland 合成器：globals、surface 栈、damage、输入、剪贴板，以及跨窗口边缘双向的拖拽。',
  },
  {
    name: 'ChromiumView',
    file: 'src/chromium-view.c',
    desc: 'GTK 部件：把宿主窗口的真实指针、键盘、输入法交给合成器，并把合成结果画成 GdkTexture。',
  },
  {
    name: 'ChromiumSession',
    file: 'src/chromium-session.c',
    desc: '控制面：启动/停止 helper，跟踪页面状态，运行 JavaScript，代理下载、查找、右键菜单与权限请求。',
  },
]

const features = [
  {
    icon: 'bi-window-desktop',
    title: '真 Chromium 渲染',
    text: '页面由 Chromium 自己的 ozone/wayland 后端绘制，就是用户桌面上那个浏览器引擎，不是嵌入的WebView 替代品。',
    cls: '',
  },
  {
    icon: 'bi-signpost-split',
    title: '原生 Wayland',
    text: '没有 XWayland，没有 X11 客户端，没有 XEMBED。同进程内跑一个 libwayland-server 合成器，接收 shm / dmabuf 缓冲。',
    cls: 'green',
  },
  {
    icon: 'bi-film',
    title: '零拷贝，无 OSR',
    text: '没有离屏渲染缓冲被拷进 Cairo surface。Chromium 提交 Wayland surface，合成结果直接作为 GdkTexture 进部件。',
    cls: '',
  },
  {
    icon: 'bi-keyboard',
    title: '输入全桥接',
    text: '指针、键盘（含 axis_value120 滚轮刻度）、输入法组合、光标形状、剪贴板与主选区，双向跨过窗口边缘。',
    cls: 'green',
  },
  {
    icon: 'bi-shield-check',
    title: '策略归宿主',
    text: '下载落到哪、权限答不答应、右键菜单长什么样——页面问的每一个问题都由宿主回答，session 只负责代理。',
    cls: '',
  },
  {
    icon: 'bi-box-seam',
    title: '库化交付',
    text: 'meson install 装出 libcefnest.so、三个头文件与 pkg-config 文件；deb/rpm 打包由 CI 流水线产出。',
    cls: 'green',
  },
]

const globalsList =
  'wl_compositor · wl_shm · wl_seat (8) · wl_output · wl_data_device_manager (3) · xdg_wm_base · zwp_linux_dmabuf_v1 (5, 含 feedback) · wp_viewporter · wp_fractional_scale_manager_v1 · zwp_text_input_manager_v3 · wp_cursor_shape_manager_v1'
</script>

<template>
  <div>
    <!-- Hero -->
    <section class="bg-dark text-white py-5">
      <div class="container">
        <div class="row align-items-center g-5">
          <div class="col-lg-6">
            <span class="hero-badge"><i class="bi bi-stars me-1"></i>API 尚不稳定 · 设计即文档</span>
            <h1 class="display-5 fw-bold mb-3">在 GTK 4 里，运行真正的 Chromium</h1>
            <p class="lead-text lead mb-4">
              CEFNest 是一个 GTK 4 部件：页面由真 Chromium 渲染，经同一进程内的嵌套
              Wayland 合成器合成进部件。没有 XWayland，也没有离屏渲染。
            </p>
            <div class="d-flex gap-2 flex-wrap">
              <router-link to="/dev/start" class="btn btn-light btn-lg px-4">
                <i class="bi bi-rocket-takeoff me-1"></i> 快速开始
              </router-link>
              <router-link to="/api/quickstart" class="btn btn-outline-light btn-lg px-4">
                <i class="bi bi-book me-1"></i> 调用文档
              </router-link>
            </div>
          </div>
          <div class="col-lg-6">
            <div class="arch-box mb-2">
              <div class="arch-title">GTK 4 应用</div>
              <small>你的窗口与业务逻辑</small>
            </div>
            <div class="text-center arch-arrow"><i class="bi bi-arrow-down"></i></div>
            <div class="arch-box mb-2">
              <div class="arch-title">ChromiumView <small>· GtkWidget</small></div>
              <small>NestedCompositor —— 进程内的 libwayland-server 合成器，自有 socket</small>
            </div>
            <div class="text-center arch-arrow"><i class="bi bi-arrow-down"></i></div>
            <div class="arch-proto mb-1">Wayland 协议（socket 的客户端一侧）</div>
            <div class="arch-box">
              <div class="arch-title">cefnest-helper <small>· 独立进程，CEF 应用</small></div>
              <small>以 WAYLAND_DISPLAY= 指向嵌套合成器启动；控制面为 unix socket 上的 NDJSON</small>
            </div>
          </div>
        </div>
      </div>
    </section>

    <!-- 特性 -->
    <section class="py-5">
      <div class="container">
        <h2 class="fw-bold mb-4">三块积木，各干一件事</h2>
        <div class="row g-4 mb-5">
          <div v-for="f in features" :key="f.title" class="col-md-6 col-lg-4">
            <div class="feature-card p-4" :class="f.cls">
              <span class="feature-icon mb-3"><i class="bi" :class="f.icon"></i></span>
              <h5 class="fw-semibold mb-2">{{ f.title }}</h5>
              <p class="text-secondary mb-0 small">{{ f.text }}</p>
            </div>
          </div>
        </div>

        <div class="row g-4">
          <div class="col-lg-7">
            <h2 class="fw-bold mb-4">三个组成部分</h2>
            <div class="d-flex flex-column gap-3">
              <div v-for="p in pieces" :key="p.name" class="feature-card p-3 px-4">
                <div class="d-flex justify-content-between align-items-baseline flex-wrap">
                  <code class="fw-bold text-body">{{ p.name }}</code>
                  <small class="text-secondary">{{ p.file }}</small>
                </div>
                <p class="text-secondary small mb-0 mt-2">{{ p.desc }}</p>
              </div>
              <p class="small text-secondary mt-2 mb-0">
                合成器通告的 globals（括号内为协议版本）：{{ globalsList }}
              </p>
            </div>
          </div>
          <div class="col-lg-5">
            <h2 class="fw-bold mb-4">三行接入</h2>
            <CodeBlock :code="quickCode" label="pkg-config + 最小宿主" lang="c" />
            <div class="doc-note">
              完整最小宿主见 <code>examples/minimal.c</code>：窗口、ChromiumView、ChromiumSession
              三个对象，页面就跑起来了。中等示例 <code>examples/host.c</code>
              演示下载、权限、脚本结果四类信号。
            </div>
          </div>
        </div>

        <!-- 两种使用方法 -->
        <div class="row mt-5 pt-2">
          <div class="col-12">
            <h2 class="fw-bold mb-4">两种使用方法</h2>
          </div>
          <div class="col-md-6">
            <div class="feature-card p-4">
              <span class="feature-icon mb-3"><i class="bi bi-box-seam"></i></span>
              <h5 class="fw-semibold mb-2">方法一 · 安装 deb / rpm 包</h5>
              <p class="text-secondary small mb-3">
                包管理器装出共享库、头文件、helper 与 demo；宿主用 pkg-config 直接调用 runtime。
              </p>
              <CodeBlock :code="debCode" label="bash" lang="bash" />
              <p class="small text-secondary mb-0">
                依赖严格分层：helper 包（约 1.5 GB，含整个 CEF 运行时）在底层，库精确依赖它，
                demo 与 -dev 再依赖库——四个包同版本一起发布，装任何一个都按序自动拉齐，
                装齐即跑真 Chromium。
              </p>
            </div>
          </div>
          <div class="col-md-6">
            <div class="feature-card p-4">
              <span class="feature-icon mb-3"><i class="bi bi-hammer"></i></span>
              <h5 class="fw-semibold mb-2">方法二 · 项目包含部署</h5>
              <p class="text-secondary small mb-3">
                源码构建出的安装树自带 helper 与整个 CEF 运行时（约 1.5 GB），
                整体自含、随搬随用。
              </p>
              <CodeBlock :code="buildCode" label="bash" lang="bash" />
              <p class="small text-secondary mb-0">
                装出的 helper 从自身旁边解析 CEF 运行时，不需要构建树里的任何东西。
              </p>
            </div>
          </div>
        </div>
      </div>
    </section>
  </div>
</template>
