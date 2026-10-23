<script setup>
import { computed } from 'vue'
import { repo, debAsset, rpmAsset, releasesPage } from '../data/content'
import { i18n, messages, toggleLocale } from '../i18n'
import { theme, toggleTheme } from '../theme'

const m = computed(() => messages[i18n.locale])
const logo = `${import.meta.env.BASE_URL}cefnest.svg`
</script>

<template>
  <nav class="navbar cn-navbar">
    <div class="container">
      <a class="navbar-brand d-flex align-items-center gap-2" href="#top">
        <img :src="logo" alt="" width="28" height="28" class="brand-mark" />
        <span class="brand-text">CEFNest</span>
      </a>
      <div class="d-flex align-items-center gap-2">
        <a class="nav-quiet d-none d-sm-inline me-2" href="#docs">{{ m.nav.docs }}</a>
        <button
          class="btn btn-sm btn-outline-dark"
          type="button"
          :aria-label="theme.mode === 'dark' ? m.nav.themeLight : m.nav.themeDark"
          @click="toggleTheme"
        >
          <i :class="['bi', theme.mode === 'dark' ? 'bi-sun' : 'bi-moon-stars']"></i>
        </button>
        <button class="btn btn-sm btn-outline-dark" type="button" @click="toggleLocale">
          <i class="bi bi-translate me-1"></i>{{ m.nav.switch }}
        </button>
        <div class="dropdown">
          <button
            class="btn btn-sm btn-outline-dark dropdown-toggle"
            type="button"
            data-bs-toggle="dropdown"
            aria-expanded="false"
          >
            <i class="bi bi-download me-1"></i>{{ m.nav.download }}
          </button>
          <ul class="dropdown-menu dropdown-menu-end">
            <li>
              <a class="dropdown-item" :href="debAsset">
                <i class="bi bi-ubuntu me-2"></i>{{ m.nav.downloadDeb }}
              </a>
            </li>
            <li>
              <a class="dropdown-item" :href="rpmAsset">
                <i class="bi bi-boxes me-2"></i>{{ m.nav.downloadRpm }}
              </a>
            </li>
            <li><hr class="dropdown-divider" /></li>
            <li>
              <a class="dropdown-item" :href="releasesPage" target="_blank" rel="noopener">
                <i class="bi bi-list-ul me-2"></i>{{ m.nav.allReleases }}
              </a>
            </li>
          </ul>
        </div>
        <a
          class="btn btn-sm btn-outline-dark btn-repo"
          :href="repo"
          target="_blank"
          rel="noopener"
          :aria-label="m.nav.repo"
        >
          <span class="repo-label">{{ m.nav.repo }}</span>
          <i class="bi bi-box-arrow-up-right ms-1"></i>
        </a>
      </div>
    </div>
  </nav>
</template>
