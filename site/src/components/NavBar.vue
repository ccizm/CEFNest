<script setup>
import { ref, watch } from 'vue'
import { useRoute } from 'vue-router'

const route = useRoute()
const expanded = ref(false)
watch(() => route.fullPath, () => { expanded.value = false })
</script>

<template>
  <nav class="navbar navbar-expand-lg navbar-dark bg-dark sticky-top">
    <div class="container">
      <router-link class="navbar-brand fw-bold" to="/">CEFNest</router-link>
      <button
        class="navbar-toggler"
        type="button"
        :class="{ collapsed: !expanded }"
        data-bs-toggle="collapse"
        data-bs-target="#mainNav"
        @click="expanded = !expanded"
      >
        <span class="navbar-toggler-icon"></span>
      </button>
      <div id="mainNav" class="collapse navbar-collapse" :class="{ show: expanded }">
        <ul class="navbar-nav ms-auto align-items-lg-center gap-lg-2">
          <li class="nav-item">
            <router-link class="nav-link" to="/" :class="{ active: route.name === 'home' }">首页</router-link>
          </li>
          <li class="nav-item">
            <router-link class="nav-link" to="/builds" :class="{ active: route.name === 'builds' }">构建产物</router-link>
          </li>
          <li class="nav-item">
            <router-link class="nav-link" to="/docs/README" :class="{ active: route.name === 'docs' }">文档</router-link>
          </li>
          <li class="nav-item">
            <router-link class="nav-link" to="/api/quickstart" :class="{ active: route.name === 'api' }">调用文档</router-link>
          </li>
          <li class="nav-item">
            <a class="btn btn-sm btn-outline-light" href="https://cnb.cool/yanzhiming/CEFNest" target="_blank" rel="noopener">
              <i class="bi bi-code-square"></i> 仓库
            </a>
          </li>
        </ul>
      </div>
    </div>
  </nav>
</template>
