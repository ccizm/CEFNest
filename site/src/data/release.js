// 当前发布版本与 Release 资产。
//
// CNB 的 Release 页与附件直链分别形如
//   /-/releases/tag/<tag>
//   /-/releases/download/<tag>/<file>
// 版本号只在这里写一次；改名或升修订号时，页面上的文件名、直链与命令一起动。
const repository = 'https://cnb.cool/yanzhiming/CEFNest'
const version = '154.0.34-6'
const tag = `v${version}`

export const release = {
  version,
  tag,
  cef: '154.0.34+g14c5a08',
  chromium: '154.0.8037.98',
  repository,
  page: `${repository}/-/releases/tag/${tag}`,
}

export function assetUrl (name) {
  return `${repository}/-/releases/download/${tag}/${name}`
}

// 四个包，两种格式。deb 与 rpm 的内容一一对应，名字从不相同：
// libcefnest0 / cefnest，libcefnest-dev / cefnest-devel。
export const packages = [
  {
    key: 'runtime',
    title: '运行库',
    icon: 'bi-box-seam',
    names: 'libcefnest0 · cefnest',
    desc: '嵌套合成器、ChromiumView 部件与 ChromiumSession 控制面。',
    depends: '精确依赖 cefnest-helper',
    deb: `libcefnest0_${version}_amd64.deb`,
    rpm: `cefnest-${version}.x86_64.rpm`,
  },
  {
    key: 'dev',
    title: '开发文件',
    icon: 'bi-code-slash',
    names: 'libcefnest-dev · cefnest-devel',
    desc: '三个头文件、链接用的 libcefnest.so 与 cefnest.pc。',
    depends: '精确依赖 libcefnest0',
    deb: `libcefnest-dev_${version}_amd64.deb`,
    rpm: `cefnest-devel-${version}.x86_64.rpm`,
  },
  {
    key: 'helper',
    title: 'helper 与 CEF 运行时',
    icon: 'bi-gear-wide-connected',
    names: 'cefnest-helper',
    desc: '独立进程里的 CEF 应用，连同 libcef.so、icudtl.dat、locales 等整套运行时；安装后约 1.5 GB。',
    depends: '只依赖系统库，是依赖链的底层',
    deb: `cefnest-helper_${version}_amd64.deb`,
    rpm: `cefnest-helper-${version}.x86_64.rpm`,
  },
  {
    key: 'demo',
    title: '演示程序',
    icon: 'bi-window',
    names: 'cefnest-demo',
    desc: '可脚本化的演示浏览器，以及没有 Chromium 时回退用的 shm-client。',
    depends: '精确依赖 libcefnest0 与 cefnest-helper',
    deb: `cefnest-demo_${version}_amd64.deb`,
    rpm: `cefnest-demo-${version}.x86_64.rpm`,
  },
]

export const extras = [
  {
    key: 'tree',
    title: '完整构建树',
    icon: 'bi-file-earmark-zip',
    file: 'cefnest-build.tar.gz',
    desc: 'src/、examples/、test/ 三层构建目录：库、demo、全部示例与测试客户端。保持三层解包，二进制的 $ORIGIN rpath 才能找到同批的库。',
    note: '不含 helper，也不含 CEF 运行时。',
  },
  {
    key: 'bundle',
    title: '安装包合集',
    icon: 'bi-archive',
    file: `cefnest-packages-${version}.tar.gz`,
    desc: '运行库、开发文件与 demo 的 deb / rpm 共 6 个包，适合离线分发。',
    note: '不含 helper 包；要渲染页面仍须单独下载 cefnest-helper。',
  },
  {
    key: 'sums',
    title: 'SHA-256 校验和',
    icon: 'bi-shield-check',
    file: 'SHA256SUMS',
    desc: 'dist/ 下全部上传物的 SHA-256 清单，包含构建树、安装包合集与四个包的两套格式。',
    note: '下载后先校验，再安装。',
  },
]

export const highlights = [
  'wl_shm 的请求槽 1 补齐，全局版本升到 2：Wayland 1.22 客户端销毁自己的 wl_shm 代理时，不再把嵌套合成器与宿主一起 wl_abort。',
  'CEF 子进程改用 _exit() 离开 main()，不再穿过继承来的栈保护尾言；此前的“stack smashing detected”与 SIGABRT 不再出现。',
  '新增 ::helper-exited：页面没了（::closed）与 helper 进程真正被收走分成两个时刻，宿主可以安全地等后者再拆合成器。',
]
