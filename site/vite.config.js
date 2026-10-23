import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'

// 站点的文档正文不复制进 site/：markdown 文件只有一份，就在仓库的 docs/ 与
// 根 README 里。构建期的 import.meta.glob 读得到它们，dev server 则要先允许
// 越过 site/ 这一层，否则 fs.allow 会把这两个目录挡在门外。
export default defineConfig({
  base: './',
  plugins: [vue()],
  server: {
    fs: { allow: ['..'] },
  },
})
