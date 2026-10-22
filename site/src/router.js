import { createRouter, createWebHashHistory } from 'vue-router'
import HomeView from './views/HomeView.vue'
import DevView from './views/DevView.vue'
import ApiView from './views/ApiView.vue'

const router = createRouter({
  history: createWebHashHistory(),
  scrollBehavior() {
    return { top: 0 }
  },
  routes: [
    { path: '/', name: 'home', component: HomeView },
    { path: '/dev/:section?', name: 'dev', component: DevView },
    { path: '/api/:section?', name: 'api', component: ApiView },
  ],
})

export default router
