// 当前发布版本与 Release 资产。
//
// CNB 的 Release 页与附件直链分别形如
//   /-/releases/tag/<tag>
//   /-/releases/download/<tag>/<file>
// 版本号只在这里写一次；改名或升修订号时，页面上的文件名、直链与命令一起动。
const repository = 'https://cnb.cool/yanzhiming/CEFNest'
const version = '154.0.34-9'
const tag = `v${version}`

export const release = {
  version, tag,
  cef: '154.0.34+g14c5a08',
  chromium: '154.0.8037.98',
  repository,
  page: `${repository}/-/releases/tag/${tag}`,
}

export function assetUrl (name) {
  return `${repository}/-/releases/download/${tag}/${name}`
}

// 两个包，两种格式。deb 与 rpm 的内容一一对应，名字只有开发包不同：
// cefnest / cefnest（GTK 4 组件库），cefnest-dev / cefnest-devel。
// demo 与 shm-client 是开发测试工具，不单独发包，只在构建树里。
export const packages = [
  {
    key: 'component',
    title: 'GTK 4 组件库',
    icon: 'bi-box-seam',
    names: 'cefnest',
    desc: '嵌套合成器、ChromiumView 部件、ChromiumSession 控制面；以 libcefnest.so.0 装成系统库，连同 cefnest-helper 与完整 CEF 运行时。安装后约 1.5 GB。',
    depends: '只依赖系统库；任意 GTK 4 宿主都可直接链接',
    deb: `cefnest_${version}_amd64.deb`,
    rpm: `cefnest-${version}.x86_64.rpm`,
  },
  {
    key: 'dev',
    title: '开发文件',
    icon: 'bi-code-slash',
    names: 'cefnest-dev · cefnest-devel',
    desc: '三个头文件、链接用的 libcefnest.so 与 cefnest.pc。只在编译宿主程序时需要。',
    depends: '精确依赖 cefnest',
    deb: `cefnest-dev_${version}_amd64.deb`,
    rpm: `cefnest-devel-${version}.x86_64.rpm`,
  },
]

export const extras = [
  {
    key: 'tree',
    title: '完整构建树',
    icon: 'bi-file-earmark-zip',
    file: 'cefnest-build.tar.gz',
    desc: 'src/、examples/、test/ 三层构建目录：库、demo、全部示例与测试客户端。保持三层解包，二进制的 $ORIGIN rpath 才能找到同批的库。',
    note: '不含 helper，也不含 CEF 运行时；装 cefnest 后，src/cefnest-demo 会直接找到安装位置里的 helper。demo 与 shm-client 只在这里，不单独发包。',
  },
  {
    key: 'sums',
    title: 'SHA-256 校验和',
    icon: 'bi-shield-check',
    file: 'SHA256SUMS',
    desc: 'dist/ 下全部上传物的 SHA-256 清单，包含构建树与两个包的两种格式。',
    note: '下载后先校验，再安装。',
  },
]

export const highlights = [
  '发布包从四个收敛为两个：cefnest 是 GTK 4 组件库本身，连同 helper 与 CEF 一起交付；cefnest-dev 只保留头文件、.so 与 pkg-config。',
  'cefnest-demo 与 shm-client 不再单独发包，作为开发测试工具留在 cefnest-build.tar.gz 的 src/ 与 test/ 里。',
  '构建树改为按 -Dprefix=/usr 编译，解包后的 demo 直接找到安装包放在 /usr/libexec/cefnest/<arch>/ 的 helper。',
  '旧包名通过 Replaces/Breaks、Obsoletes 迁移，从 154.0.34-6 升级不需要手工卸载。',
  '组件行为与 154.0.34-6 相同；本版只改发布形态。',
]
