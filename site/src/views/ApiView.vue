<script setup>
import { computed } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import DocLayout from '../components/DocLayout.vue'
import CodeBlock from '../components/CodeBlock.vue'
import ApiEntry from '../components/ApiEntry.vue'
import { apiSections, entries, signalRows } from '../data/apiData.js'
import { release } from '../data/release.js'

const route = useRoute()
const router = useRouter()

const section = computed(() => {
  const s = route.params.section
  if (apiSections.some((i) => i.key === s)) return s
  router.replace('/api/quickstart')
  return 'quickstart'
})

const quickCode = `static void
on_activate (GtkApplication *app, gpointer data)
{
  GtkWidget       *window  = gtk_application_window_new (app);
  ChromiumView    *view    = g_object_new (CHROMIUM_TYPE_VIEW, NULL);
  ChromiumSession *session = chromium_session_new (NULL);
  GError          *error   = NULL;

  /* 顺序是硬性的：先绑定，后启动 —— helper 要从视图的合成器取 WAYLAND_DISPLAY */
  chromium_session_set_view (session, view);
  gtk_window_set_child (GTK_WINDOW (window), GTK_WIDGET (view));
  gtk_window_present (GTK_WINDOW (window));

  if (!chromium_session_start (session, "https://cnb.cool/yanzhiming/CEFNest", &error))
    g_warning ("cannot start the helper: %s", error->message);

  /* 会话归窗口所有，窗口关闭即停止 */
  g_object_set_data_full (G_OBJECT (window), "session",
                          session, g_object_unref);
}`

const hostCode = `/* examples/host.c —— 页面站在宿主的回答上的四个信号 */
g_signal_connect (session, "download-begin",      /* 宿主决定文件落到哪 */
                  G_CALLBACK (on_download_begin), NULL);
g_signal_connect (session, "permission-request",  /* 宿主决定答不答应 */
                  G_CALLBACK (on_permission), NULL);
g_signal_connect (session, "script-result",       /* JS 执行的回执 */
                  G_CALLBACK (on_script_result), NULL);
g_signal_connect (session, "browser-created",     /* 标签/窗口的键 */
                  G_CALLBACK (on_browser_created), NULL);`

/* ---------- 常用示例 ---------- */
const exWindow = `/* browser.c —— 带 URL 栏与导航按钮的完整 GTK4 窗口宿主。
   编译：cc browser.c -o browser $(pkg-config --cflags --libs cefnest)
   这副骨架是其余各示例的底座：后面的处理器都往它上面挂。 */
#include <gtk/gtk.h>
#include <cefnest/chromium-view.h>
#include <cefnest/chromium-session.h>

static void
on_back (GtkButton *btn, gpointer session)
{
  chromium_session_go_back (session);
}

static void
on_forward (GtkButton *btn, gpointer session)
{
  chromium_session_go_forward (session);
}

static void
on_reload (GtkButton *btn, gpointer session)
{
  chromium_session_reload (session, FALSE);   /* TRUE = 强制绕缓存 */
}

static void
on_url_activate (GtkEntry *entry, gpointer session)
{
  chromium_session_load_uri (session,
                             gtk_editable_get_text (GTK_EDITABLE (entry)));
}

static void
on_load_end (ChromiumSession *session, const char *url,
             int http_code, gpointer window)
{
  /* 把窗口标题换成页面标题 */
  gtk_window_set_title (window, chromium_session_get_title (session));
}

static gboolean
on_close (GtkWindow *window, gpointer session)
{
  chromium_session_stop (session);
  return FALSE;   /* FALSE = 继续默认的窗口关闭流程 */
}

static void
on_activate (GtkApplication *app, gpointer data)
{
  ChromiumSession *session = chromium_session_new (NULL); /* NULL = 自动找 helper */
  ChromiumView    *view    = g_object_new (CHROMIUM_TYPE_VIEW, NULL);
  GError          *error   = NULL;

  GtkWidget *window = gtk_application_window_new (app);
  GtkWidget *box    = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *bar    = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *back   = gtk_button_new_from_icon_name ("go-previous-symbolic");
  GtkWidget *fwd    = gtk_button_new_from_icon_name ("go-next-symbolic");
  GtkWidget *reload = gtk_button_new_from_icon_name ("view-refresh-symbolic");
  GtkWidget *entry  = gtk_entry_new ();

  g_signal_connect (back,   "clicked",  G_CALLBACK (on_back), session);
  g_signal_connect (fwd,    "clicked",  G_CALLBACK (on_forward), session);
  g_signal_connect (reload, "clicked",  G_CALLBACK (on_reload), session);
  g_signal_connect (entry,  "activate", G_CALLBACK (on_url_activate), session);

  gtk_box_append (GTK_BOX (bar), back);
  gtk_box_append (GTK_BOX (bar), fwd);
  gtk_box_append (GTK_BOX (bar), reload);
  gtk_box_append (GTK_BOX (bar), entry);
  gtk_widget_set_hexpand (entry, TRUE);
  gtk_entry_set_placeholder_text (GTK_ENTRY (entry), "输入网址，回车打开");

  gtk_box_append (GTK_BOX (box), bar);
  gtk_box_append (GTK_BOX (box), GTK_WIDGET (view));
  gtk_widget_set_vexpand (GTK_WIDGET (view), TRUE);   /* 页面占满剩余空间 */

  gtk_window_set_title (GTK_WINDOW (window), "CEFNest browser");
  gtk_window_set_default_size (GTK_WINDOW (window), 1000, 700);
  gtk_window_set_child (GTK_WINDOW (window), box);
  gtk_window_present (GTK_WINDOW (window));

  /* 顺序是硬性的：视图先进窗口，会话再绑定、启动——
     helper 的 WAYLAND_DISPLAY 取自视图的合成器 */
  chromium_session_set_view (session, view);
  g_signal_connect (session, "load-end", G_CALLBACK (on_load_end), window);
  g_signal_connect (window, "close-request", G_CALLBACK (on_close), session);
  if (!chromium_session_start (session, "https://cnb.cool/yanzhiming/CEFNest", &error))
    {
      g_printerr ("cannot start the helper: %s\\n", error->message);
      g_clear_error (&error);
    }

  /* 会话归窗口所有：窗口销毁时会话随之释放 */
  g_object_set_data_full (G_OBJECT (window), "session",
                          session, g_object_unref);
}

int
main (int argc, char **argv)
{
  GtkApplication *app = gtk_application_new ("com.example.browser",
                                             G_APPLICATION_DEFAULT_FLAGS);
  int status;

  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
  status = g_application_run (G_APPLICATION (app), argc, argv);
  g_object_unref (app);
  return status;
}`

const exLoad = `static void
on_load_end (ChromiumSession *session, const char *url,
             int http_code, gpointer data)
{
  /* 失败的导航也发 ::load-end，http_code 为 0——
     要区分"加载了"与"没加载出来"，另挂 ::load-error */
  g_print ("loaded %s (http %d), title: %s\\n", url, http_code,
           chromium_session_get_title (session));
}

  g_signal_connect (session, "load-end", G_CALLBACK (on_load_end), NULL);

  /* 启动后另开一个页面 */
  chromium_session_load_uri (session, "https://cnb.cool/yanzhiming/CEFNest");

  /* 前进 / 后退 / 刷新 / 停止 */
  chromium_session_go_back (session);
  chromium_session_reload (session, FALSE);   /* TRUE = 绕缓存 */

  /* 非法域名：::load-error 先报真实码，约一秒后再报一次 ERR_ABORTED (-3) */`

const exJs = `/* 请求：带一个宿主自定的 id，回执带同一个 id 回来 */
chromium_session_run_javascript (session, "document.title", 42);

static void
on_script_result (ChromiumSession *session, int id, gboolean ok,
                  const char *value, const char *message, gpointer data)
{
  if (ok)
    g_print ("js[%d] = %s\\n", id, value);   /* value 是 JSON 文本："null" 表示无返回值 */
  else
    g_printerr ("js[%d] failed: %s\\n", id, message);  /* 失败 / 不可序列化 / 超通道容量 */
}

/* CSP 严格的页面（script-src 禁止 unsafe-eval）用表达式形式，
   代码不经 eval() 包裹，同样以 ::script-result 应答 */
chromium_session_run_javascript_expression (session, "1 + 1", 43);`

const exMessage = `/* 页面 → 宿主：renderer 在每个 context 注册了 cefnestPostMessage()。
   页面里执行 cefnestPostMessage(JSON.stringify ({kind: "ping"})) 后： */
static void
on_page_message (ChromiumSession *session, const char *json, gpointer data)
{
  g_print ("page says: %s\\n", json);   /* 整串收回，内部 schema 宿主自定 */
}

/* 宿主 → 页面：没有专用命令，派发一个 MessageEvent 就是一趟 */
chromium_session_run_javascript_expression (session,
    "window.dispatchEvent (new MessageEvent ('message',"
    " { data: { kind: \\"pong\\" } }))",
    7);`

const exDownload = `/* ::download-begin 是问题，accept/reject 是回答——不回答的下载永远停靠、不落盘 */
static void
on_download_begin (ChromiumSession *session, int id, const char *url,
                   const char *suggested_name, const char *mime_type,
                   gint64 total_bytes, gpointer data)
{
  g_autofree char *path = g_build_filename (
      g_get_user_special_dir (G_USER_DIRECTORY_DOWNLOAD),
      suggested_name, NULL);
  /* 目录是宿主的特权，页面无权置喙；父目录不存在 helper 不会创建。
     total_bytes 在服务器未声明长度时为 -1 */
  chromium_session_accept_download (session, id, path);
  /* 拒绝：chromium_session_reject_download (session, id);
     在途控制：pause_download / resume_download */
}

static void
on_download_end (ChromiumSession *session, int id, const char *state,
                 const char *path, gint64 received, gpointer data)
{
  /* 唯一收尾处：state = "complete" / "cancelled" / "interrupted"，
     接受与拒绝两条路都终结于此，宿主的下载列表只盯它即可 */
  g_print ("download %d: %s\\n", id, state);
}

  g_signal_connect (session, "download-begin",
                    G_CALLBACK (on_download_begin), NULL);
  g_signal_connect (session, "download-end",
                    G_CALLBACK (on_download_end), NULL);`

const exFind = `/* 开始搜索。应答在 ::find-result：有匹配答两次（计数 + final=TRUE），
   无匹配答一次（count 0，final=TRUE） */
chromium_session_find (session, "cefnest", TRUE, TRUE, FALSE);

static void
on_find_result (ChromiumSession *session, int identifier, int count,
                int active_ordinal, gboolean final, gpointer data)
{
  if (final)
    g_print ("matches: %d, active: %d\\n", count, active_ordinal);
}

  g_signal_connect (session, "find-result", G_CALLBACK (on_find_result), NULL);

  /* 上 / 下一个匹配：pattern 每次都要给全（helper 不保存查找状态） */
  chromium_session_find (session, "cefnest", TRUE,  TRUE, TRUE);   /* 下一个 */
  chromium_session_find (session, "cefnest", FALSE, TRUE, TRUE);   /* 上一个 */

  /* 结束并清掉 Chromium 画的高亮 */
  chromium_session_stop_finding (session, TRUE);`

const exMenu = `/* 什么都不连 = session 在视图上弹自带的 popover，右键即用。
   要自绘时挂 ::context-menu，返回 TRUE 并承担回答义务： */
static gboolean
on_context_menu (ChromiumSession *session, int x, int y,
                 GList *items, gpointer data)
{
  for (GList *l = items; l; l = l->next)
    {
      ChromiumMenuItem *m = l->data;
      /* id 是 CEF 的编号：100 后退、117 全选、132 查看源码…
         helper 扩充的链接/图片/媒体行在 26501–26507 */
      if (m->enabled && g_str_equal (m->type, "command"))
        g_print ("item %d: %s\\n", m->id, m->label);
    }

  /* 自绘后必须恰好回答一次：选中一项，或整个作罢 */
  chromium_session_context_menu_select (session, 100);   /* 执行"后退" */
  return TRUE;
  /* 不回答的菜单连累后面每一次右键——Chromium 只握一个未答的菜单请求 */
}

  g_signal_connect (session, "context-menu", G_CALLBACK (on_context_menu), NULL);`

const exPermission = `/* 页面问宿主要麦克风/定位/剪贴板……不回答的代价比沉默大：
   promise 悬着，第二个 media 请求永远不来 */
static gboolean
on_permission (ChromiumSession *session, int id, const char *kind,
               const char *origin, GList *permissions, int raw,
               gpointer data)
{
  gboolean allow = g_str_equal (kind, "media") &&
                   g_str_has_suffix (origin, "example.com");

  chromium_session_permission_respond (
      session, id, allow ? CHROMIUM_PERMISSION_ACCEPT
                         : CHROMIUM_PERMISSION_DENY);
  return TRUE;   /* TRUE = 我来答；FALSE / 不连接 = session 代答拒绝 */
}

  g_signal_connect (session, "permission-request",
                    G_CALLBACK (on_permission), NULL);`

const exTabs = `/* 标签管理器：CEF 分配 id，宿主以它为键建自己的标签表。
   ::browser-selected 可能先于 ::browser-created——未见的 id 也要先立标签 */
typedef struct { int id; char *title; gboolean seen; } Tab;
static GHashTable *tabs;   /* GINT_TO_POINTER (id) -> Tab* */

static Tab *
tab_ensure (int id)
{
  Tab *t = g_hash_table_lookup (tabs, GINT_TO_POINTER (id));
  if (!t)
    {
      t = g_new0 (Tab, 1);
      t->id = id;
      g_hash_table_insert (tabs, GINT_TO_POINTER (id), t);
    }
  return t;
}

static void
on_browser_created (ChromiumSession *s, int id, const char *url,
                    gboolean windowed, gpointer data)
{
  tab_ensure (id)->seen = TRUE;
  if (!windowed)
    add_tab_ui (id, url);   /* windowed=FALSE = 进共享标签窗口 */
}

static void
on_browser_selected (ChromiumSession *s, int id, gpointer data)
{
  mark_tab_active (tab_ensure (id));   /* 高亮——它可能还没见过 created */
}

static void
on_browser_closed (ChromiumSession *s, int id, gpointer data)
{
  remove_tab_ui (id);
  g_hash_table_remove (tabs, GINT_TO_POINTER (id));
  /* 标签与窗口都会走到这里，::browser-closed 只发一次 */
}

/* 三个动作 */
chromium_session_new_browser    (session, "https://example.org", FALSE); /* 新标签 */
chromium_session_select_browser (session, id);                           /* 激活 */
chromium_session_close_browser  (session, id);                           /* 关闭标签或窗口 */`

const exWindows = `/* 窗口模式：TRUE 让 helper 建独立的 CefWindow——
   它经嵌套合成器成为桌面上的独立顶层窗口，不在你的 GTK 部件树里。
   宿主对它只有 select / close 两个动作，改不了它的布局。 */
chromium_session_new_browser (session, "https://example.net", TRUE);

static void
on_browser_closed (ChromiumSession *s, int id, gpointer data)
{
  remove_window_ui (id);   /* 标签同样走 ::browser-closed；这里收掉窗口 UI */
}

/* 窗口与标签都能被宿主关掉；未知 id 答 status: no-such-browser */
chromium_session_close_browser (session, id);

/* 关掉最后一个 browser 会结束 helper：::closed 之后还有 ::helper-exited */

/* 多会话 = 多 helper：每个 ChromiumSession 拉起自己的 helper 进程。
   要两个视图各嵌一处才建两个会话；标签切换应该用上面的多标签 API */`

const exEmbed = `/* ChromiumView 就是个普通 GTK 部件——单页面嵌进既有布局，
   侧栏 + 内容双栏应用： */
  GtkWidget    *paned   = gtk_paned_new (GTK_ORIENTATION_HORIZONTAL);
  GtkWidget    *sidebar = gtk_list_box_new ();   /* 自家业务列表 */
  ChromiumView *view    = g_object_new (CHROMIUM_TYPE_VIEW, NULL);

  gtk_widget_set_size_request (sidebar, 240, -1);
  gtk_paned_set_start_child (GTK_PANED (paned), sidebar);
  gtk_paned_set_end_child   (GTK_PANED (paned), GTK_WIDGET (view));
  gtk_paned_set_position    (GTK_PANED (paned), 240);
  gtk_window_set_child (GTK_WINDOW (window), paned);

  /* 一个会话一个视图：标签切换在同一个视图里换浏览器，
     不必为每个页面开新会话（新会话 = 新 helper 进程） */
  session = chromium_session_new (NULL);
  chromium_session_set_view (session, view);`

const exPopup = `/* ::popup-requested (url) —— target=_blank / window.open / 中键。
   helper 已经行动过：前台请求就地导航了当前视图；只剩后台标签
   是"报给你、然后晾着"——开不开、开成什么，宿主定： */
static void
on_popup_requested (ChromiumSession *s, const char *url, gpointer data)
{
  chromium_session_new_browser (s, url, FALSE);   /* 开成标签 */
}

/* 右键菜单里 helper 不代劳的两件事：
   ::open-url（"在新窗口打开链接"）要宿主给个窗口；
   ::copy-text（复制链接/图片地址）要宿主放进系统剪贴板 */
static void
on_open_url (ChromiumSession *s, const char *url, gpointer data)
{
  chromium_session_new_browser (s, url, TRUE);    /* 新窗口 */
}

static void
on_copy_text (ChromiumSession *s, const char *text, gpointer data)
{
  gdk_clipboard_set_text (
      gdk_display_get_clipboard (gdk_display_get_default ()), text);
}

  g_signal_connect (session, "popup-requested", G_CALLBACK (on_popup_requested), NULL);
  g_signal_connect (session, "open-url",        G_CALLBACK (on_open_url), NULL);
  g_signal_connect (session, "copy-text",       G_CALLBACK (on_copy_text), NULL);`

const exErrors = `/* 同一次失败导航报两次：真实码在前，约一秒后补一次 ERR_ABORTED (-3)。
   去重过滤器： */
static void
on_load_error (ChromiumSession *s, int code, const char *url,
               const char *message, gpointer data)
{
  if (code == -3)   /* 补报，跳过 */
    return;
  show_error_page (url, message);   /* "ERR_NAME_NOT_RESOLVED" 之类 */
}

/* renderer 死了：页面停在最后一帧，之后所有命令石沉大海——
   从外面看就是"页面不动了"。status：0 异常退出 1 被杀 2 崩溃(SIGSEGV) 3 内存不足 */
static void
on_renderer_exited (ChromiumSession *s, int status, int error_code,
                    const char *message, gpointer data)
{
  show_crash_banner (status, message);   /* 例如提供"重新加载"按钮 */
}

/* ::closed (reason) —— 页面没了，本会话不会复活。reason 说明死因；
   进程何时真正收走看 ::helper-exited */
static void
on_closed (ChromiumSession *s, const char *reason, gpointer data)
{
  show_fatal_banner (reason);   /* 唯一确定的事：别再对这个会话发命令 */
}

/* ::helper-exited —— 子进程已被 wait 收走。拆与 helper 共用的合成器等这一条：
   ::closed 触发时，CefShutdown() 可能还在用那条 Wayland 连接 */
static void
on_helper_exited (ChromiumSession *s, gpointer data)
{
  teardown_shared_compositor ();
}

  g_signal_connect (session, "load-error",      G_CALLBACK (on_load_error), NULL);
  g_signal_connect (session, "renderer-exited", G_CALLBACK (on_renderer_exited), NULL);
  g_signal_connect (session, "closed",          G_CALLBACK (on_closed), NULL);
  g_signal_connect (session, "helper-exited",   G_CALLBACK (on_helper_exited), NULL);`

const exProgress = `/* ::download-progress (id, received, total, percent, speed)
   跟进在途下载；percent 在总长未知时为 -1，speed 单位字节/秒 */
static void
on_download_progress (ChromiumSession *s, int id, gint64 received,
                      gint64 total, int percent, gint64 speed,
                      gpointer bar)
{
  if (percent >= 0)
    gtk_progress_bar_set_fraction (bar, percent / 100.0);

  g_autofree char *label =
    g_strdup_printf ("%" G_GINT64_FORMAT " B/s · %d %%",
                     speed, percent);
  gtk_progress_bar_set_text (bar, label);
}

  g_signal_connect (session, "download-progress",
                    G_CALLBACK (on_download_progress), progress_bar);

/* 在途控制——id 从 ::download-begin 拿 */
chromium_session_pause_download  (session, id);
chromium_session_resume_download (session, id);
/* ::download-end 的 "complete" 是下载会话结束，不是文件已刷盘的时刻 */`

const exObs = `/* 帧计数器：合成器呈现过的顶层帧总数（是计数，不是速率）。
   宿主自己定时采样画速率图；页面冻结时它不再增长 */
static gboolean
tick (gpointer data)
{
  g_print ("frames: %" G_GUINT64_FORMAT "\\n",
           chromium_session_get_frame_count (CHROMIUM_SESSION (data)));
  return G_SOURCE_CONTINUE;
}

  g_timeout_add_seconds (1, tick, session);

/* 无障碍：默认关；打开后页面的无障碍树桥进 AT-SPI，开销不小，
   只在屏幕阅读器在场时开 */
chromium_session_set_accessibility (session, TRUE);`

const exMisc = `/* 缩放："in"/"out" 各走一个 Chrome UI 档（0.5），"reset" 回 100%。
   Chromium 按来源持久化缩放——导航到存过级别的来源拿到的是存的值 */
chromium_session_zoom (session, "in");
chromium_session_zoom (session, "reset");

static void
on_zoom_changed (ChromiumSession *session, double level, gpointer data)
{
  g_print ("zoom: %.2f\\n", level);   /* 0.0 = 100% */
}

  g_signal_connect (session, "zoom-changed", G_CALLBACK (on_zoom_changed), NULL);

  /* 静音：页面声音走 helper 的 audio 进程直通扬声器，绕开部件——
     这是嵌入应用唯一的开关。get_muted 隔一个往返才反映 */
  chromium_session_set_muted (session, TRUE);`

</script>

<template>
  <DocLayout :items="apiSections" heading="调用文档" base="api">
    <!-- ============ 最小宿主 ============ -->
    <template v-if="section === 'quickstart'">
      <h1>最小宿主</h1>
      <p>
        接入这个库只需要三个对象：<strong>窗口</strong>、<strong>ChromiumView</strong>（拥有嵌套合成器的部件）、
        <strong>ChromiumSession</strong>（拥有 helper 与页面状态的控制面）。当前版本
        <strong>{{ release.version }}</strong>；库随安装装出头文件与 <code>cefnest.pc</code>，
        宿主一行编译：
      </p>
      <CodeBlock label="bash" lang="bash" :code="`cc host.c -o host \$(pkg-config --cflags --libs cefnest)`" />
      <div class="doc-note">
        从<router-link to="/downloads">下载页</router-link>安装 <code>libcefnest0</code> 与
        <code>libcefnest-dev</code>，或按<router-link to="/dev/build">从源码构建</router-link>
        装进同一个 prefix——两条路都会留下 <code>cefnest.pc</code>，同一行命令即可。渲染页面还需要
        <code>cefnest-helper</code>（安装包已带；源码构建时开 <code>install-helper</code>）。
        下文的示例程序两种路径都适用。
      </div>
      <h2>三个对象，一条硬性顺序</h2>
      <CodeBlock label="examples/minimal.c · 节选" :code="quickCode" lang="c" />
      <div class="doc-note">
        顺序是硬性的：先创建视图并 <code>chromium_session_set_view()</code>，再
        <code>start()</code>——helper 的 <code>WAYLAND_DISPLAY</code> 取自视图的合成器。
        完整源码在 <code>examples/minimal.c</code>，运行：
        <code>CEFNEST_URL=https://cnb.cool/yanzhiming/CEFNest ./builddir/examples/cefnest-minimal</code>
      </div>
      <h2>会话的契约</h2>
      <p>
        页面站在宿主的回答上：一个下载、一次查找、一个右键菜单、一个权限请求，都是
        session 代理给宿主的问题，各有各的回复形状，各有各的不回答的代价。
        中等示例 <code>examples/host.c</code> 把四个信号接成了可脚本化的测试：
      </p>
      <CodeBlock label="examples/host.c · 节选" :code="hostCode" lang="c" />
      <h2>动手前值得知道</h2>
      <ul>
        <li>页面站在宿主的回答上：下载、查找、右键菜单、权限请求都要宿主应答，左侧各节逐一给出回复形状与不回答的代价。</li>
        <li>API 尚不稳定；已知边角行为见<router-link to="/dev/limits">已知限制</router-link>。</li>
      </ul>
    </template>

    <!-- ============ 常用示例 ============ -->
    <template v-else-if="section === 'examples'">
      <h1>常用示例</h1>
      <p>
        第一段是<strong>完整的 GTK4 窗口骨架</strong>——可直接编译运行的小浏览器：
        URL 栏、导航按钮、页面视图、窗口标题随页面更新。其余各节是按任务拆出的
        信号处理器，全都往这副骨架上挂。约定：处理器最后一个参数恒为
        <code>gpointer data</code>；标注 <code>→ gboolean</code> 的两个信号
        （右键菜单、权限）返回 TRUE 表示宿主接管回答，FALSE 交回 session 默认行为。
      </p>

      <h2>完整 GTK4 窗口宿主</h2>
      <CodeBlock label="C · browser.c · 可直接编译" :code="exWindow" lang="c" />

      <h2>页面加载与状态</h2>
      <CodeBlock label="C · 导航 / 后退 / 刷新 / 标题" :code="exLoad" lang="c" />

      <h2>执行 JavaScript 与双向消息</h2>
      <CodeBlock label="C · JS 执行与回执" :code="exJs" lang="c" />
      <CodeBlock label="C · 页面 ↔ 宿主双向消息" :code="exMessage" lang="c" />

      <h2>嵌入既有布局（单页面）</h2>
      <CodeBlock label="C · 视图嵌进 Paned 双栏" :code="exEmbed" lang="c" />

      <h2>多标签管理器</h2>
      <CodeBlock label="C · 以 browser id 为键的标签表" :code="exTabs" lang="c" />

      <h2>多窗口</h2>
      <CodeBlock label="C · 独立 CefWindow 模式" :code="exWindows" lang="c" />

      <h2>弹窗与链接</h2>
      <CodeBlock label="C · popup / open-url / copy-text" :code="exPopup" lang="c" />

      <h2>下载</h2>
      <CodeBlock label="C · 宿主决定落盘路径" :code="exDownload" lang="c" />
      <CodeBlock label="C · 进度条与在途控制" :code="exProgress" lang="c" />

      <h2>页内查找</h2>
      <CodeBlock label="C · 查找 / 步进 / 清除高亮" :code="exFind" lang="c" />

      <h2>右键菜单</h2>
      <CodeBlock label="C · 自绘菜单并回答" :code="exMenu" lang="c" />

      <h2>权限请求</h2>
      <CodeBlock label="C · 按来源放行" :code="exPermission" lang="c" />

      <h2>失败与进程死亡</h2>
      <CodeBlock label="C · 错误页 / 崩溃横幅 / helper 永别" :code="exErrors" lang="c" />

      <h2>缩放与静音</h2>
      <CodeBlock label="C · 用户偏好" :code="exMisc" lang="c" />

      <h2>可观测性</h2>
      <CodeBlock label="C · 帧计数与无障碍" :code="exObs" lang="c" />
    </template>

    <!-- ============ 信号总表 ============ -->
    <template v-else-if="section === 'signals'">
      <h1>信号总表</h1>
      <p>
        ChromiumSession 的全部 24 个信号，按主题排列；参数与语义以
        <code>src/chromium-session.c</code> 的注册为准。
        两个返回 <code>gboolean</code> 的信号（context-menu、permission-request）是
        "宿主回答"型：返回 TRUE 即接管回答义务，FALSE 交回 session 默认行为。
      </p>
      <div class="table-responsive">
        <table class="table table-sm table-hover align-middle">
          <thead>
            <tr><th style="width: 15%">信号</th><th style="width: 40%">参数</th><th>语义</th></tr>
          </thead>
          <tbody>
            <tr v-for="row in signalRows" :key="row.name">
              <td><code class="text-nowrap">::{{ row.name }}</code></td>
              <td><small class="font-monospace text-secondary">{{ row.args }}</small></td>
              <td><small>{{ row.desc }}</small></td>
            </tr>
          </tbody>
        </table>
      </div>
    </template>

    <!-- ============ 数据驱动的普通节 ============ -->
    <template v-else>
      <h1>{{ apiSections.find((s) => s.key === section)?.label }}</h1>
      <p class="text-secondary small">调用契约的完整参考，全部在本站呈现。</p>
      <ApiEntry
        v-for="(e, i) in entries[section]"
        :key="i"
        :kind="e.kind || 'method'"
        :sig="e.sig"
        :desc="e.desc"
      />
      <template v-if="section === 'lifecycle'">
        <div class="doc-note">
          <code>start()</code> 之前用 <code>chromium_session_new(path)</code> 指名 helper，
          或让它按 <code>CEFNEST_HELPER</code> → <code>CHROMIUM_HELPER</code> → 编入的安装路径
          → cmake 开发产物解析。完整的五步规则与失败形态见
          <router-link to="/dev/run">运行与调试</router-link>。
        </div>
      </template>
    </template>
  </DocLayout>
</template>
