import { reactive, watchEffect } from 'vue'

const STORAGE_KEY = 'cefnest-theme'
const systemDark = window.matchMedia('(prefers-color-scheme: dark)')

function savedTheme () {
  try {
    const saved = localStorage.getItem(STORAGE_KEY)
    if (saved === 'light' || saved === 'dark') return saved
  } catch {
    // localStorage unavailable (privacy mode); follow the system.
  }
  return null
}

export const theme = reactive({
  mode: savedTheme() ?? (systemDark.matches ? 'dark' : 'light'),
})

export function setTheme (mode) {
  if (mode !== 'light' && mode !== 'dark') return
  theme.mode = mode
  try {
    localStorage.setItem(STORAGE_KEY, mode)
  } catch {}
}

export function toggleTheme () {
  setTheme(theme.mode === 'dark' ? 'light' : 'dark')
}

// Follow the system while the user has not picked a side.
systemDark.addEventListener('change', (e) => {
  if (savedTheme()) return
  theme.mode = e.matches ? 'dark' : 'light'
})

watchEffect(() => {
  const root = document.documentElement
  root.dataset.theme = theme.mode
  root.dataset.bsTheme = theme.mode
  document
    .querySelector('meta[name="theme-color"]')
    ?.setAttribute('content', theme.mode === 'dark' ? '#242428' : '#fafafb')
})
