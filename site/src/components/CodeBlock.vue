<script setup>
import { ref, onMounted } from 'vue'
import hljs from 'highlight.js/lib/core'
import c from 'highlight.js/lib/languages/c'
import bash from 'highlight.js/lib/languages/bash'
import 'highlight.js/styles/github-dark.css'

hljs.registerLanguage('c', c)
hljs.registerLanguage('bash', bash)

const props = defineProps({
  code: { type: String, required: true },
  label: { type: String, default: '' },
  lang: { type: String, default: '' },
})
const root = ref(null)
const copied = ref(false)

onMounted(() => {
  const el = root.value?.querySelector('code')
  if (el && props.lang) {
    el.classList.add(`language-${props.lang}`)
    hljs.highlightElement(el)
  }
})

async function copy() {
  try {
    await navigator.clipboard.writeText(props.code)
    copied.value = true
    setTimeout(() => { copied.value = false }, 1500)
  } catch {
    /* 剪贴板不可用时静默 */
  }
}
</script>

<template>
  <div ref="root" class="code-block">
    <div class="code-block-bar">
      <span class="code-block-label">{{ label }}</span>
      <button class="code-copy-btn" type="button" @click="copy">
        <i class="bi" :class="copied ? 'bi-check2' : 'bi-clipboard'"></i>
        {{ copied ? '已复制' : '复制' }}
      </button>
    </div>
    <pre><code>{{ code }}</code></pre>
  </div>
</template>
