<script setup>
import { computed, ref } from 'vue'
import CodeBlock from '../components/CodeBlock.vue'
import { release, packageKinds, buildTree, checksums, assetUrl } from '../data/release.js'
import { builds, buildStatuses, asOf } from '../data/builds.js'

const query = ref('')
const shown = ref(3)

const filtered = computed(() => {
  const q = query.value.trim()
  return q ? builds.filter((b) => b.version.includes(q)) : builds
})
const visible = computed(() => filtered.value.slice(0, shown.value))

const formats = [
  {
    key: 'deb', title: 'Debian / Ubuntu', arch: 'deb · amd64', icon: 'bi-ubuntu',
    component: 'deb', dev: 'debDev',
    hint: '包在 Ubuntu 24.04 上构建：Debian 13、Ubuntu 24.04 及 glibc 2.39+ 的同代发行版可直接安装；更老的 Debian / Ubuntu 请从源码构建。',
  },
  {
    key: 'rpm', title: 'Fedora / RPM 系', arch: 'rpm · x86_64', icon: 'bi-fedora',
    component: 'rpm', dev: 'rpmDev',
    hint: 'RPM 的包名是 cefnest / cefnest-devel；适用于 Fedora 40+ 等 glibc 2.39+ 的发行版，更老的系统请从源码构建。',
  },
]

const pageOf = (tag) => `${release.repository}/-/releases/tag/${tag}`

function cell (row, key) {
  if (!row.files) return null
  const name = row.files[key]
  return {
    name,
    url: row.status === 'published' ? assetUrl(name, row.tag) : '',
  }
}

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
  <div class="container py-4 py-lg-5">
    <h1 class="h2 fw-bold mb-2">构建产物</h1>
    <p class="text-secondary mb-1">
      每个版本 tag 一行：日期取 tag 的创建时间，文件名按该版本的包名规则给出。deb 与 rpm 内容
      一一对应，两个包共用一个版本号——CEF 的 <code>X.Y.Z</code> 加修订号。
    </p>
    <p class="small text-secondary mb-4">
      CEF {{ release.cef }} · Chromium {{ release.chromium }} · 本版资产为 amd64 / x86_64 ·
      需 glibc 2.39+ / GTK 4.14+ · 状态判定截至 {{ asOf }}
    </p>

    <div class="doc-note warn mb-4">
      <strong>状态列不是装饰。</strong>
      冒烟阶段排在创建 Release 之前，所以一条红的冒烟意味着那一版根本没发包：附件直链会 404，
      表里的文件名只是流水线将要上传的东西。判定过程与各次真跑的输出在
      <RouterLink to="/docs/testing/cnb-ci">CNB 发版流水线</RouterLink>；跑绿之前，可用的获取方式是
      <RouterLink to="/docs/root/README.zh-CN">从源码构建</RouterLink>。
    </div>

    <div class="d-flex flex-wrap align-items-center gap-2 mb-3">
      <label class="form-label mb-0 small text-secondary" for="buildFilter">按版本过滤（子串匹配）</label>
      <input
        id="buildFilter"
        v-model="query"
        class="form-control form-control-sm"
        style="max-width: 14rem"
        placeholder="154.0.34"
      >
      <span class="small text-secondary">{{ filtered.length }} / {{ builds.length }} 个版本</span>
      <a class="btn btn-sm btn-outline-secondary ms-auto" :href="release.page" target="_blank" rel="noopener">
        <i class="bi bi-journal-text me-1"></i>{{ release.version }} 的 Release 页
      </a>
    </div>

    <div v-for="f in formats" :key="f.key" class="mb-5">
      <h2 class="h5 fw-semibold mb-1">
        <i class="bi" :class="f.icon"></i> {{ f.title }}
        <small class="text-secondary">{{ f.arch }}</small>
      </h2>
      <p class="small text-secondary mb-2">{{ f.hint }}</p>
      <div class="table-responsive">
        <table class="table table-sm builds-table align-middle">
          <thead>
            <tr>
              <th scope="col">日期</th>
              <th scope="col">版本</th>
              <th scope="col" :title="packageKinds[0].desc">组件包</th>
              <th scope="col" :title="packageKinds[1].desc">开发包</th>
              <th scope="col" :title="buildTree.desc">构建树</th>
              <th scope="col">状态</th>
            </tr>
          </thead>
          <tbody>
            <tr v-for="row in visible" :key="row.tag">
              <td class="text-nowrap small">{{ row.date }} <span class="text-secondary">{{ row.time }}</span></td>
              <td class="small">
                <a class="fw-semibold" :href="pageOf(row.tag)" target="_blank" rel="noopener">{{ row.version }}</a>
              </td>
              <td class="font-monospace small file-cell">
                <a v-if="cell(row, f.component)?.url" :href="cell(row, f.component).url">
                  {{ cell(row, f.component).name }}
                </a>
                <span v-else-if="cell(row, f.component)" class="text-secondary">{{ cell(row, f.component).name }}</span>
                <span v-else class="text-secondary">—</span>
              </td>
              <td class="font-monospace small file-cell">
                <a v-if="cell(row, f.dev)?.url" :href="cell(row, f.dev).url">{{ cell(row, f.dev).name }}</a>
                <span v-else-if="cell(row, f.dev)" class="text-secondary">{{ cell(row, f.dev).name }}</span>
                <span v-else class="text-secondary">—</span>
              </td>
              <td class="font-monospace small file-cell">
                <a v-if="row.files && row.status === 'published'" :href="assetUrl(buildTree.file, row.tag)">
                  {{ buildTree.file }}
                </a>
                <span v-else-if="row.files" class="text-secondary">{{ buildTree.file }}</span>
                <span v-else class="text-secondary">—</span>
              </td>
              <td class="status-cell">
                <span class="badge status-badge" :class="buildStatuses[row.status].cls">
                  {{ buildStatuses[row.status].label }}
                </span>
                <div v-if="row.note" class="small text-secondary build-note">{{ row.note }}</div>
              </td>
            </tr>
          </tbody>
        </table>
      </div>

      <div v-if="visible.length < filtered.length" class="d-flex gap-2">
        <button class="btn btn-sm btn-outline-primary" type="button" @click="shown += 5">
          显示更多构建
        </button>
        <button class="btn btn-sm btn-link" type="button" @click="shown = filtered.length">
          全部 {{ filtered.length }} 个版本
        </button>
      </div>
    </div>

    <h2 class="h5 fw-semibold mb-2">两类包各装了什么</h2>
    <div class="row g-3 mb-4">
      <div v-for="p in packageKinds" :key="p.key" class="col-md-6">
        <div class="feature-card p-3 p-lg-4 h-100">
          <div class="d-flex align-items-center gap-2 mb-2">
            <span class="feature-icon"><i class="bi" :class="p.icon"></i></span>
            <div>
              <h3 class="h6 fw-semibold mb-0">{{ p.title }}</h3>
              <small class="text-secondary font-monospace">{{ p.names }}</small>
            </div>
          </div>
          <p class="text-secondary small mb-2">{{ p.desc }}</p>
          <p class="small text-secondary mb-0"><i class="bi bi-diagram-3 me-1"></i>{{ p.depends }}</p>
        </div>
      </div>
    </div>
    <div class="row g-3 mb-4">
      <div class="col-md-6">
        <div class="feature-card p-3 p-lg-4 h-100">
          <h3 class="h6 fw-semibold mb-1"><i class="bi bi-file-earmark-zip me-2"></i>{{ buildTree.file }}</h3>
          <p class="text-secondary small mb-2">{{ buildTree.desc }}</p>
          <p class="small text-secondary mb-0"><strong>{{ buildTree.note }}</strong></p>
        </div>
      </div>
      <div class="col-md-6">
        <div class="feature-card p-3 p-lg-4 h-100">
          <h3 class="h6 fw-semibold mb-1"><i class="bi bi-shield-check me-2"></i>{{ checksums.file }}</h3>
          <p class="text-secondary small mb-2">{{ checksums.desc }}</p>
          <p class="small text-secondary mb-0">
            下载后先校验再安装。本地装开发包时两个文件要一起给出：包管理器不会自动搜索同目录的
            其它本地包。
          </p>
        </div>
      </div>
    </div>

    <h2 class="h5 fw-semibold mb-2">安装与校验</h2>
    <CodeBlock label="bash · 校验" lang="bash" :code="verify" />
    <div class="row g-3">
      <div class="col-lg-6"><CodeBlock label="bash · deb" lang="bash" :code="debInstall" /></div>
      <div class="col-lg-6"><CodeBlock label="bash · rpm" lang="bash" :code="rpmInstall" /></div>
    </div>
    <CodeBlock label="bash · 编译宿主并运行 demo" lang="bash" :code="afterInstall" />

    <div class="doc-note warn mt-4">
      <strong>helper 默认开启 Chromium 沙箱。</strong>
      它需要可用的非特权用户命名空间。Ubuntu 24.04 默认限制 userns，helper 会以
      <code>No usable sandbox!</code> 响亮失败，不会静默降级。给
      <code>/usr/libexec/cefnest/&lt;arch&gt;/cefnest-helper</code> 单独加一条允许 userns 的
      AppArmor profile，或显式设置 <code>CHROMIUM_HELPER_NO_SANDBOX=1</code>——两条路的完整写法见
      <RouterLink to="/docs/internals/sandbox-policy">沙箱决定</RouterLink>。包里的
      <code>chrome-sandbox</code> 是普通 0755，没有 setuid 安装步骤。
    </div>

    <p class="small text-secondary mt-4">
      需要 arm64 或其它架构？本版只发布 x86_64 资产。CEF 的 linuxarm64 路径已接好，但安装包需要在
      目标机构建；<RouterLink to="/docs/root/README.zh-CN">从源码构建</RouterLink>同样能装出一棵自含树。
    </p>
  </div>
</template>

<style scoped>
.builds-table th { font-size: 0.82rem; white-space: nowrap; }
.builds-table td { font-size: 0.84rem; }
.builds-table a { text-decoration: none; }
/* 文件名逐字断行会把包名切成看不懂的碎片；让它占满一行，窄屏交给外层横向滚动。 */
.file-cell { white-space: nowrap; }
.status-cell { min-width: 13rem; }
.status-badge { font-size: 0.72rem; font-weight: 600; }
.build-note { max-width: 22rem; }
</style>
