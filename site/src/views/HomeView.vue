<script setup>
import CodeBlock from '../components/CodeBlock.vue'
import { release } from '../data/release.js'

const quickCode = `cc host.c -o host $(pkg-config --cflags --libs cefnest)

GtkWidget       *view    = g_object_new (CHROMIUM_TYPE_VIEW, NULL);
ChromiumSession *session = chromium_session_new (NULL);

chromium_session_set_view (session, CHROMIUM_VIEW (view));
g_signal_connect (session, "load-end", G_CALLBACK (on_load_end), NULL);
chromium_session_start (session, "https://cnb.cool/yanzhiming/CEFNest", &error);`

const pieces = [
  {
    name: 'NestedCompositor',
    file: 'src/nested*.c',
    desc: '嵌套 Wayland 合成器：globals、surface 栈、damage、输入、剪贴板，以及跨窗口边缘双向的拖拽。',
  },
  {
    name: 'ChromiumView',
    file: 'src/chromium-view.c',
    desc: 'GTK 4 部件：把宿主窗口的真实指针、键盘与输入法交给合成器，并把合成结果画成 GdkTexture。',
  },
  {
    name: 'ChromiumSession',
    file: 'src/chromium-session.c',
    desc: '控制面：启动与停止 helper，跟踪页面状态，执行 JavaScript，代理下载、查找、右键菜单与权限请求。',
  },
]

const features = [
  {
    icon: 'bi-window-desktop',
    title: '真 Chromium 渲染',
    text: '页面跑在 Chromium 自己的 ozone/wayland 后端里，是用户桌面上那个浏览器引擎，不是另一套引擎的近似实现。',
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
    title: '直接合成，无 OSR',
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
    text: 'meson install 装出 libcefnest.so、三个头文件与 pkg-config 文件；deb / rpm 由发版流水线从同一棵安装树产出。',
    cls: 'green',
  },
]

const globalsList =
  'wl_compositor (4) · wl_shm (2) · wl_seat (8) · wl_output (4) · wl_data_device_manager (3) · xdg_wm_base (3) · zwp_linux_dmabuf_v1 (5) · wp_viewporter (1) · wp_fractional_scale_manager_v1 (1) · zwp_primary_selection_device_manager_v1 (1) · zwp_text_input_manager_v3 · wp_cursor_shape_manager_v1'
</script>

<template>
  <div>
    <!-- Hero -->
    <section class="bg-dark text-white py-5">
      <div class="container">
        <div class="row align-items-center g-5">
          <div class="col-lg-6">
            <span class="hero-badge"><i class="bi bi-stars me-1"></i>{{ release.version }} · API 尚不稳定</span>
            <h1 class="display-5 fw-bold mb-3">在 GTK 4 里，运行真正的 Chromium</h1>
            <p class="lead-text lead mb-4">
              CEFNest 是一个 GTK 4 部件：页面由真 Chromium 渲染，经同一进程内的嵌套
              Wayland 合成器合成进部件。没有 XWayland，也没有离屏渲染。
            </p>
            <div class="d-flex gap-2 flex-wrap">
              <router-link to="/downloads" class="btn btn-light btn-lg px-4">
                <i class="bi bi-download me-1"></i> 下载 {{ release.version }}
              </router-link>
              <router-link to="/api/quickstart" class="btn btn-outline-light btn-lg px-4">
                <i class="bi bi-code-slash me-1"></i> 三行接入
              </router-link>
            </div>
            <p class="small text-white-50 mt-3 mb-0">
              CEF {{ release.cef }} · Chromium {{ release.chromium }} · GTK 4.14+ · 原生 Wayland
            </p>
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

    <!-- 为什么 -->
    <section class="py-5">
      <div class="container">
        <div class="row g-4 align-items-end mb-4">
          <div class="col-lg-8">
            <h2 class="fw-bold mb-2">为什么必须自带一个合成器</h2>
            <p class="text-secondary mb-0">
              两条约束排除了所有不自带合成器的“GTK 嵌浏览器”方案：页面必须按桌面上真实浏览器的方式
              说话，而部件又不能靠离屏拷贝拿到像素。
            </p>
          </div>
          <div class="col-lg-4 text-lg-end">
            <router-link to="/dev/arch" class="small">架构与实现 <i class="bi bi-arrow-right ms-1"></i></router-link>
          </div>
        </div>
        <div class="row g-4">
          <div v-for="f in features.slice(0, 3)" :key="f.title" class="col-md-4">
            <div class="feature-card p-4" :class="f.cls">
              <span class="feature-icon mb-3"><i class="bi" :class="f.icon"></i></span>
              <h3 class="h5 fw-semibold mb-2">{{ f.title }}</h3>
              <p class="text-secondary mb-0 small">{{ f.text }}</p>
            </div>
          </div>
        </div>
      </div>
    </section>

    <!-- 组成 -->
    <section class="py-5 bg-body-tertiary">
      <div class="container">
        <div class="row g-5">
          <div class="col-lg-7">
            <h2 class="fw-bold mb-4">三块积木，各干一件事</h2>
            <div class="d-flex flex-column gap-3">
              <div v-for="p in pieces" :key="p.name" class="feature-card p-3 px-4">
                <div class="d-flex justify-content-between align-items-baseline flex-wrap gap-2">
                  <code class="fw-bold text-body">{{ p.name }}</code>
                  <small class="text-secondary">{{ p.file }}</small>
                </div>
                <p class="text-secondary small mb-0 mt-2">{{ p.desc }}</p>
              </div>
            </div>
            <p class="small text-secondary mt-3 mb-0">
              合成器通告的 globals（括号内为协议版本）：{{ globalsList }}
            </p>
            <div class="row g-4 mt-1">
              <div v-for="f in features.slice(3)" :key="f.title" class="col-md-6">
                <div class="feature-card p-4 h-100" :class="f.cls">
                  <span class="feature-icon mb-2"><i class="bi" :class="f.icon"></i></span>
                  <h3 class="h6 fw-semibold mb-1">{{ f.title }}</h3>
                  <p class="text-secondary mb-0 small">{{ f.text }}</p>
                </div>
              </div>
            </div>
          </div>
          <div class="col-lg-5">
            <h2 class="fw-bold mb-4">三行接入</h2>
            <CodeBlock :code="quickCode" label="pkg-config + 最小宿主" lang="c" />
            <div class="doc-note">
              顺序是硬性的：<code>set_view()</code> 必须先于 <code>start()</code>——helper 的
              <code>WAYLAND_DISPLAY</code> 取自视图的合成器。
              完整最小宿主见 <router-link to="/api/quickstart">最小宿主</router-link> 与
              <code>examples/minimal.c</code>。
            </div>
          </div>
        </div>
      </div>
    </section>

    <!-- 本版变更 -->
    <section class="py-5">
      <div class="container">
        <div class="row g-4 align-items-start">
          <div class="col-lg-7">
            <h2 class="fw-bold mb-3">{{ release.version }} 带来了什么</h2>
            <ul class="release-list mb-3">
              <li><strong>wl_shm 请求槽 1 补齐，全局版本升到 2。</strong> Wayland 1.22 客户端销毁自己的 wl_shm 代理时，不再把嵌套合成器与宿主一起 wl_abort。</li>
              <li><strong>CEF 子进程改用 _exit()。</strong> 不再穿过继承来的栈保护尾言，此前的“stack smashing detected”与 SIGABRT 不再出现。</li>
              <li><strong>新增 ::helper-exited。</strong> “页面没了”与 helper 进程真正被收走分成两个时刻，宿主可以等后者再拆合成器。</li>
            </ul>
            <router-link to="/downloads" class="btn btn-primary">
              <i class="bi bi-download me-1"></i> 下载 {{ release.version }}
            </router-link>
          </div>
          <div class="col-lg-5">
            <div class="feature-card p-4">
              <h3 class="h6 fw-semibold mb-3">接下来去哪里</h3>
              <div class="d-flex flex-column gap-3">
                <router-link to="/downloads" class="docs-next">
                  <span><i class="bi bi-box-seam me-2"></i>下载与安装</span>
                  <small class="text-secondary">四个包、校验、deb / rpm 命令</small>
                </router-link>
                <router-link to="/dev/build" class="docs-next">
                  <span><i class="bi bi-hammer me-2"></i>从源码构建</span>
                  <small class="text-secondary">CEF、helper、meson 与自含安装树</small>
                </router-link>
                <router-link to="/dev/limits" class="docs-next">
                  <span><i class="bi bi-exclamation-triangle me-2"></i>已知限制</span>
                  <small class="text-secondary">输入、编解码、沙箱与生命周期边界</small>
                </router-link>
              </div>
            </div>
          </div>
        </div>
      </div>
    </section>
  </div>
</template>
