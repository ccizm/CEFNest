// 当前发布版本与包名规则。
//
// CNB 的 Release 页与附件直链分别形如
//   /-/releases/tag/<tag>
//   /-/releases/download/<tag>/<file>
// 版本号只在这里写一次；升修订号时，页面上的文件名、直链与命令一起动。
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

// 包名只由版本号决定，构建树与校验和清单不带版本号。
export function fileNames (v) {
  return {
    deb: `cefnest_${v}_amd64.deb`,
    debDev: `cefnest-dev_${v}_amd64.deb`,
    rpm: `cefnest-${v}.x86_64.rpm`,
    rpmDev: `cefnest-devel-${v}.x86_64.rpm`,
    tree: 'cefnest-build.tar.gz',
    sums: 'SHA256SUMS',
  }
}

export const currentFiles = fileNames(version)

export function assetUrl (name, at = tag) {
  return `${repository}/-/releases/download/${at}/${name}`
}

// 两类包，两种格式。deb 与 rpm 的内容一一对应，名字只有开发包不同：
// cefnest / cefnest（GTK 4 组件库），cefnest-dev / cefnest-devel。
// demo 与 shm-client 是开发测试工具，不单独发包，只在构建树里。
export const packageKinds = [
  {
    key: 'component',
    title: 'GTK 4 组件库',
    icon: 'bi-box-seam',
    names: 'cefnest',
    desc: '嵌套合成器、ChromiumView 部件、ChromiumSession 控制面；以 libcefnest.so.0 装成系统库，连同 cefnest-helper 与完整 CEF 运行时。安装后约 1.5 GB。',
    depends: '只依赖系统库；任意 GTK 4 宿主都可直接链接',
  },
  {
    key: 'dev',
    title: '开发文件',
    icon: 'bi-code-slash',
    names: 'cefnest-dev · cefnest-devel',
    desc: '三个头文件、链接用的 libcefnest.so 与 cefnest.pc。只在编译宿主程序时需要。',
    depends: '精确依赖 cefnest',
  },
]

export const buildTree = {
  file: currentFiles.tree,
  desc: 'src/、examples/、test/ 三层构建目录：库、demo、全部示例与测试客户端。保持三层解包，二进制的 $ORIGIN rpath 才能找到同批的库。',
  note: '不含 helper，也不含 CEF 运行时；装 cefnest 后，src/cefnest-demo 会直接找到安装位置里的 helper。',
}

export const checksums = {
  file: currentFiles.sums,
  desc: 'dist/ 下全部上传物的 SHA-256 清单，包含构建树与两个包的两种格式。',
}
