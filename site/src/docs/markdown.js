import MarkdownIt from 'markdown-it'
import hljs from 'highlight.js/lib/core'
import bash from 'highlight.js/lib/languages/bash'
import c from 'highlight.js/lib/languages/c'
import cpp from 'highlight.js/lib/languages/cpp'
import json from 'highlight.js/lib/languages/json'
import plaintext from 'highlight.js/lib/languages/plaintext'
import { bySlug, bySrc, slugOf } from './manifest.js'

hljs.registerLanguage('bash', bash)
hljs.registerLanguage('c', c)
hljs.registerLanguage('cpp', cpp)
hljs.registerLanguage('json', json)
hljs.registerLanguage('plaintext', plaintext)

const aliases = { sh: 'bash', text: 'plaintext' }

// 站点文档的正文就是仓库里的 markdown 文件，这里只负责把它变成 HTML。
// html:false 是这条管线唯一的安全边界：markdown 里写的原始 HTML 一律当文本处理，
// 所以下游的 v-html 只可能拿到我们自己那套排版。
const md = new MarkdownIt({
  html: false,
  linkify: true,
  highlight (code, lang) {
    const name = aliases[lang] || lang
    if (hljs.getLanguage(name)) {
      try {
        return hljs.highlight(code, { language: name, ignoreIllegals: true }).value
      } catch {
        /* 高亮失败只影响配色，正文照旧 */
      }
    }
    return md.utils.escapeHtml(code)
  },
})

function resolvePath (fromDir, rel) {
  const parts = fromDir ? fromDir.split('/') : []
  for (const seg of rel.split('/')) {
    if (seg === '' || seg === '.') continue
    if (seg === '..') parts.pop()
    else parts.push(seg)
  }
  return parts.join('/')
}

// 文档之间用仓库路径互相引用（README.md 开头那条「简体中文」就是）。站点上的地址是
// /docs/<slug>，所以把指向 .md 的相对链接换过来；换不动的原样留着，不做猜测。
// 地址里不带 `#小节`：路由本身就是 hash 模式，第二个 # 会被 vue-router 吃掉。
const renderLinkOpen = md.renderer.rules.link_open ||
  ((tokens, idx, options, env, self) => self.renderToken(tokens, idx, options))

md.renderer.rules.link_open = (tokens, idx, options, env, self) => {
  const token = tokens[idx]
  const href = token.attrGet('href') || ''
  if (/^https?:/i.test(href)) {
    token.attrSet('target', '_blank')
    token.attrSet('rel', 'noopener')
    return renderLinkOpen(tokens, idx, options, env, self)
  }
  const file = href.split('#')[0]
  if (!file.endsWith('.md')) return renderLinkOpen(tokens, idx, options, env, self)
  const doc = bySlug.get(slugOf(resolvePath(env.srcDir || '', file)))
  if (doc) token.attrSet('href', `#/docs/${doc.slug}`)
  return renderLinkOpen(tokens, idx, options, env, self)
}

// 文档互相引用时写的是行内代码路径（`docs/guides/packaging.md`），不是 markdown
// 链接——仓库里读起来正好，站点上却点不动。这里把那条形同「一个已知文档的完整
// 路径」的行内代码变成链接；对不上的一律照旧排成代码，不做猜测。
const renderCodeInline = md.renderer.rules.code_inline ||
  ((tokens, idx, options, env, self) => self.renderToken(tokens, idx, options))

md.renderer.rules.code_inline = (tokens, idx, options, env, self) => {
  const text = tokens[idx].content
  const doc = bySrc.get(text) || bySrc.get(`docs/${text}`)
  if (!doc) return renderCodeInline(tokens, idx, options, env, self)
  const code = md.utils.escapeHtml(text)
  const href = md.utils.escapeHtml(`#/docs/${doc.slug}`)
  return `<a href="${href}"><code>${code}</code></a>`
}

// glob 的键带着这层目录的 ../，去掉之后键就是仓库相对路径。
const globbed = {
  ...import.meta.glob('../../../docs/**/*.md', { query: '?raw', import: 'default' }),
  ...import.meta.glob('../../../README*.md', { query: '?raw', import: 'default' }),
}

export const markdownFiles = Object.fromEntries(
  Object.entries(globbed).map(([key, load]) => [key.replace(/^(\.\.\/)+/, ''), load]),
)

export function hasDoc (src) {
  return typeof markdownFiles[src] === 'function'
}

export async function renderDoc (src) {
  const load = markdownFiles[src]
  if (!load) throw new Error(`站点没有登记这个 markdown 文件：${src}`)
  const raw = await load()
  const srcDir = src.includes('/') ? src.slice(0, src.lastIndexOf('/')) : ''
  return md.render(raw, { srcDir })
}
