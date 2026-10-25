<script setup>
import { computed } from 'vue'
import SiteNavbar from './components/SiteNavbar.vue'
import CodeBlock from './components/CodeBlock.vue'
import { embed, quickstart, repoLinks } from './data/content'
import { i18n, messages } from './i18n'

const m = computed(() => messages[i18n.locale])
const quick = computed(() => quickstart[i18n.locale])
const lk = computed(() => repoLinks(i18n.locale))
const logo = `${import.meta.env.BASE_URL}cefnest.svg`
const ga = `${import.meta.env.BASE_URL}ga-beian.webp`
</script>

<template>
  <SiteNavbar />

  <header id="top" class="hero">
    <div class="container narrow">
      <img class="hero-logo" :src="logo" alt="CEFNest logo" width="104" height="104" />
      <p class="eyebrow">{{ m.hero.eyebrow }}</p>
      <h1><span class="cef-c">C</span><span class="cef-e">E</span><span class="cef-f">F</span>Nest</h1>
      <p class="tagline">{{ m.hero.tagline }}</p>
      <p class="lead">{{ m.hero.lead }}</p>
      <div class="actions">
        <a class="btn btn-dark px-4" href="#start">{{ m.hero.start }}</a>
        <a class="btn btn-outline-secondary px-4" :href="lk.repo" target="_blank" rel="noopener">
          {{ m.hero.source }}
        </a>
      </div>
      <ul class="keypoints">
        <li v-for="p in m.hero.points" :key="p"><i class="bi bi-check2"></i>{{ p }}</li>
      </ul>
    </div>
  </header>

  <main>
    <section id="start" class="section">
      <div class="container narrow">
        <h2>{{ m.start.title }}</h2>
        <p class="section-note">{{ m.start.note }}</p>
        <CodeBlock :label="m.start.runLabel" :code="quick" lang="bash" />

        <h3 class="subhead">{{ m.start.embedTitle }}</h3>
        <CodeBlock :label="m.start.embedLabel" :code="embed" lang="c" />
        <p class="hint">
          {{ m.start.hintLead }}
          <code>chromium_session_stop()</code>{{ m.start.hintTail }}
        </p>
      </div>
    </section>

    <section id="docs" class="section">
      <div class="container narrow">
        <h2>{{ m.docs.title }}</h2>
        <p class="section-note">{{ m.docs.note }}</p>
        <ul class="doc-links">
          <li v-for="link in m.docs.links" :key="link.href">
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
    <div class="container narrow footer-cols">
      <span>CEFNest · LGPL-2.1</span>
      <a href="https://beian.miit.gov.cn/" target="_blank" rel="noopener noreferrer">
        皖ICP备2023011518号-1
      </a>
      <a
        href="https://www.beian.gov.cn/portal/registerSystemInfo?recordcode=32050802011692"
        target="_blank"
        rel="noopener noreferrer"
      >
        <img :src="ga" alt="公安备案" width="14" height="14" />
        <span>苏公网安备32050802011692号</span>
      </a>
    </div>
  </footer>
</template>
