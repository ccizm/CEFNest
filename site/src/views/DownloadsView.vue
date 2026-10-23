<script setup>
import CodeBlock from '../components/CodeBlock.vue'
import { release, packages, extras, highlights, assetUrl } from '../data/release.js'

const verify = `# 与下载的包放在同一目录；只校验已经下载的文件
sha256sum -c --ignore-missing SHA256SUMS`

const debInstall = `# 只运行宿主：一个包就够，helper 与 CEF 都在里面
sudo apt install ./cefnest_${release.version}_amd64.deb

# 还要编译宿主：把开发包一并给出
sudo apt install ./cefnest_${release.version}_amd64.deb \\
  ./cefnest-dev_${release.version}_amd64.deb`

const rpmInstall = `# 只运行宿主
sudo dnf install ./cefnest-${release.version}.x86_64.rpm

# 编译宿主时再加开发包
sudo dnf install ./cefnest-${release.version}.x86_64.rpm \\
  ./cefnest-devel-${release.version}.x86_64.rpm`

const afterInstall = `# 宿主对已安装的库编译
cc host.c -o host $(pkg-config --cflags --libs cefnest)

# demo 不随包发布：它在构建树里，$ORIGIN 已指向同批的库
tar xzf cefnest-build.tar.gz
CHROMIUM_URL=https://cnb.cool/yanzhiming/CEFNest ./src/cefnest-demo`
</script>

<template>
  <div class="downloads-page">
    <!-- Hero -->
    <section class="bg-dark text-white py-5">
      <div class="container">
        <div class="row align-items-center g-4">
          <div class="col-lg-8">
            <span class="hero-badge"><i class="bi bi-tag me-1"></i>{{ release.version }} · 当前版本</span>
            <h1 class="display-5 fw-bold mb-3">下载 CEFNest</h1>
            <p class="lead-text lead mb-3">
              两个安装包：组件库带着 helper 与完整的 CEF，开发包只放编译宿主需要的头文件与
              pkg-config。deb 与 rpm 内容相同，按发行版选一套即可。
            </p>
            <p class="small text-white-50 mb-4">
              CEF {{ release.cef }} · Chromium {{ release.chromium }} · 本版资产为 amd64 / x86_64 ·
              需 glibc 2.39+ / GTK 4.14+
            </p>
            <div class="d-flex gap-2 flex-wrap">
              <a :href="release.page" class="btn btn-light btn-lg px-4" target="_blank" rel="noopener">
                <i class="bi bi-journal-text me-1"></i> Release 说明
              </a>
              <a :href="assetUrl('SHA256SUMS')" class="btn btn-outline-light btn-lg px-4">
                <i class="bi bi-shield-check me-1"></i> SHA-256 校验和
              </a>
              <router-link to="/dev/build" class="btn btn-link text-white btn-lg px-2">
                从源码构建 <i class="bi bi-arrow-right"></i>
              </router-link>
            </div>
          </div>
          <div class="col-lg-4">
            <div class="arch-box">
              <div class="arch-title mb-2">运行与编译各取所需</div>
              <div class="small">
                <div><code class="text-white">cefnest</code> <span class="text-white-50">GTK 4 组件库 + helper + CEF</span></div>
                <div class="ms-2 text-white-50">↓ 编译宿主时再加</div>
                <div><code class="text-white">cefnest-dev</code> <span class="text-white-50">头文件 · .so · cefnest.pc</span></div>
              </div>
              <hr class="border-light opacity-25">
              <small>两个包同版本发布；demo 与 shm-client 只留在构建树里。</small>
            </div>
          </div>
        </div>
      </div>
    </section>

    <section class="py-5">
      <div class="container">
        <!-- 两个包 -->
        <div class="d-flex justify-content-between align-items-end flex-wrap gap-2 mb-4">
          <div>
            <h2 class="fw-bold mb-1">两个安装包</h2>
            <p class="text-secondary mb-0">
              同一版本号、同一次构建。运行宿主只需要第一个；要编译宿主时再加开发包。
            </p>
          </div>
          <a :href="release.page" class="small" target="_blank" rel="noopener">
            Release 页 <i class="bi bi-box-arrow-up-right ms-1"></i>
          </a>
        </div>
        <div class="row g-4">
          <div v-for="p in packages" :key="p.key" class="col-md-6">
            <div class="feature-card p-4 d-flex flex-column">
              <div class="d-flex align-items-center gap-2 mb-2">
                <span class="feature-icon"><i class="bi" :class="p.icon"></i></span>
                <div>
                  <h3 class="h5 fw-semibold mb-0">{{ p.title }}</h3>
                  <small class="text-secondary font-monospace">{{ p.names }}</small>
                </div>
              </div>
              <p class="text-secondary small mb-2">{{ p.desc }}</p>
              <p class="small text-secondary mb-3">
                <i class="bi bi-diagram-3 me-1"></i>{{ p.depends }}
              </p>
              <div class="d-flex gap-2 flex-wrap mt-auto">
                <a
                  class="btn btn-sm btn-outline-primary"
                  :href="assetUrl(p.deb)"
                  :title="p.deb"
                >
                  <i class="bi bi-download me-1"></i> deb · amd64
                </a>
                <a
                  class="btn btn-sm btn-outline-primary"
                  :href="assetUrl(p.rpm)"
                  :title="p.rpm"
                >
                  <i class="bi bi-download me-1"></i> rpm · x86_64
                </a>
              </div>
              <p class="small font-monospace text-secondary text-break mt-2 mb-0">
                {{ p.deb }}<br>{{ p.rpm }}
              </p>
            </div>
          </div>
        </div>

        <div class="doc-note mt-4">
          <strong>本地安装开发包时，两个文件要一起给出。</strong>
          包管理器不会自动搜索同目录的其它本地包；把 <code>cefnest</code> 与
          <code>cefnest-dev</code> 同时传给 <code>apt</code> / <code>dnf</code>，它才会按需解算。
          只运行宿主则只需第一个文件。从 154.0.34-6 升级不必手工卸载旧包：deb 侧用
          <code>Replaces</code> / <code>Breaks</code>，rpm 侧用 <code>Obsoletes</code> 替换
          <code>libcefnest0</code>、<code>cefnest-helper</code> 与 <code>cefnest-demo</code>。
        </div>

        <!-- 安装 -->
        <h2 class="fw-bold mt-5 mb-1">安装与校验</h2>
        <p class="text-secondary mb-4">
          先把下载的文件与 <code>SHA256SUMS</code> 放进同一目录。校验命令只检查已经下载的条目。
        </p>
        <CodeBlock label="bash · 校验" lang="bash" :code="verify" />
        <div class="row g-4">
          <div class="col-lg-6">
            <div class="feature-card p-4">
              <h3 class="h5 fw-semibold mb-3"><i class="bi bi-ubuntu me-2"></i>Debian / Ubuntu</h3>
              <CodeBlock label="bash" lang="bash" :code="debInstall" />
              <p class="small text-secondary mb-0">
                包在 Ubuntu 24.04 上构建：Debian 13、Ubuntu 24.04 及 glibc 2.39+ 的同代发行版
                可直接安装；更老的 Debian / Ubuntu 请从源码构建。组件包安装后约 1.5 GB。
              </p>
            </div>
          </div>
          <div class="col-lg-6">
            <div class="feature-card p-4">
              <h3 class="h5 fw-semibold mb-3"><i class="bi bi-fedora me-2"></i>Fedora / RPM 系</h3>
              <CodeBlock label="bash" lang="bash" :code="rpmInstall" />
              <p class="small text-secondary mb-0">
                RPM 的包名是 <code>cefnest</code> / <code>cefnest-devel</code>；适用于
                Fedora 40+ 等 glibc 2.39+ 的发行版，更老的系统请从源码构建。
              </p>
            </div>
          </div>
        </div>
        <CodeBlock label="bash · 编译宿主并运行 demo" lang="bash" :code="afterInstall" />

        <div class="doc-note warn">
          <strong>helper 默认开启 Chromium 沙箱。</strong>
          它需要可用的非特权用户命名空间。Ubuntu 24.04 默认限制 userns，helper 会以
          <code>No usable sandbox!</code> 响亮失败，不会静默降级。给
          <code>/usr/libexec/cefnest/&lt;arch&gt;/cefnest-helper</code> 单独加一条允许 userns
          的 AppArmor profile，或显式设置 <code>CHROMIUM_HELPER_NO_SANDBOX=1</code>——两条路的
          完整写法见 <router-link to="/dev/start">开始之前</router-link>。包里的
          <code>chrome-sandbox</code> 是普通 0755，没有 setuid 安装步骤。
        </div>

        <!-- 其它资产 -->
        <h2 class="fw-bold mt-5 mb-1">其它资产</h2>
        <p class="text-secondary mb-4">完整构建树与校验清单；demo 和 shm-client 只在构建树里。</p>
        <div class="row g-4">
          <div v-for="e in extras" :key="e.key" class="col-md-6">
            <div class="feature-card p-4 h-100 d-flex flex-column">
              <span class="feature-icon mb-3"><i class="bi" :class="e.icon"></i></span>
              <h3 class="h6 fw-semibold mb-1">{{ e.title }}</h3>
              <p class="small font-monospace text-secondary text-break mb-2">{{ e.file }}</p>
              <p class="small text-secondary mb-2">{{ e.desc }}</p>
              <p class="small text-secondary mb-3"><strong>{{ e.note }}</strong></p>
              <a class="btn btn-sm btn-outline-primary mt-auto align-self-start" :href="assetUrl(e.file)">
                <i class="bi bi-download me-1"></i> 下载
              </a>
            </div>
          </div>
        </div>

        <!-- 本版变更 -->
        <div class="row g-4 mt-5 align-items-start">
          <div class="col-lg-8">
            <h2 class="fw-bold mb-3">本版变更</h2>
            <ul class="release-list mb-3">
              <li v-for="(h, i) in highlights" :key="i">{{ h }}</li>
            </ul>
            <a :href="release.page" target="_blank" rel="noopener" class="small">
              完整的中英文 Release 说明 <i class="bi bi-box-arrow-up-right ms-1"></i>
            </a>
          </div>
          <div class="col-lg-4">
            <div class="feature-card p-4">
              <h3 class="h6 fw-semibold mb-2">需要 arm64 或其它架构？</h3>
              <p class="small text-secondary mb-3">
                本版只发布 x86_64 资产。CEF 的 linuxarm64 路径已接好，但安装包需要在目标机构建；
                源码构建同样能装出一棵自含树。
              </p>
              <router-link to="/dev/build" class="btn btn-sm btn-outline-primary">
                从源码构建
              </router-link>
            </div>
          </div>
        </div>
      </div>
    </section>
  </div>
</template>
