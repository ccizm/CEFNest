import { createRouter, createWebHashHistory } from 'vue-router'
import HomeView from './views/HomeView.vue'
import BuildsView from './views/BuildsView.vue'
import DocsView from './views/DocsView.vue'
import ApiView from './views/ApiView.vue'

const router = createRouter({
  history: createWebHashHistory(),
  scrollBehavior() {
    return { top: 0 }
  },
  routes: [
    { path: '/', name: 'home', component: HomeView },
    { path: '/builds', name: 'builds', component: BuildsView },
    { path: '/docs/:slug+', name: 'docs', component: DocsView },
    { path: '/api/:section?', name: 'api', component: ApiView },

    // 旧地址留着：下载页并进了构建产物页，开发文档改成 markdown。
    { path: '/downloads', redirect: '/builds' },
    { path: '/dev', redirect: '/docs/README' },
    { path: '/dev/start', redirect: '/docs/guides/first-run' },
    { path: '/dev/run', redirect: '/docs/guides/first-run' },
    { path: '/dev/build', redirect: '/docs/root/README.zh-CN' },
    { path: '/dev/arch', redirect: '/docs/internals/nested-split' },
    { path: '/dev/limits', redirect: '/docs/known-limitations' },
    { path: '/dev/:section+', redirect: '/docs/README' },
    { path: '/:pathMatch(.*)*', redirect: '/' },
  ],
})

export default router
