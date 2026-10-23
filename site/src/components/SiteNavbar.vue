<script setup>
import { repo } from '../data/content'

const logo = `${import.meta.env.BASE_URL}cefnest.svg`
</script>

<template>
  <nav class="navbar cn-navbar">
    <div class="container">
      <a class="navbar-brand d-flex align-items-center gap-2" href="#top">
        <img :src="logo" alt="" width="28" height="28" class="brand-mark" />
        <span class="brand-text">CEFNest</span>
      </a>
      <div class="d-flex align-items-center gap-3">
        <a class="nav-quiet d-none d-sm-inline" href="#docs">文档</a>
        <a class="btn btn-sm btn-outline-dark" :href="repo" target="_blank" rel="noopener">
          仓库 <i class="bi bi-box-arrow-up-right ms-1"></i>
        </a>
      </div>
    </div>
  </nav>
</template>
