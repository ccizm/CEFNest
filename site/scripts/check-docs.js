// 站点目录与仓库 markdown 必须一一对应。
//
// 少了这一道，一篇新文档会安静地不进站点，而一个改名的旧文档会在浏览器里变成
// 一页空白——两种都要等到有人点开才发现。构建前先把两边数一遍。
import { existsSync, readdirSync, statSync } from 'node:fs'
import { dirname, join, relative, sep } from 'node:path'
import { fileURLToPath } from 'node:url'

import { groups, slugOf } from '../src/docs/manifest.js'

const here = dirname(fileURLToPath(import.meta.url))
const repoRoot = join(here, '..', '..')

const listed = groups.flatMap((g) => g.items.map((i) => i.src))

function markdownUnder (dir) {
  const out = []
  for (const entry of readdirSync(dir)) {
    const p = join(dir, entry)
    if (statSync(p).isDirectory()) out.push(...markdownUnder(p))
    else if (entry.endsWith('.md')) out.push(relative(repoRoot, p).split(sep).join('/'))
  }
  return out
}

const onDisk = [
  ...markdownUnder(join(repoRoot, 'docs')),
  ...['README.md', 'README.zh-CN.md'].filter((f) => existsSync(join(repoRoot, f))),
]

const missing = onDisk.filter((src) => !listed.includes(src))
const stale = listed.filter((src) => !onDisk.includes(src))

const slugs = new Map()
for (const src of listed) {
  const slug = slugOf(src)
  if (slugs.has(slug)) stale.push(`${src}（slug ${slug} 与 ${slugs.get(slug)} 相撞）`)
  else slugs.set(slug, src)
}

if (missing.length || stale.length) {
  for (const src of missing) console.error(`check-docs: 仓库里有 ${src}，站点目录没有登记`)
  for (const src of stale) console.error(`check-docs: 站点目录登记了 ${src}，仓库里找不到对应文件`)
  process.exit(1)
}

console.log(`check-docs: ${onDisk.length} 篇 markdown，全部已进站点目录`)
