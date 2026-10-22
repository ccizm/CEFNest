<script setup>
defineProps({
  items: { type: Array, required: true }, // [{ key, label }]
  heading: { type: String, required: true },
})
</script>

<template>
  <div class="doc-page">
    <div class="container">
      <div class="row g-4">
        <div class="col-lg-3">
          <nav class="doc-sidebar">
            <div class="sidebar-heading mb-2">{{ heading }}</div>
            <ul class="nav flex-column gap-0">
              <li v-for="it in items" :key="it.key" class="nav-item">
                <router-link
                  class="nav-link"
                  :to="`/${heading === '开发者文档' ? 'dev' : 'api'}/${it.key}`"
                >
                  {{ it.label }}
                </router-link>
              </li>
            </ul>
          </nav>
        </div>
        <div class="col-lg-9">
          <article class="doc-content">
            <slot />
          </article>
        </div>
      </div>
    </div>
  </div>
</template>
