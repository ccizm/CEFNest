<script setup>
import SiteNavbar from './components/SiteNavbar.vue'
import CodeBlock from './components/CodeBlock.vue'
import { embed, links, quickstart, repo } from './data/content'
</script>

<template>
  <SiteNavbar />

  <header id="top" class="hero">
    <div class="container narrow">
      <p class="eyebrow">GTK 4.14+ · 原生 Wayland · CEF 154.0.34</p>
      <h1>CEFNest</h1>
      <p class="tagline">在 GTK 4 里嵌入真正的 Chromium</p>
      <p class="lead">
        一个 GTK 4 控件：页面由真正的 Chromium 渲染，经同一进程内的 Wayland
        合成器直接合成进控件。没有 XWayland，也没有离屏渲染。
      </p>
      <div class="actions">
        <a class="btn btn-dark px-4" href="#start">快速开始</a>
        <a class="btn btn-outline-secondary px-4" :href="repo" target="_blank" rel="noopener">
          浏览源码
        </a>
      </div>
      <ul class="keypoints">
        <li><i class="bi bi-check2"></i>真 Chromium</li>
        <li><i class="bi bi-check2"></i>原生 Wayland</li>
        <li><i class="bi bi-check2"></i>无离屏渲染</li>
        <li><i class="bi bi-check2"></i>一个 helper 多标签</li>
      </ul>
    </div>
  </header>

  <main>
    <section id="start" class="section">
      <div class="container narrow">
        <h2>快速开始</h2>
        <p class="section-note">第一次会下载约 330 MB，解包后约 1.5 GB。</p>
        <CodeBlock label="build & run" :code="quickstart" />

        <h3 class="subhead">嵌入自己的应用</h3>
        <CodeBlock label="embed" :code="embed" />
        <p class="hint">
          顺序很重要：view 必须先进入窗口，再启动 session。关闭窗口时调用
          <code>chromium_session_stop()</code>。
        </p>
      </div>
    </section>

    <section id="docs" class="section">
      <div class="container narrow">
        <h2>文档</h2>
        <p class="section-note">完整内容在仓库里。</p>
        <ul class="doc-links">
          <li v-for="link in links" :key="link.href">
            <a :href="link.href" target="_blank" rel="noopener">
              <span>{{ link.label }}</span>
              <small>{{ link.note }}</small>
            </a>
          </li>
        </ul>
      </div>
    </section>
  </main>

  <footer class="site-footer">
    <div class="container narrow d-flex flex-wrap justify-content-between gap-2">
      <span>CEFNest · LGPL-2.1</span>
      <span>
        <a :href="repo" target="_blank" rel="noopener">源码</a>
        <span class="mx-2">·</span>
        Vite + Vue + Bootstrap
      </span>
    </div>
  </footer>
</template>
