<script setup>
import { computed, onBeforeUnmount, ref } from 'vue'
import hljs from 'highlight.js/lib/core'
import bash from 'highlight.js/lib/languages/bash'
import c from 'highlight.js/lib/languages/c'
import { i18n, messages } from '../i18n'

hljs.registerLanguage('bash', bash)
hljs.registerLanguage('c', c)

const props = defineProps({
  code: { type: String, required: true },
  label: { type: String, default: '' },
  lang: { type: String, default: '' },
})

const m = computed(() => messages[i18n.locale])

const highlighted = computed(() => {
  if (props.lang && hljs.getLanguage(props.lang)) {
    return hljs.highlight(props.code, { language: props.lang }).value
  }
  return hljs.highlightAuto(props.code).value
})

const copied = ref(false)
let timer = null

async function copy () {
  try {
    await navigator.clipboard.writeText(props.code)
    copied.value = true
    if (timer) clearTimeout(timer)
    timer = setTimeout(() => { copied.value = false }, 1600)
  } catch {
    // Clipboard access can be denied (or absent on file://); the code is
    // still selectable, so a failed copy is not worth an alert.
  }
}

onBeforeUnmount(() => {
  if (timer) clearTimeout(timer)
})
</script>

<template>
  <div class="code-card">
    <div class="code-head">
      <span class="code-title">{{ label }}</span>
      <div class="code-actions">
        <button
          class="code-copy"
          type="button"
          :aria-label="copied ? m.code.copiedAria : m.code.copyAria"
          @click="copy"
        >
          <i :class="['bi', copied ? 'bi-check2' : 'bi-clipboard']"></i>
          <span>{{ copied ? m.code.copied : m.code.copy }}</span>
        </button>
        <span class="code-close" aria-hidden="true"><i class="bi bi-x-lg"></i></span>
      </div>
    </div>
    <pre class="code-body" tabindex="0"><code class="hljs" v-html="highlighted"></code></pre>
  </div>
</template>
