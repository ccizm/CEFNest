<script setup>
import { computed, nextTick, ref, watch } from 'vue'
import { useRoute } from 'vue-router'
import DocLayout from '../components/DocLayout.vue'
import { groups, bySlug, docList } from '../docs/manifest.js'
import { renderDoc, hasDoc } from '../docs/markdown.js'

const route = useRoute()

// /docs/guides/minimal-host → 'guides/minimal-host'
const slug = computed(() => [].concat(route.params.slug || []).join('/'))
const doc = computed(() => bySlug.get(slug.value))

const sections = groups.map((g) => ({
  title: g.title,
  items: g.items.map((i) => ({ to: `/docs/${i.slug}`, label: i.label })),
}))

const article = ref(null)
const html = ref('')
const toc = ref([])
const problem = ref('')

const pos = computed(() => docList.findIndex((d) => d.slug === slug.value))
const prev = computed(() => (pos.value > 0 ? docList[pos.value - 1] : null))
const next = computed(() => (pos.value >= 0 && pos.value < docList.length - 1 ? docList[pos.value + 1] : null))

// 侧边栏切换时旧的正文不能留在屏幕上——它读起来像新那篇的内容。
watch(doc, async (d) => {
  html.value = ''
  toc.value = []
  problem.value = ''
  if (!d) {
    problem.value = `站点目录里没有「${slug.value || '（空）'}」这一篇。`
    return
  }
  if (!hasDoc(d.src)) {
    problem.value = `目录登记了 ${d.src}，但构建时没有这个文件——它多半被改名或删掉了。`
    return
  }
  try {
    html.value = await renderDoc(d.src)
    await nextTick()
    toc.value = collectToc()
  } catch (e) {
    problem.value = e.message
  }
}, { immediate: true })

// 站点用的是 hash 路由，第二个 # 会被 vue-router 吃掉，所以小节链接不做锚点，
// 记下 id 再用脚本滚。
function collectToc () {
  const root = article.value
  if (!root) return []
  const nodes = Array.from(root.querySelectorAll('.markdown h2, .markdown h3'))
  nodes.forEach((el, i) => { el.id = `sec-${i + 1}` })
  return nodes.map((el, i) => ({ id: `sec-${i + 1}`, label: el.textContent, level: el.tagName }))
}

function jump (id) {
  const el = article.value?.querySelector(`#${CSS.escape(id)}`)
  if (el) el.scrollIntoView({ behavior: 'smooth', block: 'start' })
}
</script>

<template>
  <DocLayout :sections="sections" heading="文档">
    <div v-if="problem" class="doc-note warn">
      <strong>这一页没有正文。</strong>
      {{ problem }}
      <div class="mt-2">
        <RouterLink :to="`/docs/${docList[0].slug}`">从目录第一篇开始</RouterLink>
        · <RouterLink to="/">文档索引</RouterLink>
      </div>
    </div>

    <template v-else>
      <div class="doc-src">
        <span class="text-secondary small">正文来自仓库的 markdown 文件</span>
        <code class="small">{{ doc.src }}</code>
      </div>

      <div v-if="toc.length > 1" class="doc-toc">
        <div class="sidebar-heading mb-1">本页小节</div>
        <button
          v-for="t in toc"
          :key="t.id"
          type="button"
          class="doc-toc-item"
          :class="{ deep: t.level === 'H3' }"
          @click="jump(t.id)"
        >{{ t.label }}</button>
      </div>

      <!-- 内容来自仓库里的 markdown；markdown-it 关掉了内嵌 HTML，这里只会拿到我们自己排出来的标签。 -->
      <div ref="article" class="markdown" v-html="html"></div>

      <nav class="doc-pager d-flex flex-wrap justify-content-between gap-2 mt-4">
        <RouterLink v-if="prev" class="btn btn-sm btn-outline-primary" :to="`/docs/${prev.slug}`">
          <i class="bi bi-arrow-left me-1"></i>{{ prev.label }}
        </RouterLink>
        <span v-else></span>
        <RouterLink v-if="next" class="btn btn-sm btn-outline-primary" :to="`/docs/${next.slug}`">
          {{ next.label }}<i class="bi bi-arrow-right ms-1"></i>
        </RouterLink>
      </nav>
    </template>
  </DocLayout>
</template>
