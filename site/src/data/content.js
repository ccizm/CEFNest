// The release the download links point at. It moves together with the release
// strings in .cnb.yml — the drift check-release-tag.sh guards over there cannot
// reach this file, so bump both by hand.
export const releaseTag = 'v156.0.14-2'
export const releaseVersion = '156.0.14-2'

// 中文版指向 CNB，英文版指向 GitHub：同一份仓库、同一批发布产物，但两边路径
// 形态不同——CNB 的子页面带 /- 前缀（/-/blob、/-/releases），GitHub 不带。
export const repos = {
  'zh-CN': 'https://cnb.cool/yanzhiming/CEFNest',
  en: 'https://github.com/ccizm/CEFNest',
}

// 每个语言一份链接，形态跟仓库走；文档链接的文件名（中文还是英文 README）
// 由 i18n 的 docs.links 自己拼。
export function repoLinks (locale) {
  const repo = repos[locale] ?? repos['zh-CN']
  const gh = repo.includes('github.com')
  const blob = `${repo}${gh ? '/blob/main' : '/-/blob/main'}`
  const releases = `${repo}${gh ? '/releases' : '/-/releases'}`
  return {
    repo,
    blob,
    releases,
    debAsset: `${releases}/download/${releaseTag}/cefnest_${releaseVersion}_amd64.deb`,
    rpmAsset: `${releases}/download/${releaseTag}/cefnest-${releaseVersion}.x86_64.rpm`,
  }
}

export const quickstart = {
  'zh-CN': `# 构建 CEF、helper、控件与 demo
./scripts/fetch-cef.sh
cmake -S helper -B helper/build \\
  -DCMAKE_BUILD_TYPE=Release
cmake --build helper/build -j
meson setup builddir && ninja -C builddir

# 跑 demo
CHROMIUM_URL=https://example.com \\
  ./builddir/src/cefnest-demo`,
  en: `# Build CEF, the helper, the widget and the demo
./scripts/fetch-cef.sh
cmake -S helper -B helper/build \\
  -DCMAKE_BUILD_TYPE=Release
cmake --build helper/build -j
meson setup builddir && ninja -C builddir

# Run the demo
CHROMIUM_URL=https://example.com \\
  ./builddir/src/cefnest-demo`,
}

export const embed = `GtkWidget       *view    = g_object_new (CHROMIUM_TYPE_VIEW, NULL);
ChromiumSession *session = chromium_session_new (NULL);

gtk_window_set_child (GTK_WINDOW (window), view);
chromium_session_set_view (session, CHROMIUM_VIEW (view));
chromium_session_start (session, "https://example.com", &error);`
