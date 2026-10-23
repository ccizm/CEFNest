export const repo = 'https://cnb.cool/yanzhiming/CEFNest'

export const quickstart = `# 构建 CEF、helper、控件与 demo
./scripts/fetch-cef.sh
cmake -S helper -B helper/build \\
  -DCMAKE_BUILD_TYPE=Release
cmake --build helper/build -j
meson setup builddir && ninja -C builddir

# 跑 demo
CHROMIUM_URL=https://example.com \\
  ./builddir/src/cefnest-demo`

export const embed = `GtkWidget       *view    = g_object_new (CHROMIUM_TYPE_VIEW, NULL);
ChromiumSession *session = chromium_session_new (NULL);

gtk_window_set_child (GTK_WINDOW (window), view);
chromium_session_set_view (session, CHROMIUM_VIEW (view));
chromium_session_start (session, "https://example.com", &error);`

export const links = [
  {
    label: 'README',
    note: '构建、安装、API 与已知限制',
    href: `${repo}/-/blob/main/README.md`,
  },
  {
    label: '控制通道',
    note: 'NDJSON 命令与事件',
    href: `${repo}/-/blob/main/docs/control-channel.md`,
  },
  {
    label: '示例',
    note: '十三个最小宿主',
    href: `${repo}/-/blob/main/examples/README.md`,
  },
  {
    label: 'API 头文件',
    note: '每个调用的完整契约',
    href: `${repo}/-/blob/main/src/chromium-session.h`,
  },
]
