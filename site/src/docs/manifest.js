// 站点文档的目录：每一项就是仓库里的一个 markdown 文件。
//
// 正文只有 markdown 一份，这里只记「分组、顺序、侧边栏短标签」。页面标题取
// markdown 自己的第一行 `# `，所以改标题不用动这个文件；只有新增或改名文档时
// 才需要在这里加一行——漏了的话 site/scripts/check-docs.js 会在构建前报出来。

export const groups = [
  {
    key: 'start',
    title: '开始',
    items: [
      { src: 'docs/README.md', label: '文档索引 · 先读哪篇' },
      { src: 'README.zh-CN.md', label: '项目说明与构建' },
      { src: 'docs/guides/first-run.md', label: '运行前提与排查' },
      { src: 'docs/guides/minimal-host.md', label: '最小宿主' },
      { src: 'docs/guides/control-channel.md', label: '控制通道' },
      { src: 'docs/guides/packaging.md', label: '打包与 helper 查找' },
      { src: 'docs/known-limitations.md', label: '已知限制' },
    ],
  },
  {
    key: 'features',
    title: '功能行为记录',
    items: [
      { src: 'docs/features/session-lifecycle.md', label: '会话生命周期' },
      { src: 'docs/features/multi-browser.md', label: '单 helper 多 browser' },
      { src: 'docs/features/clipboard-bridge.md', label: '剪贴板桥' },
      { src: 'docs/features/drag-loop.md', label: '拖放环' },
      { src: 'docs/features/wheel-resolution-and-key-repeat.md', label: '滚轮与键盘重复' },
      { src: 'docs/features/download-path.md', label: '下载' },
      { src: 'docs/features/find-path.md', label: '页内查找' },
      { src: 'docs/features/context-menu.md', label: '右键菜单' },
      { src: 'docs/features/permission-prompt.md', label: '权限提示' },
      { src: 'docs/features/zoom-path.md', label: '页面缩放' },
      { src: 'docs/features/audio-path.md', label: '音频' },
      { src: 'docs/features/pdf-path.md', label: 'PDF' },
      { src: 'docs/features/a11y.md', label: '无障碍' },
      { src: 'docs/features/fullscreen.md', label: '页面全屏' },
    ],
  },
  {
    key: 'internals',
    title: '设计记录',
    items: [
      { src: 'docs/internals/nested-split.md', label: 'nested 拆分' },
      { src: 'docs/internals/frame-clock-pacing.md', label: '帧调度' },
      { src: 'docs/internals/frame-metrics.md', label: '帧计量' },
      { src: 'docs/internals/popup-compositing.md', label: '弹出层合成' },
      { src: 'docs/internals/resource-limits.md', label: '资源上限' },
      { src: 'docs/internals/empty-request-slot.md', label: '空请求槽' },
      { src: 'docs/internals/clipboard-offer-during-client-init.md', label: '早一次 dispatch 的 data_offer' },
      { src: 'docs/internals/sandbox-policy.md', label: '沙箱决定' },
      { src: 'docs/internals/protocol-ceilings.md', label: '协议天花板' },
      { src: 'docs/internals/dmabuf-formats.md', label: 'dmabuf 格式表' },
      { src: 'docs/internals/map-cycle.md', label: 'map 周期' },
      { src: 'docs/internals/stack-canary-after-fork.md', label: 'fork 后的栈金丝雀' },
    ],
  },
  {
    key: 'testing',
    title: '测试与 CI',
    items: [
      { src: 'docs/testing/overview.md', label: '测试总览' },
      { src: 'docs/testing/headless-ci.md', label: 'headless 会话' },
      { src: 'docs/testing/cnb-ci.md', label: 'CNB 发版流水线' },
      { src: 'docs/testing/real-sites.md', label: '真实页面巡检' },
    ],
  },
  {
    key: 'notes',
    title: '决策与存档',
    items: [
      { src: 'docs/structural-decisions.md', label: '结构性决策' },
      { src: 'docs/code-review-2026-10-22.md', label: '评审存档 2026-10-22' },
      { src: 'README.md', label: 'README（英文）' },
    ],
  },
]

// slug 规则必须无碰撞：docs/ 下面的去掉前缀，仓库根的那两份加 root/。
// 于是 docs/README.md 是 README，根 README.md 是 root/README，两者不会撞在
// /docs/README 上——地址短，又各自唯一。
export const slugOf = (src) => {
  if (src.startsWith('docs/')) return src.slice('docs/'.length).replace(/\.md$/, '')
  return `root/${src.replace(/\.md$/, '')}`
}

export const bySlug = new Map()
export const bySrc = new Map()
for (const g of groups) {
  for (const it of g.items) {
    it.slug = slugOf(it.src)
    it.group = g
    bySlug.set(it.slug, it)
    bySrc.set(it.src, it)
  }
}

export const docList = groups.flatMap((g) => g.items)
