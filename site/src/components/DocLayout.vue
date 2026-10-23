<script setup>
defineProps({
  // [{ title: '分组名（可空）', items: [{ to, label }] }]
  sections: { type: Array, required: true },
  heading: { type: String, required: true },
})
</script>

<template>
  <div class="doc-page">
    <div class="container">
      <div class="row g-4">
        <div class="col-lg-3">
          <nav class="doc-sidebar">
            <div class="sidebar-title mb-2">{{ heading }}</div>
            <template v-for="s in sections" :key="s.title || s.items[0].to">
              <div v-if="s.title" class="sidebar-heading">{{ s.title }}</div>
              <ul class="nav flex-column mb-2">
                <li v-for="it in s.items" :key="it.to" class="nav-item">
                  <RouterLink
                    class="nav-link"
                    :to="it.to"
                    active-class="active"
                  >{{ it.label }}</RouterLink>
                </li>
              </ul>
            </template>
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
