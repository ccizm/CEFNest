<script setup>
import { computed } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import DocLayout from '../components/DocLayout.vue'
import CodeBlock from '../components/CodeBlock.vue'

const route = useRoute()
const router = useRouter()

const items = [
  { key: 'start', label: '快速开始' },
  { key: 'arch', label: '架构总览' },
  { key: 'build', label: '构建与安装' },
  { key: 'limits', label: '已知限制' },
]
const section = computed(() => {
  const s = route.params.section
  if (items.some((i) => i.key === s)) return s
  router.replace('/dev/start')
  return 'start'
})
</script>

<template>
  <DocLayout :items="items" heading="开发者文档">
    <!-- ============ 快速开始 ============ -->
    <template v-if="section === 'start'">
      <h1>快速开始</h1>
      <p>
        CEFNest 有<strong>两种使用方法</strong>，区别在 Chromium 运行时从哪来：
        <strong>方法一</strong>安装 deb / rpm 包，宿主以 pkg-config 调用 runtime；
        <strong>方法二</strong>在项目内构建部署，装出的树自带 helper 与整个 CEF 运行时。
        两条硬性前提相同：<strong>GTK 4.14+</strong> 与带 <code>axis_value120</code>
        的 Wayland（1.21+）。没有系统桌面也能跑——测试基建自带嵌套无头合成器。
      </p>

      <h2>方法一：安装 deb / rpm，调用 runtime</h2>
      <p>
        流水线产出四个包：<code>libcefnest0</code>（共享库）、
        <code>libcefnest-dev</code>（头文件与 <code>cefnest.pc</code>）、
        <code>cefnest-helper</code>（helper 与整个 CEF 运行时，约 1.5 GB，版本号跟随其构建所用的
        CEF 发行版）、<code>cefnest-demo</code>（演示程序，依赖 helper 包，含回退用的
        <code>shm-client</code>）。装好后宿主一行编译、直接调用 runtime：
      </p>
      <CodeBlock
        label="bash"
        lang="bash"
        :code="`# Debian 系 / RPM 系包名相同
apt  install libcefnest0 libcefnest-dev cefnest-helper cefnest-demo   # 或 dnf install …

# 宿主对 runtime 编译
cc host.c -o host \$(pkg-config --cflags --libs cefnest)

# 跑 demo 体验
cefnest-demo`"
      />
      <div class="doc-note">
        helper 与 CEF 运行时都在 <code>cefnest-helper</code> 包里，随 demo 的依赖关系自动装上——
        装齐即跑真 Chromium，不需要方法二。只做开发、不跑浏览器时可以只装
        <code>libcefnest0</code> 与 <code>libcefnest-dev</code>，此时 demo 回退到
        <code>shm-client</code>，合成器、seat、剪贴板桥照样可演练。
      </div>

      <h2>方法二：项目包含部署</h2>
      <p>
        在项目内完成全部构建，装出一棵自含安装树：helper 与整个 CEF 运行时随行，
        约 1.5 GB，之后不依赖构建树里的任何东西，整体可搬。
      </p>
      <CodeBlock
        label="bash"
        lang="bash"
        :code="`# 1. 拉取固定版本的 CEF 二进制发行包（gitignore，解压后约 500 MB）
./scripts/fetch-cef.sh

# 2. Chromium 侧 —— 自动使用 third_party/cef/dist
cmake -S helper -B helper/build -DCMAKE_BUILD_TYPE=Release
cmake --build helper/build -j

# 3. 部件 + helper + CEF 运行时，装出自含树
meson setup builddir -Dprefix=$HOME/.local/cefnest-stage -Dinstall-helper=true
ninja -C builddir install

# 运行：无需任何环境变量
CHROMIUM_URL=https://cnb.cool/yanzhiming/CEFNest $HOME/.local/cefnest-stage/bin/cefnest-demo`"
      />
      <div class="doc-note">
        装出的 helper 从自身旁边解析 CEF 运行时（整个 <code>third_party/cef/dist</code>
        被挪走后实测过）；安装的 demo 的 <code>DT_RUNPATH</code> 是
        <code>$ORIGIN</code> 相对，也不依赖构建树。
      </div>

      <h2>依赖</h2>
      <div class="table-responsive">
        <table class="table table-sm table-striped align-middle">
          <thead><tr><th>软件包</th><th>开发机版本</th><th>最低要求</th></tr></thead>
          <tbody>
            <tr><td>gtk4</td><td>4.18.6</td><td>4.14</td></tr>
            <tr><td>wayland（server + client）</td><td>1.23.1</td><td>1.21+（axis_value120）</td></tr>
            <tr><td>wayland-protocols</td><td>1.44</td><td>须含 cursor-shape / fractional-scale / text-input</td></tr>
            <tr><td>xkbcommon</td><td>1.7.0</td><td>—</td></tr>
            <tr><td>gbm / libdrm</td><td>25.0.7 / 2.4.124</td><td>—</td></tr>
            <tr><td>json-glib</td><td>1.10.6</td><td>—</td></tr>
            <tr><td>meson / ninja / cmake</td><td>1.7 / 1.12 / 3.16+</td><td>meson 1.3</td></tr>
            <tr><td>CEF（linux64/arm64 minimal）</td><td>154.0.34+g14c5a08</td><td>能用自带 CMake helpers 构建者</td></tr>
          </tbody>
        </table>
      </div>
      <div class="doc-note mb-3">
        <code>wayland-scanner</code> 必须在 <code>PATH</code> 上——协议代码在构建期生成，仓库里不提交生成物。
      </div>

      <h2>开发构建（不入安装树）</h2>
      <p>
        与方法二的差别只在最后一步不开 <code>install-helper</code>：装出来的是纯开发环境，
        demo 从 <code>builddir</code> 直接运行，helper 用 cmake 的开发产物。
      </p>
      <CodeBlock
        label="bash"
        lang="bash"
        :code="`# 1. 拉取固定版本的 CEF 二进制发行包（gitignore，解压后约 500 MB）
./scripts/fetch-cef.sh

# 2. Chromium 侧 —— 自动使用 third_party/cef/dist
cmake -S helper -B helper/build -DCMAKE_BUILD_TYPE=Release
cmake --build helper/build -j

# 3. 部件、demo、测试
meson setup builddir
ninja -C builddir`"
      />
      <p>
        跳过第 1 步照样能构建：demo 会退而驱动 <code>test/shm-client</code>，
        足以演练合成器、seat、剪贴板桥和弹窗——只是没有真 Chromium。
      </p>
      <div class="doc-note warn">
        更换 CEF 版本意味着重跑全部门禁：Chromium 里程碑同时改变 client 请求的
        Wayland 协议集合，须逐项重新实测。
      </div>

      <h2>运行 demo</h2>
      <CodeBlock
        label="bash"
        lang="bash"
        :code="`# 真 Chromium —— cmake 构建过 helper 后无需任何环境变量
CHROMIUM_URL=https://cnb.cool/yanzhiming/CEFNest ./builddir/src/cefnest-demo

# 不涉及 Chromium：指一个不存在的 helper，demo 退回 shm-client
CEFNEST_HELPER=/nonexistent-helper ./builddir/src/cefnest-demo`"
      />
      <p>
        demo 的全部配置都来自环境变量，便于复现。核心八个：
        <code>CHROMIUM_URL</code>（要打开的页面）、
        <code>CEFNEST_HELPER</code>（含 <code>/</code> 的路径，优先级最高）、
        <code>CHROMIUM_HELPER</code>（设为任意值即强制会话模式）、
        <code>CHROMIUM_SESSION_SCRIPT</code>（一行一条命令的脚本文件，
        支持 <code>load / js / find / menu-pick / download-accept</code> 等）、
        <code>CHROMIUM_DUMP</code> / <code>CHROMIUM_DUMP_DELAY</code>（N 毫秒后抓 PNG 并退出）、
        <code>CHROMIUM_HELPER_NO_SANDBOX</code>（必须恰为 <code>1</code> 才关沙箱，写错 anything else 都保持开启）、
        <code>CHROMIUM_USER_AGENT</code>（宿主在 <code>chromium_session_start()</code> 前放进自己环境的默认 UA，
        随环境继承给 helper；<code>examples/cefnest-useragent</code> 演示运行中切换）、
        <code>G_MESSAGES_DEBUG</code>（<strong>空格</strong>分隔的域列表：
        <code>all</code> 或 <code>nested chromium-view chromium-session</code>——逗号列表什么都匹配不上）。
        其余（下载/权限/拖拽面板模式、剪贴板与主选区种子、无障碍、帧指标、窗口几何）随 demo 源码给出。
      </p>
    </template>

    <!-- ============ 架构总览 ============ -->
    <template v-else-if="section === 'arch'">
      <h1>架构总览</h1>
      <p>
        两条约束决定了整个项目：<strong>原生 Wayland</strong>（没有 XWayland / X11 client / XEMBED）
        与<strong>无 OSR</strong>（没有离屏渲染缓冲拷贝进 Cairo surface）。
        这排除了所有不自带合成器的"GTK 嵌浏览器"方案，所以仓库带了一个合成器。
      </p>

      <h2>数据与控制</h2>
      <ul>
        <li><strong>像素路</strong>：Chromium 以 ozone/wayland 连到进程内的嵌套合成器 socket，
          提交 shm / dmabuf 缓冲；合成器按 surface 栈合成，部件把它画成 GdkTexture。</li>
        <li><strong>输入路</strong>：部件持有真实指针与键盘，把事件翻译成 wl_pointer / wl_keyboard
          语义前转；输入法经 zwp_text_input_v3 由部件的 GtkIMContext 代答。</li>
        <li><strong>控制路</strong>：unix socket 上的双向 NDJSON。session 是宿主一半，
          helper 是 Chromium 一半；失败的加载、死掉的 renderer 都在这条路上成形。</li>
      </ul>

      <h2>会话如何找到 helper</h2>
      <p>按序淘汰，命中即止：</p>
      <ol>
        <li><code>CEFNEST_HELPER</code>——含 <code>/</code> 的路径，最高优先；</li>
        <li><code>CHROMIUM_HELPER</code>——同样须含 <code>/</code> 才算路径；</li>
        <li>配置期编入库内的安装路径（<code>prefix/libexecdir</code> + 主机架构）；</li>
        <li>cmake 开发构建产物 <code>helper/build/Release</code>；</li>
        <li>都没有——返回配置路径本身，让 <code>start()</code> 报一个可对照的错。</li>
      </ol>
      <div class="doc-note">
        装好的 helper 不依赖构建树：CEF 运行时随行安装、就地解析——
        把整个 <code>third_party/cef/dist</code> 挪走后实测过。
      </div>

      <h2>进程与沙箱</h2>
      <p>
        helper 是独立进程的 CEF 应用；renderer 由 Chromium 自己的沙箱包裹，
        helper 本身不在沙箱里，也设不了——不需要 setuid helper，没有用户命名空间的
        宿主照样能跑，进程树与 fd 数在反复导航和强杀 renderer 之间保持平稳。
        页面声音经 helper 派生的 audio 服务直达宿主扬声器，绕开部件；
        <code>set_muted</code> 是嵌入应用唯一的开关。
      </p>

      <h2>设计要点</h2>
      <ul class="mb-0">
        <li><strong>按协议拆模块</strong>：合成器按 Wayland 协议拆成 seat / dmabuf / xdg /
          cursor / text-input / data-device 等模块，每个全局一份代码。</li>
        <li><strong>帧时钟节拍</strong>：client 的帧回调按 GTK 帧时钟而不是定时器放行——
          隐藏窗口连 client 一起暂停；没有时钟的测试环境退回 16 ms 定时器，并有看门狗兜底
          永远不来的显示周期。</li>
        <li><strong>资源上限</strong>：每 pool 与每连接都有 shm 上限和分配检查，dmabuf fd
          有泄漏防线；每次 map 挂上的东西都必须有拆下的路。</li>
        <li><strong>格式实测</strong>：dmabuf 的 feedback 格式表由本进程能导入什么（GDK
          通告的集合）决定，而不是静态四格式通告；client 绑定的协议与版本上限逐项实测判定。</li>
        <li><strong>双剪贴板</strong>：普通剪贴板与主选区（中键）各自独立成桥，双向同步。</li>
      </ul>
    </template>

    <!-- ============ 构建与安装 ============ -->
    <template v-else-if="section === 'build'">
      <h1>构建与安装</h1>
      <p>
        组件安装为共享库 + pkg-config 文件 + 三个头文件 + demo。
      </p>
      <CodeBlock
        label="bash"
        lang="bash"
        :code="`meson setup builddir -Dprefix=$HOME/.local/cefnest-stage -Dinstall-helper=true
ninja -C builddir install`"
      />
      <h2>meson 选项</h2>
      <div class="table-responsive">
        <table class="table table-sm table-striped align-middle">
          <thead><tr><th>选项</th><th>默认</th><th>决定什么</th></tr></thead>
          <tbody>
            <tr>
              <td><code>helper-path</code></td><td>空，由 prefix + libexecdir + 主机架构推导</td>
              <td>编入库内的路径；无人指名 helper 时使用</td>
            </tr>
            <tr>
              <td><code>helper-build-dir</code></td><td><code>helper/build/Release</code></td>
              <td>cmake 的产物：开发回退路径，也是安装来源</td>
            </tr>
            <tr>
              <td><code>install-helper</code></td><td><code>false</code></td>
              <td>连同 helper 与整个 CEF 运行时一起安装——约 1.5 GB，故为选装</td>
            </tr>
            <tr>
              <td><code>install-demo</code></td><td><code>true</code></td>
              <td>安装 <code>cefnest-demo</code> 及其回退用的 <code>shm-client</code></td>
            </tr>
          </tbody>
        </table>
      </div>
      <div class="doc-note warn">
        cmake 没跑过的树上开 <code>install-helper</code> 是配置期错误——不会装出一个
        信心满满的空目录。
      </div>

      <h2>二进制包</h2>
      <p>
        CNB 流水线把安装树打成 deb 与 rpm，共四个包：<code>libcefnest0</code>、
        <code>libcefnest-dev</code>、<code>cefnest-helper</code>、<code>cefnest-demo</code>。
        helper 与整个 CEF 运行时单独放在 <code>cefnest-helper</code> 包里——
        树是硬链接进包目录的，约 1.5 GB，包版本号跟随其构建所用的 CEF 发行版；
        <code>cefnest-demo</code> 依赖它。只要库的宿主可以不装 helper 包，
        此时 demo 回退到自家包里的 <code>shm-client</code>。装好后宿主一行编译：
      </p>
      <div class="table-responsive mb-3">
        <table class="table table-sm table-bordered align-middle mb-0">
          <thead>
            <tr><th>内容</th><th>deb 产物</th><th>rpm 产物</th></tr>
          </thead>
          <tbody>
            <tr>
              <td>运行库</td>
              <td><code>libcefnest0_版本-1_架构.deb</code></td>
              <td><code>cefnest-版本-1.架构.rpm</code></td>
            </tr>
            <tr>
              <td>开发文件（头文件 + pkg-config）</td>
              <td><code>libcefnest-dev_版本-1_架构.deb</code></td>
              <td><code>cefnest-devel-版本-1.架构.rpm</code></td>
            </tr>
            <tr>
              <td>helper + CEF 运行时（约 1.5 GB）</td>
              <td><code>cefnest-helper_版本-1_架构.deb</code></td>
              <td><code>cefnest-helper-版本-1.架构.rpm</code></td>
            </tr>
            <tr>
              <td>demo</td>
              <td><code>cefnest-demo_版本-1_架构.deb</code></td>
              <td><code>cefnest-demo-版本-1.架构.rpm</code></td>
            </tr>
          </tbody>
        </table>
      </div>
      <p>
        架构位：deb 为 <code>_amd64</code> / <code>_arm64</code>，rpm 为
        <code>.x86_64</code> / <code>.aarch64</code>。每个包同时生成一份
        <strong>不带版本号的短名拷贝</strong>（<code>libcefnest0.deb</code>、
        <code>cefnest.rpm</code> 等），本地安装不用抄版本；全部产物另打成
        <code>cefnest-packages.tar.gz</code>，配 <code>SHA256SUMS</code> 校验。
        两系依赖关系一致：demo 依赖运行库与 helper；helper 独立可装；运行库对
        helper 只是 Recommends。
      </p>
      <CodeBlock label="bash" lang="bash" :code="`cc host.c -o host \$(pkg-config --cflags --libs cefnest)`" />
      <p>
        前缀之外的二进制需要 <code>LD_LIBRARY_PATH</code> 或 ldconfig 条目；
        安装的 demo 不需要——它的 <code>DT_RUNPATH</code> 是 <code>$ORIGIN</code> 相对。
      </p>
    </template>

    <!-- ============ 已知限制 ============ -->
    <template v-else-if="section === 'limits'">
      <h1>已知限制</h1>
      <p>使用前值得知道的全部边角行为：</p>
      <ul class="mb-0">
        <li>拖出与拖入端到端可用，人工验证过。</li>
        <li>单 seat、单指针、无触控。</li>
        <li>独立宿主窗口能拿到输入——但从一个窗口里起的拖拽会从错误的 surface 离开。</li>
        <li>标签模式的浏览器关不掉，请求被无声丢弃。</li>
        <li>该 CEF 发行包没有 H.264/MP4；VP9 能放。</li>
        <li>无捏合缩放，无数位板输入。</li>
        <li>失败的导航报告两次，且 <code>::load-end</code> 照常发出。</li>
        <li>只有 renderer 的死亡被报告，且只报死因。</li>
        <li>下载可接受 / 拒绝 / 取消 / 暂停续传——但 <code>complete</code> 不是文件落盘的时刻。</li>
        <li>查找一次走一个匹配。</li>
        <li>helper 能扩展右键菜单；宿主还不能。</li>
        <li>权限只能整体授予或拒绝，不能收窄——不回答的权限连累页面的下一个问题。</li>
        <li>页面的声音完全绕开部件；<code>set_muted</code> 是唯一的开关。</li>
        <li>renderer 有沙箱；helper 没有，也设不了。</li>
        <li>只支持原生 Wayland——这是设计，不是疏漏。</li>
        <li><strong>API 尚不稳定。</strong></li>
      </ul>
    </template>
  </DocLayout>
</template>
