// 构建产物表：每一行就是仓库里的一个版本 tag。
//
// 日期与时间取 tag 的创建时间，说明取它指向的那次提交——这些都是量到的，不是计划。
// status 只写被观察到的事实，判定过程在 docs/testing/cnb-ci.md §4。
// 只有 published 这一行的附件直链真的存在：冒烟阶段排在创建 Release 之前，
// 一条红的冒烟意味着那一版根本没发包，直链会 404。
import { fileNames } from './release.js'

export const asOf = '2026-10-23 18:48 +0800'

export const buildStatuses = {
  published: { label: '资产已发布', cls: 'text-bg-success' },
  running: { label: '流水线正在跑', cls: 'text-bg-warning' },
  red: { label: '冒烟红，未发包', cls: 'text-bg-danger' },
  untracked: { label: '早于流水线首跑', cls: 'text-bg-secondary' },
}

const two = (v) => fileNames(v)

export const builds = [
  {
    version: '154.0.34-9', tag: 'v154.0.34-9', date: '2026-10-23', time: '18:33',
    status: 'running', files: two('154.0.34-9'), docs: 'docs/testing/cnb-ci.md',
    note: 'tzdata 的兼容准备被抽成 scripts/ci-prepare-tzdata.sh，并同时接在构建与冒烟两个 job 前面。',
  },
  {
    version: '154.0.34-8', tag: 'v154.0.34-8', date: '2026-10-23', time: '15:58',
    status: 'red', files: two('154.0.34-8'), docs: 'docs/testing/cnb-ci.md',
    note: '与 -7 同一份组件源码，只为把那条红读出名字。红在 tzdata 的 postinst：它把 16 个包一起带崩。',
  },
  {
    version: '154.0.34-7', tag: 'v154.0.34-7', date: '2026-10-23', time: '15:16',
    status: 'red', files: two('154.0.34-7'), docs: 'docs/testing/cnb-ci.md',
    note: '发布形态收敛为两个包，demo 只留在构建树里。这是发版流水线的第一次真跑，三条红里有两条是 reporting 的假红。',
  },
  {
    version: '154.0.34-6', tag: 'v154.0.34-6', date: '2026-10-23', time: '13:24',
    status: 'untracked', files: null, docs: 'docs/features/session-lifecycle.md',
    note: '加 ::helper-exited；仍是四个包（libcefnest0 / cefnest-helper / cefnest-demo / cefnest-dev）。',
  },
  {
    version: '154.0.34-5', tag: 'v154.0.34-5', date: '2026-10-23', time: '09:14',
    status: 'untracked', files: null, docs: null,
    note: '修订号与 Release 说明。',
  },
  {
    version: '154.0.34-4', tag: 'v154.0.34-4', date: '2026-10-23', time: '02:57',
    status: 'untracked', files: null, docs: null,
    note: '包依赖分层，构建只在 release tag 上触发。',
  },
  {
    version: '154.0.34-3', tag: 'v154.0.34-3', date: '2026-10-23', time: '02:53',
    status: 'untracked', files: null, docs: null,
    note: '四个包互相依赖，并写上 maintainer。',
  },
  {
    version: '154.0.34-2', tag: 'v154.0.34-2', date: '2026-10-23', time: '02:50',
    status: 'untracked', files: null, docs: null,
    note: 'user-agent 支持、焦点与缓存修复、示例与站点。',
  },
  {
    version: '154.0.34', tag: 'v154.0.34', date: '2026-10-23', time: '02:05',
    status: 'untracked', files: null, docs: null,
    note: '钉住 CEF 154.0.34，改为推 tag 发包。',
  },
]
