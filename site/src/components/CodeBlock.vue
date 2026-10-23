<script setup>
import { onBeforeUnmount, ref } from 'vue'

const props = defineProps({
  code: { type: String, required: true },
  label: { type: String, default: '代码' },
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
      <span class="code-dots" aria-hidden="true"><i></i><i></i><i></i></span>
      <span class="code-label">{{ label }}</span>
      <button
        class="code-copy"
        type="button"
        :aria-label="copied ? '已复制' : '复制代码'"
        @click="copy"
      >
        <i :class="['bi', copied ? 'bi-check2' : 'bi-clipboard']"></i>
        <span>{{ copied ? '已复制' : '复制' }}</span>
      </button>
    </div>
    <pre class="code-body" tabindex="0"><code>{{ code }}</code></pre>
  </div>
</template>
