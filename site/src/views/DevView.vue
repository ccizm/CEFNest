<script setup>
import { computed } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import DocLayout from '../components/DocLayout.vue'
import CodeBlock from '../components/CodeBlock.vue'
import { release } from '../data/release.js'

const route = useRoute()
const router = useRouter()

const items = [
  { key: 'start', label: '开始之前' },
  { key: 'build', label: '从源码构建' },
  { key: 'run', label: '运行与调试' },
  { key: 'arch', label: '架构与实现' },
  { key: 'limits', label: '已知限制' },
]
const section = computed(() => {
  const s = route.params.section
  if (items.some((i) => i.key === s)) return s
  router.replace('/dev/start')
  return 'start'
})

const runtimeRows = [
  {
    what: '桌面与显示',
    want: '原生 Wayland 会话，GDK 走 Wayland 后端',
    why: 'X11 / XWayland 下没有可交给 client 的 Wayland surface，部件不会工作',
  },
  {
    what: 'Wayland 库',
    want: '1.21+，含 axis_value120',
    why: '指针高分辨率滚轮与合成器实现都要这一代协议',
  },
  {
    what: 'GTK',
    want: '4.14+',
    why: '部件、帧时钟与输入法接口的下限',
  },
  {
    what: 'Chromium 沙箱',
    want: '可用的非特权用户命名空间',
    why: '禁 userns 的宿主会以 No usable sandbox! 拒绝启动，不会静默降级',
  },
  {
    what: 'CEF 运行时',
    want: '由 cefnest-helper 携带',
    why: '包内已含 libcef.so、locales、icudtl.dat 等，安装后约 1.5 GB',
  },
]

const depRows = [
  ['gtk4', '4.18.6', '4.14'],
  ['wayland（server + client）', '1.23.1', '1.21+（axis_value120）'],
  ['wayland-protocols', '1.44', '须含 cursor-shape / fractional-scale / text-input'],
  ['xkbcommon', '1.7.0', '—'],
  ['gbm / libdrm', '25.0.7 / 2.4.124', '—'],
  ['json-glib', '1.10.6', '—'],
  ['meson / ninja / cmake', '1.7 / 1.12 / 3.16+', 'meson 1.3'],
  ['CEF（linux64/arm64 minimal）', release.cef, '能用自带 CMake helpers 构建的版本'],
]

const optionRows = [
  {
    name: 'helper-path',
    def: '空，由 prefix + libexecdir + 主机架构推导',
    what: '编入库内的 helper 路径；没人指名 helper 时使用',
  },
  {
    name: 'helper-build-dir',
    def: 'helper/build/Release',
    what: 'cmake 产物目录：开发回退路径，也是安装来源',
  },
  {
    name: 'install-helper',
    def: 'false',
    what: '连同 helper 与整个 CEF 运行时一起安装——约 1.5 GB，故为选装',
  },
  {
    name: 'install-demo',
    def: 'true',
    what: '安装 cefnest-demo 及其回退用的 shm-client',
  },
  {
    name: 'cefnest-version',
    def: '0.0.1',
    what: '库、pkg-config 与包的版本；CNB 发版时设成所捆绑的 CEF X.Y.Z',
  },
]

const envRows = [
  {
    name: 'CHROMIUM_URL',
    what: '启动时要打开的页面。',
  },
  {
    name: 'CEFNEST_HELPER',
    what: '含 / 的绝对或相对路径，优先级最高；值是含 / 的路径才当作路径。',
  },
  {
    name: 'CHROMIUM_HELPER',
    what: '设任意值即强制 demo 进入 session 模式；要当路径用同样必须含 /。',
  },
  {
    name: 'CHROMIUM_SESSION_SCRIPT',
    what: '一行一条命令的脚本文件；第一条页面命令会等 helper 就绪再发。',
  },
  {
    name: 'CHROMIUM_DUMP / CHROMIUM_DUMP_DELAY',
    what: 'N 毫秒后抓部件写一张 PNG 并退出；延时默认 2000 ms。',
  },
  {
    name: 'CHROMIUM_HELPER_NO_SANDBOX',
    what: '必须恰为 1 才关闭 Chromium 沙箱；其它任何值（包括打错）都保持开启。',
  },
  {
    name: 'CHROMIUM_USER_AGENT',
    what: '宿主在 chromium_session_start() 前放进自己的环境，随环境继承给 helper，成为 Chromium 的默认 UA。',
  },
  {
    name: 'G_MESSAGES_DEBUG',
    what: '空格分隔的日志域：all，或 nested chromium-view；逗号列表什么都匹配不上。会话自己的调试消息没有单独域，要看就用 all。',
  },
]

const demoEnvRows = [
  ['CHROMIUM_WINDOW', 'fullscreen 或 maximize，给测试一个确定的窗口几何。'],
  ['CHROMIUM_CLIPSET / CLIPWATCH', '预置宿主普通剪贴板，并把桥接结果打出来。'],
  ['CHROMIUM_PRIMARYSET / PRIMARYWATCH', '预置 / 读取主选区（中键剪贴板）。'],
  ['CHROMIUM_SPELLCHECK', '默认关并下发 --disable-spell-checking；设为 1 恢复 Chromium 拼写建议。'],
  ['CHROMIUM_A11Y', '设为 1 启动时强制开启无障碍；否则监听会话总线上的屏幕阅读器状态。'],
  ['CHROMIUM_FRAME_METRICS', '设为 1 每秒打印 demo: frame-count N。'],
  ['CEFNEST_DOWNLOAD_DIR', 'demo 自动接受下载时的落盘目录；默认是 XDG 下载目录。'],
  ['CEFNEST_DOWNLOAD_ASK', '设任意值：每个下载都等“保存 / 丢弃”或脚本步骤，不直接落盘。'],
  ['CEFNEST_DOWNLOAD_NO_SAVE_AS', '设任意值：图片下载跳过 GtkFileDialog，走普通下载目录 / 询问路径。'],
  ['CEFNEST_PERMISSION', 'ask（默认，每次弹行）/ deny / allow / ignore（session 直接拒绝）。'],
  ['CEFNEST_DRAG_OUT', '默认接管页面拖出；take 只读并打日志；off 不连接 ::drag-out。'],
]

const scriptGroups = [
  {
    title: '页面与导航',
    cmds: 'load URL · back · forward · reload · reload-hard · stop · zoom-in · zoom-out · zoom-reset · mute · unmute',
  },
  {
    title: 'JavaScript 与状态',
    cmds: 'js CODE · js-expr EXPR · a11y-on · a11y-off · frames',
  },
  {
    title: '标签与窗口',
    cmds: 'new-tab URL · new-window URL · select-browser ID · close-browser ID',
  },
  {
    title: '下载',
    cmds: 'download-accept PATH · download-reject · download-accept-name SUBSTR PATH · download-reject-name SUBSTR',
  },
  {
    title: '页内查找',
    cmds: 'find PATTERN · find-case PATTERN · find-next · find-prev · find-stop',
  },
  {
    title: '菜单与权限',
    cmds: 'menu-pick ID · menu-dismiss · perm-accept ID · perm-deny ID · perm-dismiss ID',
  },
  {
    title: '合成器输入',
    cmds: 'hover X Y · click X Y [BTN] · drag X0 Y0 X1 Y1 [BTN] · drag-hold … · release [BTN] · drop-in … · drop-in-hold · drop-in-move X Y · drop-in-drop · drop-in-cancel · wheel DX DY',
  },
  {
    title: '抓图、等待与收尾',
    cmds: 'wait MS · shot PATH · dump PATH · surfaces · close · quit',
  },
]

const troubleRows = [
  {
    symptom: 'helper 启动即报 No usable sandbox!',
    fix: '宿主限制了非特权用户命名空间。给 helper 加允许 userns 的 AppArmor profile（推荐），或显式设 CHROMIUM_HELPER_NO_SANDBOX=1。',
  },
  {
    symptom: 'start() 报 no helper binary at …',
    fix: '错误里那串就是本次选中的路径。装好 cefnest-helper，或用 CEFNEST_HELPER 指一个绝对路径；开发树则先跑 cmake。',
  },
  {
    symptom: '窗口起来了，页面区域一直空白',
    fix: '先确认 helper 在跑、日志里没有沙箱或连接错误；再确认是原生 Wayland 会话（GDK_BACKEND=wayland），X11 下不会渲染。',
  },
  {
    symptom: '设了 G_MESSAGES_DEBUG 却没有日志',
    fix: '域列表要用空格分隔。想看全部就用 all；nested 与 chromium-view 是两个可用的具体域。',
  },
  {
    symptom: '关掉最后一个标签，整个页面都没了',
    fix: '一个 session 只有一个 helper，最后一个 browser 关闭即结束 helper。要保留页面就先开新 browser，或接受 ::closed。',
  },
  {
    symptom: '先看到 Lost connection to Wayland compositor，才收到 ::helper-exited',
    fix: '这是拆除阶段的正常顺序：信号触发的 CefShutdown() 还在用 Wayland 连接。等 ::helper-exited 再销毁共享合成器。',
  },
  {
    symptom: '自定义前缀编译出的宿主找不到 libcefnest.so',
    fix: '为自定义前缀加 LD_LIBRARY_PATH 或 ldconfig 条目。安装的 demo 不需要——它的 DT_RUNPATH 是 $ORIGIN 相对。',
  },
]

const limitGroups = [
  {
    title: '输入与窗口',
    items: [
      '拖出与拖入都已端到端可用，人工验证过：页面内的拖拽走完整 wl_data_device 协商，拖出把文本或图片交给宿主，拖入经 GtkDropTarget 接受文件列表与字符串。',
      '从一个独立宿主窗口里发起的拖拽，会从错误的 surface 离开——它仍建在 demo 的主顶层窗口上。独立窗口拿到输入本身有门禁覆盖。',
      '单 seat、单指针、无触控；无捏合缩放，无数位板输入。',
      '只支持原生 Wayland。X11 / XWayland 下没有能交给 client 的 Wayland surface，这是设计，不是疏漏。',
    ],
  },
  {
    title: '页面能力',
    items: [
      '该 CEF 发行包不含 H.264 / MP4；YouTube 仍能经 VP9 播放，MP4-only 页面会失败。',
      '页面的声音经 helper 的 audio 进程直达扬声器，完全绕开部件；set_muted 是唯一开关，也没有“页面开始出声”的通知。',
      '::popup-requested 只带 URL，不带 CEF 的 disposition，宿主分不出页面要的是后台标签还是前台窗口。前台请求已经就地导航；后台标签只报告。',
    ],
  },
  {
    title: '生命周期与错误',
    items: [
      '一次失败的导航报告两次：先真实错误码，约一秒后补 ERR_ABORTED (-3)；::load-end 照常发出，http_code 为 0。',
      '只有 renderer 的死亡会被报告，且只有 wait status 与错误码；发行包不带 crash handler，没有 minidump，也没有栈。其它沙箱子进程死掉时，宿主什么也听不到。',
      '一个 helper 承载多个 browser：标签共享一个 CefWindow，窗口式 browser 各有独立 CefWindow。两种都能被宿主关闭，也都会发 ::browser-closed；未知 id 答 no-such-browser。',
      '关掉最后一个 browser 会结束 helper。::closed 表示页面没了，::helper-exited 表示子进程已被 wait 收走；拆共享合成器要等后者。',
    ],
  },
  {
    title: '下载、查找、菜单与权限',
    items: [
      '下载可接受、拒绝、取消，也可经 chromium_session_pause_download() / resume_download() 暂停与恢复；但 complete 不是文件落盘的时刻——字节到了之后，CEF 的终局事件还会晚 3–6 秒，和下载数据库的批量刷新一起到。',
      '查找一次走一个匹配；全新搜索的 active ordinal 是 0，想跳第五个就发五步。一次三次实测里，第一步报过第三个匹配——序数是 Blink 自己的顺序，门禁故意不钉死。',
      'helper 会扩充右键菜单（链接 / 图片 / 媒体行）并执行命令；宿主还不能添加自己的行。勾选与单选标记不画，子菜单行不打开——菜单每次命令后都从新模型重建。',
      '权限只能整体授予或拒绝，不能收窄：media 请求必须按原样回同一个掩码，“麦克风可以、摄像头不行”在这里不存在。扣着不答的请求会让页面的 promise 悬着、media 队列堵住，紧接着的下一次询问还会消失；同一个权限问两次就到处理器两次，什么也不记忆。',
      'navigator.storage.persist()、permissions.query() 与空闲检测这三类询问根本不经过 CefPermissionHandler，任何宿主策略都覆盖不到。',
    ],
  },
  {
    title: '沙箱、部署与 API',
    items: [
      'renderer 有沙箱，helper（浏览器进程）没有，也不可能受沙箱保护——Wayland 连接、GPU 缓冲与控制 socket 都在它手里。信任边界是 renderer 里的页面代码。',
      '本版发布资产只有 amd64 / x86_64。CEF 的 linuxarm64 路径已接好，但还没有在 arm64 主机上实测；需要时在目标机从源码构建。',
      'API 尚不稳定：信号可能加参数，头文件可能在版本之间变形状。宿主应对着自己构建的版本编译。',
    ],
  },
]

const buildDev = `# 1. 拉取已验证的 stable CEF 发行包（下载约 500 MB，解压约 1.5 GB）
./scripts/fetch-cef.sh

# 2. Chromium 侧 —— 自动使用 third_party/cef/dist
cmake -S helper -B helper/build -DCMAKE_BUILD_TYPE=Release
cmake --build helper/build -j

# 3. 部件、demo、示例与测试
meson setup builddir
ninja -C builddir`

const buildInstall = `# 已配置过 builddir（例如先跑了开发构建）：用 configure 改选项。
# 全新的构建目录则直接 meson setup builddir -Dprefix=… -Dinstall-helper=true
meson configure builddir \\
  -Dprefix=\$HOME/.local/cefnest-stage \\
  -Dinstall-helper=true
ninja -C builddir install

# 装出的树自带 helper 与整个 CEF 运行时，不依赖构建树
CHROMIUM_URL=https://cnb.cool/yanzhiming/CEFNest \\
  \$HOME/.local/cefnest-stage/bin/cefnest-demo`

const minimalHost = `GError          *error   = NULL;
GtkWidget       *window  = gtk_application_window_new (app);
ChromiumView    *view    = g_object_new (CHROMIUM_TYPE_VIEW, NULL);
ChromiumSession *session = chromium_session_new (NULL);  /* NULL = 自动找 helper */

/* 顺序是硬性的：先有视图，再 set_view()，最后 start()。
   helper 的 WAYLAND_DISPLAY 取自视图的合成器。 */
chromium_session_set_view (session, view);
gtk_window_set_child (GTK_WINDOW (window), GTK_WIDGET (view));
gtk_window_present (GTK_WINDOW (window));

if (!chromium_session_start (session, "https://cnb.cool/yanzhiming/CEFNest", &error))
  g_printerr ("cannot start the helper: %s\\n", error->message);

/* 会话归窗口所有，窗口销毁即释放 */
g_object_set_data_full (G_OBJECT (window), "session", session, g_object_unref);`

const sandboxProfile = `abi <abi/4.0>,
include <tunables/global>

profile cefnest-helper /usr/libexec/cefnest/x86_64/cefnest-helper flags=(unconfined) {
  userns,
  include if exists <local/cefnest-helper>
}`

const runModes = `# 1. 开发树 + 真 Chromium（cmake 构建过 helper）
CHROMIUM_URL=https://cnb.cool/yanzhiming/CEFNest ./builddir/src/cefnest-demo

# 2. 安装树 —— helper 在安装路径里，不用再指路径
CHROMIUM_URL=https://cnb.cool/yanzhiming/CEFNest cefnest-demo

# 3. 不带 Chromium：指一个不存在的 helper，demo 退回 shm-client
CEFNEST_HELPER=/nonexistent-helper ./builddir/src/cefnest-demo`

const scriptExample = `# 第一条页面命令会等 helper 的 ready；没有 helper 的脚本在这里结束
load https://cnb.cool/yanzhiming/CEFNest
wait 2500
shot /tmp/cefnest.png
js-expr document.title
wait 500
close`

const globalsText = `wl_compositor (4)
wl_shm (2)
wl_seat (8)
wl_output (4)
wl_data_device_manager (3)
xdg_wm_base (3)
zwp_linux_dmabuf_v1 (5，带 feedback)
wp_viewporter (1)
wp_fractional_scale_manager_v1 (1)
zwp_primary_selection_device_manager_v1 (1)
zwp_text_input_manager_v3 (1)
wp_cursor_shape_manager_v1 (1)`
</script>

<template>
  <DocLayout :items="items" heading="使用文档" base="dev">
    <!-- ============ 开始之前 ============ -->
    <template v-if="section === 'start'">
      <h1>开始之前</h1>
      <p>
        CEFNest 是一个 GTK 4 部件：页面由<strong>真正的 Chromium</strong> 渲染，经同一进程内的
        嵌套 Wayland 合成器合成进部件。当前版本 <strong>{{ release.version }}</strong>，
        对应 CEF {{ release.cef }} / Chromium {{ release.chromium }}。在写第一行宿主代码之前，
        先确认下面五条运行前提，再决定用安装包还是源码构建。
      </p>

      <h2>运行前提</h2>
      <div class="table-responsive">
        <table class="table table-sm table-striped align-middle">
          <thead>
            <tr><th style="width: 18%">条件</th><th style="width: 32%">要求</th><th>为什么</th></tr>
          </thead>
          <tbody>
            <tr v-for="r in runtimeRows" :key="r.what">
              <td class="fw-semibold">{{ r.what }}</td>
              <td>{{ r.want }}</td>
              <td class="text-secondary small">{{ r.why }}</td>
            </tr>
          </tbody>
        </table>
      </div>

      <h2>获取安装树</h2>
      <p>
        组件本身、helper 与 CEF 运行时可以来自安装包，也可以在项目内构建。两条路装出的东西是同一套
        ABI；区别只在谁来编译、CEF 运行时从哪来。
      </p>
      <div class="row g-3 mb-3">
        <div class="col-md-6">
          <div class="feature-card p-4 h-100 d-flex flex-column">
            <div class="d-flex align-items-center gap-2 mb-2">
              <span class="feature-icon"><i class="bi bi-box-seam"></i></span>
              <h3 class="h6 fw-semibold mb-0">安装 deb / rpm</h3>
            </div>
            <p class="small text-secondary mb-3">
              四个包：<code>cefnest-helper</code>、<code>libcefnest0</code>、
              <code>libcefnest-dev</code>、<code>cefnest-demo</code>，同版本发布、锁步升级。
              编译宿主需要 <code>-dev</code>；渲染页面需要 helper。本版资产为 amd64 / x86_64，
              包在 Ubuntu 24.04 上构建，需 glibc 2.39+ 与 GTK 4.14+。
            </p>
            <router-link to="/downloads" class="btn btn-sm btn-primary mt-auto align-self-start">
              <i class="bi bi-download me-1"></i> 前往下载
            </router-link>
          </div>
        </div>
        <div class="col-md-6">
          <div class="feature-card p-4 h-100 d-flex flex-column">
            <div class="d-flex align-items-center gap-2 mb-2">
              <span class="feature-icon"><i class="bi bi-hammer"></i></span>
              <h3 class="h6 fw-semibold mb-0">在项目内构建</h3>
            </div>
            <p class="small text-secondary mb-3">
              cmake 构建 helper，meson 构建部件；开 <code>-Dinstall-helper=true</code>
              装出一棵自含树。适合开发、arm64，或没有现成包的环境。
            </p>
            <router-link to="/dev/build" class="btn btn-sm btn-outline-primary mt-auto align-self-start">
              从源码构建 <i class="bi bi-arrow-right ms-1"></i>
            </router-link>
          </div>
        </div>
      </div>
      <div class="doc-note">
        依赖是单向的：<code>cefnest-helper</code> 是底层，<code>libcefnest0</code>
        精确依赖对应版本的 helper，<code>-dev</code> 与 demo 再依赖运行库。本地安装四个文件时要一起
        交给包管理器——它不会自动搜索同目录的其它包。细节在
        <router-link to="/downloads">下载页</router-link>。
      </div>

      <h2>接入的最小骨架</h2>
      <p>
        宿主只需要三个对象：一个普通的 <code>GtkWindow</code>、一个 <code>ChromiumView</code>，
        以及拥有 helper 与页面状态的 <code>ChromiumSession</code>。装好开发文件后，
        一行命令就能编译：
      </p>
      <CodeBlock label="bash" lang="bash" :code="`cc host.c -o host \$(pkg-config --cflags --libs cefnest)`" />
      <CodeBlock label="C · 最小宿主" lang="c" :code="minimalHost" />
      <div class="doc-note">
        顺序是硬性的：<strong>先创建视图并 <code>set_view()</code>，再 <code>start()</code></strong>。
        helper 的 <code>WAYLAND_DISPLAY</code> 取自视图的合成器；没有视图时 <code>start()</code>
        会直接返回失败。
      </div>

      <h2>沙箱：默认开，失败是响的</h2>
      <p>
        helper 启动 Chromium 时<strong>默认开启沙箱</strong>；关闭的唯一办法是显式设置
        <code>CHROMIUM_HELPER_NO_SANDBOX=1</code>（必须恰为 <code>1</code>）。没有静默回退。
        沙箱保护的是 renderer 里的页面代码，不是 helper 整体——浏览器进程握着 Wayland 连接、
        GPU 缓冲与控制 socket，永远不沙箱。
      </p>
      <p>
        它依赖可用的<strong>非特权用户命名空间</strong>。Ubuntu 24.04 默认打开
        <code>apparmor_restrict_unprivileged_userns</code>（23.10 已有这个开关），helper 会在启动那一步
        打出 <code>No usable sandbox!</code> 然后死掉。两条被认可的出路：
      </p>
      <ol>
        <li>
          <strong>给 helper 单独加一条 AppArmor profile</strong>（推荐，粒度是一个应用，页面仍在沙箱里）。
          把安装路径填进 <code>/etc/apparmor.d/cefnest-helper</code>，然后
          <code>sudo apparmor_parser -r</code> 或 <code>sudo systemctl reload apparmor</code>。
        </li>
        <li>
          <strong>显式放弃沙箱</strong>：设 <code>CHROMIUM_HELPER_NO_SANDBOX=1</code>。页面代码从此以
          宿主用户身份运行；启动横幅会留下 <code>sandbox=OFF</code> 的痕迹。
        </li>
      </ol>
      <CodeBlock label="AppArmor · /etc/apparmor.d/cefnest-helper" lang="plaintext" :code="sandboxProfile" />
      <div class="doc-note warn">
        包里的 <code>chrome-sandbox</code> 原样安装为 <code>0755</code>，没有 setuid 位，也没有
        setuid 安装步骤；不要为它执行 <code>chmod 4755</code>。也不要为了这个组件去关整机的
        <code>apparmor_restrict_unprivileged_userns</code>。profile 这一形按 Ubuntu 发布说明写成，
        本项目尚未在开启限制的 Ubuntu 上实测；rpm 的安装路径在
        <code>/usr/libexec/cefnest/x86_64/</code> 同一处。
      </div>

      <h2>版本与稳定性</h2>
      <p>
        本页对应 <strong>{{ release.version }}</strong>，资产在
        <a :href="release.page" target="_blank" rel="noopener">Release 页</a>。
        四个包共用这个版本号（CEF stable X.Y.Z 加修订号），同一次构建发布、一起升级。
        <strong>API 尚不稳定</strong>：信号可能加参数，头文件可能在版本之间变形状。宿主应对着自己
        构建的版本编译；已知边角行为见<router-link to="/dev/limits">已知限制</router-link>。
      </p>
    </template>

    <!-- ============ 从源码构建 ============ -->
    <template v-else-if="section === 'build'">
      <h1>从源码构建</h1>
      <p>
        构建分两个阶段：Chromium 侧用 cmake 产出 helper，GTK 侧用 meson 产出库、demo、示例与测试。
        第一阶段可选——不拉 CEF 也能构建，demo 会退回 <code>shm-client</code>，足以演练合成器、
        seat、剪贴板桥和弹窗。
      </p>

      <h2>开发构建</h2>
      <CodeBlock label="bash" lang="bash" :code="buildDev" />
      <p>
        <code>scripts/fetch-cef.sh</code> 默认取已验证的 stable
        <code>{{ release.cef }}</code>；<code>CEF_VERSION=latest</code> 取 CEF index 当前标为 stable
        的最新构建，<code>CEF_VERSION=&lt;完整版本&gt;</code> 固定其它构建，
        <code>CEF_CHANNEL=any</code> 取任意 channel 的最新构建。也可以把
        <code>-DCEF_ROOT=…</code> 指给一份已经解开的发行包。
      </p>
      <div class="doc-note">
        跳过第一步照样能构建。此时 demo 驱动 <code>test/shm-client</code> 而不是 Chromium：
        合成器、输入、剪贴板与脚本步骤都能跑，但没有真页面。要验证页面行为，还是得跑一次 cmake。
      </div>

      <h2>自含安装树</h2>
      <p>
        在开发构建之后，打开 <code>install-helper</code> 再安装一次，装出的树会带上 helper 与整个
        CEF 运行时，约 1.5 GB。之后它不依赖构建树里的任何东西，整体可以搬走。
      </p>
      <CodeBlock label="bash" lang="bash" :code="buildInstall" />
      <div class="doc-note">
        装出的 helper 从自身旁边解析 CEF 运行时——把整个 <code>third_party/cef/dist</code>
        挪走后实测过。安装的 demo 的 <code>DT_RUNPATH</code> 是 <code>$ORIGIN</code> 相对，
        同样不依赖构建树。宿主二进制若不在这个前缀里链接，运行时仍需
        <code>LD_LIBRARY_PATH</code> 或 ldconfig 条目。
      </div>

      <h2>依赖</h2>
      <div class="table-responsive">
        <table class="table table-sm table-striped align-middle">
          <thead><tr><th>软件包</th><th>开发机版本</th><th>最低要求</th></tr></thead>
          <tbody>
            <tr v-for="d in depRows" :key="d[0]">
              <td>{{ d[0] }}</td><td>{{ d[1] }}</td><td>{{ d[2] }}</td>
            </tr>
          </tbody>
        </table>
      </div>
      <div class="doc-note mb-3">
        <code>wayland-scanner</code> 必须在 <code>PATH</code> 上——协议代码在构建期生成，
        仓库里不提交生成物。
      </div>

      <h2>meson 选项</h2>
      <div class="table-responsive">
        <table class="table table-sm table-striped align-middle">
          <thead><tr><th>选项</th><th>默认</th><th>决定什么</th></tr></thead>
          <tbody>
            <tr v-for="o in optionRows" :key="o.name">
              <td><code>{{ o.name }}</code></td>
              <td>{{ o.def }}</td>
              <td class="text-secondary small">{{ o.what }}</td>
            </tr>
          </tbody>
        </table>
      </div>
      <div class="doc-note warn">
        在没跑过 cmake 的树上开 <code>install-helper</code> 是配置期错误——不会装出一个
        信心满满的空目录。把 <code>helper-build-dir</code> 指到真正的 cmake 产物目录即可。
      </div>

      <h2>更换 CEF 版本</h2>
      <p>
        一次 CEF 版本升级意味着重跑整套门禁：Chromium 里程碑会改变 client 请求的 Wayland 协议集合，
        须逐项重新实测。项目里的重测记录是 <code>docs/internals/protocol-ceilings.md</code>；
        构建产物的版本号则由 <code>cefnest-version</code> 决定，CNB 发版时会自动设成所捆绑的 CEF
        X.Y.Z。
      </p>
    </template>

    <!-- ============ 架构与实现 ============ -->
    <template v-else-if="section === 'arch'">
      <h1>架构与实现</h1>
      <p>
        两条约束决定了整个项目：<strong>原生 Wayland</strong>（没有 XWayland、X11 client 或
        XEMBED）与<strong>无 OSR</strong>（没有把离屏渲染缓冲拷进 Cairo surface 的步骤）。
        这排除了所有不自带合成器的“GTK 嵌浏览器”方案，所以仓库带了一个合成器。
      </p>

      <h2>三条路</h2>
      <div class="table-responsive">
        <table class="table table-sm table-striped align-middle">
          <thead><tr><th style="width: 14%">路径</th><th>怎么走</th></tr></thead>
          <tbody>
            <tr>
              <td class="fw-semibold">像素</td>
              <td>
                Chromium 以 ozone/wayland 连到进程内的嵌套合成器 socket，提交 shm / dmabuf 缓冲；
                合成器按 surface 栈合成，部件把它画成 <code>GdkTexture</code>。
              </td>
            </tr>
            <tr>
              <td class="fw-semibold">输入</td>
              <td>
                部件持有真实指针与键盘，把事件翻译成 <code>wl_pointer</code> / <code>wl_keyboard</code>
                语义前转；输入法经 <code>zwp_text_input_v3</code> 由部件的 <code>GtkIMContext</code> 代答。
              </td>
            </tr>
            <tr>
              <td class="fw-semibold">控制</td>
              <td>
                unix socket 上的双向 NDJSON。<code>ChromiumSession</code> 是宿主一半，helper 是
                Chromium 一半；失败的加载、死掉的 renderer 都在这条路上成形。
              </td>
            </tr>
          </tbody>
        </table>
      </div>

      <h2>三个组件</h2>
      <div class="table-responsive">
        <table class="table table-sm table-striped align-middle">
          <thead><tr><th>组件</th><th>位置</th><th>职责</th></tr></thead>
          <tbody>
            <tr>
              <td><code>NestedCompositor</code></td>
              <td><code>src/nested*.c</code></td>
              <td>合成器：globals、surface、damage、输入、剪贴板、跨窗口边界的双向拖放。</td>
            </tr>
            <tr>
              <td><code>ChromiumView</code></td>
              <td><code>src/chromium-view.c</code></td>
              <td>GTK 部件：把 GdkSurface 的输入交给合成器，把合成结果画出来。</td>
            </tr>
            <tr>
              <td><code>ChromiumSession</code></td>
              <td><code>src/chromium-session.c</code></td>
              <td>控制面：启动/停止 helper、跟踪页面状态、执行 JavaScript、代理下载、查找、菜单与权限。</td>
            </tr>
          </tbody>
        </table>
      </div>

      <h2>进程、沙箱与生命周期</h2>
      <p>
        helper 是独立进程的 CEF 应用；renderer 由 Chromium 自己的沙箱包裹，helper 本身不在沙箱里，
        也设不了——Wayland 连接、GPU 缓冲和控制 socket 都在它手里。不需要 setuid helper；
        禁掉非特权用户命名空间的宿主则根本起不来。进程树与 fd 数在反复导航和强杀 renderer 之间保持平稳。
      </p>
      <p>
        页面声音经 helper 派生的 audio 服务直达宿主扬声器，绕开部件；<code>set_muted</code>
        是嵌入应用唯一的开关。一个 helper 可承载多个 CEF browser：标签共享一个
        <code>CefWindow</code>，窗口式 browser 各有独立 <code>CefWindow</code>。两种都能被宿主关闭，
        也都会发 <code>::browser-closed</code>；关掉最后一个 browser 才结束 helper。
      </p>
      <p>
        生命周期分两个时刻：<code>::closed</code> 表示页面没了、本会话不会再来；
        <code>::helper-exited</code> 表示子进程已被 wait 收走。后者触发时，
        <code>CefShutdown()</code> 可能还在用那条 Wayland 连接，所以宿主应等它再拆共享合成器。
      </p>

      <h2>帧时钟与资源边界</h2>
      <ul>
        <li><strong>帧时钟节拍</strong>：client 的帧回调按 GTK 帧时钟而不是定时器放行——隐藏窗口连
          client 一起暂停；没有时钟的测试环境退回 16 ms 定时器，并有看门狗兜底永远不来的显示周期。</li>
        <li><strong>资源上限</strong>：每 pool 与每连接都有 shm 上限和分配检查，dmabuf fd 有泄漏防线；
          每次 map 挂上的东西都必须有拆下的路。</li>
        <li><strong>格式实测</strong>：dmabuf 的 feedback 格式表由本进程能导入什么（GDK 通告的集合）
          决定，而不是静态四格式通告；client 绑定的协议与版本上限逐项实测判定。</li>
        <li><strong>双剪贴板</strong>：普通剪贴板与主选区（中键）各自独立成桥，双向同步。</li>
      </ul>

      <h2>通告的 globals</h2>
      <CodeBlock label="Wayland globals（括号内为协议版本）" lang="plaintext" :code="globalsText" />
      <p class="small text-secondary">
        版本上限不是照协议 XML 抄的，而是逐项实测 client 能用到哪一版；记录在
        <code>docs/internals/protocol-ceilings.md</code>。Chromium 绑定的
        <code>zwp_pointer_gestures</code> 与 <code>zwp_tablet_manager_v2</code> 没有通告，
        因此页面失去的正好是捏合缩放与数位板输入。
      </p>

      <h2>设计边界</h2>
      <ul class="mb-0">
        <li><strong>原生 Wayland only</strong>：X11 会话下 GDK 给不了可交给 client 的 Wayland surface。</li>
        <li><strong>单 seat、单指针、无触控</strong>：嵌入部件是合成器认识的唯一设备。</li>
        <li><strong>API 不稳定</strong>：版本之间允许加参数、改头文件形状；对照自己构建的版本编译。</li>
      </ul>
    </template>

    <!-- ============ 运行与调试 ============ -->
    <template v-else-if="section === 'run'">
      <h1>运行与调试</h1>
      <p>
        <code>cefnest-demo</code> 是主要的实验工具：它既是 demo，也是全部门禁的驱动。所有配置都来自
        环境变量，因此一次运行可以原样复现。下面三种启动方式覆盖真 Chromium、安装树与无 Chromium
        的合成器演练。
      </p>

      <h2>三种启动方式</h2>
      <CodeBlock label="bash" lang="bash" :code="runModes" />
      <div class="doc-note">
        第 1 种之所以不用指 helper，是因为库自己找得到 cmake 的构建产物。第 3 种把
        <code>CEFNEST_HELPER</code> 指到不存在的路径，解析在这一步就停住，demo 才肯退回
        <code>shm-client</code>——环境变量里写下的路径不会被开发树里的 helper 顶掉。
        安装好的版本从 <code>prefix/bin/cefnest-demo</code> 启动，helper 在安装路径里。
      </div>

      <h2>helper 如何被找到</h2>
      <p>按序淘汰，命中即止：</p>
      <ol>
        <li><code>CEFNEST_HELPER</code>——含 <code>/</code> 的路径，最高优先；</li>
        <li><code>CHROMIUM_HELPER</code>——同样须含 <code>/</code> 才算路径；</li>
        <li>配置期编入库内的安装路径（<code>prefix/libexecdir</code> + 主机架构），存在且可执行；</li>
        <li>cmake 开发构建产物 <code>helper/build/Release</code>，存在且可执行；</li>
        <li>都没有——返回配置路径本身，让 <code>start()</code> 报一个可对照的错。</li>
      </ol>
      <p>
        <code>chromium_session_new(path)</code> 给的显式路径压过前两位；它不需要再被环境变量否掉。
        <code>CHROMIUM_HELPER</code> 设了任意值只会让 demo 进入 session 模式，值本身不含
        <code>/</code> 时并不当作路径。
      </p>

      <h2>核心环境变量</h2>
      <div class="table-responsive">
        <table class="table table-sm table-striped align-middle">
          <thead><tr><th style="width: 30%">变量</th><th>作用</th></tr></thead>
          <tbody>
            <tr v-for="e in envRows" :key="e.name">
              <td><code>{{ e.name }}</code></td>
              <td class="small">{{ e.what }}</td>
            </tr>
          </tbody>
        </table>
      </div>

      <h2>demo 面板相关变量</h2>
      <p class="text-secondary">
        这些只影响 demo 自己的 UI 与面板，不影响库的默认行为；列在这里是为了让运行可复现。
      </p>
      <div class="table-responsive">
        <table class="table table-sm table-striped align-middle">
          <thead><tr><th style="width: 32%">变量</th><th>作用</th></tr></thead>
          <tbody>
            <tr v-for="r in demoEnvRows" :key="r[0]">
              <td><code>{{ r[0] }}</code></td>
              <td class="small">{{ r[1] }}</td>
            </tr>
          </tbody>
        </table>
      </div>

      <h2>脚本化运行</h2>
      <p>
        <code>CHROMIUM_SESSION_SCRIPT</code> 指向一个文本文件，一行一条命令，空行忽略。第一步页面命令
        会等 helper 的 <code>::ready</code> 再发；没有 helper 时，页面命令会明确报错并结束脚本，
        而不是假装跑过。合成器输入与抓图不需要 session。
      </p>
      <div class="table-responsive">
        <table class="table table-sm table-striped align-middle">
          <thead><tr><th style="width: 22%">分组</th><th>命令</th></tr></thead>
          <tbody>
            <tr v-for="g in scriptGroups" :key="g.title">
              <td class="fw-semibold">{{ g.title }}</td>
              <td class="small font-monospace">{{ g.cmds }}</td>
            </tr>
          </tbody>
        </table>
      </div>
      <CodeBlock label="session 脚本示例" lang="plaintext" :code="scriptExample" />
      <div class="doc-note">
        <code>shot</code> / <code>dump</code> 写 PNG，<code>surfaces</code> 打印当前绘制栈，
        <code>frames</code> 读帧计数。输入坐标是嵌套合成器 surface 的像素坐标；
        <code>click</code> 的按钮默认左键，<code>wheel</code> 的单位是滚轮格。
      </div>

      <h2>无头验证</h2>
      <p>
        树上需要真窗口的门禁可以跑在嵌套的无头合成器里：自己的 HOME、XDG 目录和 D-Bus bus，
        不影响任何人的桌面。每个门禁一个合成器，末尾输出判定表。
      </p>
      <CodeBlock
        label="bash"
        lang="bash"
        :code="`./test/ci-session.sh ./test/find-gate.sh   # 任何门禁，无头
./test/ci-all.sh                            # 全部
CI_ALL_REAL=1 ./test/ci-all.sh real         # 外部站点，可选

meson test -C builddir                      # 不需要窗口的单元与源码审计
meson test -C builddir --suite selftest     # seat selftest + map-cycle
meson test -C builddir --suite protocol     # 三个源码审计`"
      />

      <h2>故障排查</h2>
      <div class="table-responsive">
        <table class="table table-sm table-striped align-middle">
          <thead><tr><th style="width: 34%">症状</th><th>先查什么</th></tr></thead>
          <tbody>
            <tr v-for="t in troubleRows" :key="t.symptom">
              <td class="small fw-semibold">{{ t.symptom }}</td>
              <td class="small text-secondary">{{ t.fix }}</td>
            </tr>
          </tbody>
        </table>
      </div>
      <p class="small text-secondary mb-0">
        更细的记录：<code>docs/testing/overview.md</code>（每条门禁证明什么）、
        <code>docs/testing/headless-ci.md</code>（无头 runner）、
        <code>docs/guides/packaging.md</code>（helper 解析与安装布局）。
      </p>
    </template>

    <!-- ============ 已知限制 ============ -->
    <template v-else-if="section === 'limits'">
      <h1>已知限制</h1>
      <p>
        下面这些是当前版本真实测到的边角行为，按影响面分组。完整证据与测量记录在
        <code>docs/known-limitations.md</code>；这里保留使用前值得知道的部分。
      </p>
      <div v-for="g in limitGroups" :key="g.title" class="mb-4">
        <h2>{{ g.title }}</h2>
        <ul class="mb-0">
          <li v-for="(t, i) in g.items" :key="i">{{ t }}</li>
        </ul>
      </div>
      <div class="doc-note warn">
        <strong>API 尚不稳定。</strong> 本页描述的是 {{ release.version }} 的行为；升级版本时请重读
        这一页与 Release 说明，并按自己构建的版本编译宿主。
      </div>
    </template>
  </DocLayout>
</template>
