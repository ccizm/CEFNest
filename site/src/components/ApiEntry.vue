<script setup>
// API 条目卡片：函数/信号/结构 通用展示
defineProps({
  kind: { type: String, default: 'method' }, // method | signal | getter | struct
  sig: { type: String, required: true },
  desc: { type: String, default: '' },
})
const kindLabel = { method: '函数', signal: '信号', getter: '查询', struct: '结构' }
</script>

<template>
  <div class="api-entry">
    <div class="api-sig">{{ sig }}</div>
    <div class="card-body">
      <span class="api-kind" :class="kind">{{ kindLabel[kind] || kind }}</span>
      <span v-if="desc" class="ms-2">{{ desc }}</span>
      <slot />
    </div>
  </div>
</template>
