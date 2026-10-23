<script setup>
import { release } from '../data/release.js'
import { builds, buildStatuses, asOf } from '../data/builds.js'
import { groups } from '../docs/manifest.js'

const pieces = [
  ['NestedCompositor', 'src/nested*.c', '嵌套 Wayland 合成器：globals、surface 栈、damage、输入、剪贴板，以及跨窗口边缘双向的拖拽。'],
  ['ChromiumView', 'src/chromium-view.c', 'GTK 4 部件：把宿主窗口的真实指针、键盘与输入法交给合成器，并把合成结果画成 GdkTexture。'],
  ['ChromiumSession', 'src/chromium-session.c', '控制面：启动与停止 helper，跟踪页面状态，执行 JavaScript，代理下载、查找、右键菜单与权限请求。'],
]

const latest = builds[0]
</script>

<template>
  <div class="container py-4 py-lg-5 home-page">
    <header class="home-head">
      <h1 class="h2 fw-bold mb-2">CEFNest 文档</h1>
      <p class="mb-2" style="max-width: 60rem">
        CEFNest 是一个 GTK 4 部件：页面由<strong>真正的 Chromium</strong>渲染，经同一进程内的嵌套
        Wayland 合成器合成进部件。没有 XWayland，也没有离屏渲染——这两条约束排除了所有不自带
        合成器的「GTK 嵌浏览器」方案。
      </p>
      <p class="small text-secondary mb-3">
        当前版本 {{ release.version }} · CEF {{ release.cef }} · Chromium {{ release.chromium }} ·
        GTK 4.14+ · 原生 Wayland · API 尚不稳定
      </p>
      <div class="d-flex gap-2 flex-wrap">
        <RouterLink to="/builds" class="btn btn-primary">
          <i class="bi bi-box-seam me-1"></i>构建产物
        </RouterLink>
        <RouterLink to="/api/quickstart" class="btn btn-outline-primary">
          <i class="bi bi-code-slash me-1"></i>调用文档（API）
        </RouterLink>
        <a class="btn btn-outline-secondary" :href="release.repository" target="_blank" rel="noopener">
          <i class="bi bi-code-square me-1"></i>仓库
        </a>
      </div>
    </header>

    <section class="mt-4 mt-lg-5">
      <h2 class="h5 fw-semibold mb-2">最新一次构建</h2>
      <p class="small mb-2">
        <RouterLink class="fw-semibold me-2" to="/builds">{{ latest.version }}</RouterLink>
        <span class="badge status-badge" :class="buildStatuses[latest.status].cls">
          {{ buildStatuses[latest.status].label }}
        </span>
        <span class="text-secondary ms-2">tag {{ latest.tag }} · {{ latest.date }} {{ latest.time }} · 状态截至 {{ asOf }}</span>
      </p>
      <p class="small text-secondary mb-0">
        {{ latest.note }} 每条状态都对应一次可复看的判定，写在
        <RouterLink to="/docs/testing/cnb-ci">CNB 发版流水线</RouterLink>里。
      </p>
    </section>

    <section class="mt-4 mt-lg-5">
      <h2 class="h5 fw-semibold mb-1">目录</h2>
      <p class="small text-secondary mb-3">
        这里没有第二份文档：每一行就是仓库里的一个 markdown 文件，站点直接把它们渲染出来。
        想读某类的取舍顺序，看<RouterLink to="/docs/README">文档索引 · 先读哪篇</RouterLink>。
      </p>
      <div v-for="g in groups" :key="g.key" class="doc-index-group">
        <h3 class="doc-index-title">{{ g.title }}</h3>
        <ul class="doc-index-list">
          <li v-for="it in g.items" :key="it.slug">
            <RouterLink :to="`/docs/${it.slug}`">{{ it.label }}</RouterLink>
            <code>{{ it.src }}</code>
          </li>
        </ul>
      </div>
    </section>

    <section class="mt-4 mt-lg-5">
      <h2 class="h5 fw-semibold mb-2">三块积木</h2>
      <div class="d-flex flex-column gap-2">
        <div v-for="[name, file, desc] in pieces" :key="name" class="feature-card p-3">
          <div class="d-flex justify-content-between align-items-baseline flex-wrap gap-2">
            <code class="fw-bold text-body">{{ name }}</code>
            <small class="text-secondary">{{ file }}</small>
          </div>
          <p class="text-secondary small mb-0 mt-1">{{ desc }}</p>
        </div>
      </div>
      <p class="small text-secondary mt-2 mb-0">
        合成器通告哪些协议、版本上限判到什么，写在
        <RouterLink to="/docs/internals/protocol-ceilings">协议天花板</RouterLink>与
        <RouterLink to="/docs/internals/nested-split">nested 拆分</RouterLink>。
      </p>
    </section>
  </div>
</template>

<style scoped>
.home-head { border-bottom: 1px solid var(--bs-border-color); padding-bottom: 1.5rem; }
.status-badge { font-size: 0.72rem; font-weight: 600; }
.doc-index-title {
  font-size: 0.78rem;
  font-weight: 700;
  letter-spacing: 0.05em;
  text-transform: uppercase;
  color: var(--bs-secondary-color);
  margin: 1.5rem 0 0.4rem;
}
.doc-index-list { list-style: none; padding: 0; margin: 0; }
.doc-index-list li { padding: 0.15rem 0; display: flex; flex-wrap: wrap; gap: 0.5rem; align-items: baseline; }
.doc-index-list a { text-decoration: none; }
.doc-index-list a:hover { text-decoration: underline; }
.doc-index-list code { font-size: 0.74rem; color: var(--bs-secondary-color); }
@media (min-width: 768px) {
  .doc-index-list { columns: 2; column-gap: 2.5rem; }
  .doc-index-list li { break-inside: avoid; }
}
</style>
