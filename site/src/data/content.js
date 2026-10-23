export const repo = 'https://cnb.cool/yanzhiming/CEFNest'

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
