import { reactive, watchEffect } from 'vue'
import { repoLinks } from './data/content'

const STORAGE_KEY = 'cefnest-locale'

export const messages = {
  'zh-CN': {
    meta: {
      title: 'CEFNest — 在 GTK 4 里嵌入真正的 Chromium',
      description: 'CEFNest：用真正的 Chromium 渲染网页的 GTK 4 控件。原生 Wayland，无 XWayland，无离屏渲染。',
    },
    nav: {
      docs: '文档',
      repo: '仓库',
      switch: 'English',
      themeLight: '切换到浅色模式',
      themeDark: '切换到深色模式',
      download: '下载',
      downloadDeb: 'deb 包 · amd64',
      downloadRpm: 'rpm 包 · x86_64',
      allReleases: '全部发布',
    },
    hero: {
      eyebrow: 'GTK 4.14+ · 原生 Wayland · CEF 156.0.14',
      tagline: '在 GTK 4 里嵌入真正的 Chromium',
      lead: '一个 GTK 4 控件：页面由真正的 Chromium 渲染，经同一进程内的 Wayland 合成器直接合成进控件。没有 XWayland，也没有离屏渲染。',
      start: '快速开始',
      source: '浏览源码',
      points: ['Chromium', '原生 Wayland', '无离屏渲染', 'Tab 焦点链与 GTK 打通'],
    },
    start: {
      title: '快速开始',
      note: '第一次会下载约 316 MB，解包后约 1.5 GB。',
      runLabel: '构建并运行',
      embedTitle: '嵌入自己的应用',
      embedLabel: 'embed',
      hintLead: '顺序很重要：view 必须先进入窗口，再启动 session。关闭窗口时调用',
      hintTail: '。',
    },
    docs: {
      title: '文档',
      note: '完整内容在仓库里。',
      links: [
        { label: 'README', note: '构建、安装、API 与已知限制', href: `${repoLinks('zh-CN').blob}/README.zh-CN.md` },
        { label: '数据目录', note: '默认不落盘；持久化与清理', href: `${repoLinks('zh-CN').blob}/docs/data-directory.zh-CN.md` },
        { label: '控制通道', note: 'NDJSON 命令与事件', href: `${repoLinks('zh-CN').blob}/docs/control-channel.zh-CN.md` },
        { label: '示例', note: '十五个最小宿主', href: `${repoLinks('zh-CN').blob}/examples/README.zh-CN.md` },
        { label: 'API 头文件', note: '每个调用的完整契约', href: `${repoLinks('zh-CN').blob}/src/chromium-session.h` },
        { label: '常见问题', note: '症状 → 处置', href: `${repoLinks('zh-CN').blob}/docs/troubleshooting.zh-CN.md` },
      ],
    },
    code: { copy: '复制', copied: '已复制', copyAria: '复制代码', copiedAria: '已复制' },
  },
  en: {
    meta: {
      title: 'CEFNest — real Chromium, embedded in GTK 4',
      description: 'CEFNest: a GTK 4 widget whose pages are rendered by real Chromium. Native Wayland, no XWayland, no offscreen rendering.',
    },
    nav: {
      docs: 'Docs',
      repo: 'Repository',
      switch: '中文',
      themeLight: 'Switch to light mode',
      themeDark: 'Switch to dark mode',
      download: 'Download',
      downloadDeb: 'deb · amd64',
      downloadRpm: 'rpm · x86_64',
      allReleases: 'All releases',
    },
    hero: {
      eyebrow: 'GTK 4.14+ · Native Wayland · CEF 156.0.14',
      tagline: 'Embed real Chromium in GTK 4',
      lead: 'A GTK 4 widget whose pages are rendered by real Chromium, composited straight into the widget by an in-process Wayland compositor. No XWayland, no offscreen rendering.',
      start: 'Quick start',
      source: 'Browse source',
      points: ['Chromium', 'Native Wayland', 'No offscreen rendering', 'Tab focus crosses GTK and the page'],
    },
    start: {
      title: 'Quick start',
      note: 'The first run downloads ~316 MB, about 1.5 GB unpacked.',
      runLabel: 'build & run',
      embedTitle: 'Embed it in your app',
      embedLabel: 'embed',
      hintLead: 'Order matters: add the view to the window before starting the session. When the window closes, call',
      hintTail: '.',
    },
    docs: {
      title: 'Documentation',
      note: 'The full text lives in the repository.',
      links: [
        { label: 'README', note: 'Build, install, API and known limitations', href: `${repoLinks('en').blob}/README.md` },
        { label: 'Data directory', note: 'Nothing on disk by default; persistence and cleanup', href: `${repoLinks('en').blob}/docs/data-directory.md` },
        { label: 'Control channel', note: 'NDJSON commands and events', href: `${repoLinks('en').blob}/docs/control-channel.md` },
        { label: 'Examples', note: 'Fifteen minimal hosts', href: `${repoLinks('en').blob}/examples/README.md` },
        { label: 'API headers', note: 'The full contract of every call', href: `${repoLinks('en').blob}/src/chromium-session.h` },
        { label: 'Troubleshooting', note: 'Symptom → fix', href: `${repoLinks('en').blob}/docs/troubleshooting.md` },
      ],
    },
    code: { copy: 'Copy', copied: 'Copied', copyAria: 'Copy code', copiedAria: 'Copied' },
  },
}

function detectLocale () {
  try {
    const saved = localStorage.getItem(STORAGE_KEY)
    if (saved === 'zh-CN' || saved === 'en') return saved
  } catch {
    // localStorage can be unavailable (privacy mode); fall through to the browser language.
  }
  const langs = navigator.languages?.length ? navigator.languages : [navigator.language]
  return langs.some((l) => (l || '').toLowerCase().startsWith('zh')) ? 'zh-CN' : 'en'
}

export const i18n = reactive({ locale: detectLocale() })

export function setLocale (locale) {
  if (!messages[locale]) return
  i18n.locale = locale
  try {
    localStorage.setItem(STORAGE_KEY, locale)
  } catch {}
}

export function toggleLocale () {
  setLocale(i18n.locale === 'zh-CN' ? 'en' : 'zh-CN')
}

watchEffect(() => {
  const m = messages[i18n.locale]
  document.title = m.meta.title
  document.documentElement.lang = i18n.locale
  document
    .querySelector('meta[name="description"]')
    ?.setAttribute('content', m.meta.description)
  for (const [key, content] of [
    ['property og:title', m.meta.title],
    ['property og:description', m.meta.description],
    ['name twitter:title', m.meta.title],
    ['name twitter:description', m.meta.description],
  ]) {
    const [attr, name] = key.split(' ')
    document
      .querySelector(`meta[${attr}="${name}"]`)
      ?.setAttribute('content', content)
  }
})
