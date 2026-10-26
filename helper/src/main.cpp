// CEFNest helper — a windowed Chromium, driven over a socket.
//
// The GTK widget owns the Wayland compositor; this process owns Chromium. It
// creates one frameless CefWindow holding a CefBrowserView, so the page is
// rendered by Chromium into a real Wayland surface — no off-screen rendering,
// no XWayland — and it speaks one JSON object per line on the unix socket the
// host named in CHROMIUM_HELPER_SOCKET:
//
//   host   -> helper   {"cmd":"load","url":"https://example.com"}
//   helper -> host     {"event":"address","url":"https://example.com"}
//   host   -> helper   {"cmd":"js","code":"document.title","id":7}
//   helper -> host     {"event":"js-result","id":7,"ok":true,"value":"Page"}
//
// Commands are parsed off the socket on a reader thread and re-dispatched to
// the CEF UI thread; events are serialised to the socket under a mutex. If the
// host closes the socket the helper quits, so a crashed widget never leaves an
// orphaned Chromium behind — and it quits by closing every browser it holds,
// because a second window is a second browser with its own id.
//
// One newline-delimited JSON object per line is the whole framing, so an event
// that goes out half-written hangs the helper up: the next one would be read as
// the tail of it.
//
// Without CHROMIUM_HELPER_SOCKET this is just the Gate 1 probe: a window that
// loads the URL from argv and quits on Esc.

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_command_line.h"
#include "include/cef_context_menu_handler.h"
#include "include/cef_dialog_handler.h"
#include "include/cef_display_handler.h"
#include "include/cef_download_handler.h"
#include "include/cef_jsdialog_handler.h"
#include "include/cef_download_item.h"
#include "include/cef_cookie.h"
#include "include/cef_find_handler.h"
#include "include/cef_keyboard_handler.h"
#include "include/cef_life_span_handler.h"
#include "include/cef_load_handler.h"
#include "include/cef_parser.h"
#include "include/cef_permission_handler.h"
#include "include/cef_print_handler.h"
#include "include/cef_process_message.h"
#include "include/cef_render_process_handler.h"
#include "include/cef_request_handler.h"
#include "include/cef_scheme.h"
#include "include/cef_stream.h"
#include "include/cef_task.h"
#include "include/cef_v8.h"
#include "include/cef_values.h"
#include "include/cef_version.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_browser_view_delegate.h"
#include "include/views/cef_window.h"
#include "include/views/cef_window_delegate.h"
#include "include/wrapper/cef_helpers.h"
#include "include/wrapper/cef_stream_resource_handler.h"

namespace {

constexpr char kSocketEnv[] = "CHROMIUM_HELPER_SOCKET";

/* The renderer's side of a js command: one function installed on every global,
 * one CefProcessMessage carrying one JSON text back to the browser process. */
constexpr char kReportFunction[] = "__chromiumHelperReport";
constexpr char kJsResultMessage[] = "cefnest-helper-js-result";
constexpr char kPageMessageEvent[] = "page-message";
constexpr char kPageMessageProcess[] = "cefnest-page-message";
constexpr char kPostMessageFunction[] = "cefnestPostMessage";
constexpr char kPrimarySelectionFunction[] = "cefnestPrimarySelection";
constexpr char kPrimarySelectionProcess[] = "cefnest-primary-selection";
constexpr char kPrimarySelectionEvent[] = "primary-selection";

/* The origin every plugin's resources live on. The option set is fixed here
 * and in OnRegisterCustomSchemes — two halves of one rule CEF enforces across
 * every process. */
constexpr char kExtensionScheme[] = "cefnest-extension";
constexpr char kExtensionSchemePrefix[] = "cefnest-extension://";
/* A plugin menu slot maps to one command id, inside CEF's user range and
 * clear of the rows this helper already uses (26501–26508). */
constexpr int kPluginMenuBase = 26600;
constexpr int kPluginMenuMaxSlot = 199;
constexpr char kPluginMenuCommandEvent[] = "plugin-menu-command";

/* The Web frontend's process messages. The table goes browser → renderer
 * (pushed on every change and on browser creation, requested again by the
 * renderer's first context as a safety net for a push that raced a renderer
 * still connecting); a content script's API call goes renderer → browser and
 * the host's answer comes back the other way. */
constexpr char kPluginBridgeFunction[] = "__cefnestPlugin";
constexpr char kPluginsTableProcess[] = "cefnest-plugins-table";
constexpr char kPluginsRequestProcess[] = "cefnest-plugins-request";
constexpr char kPluginMessageProcess[] = "cefnest-plugin-message";
constexpr char kPluginResponseProcess[] = "cefnest-plugin-response";
/* The ceiling §六.5 of the plugin plan fixes for the Web frontend: one
 * payload, either direction. The shim refuses to send past it, and both the
 * browser process (on receipt) and the respond path (on send) re-check — a
 * plugin's oversized message must never take the control channel down. */
constexpr size_t kPluginPayloadMaxBytes = 1u << 20;
/* One content script file's ceiling, read at plugins-load. A script bigger
 * than this is dropped with a plugin-error, not served half of. */
constexpr size_t kPluginScriptMaxBytes = 512u << 10;
/* The table rides the renderer's command line, base64, because a file:
 * navigation can land in a renderer process this side never pushed to —
 * site isolation spawns one per site, and a renderer that meets its first
 * page with an empty table has no document_start left to offer. One argv
 * entry is capped by the kernel at 128 KiB, so the encoded table is capped
 * below that; a table too big for the switch still reaches new renderers
 * through the first-context request, at the cost of that page's
 * document_start. */
constexpr char kPluginsTableSwitch[] = "cefnest-plugins-table";
constexpr size_t kPluginsTableSwitchMax = 96u << 10;

/* Events are written on the UI thread with a 200 ms send timeout, so a script
 * that returns the whole DOM would stall Chromium and then be dropped anyway.
 * Say so instead. */
constexpr size_t kMaxResultBytes = 64 * 1024;

/* The smallest new-blob worth a download-progress event for a file whose total
 * length the server never told. */
constexpr int64_t kProgressByteStep = 256 * 1024;

/* Default-off spellcheck. The user asked for no suggestion menu and no word
 * selection; the command-line switch alone is not enough on every CEF build, so
 * the renderer side also turns the attribute off and keeps new editable nodes
 * covered. CHROMIUM_SPELLCHECK=1 restores the old behavior. */
constexpr char kDisableSpellcheckScript[] =
    R"JS((function(){var fix=function(){var nodes=document.querySelectorAll("input,textarea,[contenteditable]");for(var i=0;i<nodes.length;i++){try{nodes[i].spellcheck=false;}catch(e){}}};fix();if(!window.__cefnestNoSpellcheck){window.__cefnestNoSpellcheck=1;new MutationObserver(fix).observe(document.documentElement,{childList:true,subtree:true});}})();)JS";

/* The page owns the selection; CEF on Wayland binds the primary-selection
 * device but does not create a primary source for a page selection. Push the
 * selected text to the host instead, and let the host own the primary selection
 * that the compositor mirrors back. This is the renderer-side half. */
constexpr char kPrimarySelectionScript[] =
    R"JS((function(){if(window.__cefnestPrimaryHooked)return;window.__cefnestPrimaryHooked=1;var sent=false,last=null;function read(){var a=document.activeElement;if(a&&typeof a.selectionStart==="number"&&typeof a.selectionEnd==="number"&&a.selectionEnd>a.selectionStart){return String(a.value||"").slice(a.selectionStart,a.selectionEnd);}var s=window.getSelection();return s?String(s):"";}function send(){var t=read();if(!sent&&!t)return;if(t===last)return;last=t;sent=true;if(t.length>65536)t=t.slice(0,65536);try{cefnestPrimarySelection(t);}catch(e){}}document.addEventListener("selectionchange",send,true);document.addEventListener("mouseup",send,true);document.addEventListener("keyup",send,true);send();})();)JS";

/* The component-provided chrome.* shim, one instance per plugin (__ID__ is
 * substituted with the plugin id). It rides into every context that gets
 * plugin machinery — a page one of the plugin's content scripts matches, or
 * an extension page of the plugin — and defines window.chrome on top of the
 * __cefnestPlugin bridge. Multiple plugins on one page each install their own
 * shim: the first window.chrome wins and stays (there is no isolated world —
 * see docs/plugins.md), and the page-wide hooks (__cefnestDeliver and
 * friends) are installed by that first shim alone, then fan out across every
 * shim's state — a second plugin installing must not deafen the first's
 * pending calls, listeners and menus.
 *
 * Every call goes to the host as one JSON text and comes back through
 * __cefnestDeliver, which the renderer's process-message handler drives.
 * Payloads past 1 MiB are refused here, before the channel ever sees them. */
const char kPluginShimScript[] =
    R"SHIM((function(){
  var ID = "__ID__";
  var all = window.__cefnestChromeFor = window.__cefnestChromeFor || {};
  if (all[ID]) return;
  all[ID] = 1;
  var shims = window.__cefnestShims = window.__cefnestShims || [];
  var self = {id: ID, seq: 1, pending: {}, menus: {}, listeners: [], action: null};
  shims.push(self);
  function call(api, args) {
    return new Promise(function(resolve, reject) {
      var payload;
      try {
        payload = JSON.stringify({id: ID, reply: self.seq, api: api, args: (args === undefined ? [] : args)});
      } catch (e) { reject(new Error("plugin-payload-not-serialisable")); return; }
      if (payload.length > 1048576) { reject(new Error("plugin-payload-too-large")); return; }
      var reply = self.seq;
      self.seq = self.seq + 1;
      self.pending[reply] = {resolve: resolve, reject: reject};
      try { __cefnestPlugin(payload); }
      catch (e) { delete self.pending[reply]; reject(e); }
    });
  }
  /* The window hooks below belong to the first shim that ran on this page:
     a second plugin's shim must not overwrite them, because the first
     plugin's closures — its pending map, its listeners, its menus — would go
     deaf the moment the second one installed. Each hook fans out across
     every shim on the page instead, so whichever plugin owns the state the
     hook needs is the one it reaches. */
  if (!window.__cefnestDeliver) {
    window.__cefnestDeliver = function(reply, ok, value) {
      for (var s = 0; s < shims.length; s++) {
        var p = shims[s].pending[reply];
        if (!p) continue;
        delete shims[s].pending[reply];
        if (!ok) { p.reject(new Error(value == null ? "plugin call failed" : String(value))); return; }
        var v = null;
        try { v = (value == null) ? null : JSON.parse(value); }
        catch (e) { p.reject(new Error("plugin-payload-not-serialisable")); return; }
        p.resolve(v);
        return;
      }
    };
  }
  if (!window.__cefnestDispatchMenu) {
    window.__cefnestDispatchMenu = function(slot, params) {
      for (var s = 0; s < shims.length; s++) {
        var fn = shims[s].menus[slot];
        if (!fn) continue;
        try {
          fn({menuItemId: slot, pageUrl: params && params.page_url,
              linkUrl: params && params.link_url, srcUrl: params && params.source_url,
              selectionText: params && params.selection_text}, {id: 0, url: params && params.page_url});
        } catch (e) {}
        return;
      }
    };
  }
  if (!window.__cefnestOnMessage) {
    window.__cefnestOnMessage = function(msg) {
      // The host delivers one JSON text; listeners see the parsed value, the
      // shape chrome.runtime.onMessage promises. Anything unparsable goes
      // through as it arrived.
      var v = msg;
      try { v = JSON.parse(msg); } catch (e) {}
      for (var s = 0; s < shims.length; s++) {
        var ls = shims[s].listeners;
        for (var i = 0; i < ls.length; i++) {
          try { ls[i](v, {id: shims[s].id}, function() {}); } catch (e) {}
        }
      }
    };
  }
  if (!window.__cefnestActionClicked) {
    window.__cefnestActionClicked = function() {
      for (var s = 0; s < shims.length; s++) {
        if (shims[s].action) { try { shims[s].action(); } catch (e) {} }
      }
    };
  }
  function area(name) {
    return {
      get: function(keys) { return call("storage." + name + ".get", [keys === undefined ? null : keys]); },
      set: function(items) { return call("storage." + name + ".set", [items]); },
      remove: function(keys) { return call("storage." + name + ".remove", [keys]); },
      clear: function() { return call("storage." + name + ".clear", []); }
    };
  }
  var chrome = {
    runtime: {
      id: ID,
      getURL: function(path) { return "cefnest-extension://" + ID + "/" + String(path).replace(/^\//, ""); },
      sendMessage: function(msg) { return call("runtime.sendMessage", [msg]); },
      onMessage: { addListener: function(fn) { self.listeners.push(fn); } }
    },
    storage: { local: area("local"), session: area("session"), sync: area("sync") },
    contextMenus: {
      create: function(props) {
        return call("contextMenus.create", [{title: props && props.title}]).then(function(slot) {
          if (props && typeof props.onclick === "function") self.menus[slot] = props.onclick;
          return slot;
        });
      },
      remove: function(slot) { delete self.menus[slot]; return call("contextMenus.remove", [slot]); }
    },
    tabs: {
      query: function(q) { return call("tabs.query", [q === undefined ? {} : q]); },
      update: function(id, props) { return call("tabs.update", [id, props === undefined ? {} : props]); }
    },
    action: {
      onClicked: {
        addListener: function(fn) {
          self.action = fn;
          return call("action.onClicked.listen", []);
        }
      }
    }
  };
  try {
    Object.defineProperty(window, "chrome", {value: chrome, writable: false, configurable: false});
  } catch (e) { window.chrome = chrome; }
})();)SHIM";

const char kDefaultUrl[] =
    "data:text/html;charset=utf-8,"
    "%3Cbody%20style%3D%22margin%3A0%3Bbackground%3A%23101828%3B"
    "color%3A%23e8eef8%3Bfont%3A16px%20sans-serif%22%3E"
    "%3Ctitle%3ECEFNest%20helper%3C%2Ftitle%3E"
    "%3Ch1%20style%3D%22padding%3A40px%22%3E"
    "CEF%20helper%20is%20running%3C%2Fh1%3E"
    "%3Cp%20style%3D%22padding%3A0%2040px%22%3E"
    "Start%20this%20process%20with%20CHROMIUM_HELPER_SOCKET%20set%20"
    "to%20let%20the%20GTK%20host%20navigate%20it.%3C%2Fp%3E"
    "%3C%2Fbody%3E";

bool g_quit_requested = false;
long g_auto_quit_ms = 0;
/* Off unless the host says otherwise: with this false nothing in the helper
 * ever calls ShowDevTools, so the bundled frontend stays unreachable. */
bool g_devtools_enabled = false;
/* ShowDevTools calls whose browser CEF has not reported yet. The browser's own
 * URL is not reliable in OnAfterCreated — the initial navigation may not have
 * started — so this is what tells that callback the browser is the inspector. */
int g_devtools_creating = 0;
CefRefPtr<CefBrowser> g_browser;
CefRefPtr<CefWindow> g_window;

/* One helper can own several browsers. A tab and a standalone window differ
 * only in which CefWindow the CefBrowserView is parented to; everything on the
 * control channel is keyed by the browser id CEF assigns. */
struct BrowserSlot {
  int slot = 0;                 /* request-side key, until CEF assigns id */
  bool windowed = false;
  /* CEF created this browser for ShowDevTools. Adoption moves its view into
   * a frameless window of ours, so afterwards it looks like any standalone
   * window; it must never become the active browser. */
  bool devtools = false;
  /* A background page (a plugin manifest's background.service_worker): its
   * view is attached to no window and never shown, so the nested compositor
   * never sees a toplevel for it and no FIFO slot is spent on it. */
  bool background = false;
  std::string title;            /* last OnTitleChange, for event labels */
  CefRefPtr<CefBrowser> browser;
  CefRefPtr<CefBrowserView> view;
  CefRefPtr<CefWindow> window;
  std::string start_url;
};
std::map<int, BrowserSlot> g_browser_slots;  /* keyed by CEF browser id */
std::map<int, int> g_slot_to_browser;        /* pending slot -> assigned id */

int g_next_browser_slot = 1;
int g_active_browser_id = 0;
int g_select_slot = 0;
CefRefPtr<CefWindow> g_tab_window;

int CreateNewBrowser(const std::string& url, bool windowed,
                     bool background = false);
void SelectBrowserById(int id);
void ShowDevToolsFor(CefRefPtr<CefBrowser> browser, const CefPoint& point);

// Close every browser the helper holds. OnBeforeClose only ends the message
// loop once the slot map is empty, so a close aimed at one browser leaves the
// rest running over a channel nobody is answering.
void CloseEveryBrowser();

void QuitOnce() {
  if (g_quit_requested)
    return;
  g_quit_requested = true;
  CefQuitMessageLoop();
}

void CloseEveryBrowser() {
  /* The slots are copied out before anything is closed: CloseBrowser(true) can
   * run OnBeforeClose before it returns, and that erases the very map entry a
   * loop over the slots would still be standing on. */
  struct Closing {
    CefRefPtr<CefBrowser> browser;
    CefRefPtr<CefBrowserView> view;
    CefRefPtr<CefWindow> window;
    bool windowed;
  };
  std::vector<Closing> closing;

  for (auto& entry : g_browser_slots) {
    if (!entry.second.browser || !entry.second.browser->GetHost())
      continue;
    closing.push_back({entry.second.browser, entry.second.view,
                       entry.second.window, entry.second.windowed});
    /* The same rule as a single close-browser: the window and the slot must
     * not be the last owners of the view when CloseBrowser runs, or Alloy
     * treats the close as a window close and never reaches OnBeforeClose. */
    entry.second.view = nullptr;
  }

  if (closing.empty()) {
    QuitOnce();
    return;
  }

  for (Closing& one : closing) {
    if (!one.windowed && one.view && one.window)
      one.window->RemoveChildView(one.view);
    one.view = nullptr;
    one.browser->GetHost()->CloseBrowser(true);
  }
}

std::string EnvOrEmpty(const char* name) {
  const char* value = std::getenv(name);
  return value ? value : "";
}

// The only reader of the sandbox opt-out. "1" is the only value that counts as
// yes: a typo in a security knob should cost the run, not the sandbox.
bool SandboxDisabled() {
  return EnvOrEmpty("CHROMIUM_HELPER_NO_SANDBOX") == "1";
}

std::string PickUrl(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (argv[i][0] != '-')
      return argv[i];
  }
  return kDefaultUrl;
}

// Where an ephemeral profile lives: a tmpfs directory whose contents vanish on
// reboot even when a helper crashes and never removes them. XDG_RUNTIME_DIR is
// the user's own (mode 0700, already required by Wayland); /dev/shm is the
// fallback because it is tmpfs too. Never ~/.cache and never /tmp: the first is
// something a cleaner may delete, the second is often a disk.
std::string RuntimeBaseDir() {
  const std::string runtime = EnvOrEmpty("XDG_RUNTIME_DIR");
  return runtime.empty() ? "/dev/shm" : runtime;
}

// "<base>/cefnest-helper-<pid>-XXXXXX", created by mkdtemp (mode 0700) and
// deliberately identifiable: SweepEphemeralRoots() reads the pid back out of
// the name. Empty means it could not be created, and the caller must not start
// a browser without a profile root.
std::string MakeEphemeralRoot() {
  std::string pattern =
      RuntimeBaseDir() + "/cefnest-helper-" + std::to_string(getpid()) +
      "-XXXXXX";
  if (mkdtemp(pattern.data()) == nullptr) {
    fprintf(stderr, "helper: cannot create an ephemeral profile in %s: %s\n",
            pattern.c_str(), strerror(errno));
    return "";
  }
  return pattern;
}

// A crashed helper cannot remove its own tmpfs directory, so the next one does
// it. Only "cefnest-helper-" directories whose pid is gone (ESRCH) are
// victims: a live pid may be a helper, and this process's own root must never
// be touched. Best effort: a name that does not parse or a directory that will
// not go is left alone.
void SweepEphemeralRoots(const std::string& base) {
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(base, error)) {
    std::error_code kind_error;
    if (!entry.is_directory(kind_error) || kind_error)
      continue;

    const std::string name = entry.path().filename().string();
    constexpr char kPrefix[] = "cefnest-helper-";
    if (name.rfind(kPrefix, 0) != 0)
      continue;

    size_t at = sizeof(kPrefix) - 1;
    pid_t pid = 0;
    const size_t digits = at;
    while (at < name.size() && name[at] >= '0' && name[at] <= '9') {
      if (pid > (INT_MAX - 9) / 10) {
        pid = 0;
        break;
      }
      pid = pid * 10 + (name[at++] - '0');
    }
    if (at == digits || at >= name.size() || name[at] != '-')
      continue;
    if (pid <= 0 || pid == getpid())
      continue;

    // EPERM means the pid exists and belongs to somebody else: alive enough.
    if (kill(pid, 0) == -1 && errno == ESRCH)
      std::filesystem::remove_all(entry.path(), error);
  }
}

// The one authoritative lock on a persistent data directory. It is taken by
// the helper and held for the life of the process: the kernel drops it on
// exit, crash included, so there is no stale lock to clean up and no pid file
// to go out of date. The descriptor is intentionally never closed.
bool LockDataDirectory(const std::string& dir) {
  static int lock_fd = -1;
  if (lock_fd >= 0)
    return true;

  const std::string path = dir + "/.cefnest.lock";
  lock_fd = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
  if (lock_fd < 0) {
    fprintf(stderr, "helper: cannot open %s: %s\n", path.c_str(),
            strerror(errno));
    return false;
  }
  if (flock(lock_fd, LOCK_EX | LOCK_NB) == -1) {
    close(lock_fd);
    lock_fd = -1;
    return false;
  }
  return true;
}

void RemoveTree(const std::string& path) {
  std::error_code error;
  std::filesystem::remove_all(path, error);
  if (error)
    fprintf(stderr, "helper: could not remove %s: %s\n", path.c_str(),
            error.message().c_str());
}

class Control;

Control* g_control = nullptr;

void HandleCommand(const std::string& line);

// Tasks hop the reader thread's lines onto the CEF UI thread, where every
// browser object may be touched.
class CommandTask : public CefTask {
 public:
  explicit CommandTask(std::string line) : line_(std::move(line)) {}

  void Execute() override { HandleCommand(line_); }

  IMPLEMENT_REFCOUNTING(CommandTask);

 private:
  std::string line_;
};

class HostGoneTask : public CefTask {
 public:
  void Execute() override {
    fprintf(stderr, "helper: control channel closed\n");
    CloseEveryBrowser();
  }

  IMPLEMENT_REFCOUNTING(HostGoneTask);
};

class CloseTask : public CefTask {
 public:
  explicit CloseTask(CefRefPtr<CefBrowser> browser) : browser_(browser) {}

  void Execute() override {
    if (browser_ && browser_->GetHost())
      browser_->GetHost()->CloseBrowser(true);
  }

  IMPLEMENT_REFCOUNTING(CloseTask);

 private:
  CefRefPtr<CefBrowser> browser_;
};

// SIGTERM is how `systemctl stop`, a desktop session logout and a plain `kill`
// end this process. Left at its default action it stops us inside
// CefRunMessageLoop, so neither Control::Shutdown() nor CefShutdown() runs and
// the profile keeps a SingletonLock nobody will ever release.
//
// The handler cannot do the work and cannot even post the task: CefPostTask
// takes locks, so it only drops one byte into a non-blocking pipe the watcher
// below is sitting on. The byte is the signal number, and writing a single byte
// is async-signal-safe.
sig_atomic_t g_signal_wakeup_fd = -1;

extern "C" void OnShutdownSignal(int number) {
  const int saved_errno = errno;
  const unsigned char byte = static_cast<unsigned char>(number);
  while (write(g_signal_wakeup_fd, &byte, 1) < 0 && errno == EINTR) {
  }
  errno = saved_errno;
}

class SignalTask : public CefTask {
 public:
  explicit SignalTask(int number) : number_(number) {}

  void Execute() override {
    fprintf(stderr, "helper: caught signal %d\n", number_);
    /* Closing the browsers is what ends the loop: OnBeforeClose quits once the
     * last slot is gone, and the tail of main() then shuts CEF down. QuitOnce()
     * here would cut that short and leave browsers running over a channel
     * nobody answers. */
    CloseEveryBrowser();
  }

  IMPLEMENT_REFCOUNTING(SignalTask);

 private:
  int number_;
};

// The self-pipe and the thread that hops its bytes onto the UI thread. Stop()
// has to run before CefShutdown(), for the reason Control::Shutdown() gives for
// joining its reader: a task posted after that lands on a CEF that is already
// going away.
class SignalWatcher {
 public:
  SignalWatcher() = default;
  ~SignalWatcher() { Stop(); }

  void Start() {
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) {
      fprintf(stderr, "helper: cannot install signal handlers: pipe2: %s\n",
              std::strerror(errno));
      return;
    }
    read_fd_ = fds[0];
    write_fd_ = fds[1];

    /* Non-blocking on the write end only, and that asymmetry is the design: a
     * full pipe must never block a signal handler, while the watcher has to
     * wait in read() for the next signal instead of coming back with EAGAIN
     * and quitting its loop on the first empty read. */
    const int write_flags = fcntl(write_fd_, F_GETFL);
    if (write_flags < 0 || fcntl(write_fd_, F_SETFL, write_flags | O_NONBLOCK) < 0) {
      fprintf(stderr, "helper: cannot install signal handlers: fcntl: %s\n",
              std::strerror(errno));
      close(read_fd_);
      close(write_fd_);
      read_fd_ = write_fd_ = -1;
      return;
    }

    struct sigaction action;
    std::memset(&action, 0, sizeof(action));
    action.sa_handler = OnShutdownSignal;
    action.sa_flags = SA_RESTART;
    sigemptyset(&action.sa_mask);
    for (int number : {SIGINT, SIGTERM, SIGHUP}) {
      if (sigaction(number, &action, nullptr) != 0) {
        fprintf(stderr, "helper: cannot install signal handlers: sigaction(%d): %s\n",
                number, std::strerror(errno));
        close(read_fd_);
        close(write_fd_);
        read_fd_ = write_fd_ = -1;
        return;
      }
    }

    g_signal_wakeup_fd = write_fd_;
    thread_ = std::thread([this] {
      unsigned char byte = 0;
      bool announced = false;
      for (;;) {
        ssize_t read_back = read(read_fd_, &byte, 1);
        if (read_back == 0)
          break;  // Stop() closed the write end
        if (read_back < 0) {
          if (errno == EINTR)
            continue;
          break;
        }
        if (announced)
          continue;  // drain, but the first request is the one that counts
        announced = true;
        CefPostTask(TID_UI, new SignalTask(static_cast<int>(byte)));
      }
    });
  }

  void Stop() {
    if (write_fd_ >= 0) {
      close(write_fd_);
      write_fd_ = -1;
    }
    if (thread_.joinable())
      thread_.join();
    if (read_fd_ >= 0) {
      close(read_fd_);
      read_fd_ = -1;
    }
    g_signal_wakeup_fd = -1;
  }

 private:
  int read_fd_ = -1;
  int write_fd_ = -1;
  std::thread thread_;
};

// The host side of the control channel: a connected SOCK_STREAM unix socket we
// were handed the path of, plus the thread that turns its bytes into lines.
class Control {
 public:
  Control() = default;
  Control(const Control&) = delete;
  Control& operator=(const Control&) = delete;
  ~Control() { Shutdown(); }

  bool Connect(const std::string& path) {
    sockaddr_un address;
    std::memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    if (path.empty() || path.size() + 1 > sizeof(address.sun_path)) {
      fprintf(stderr, "helper: socket path too long (%zu bytes)\n", path.size());
      return false;
    }
    std::memcpy(address.sun_path, path.c_str(), path.size());

    fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd_ < 0) {
      fprintf(stderr, "helper: socket() failed: %s\n", strerror(errno));
      return false;
    }
    if (::connect(fd_, reinterpret_cast<sockaddr*>(&address),
                  sizeof(address)) != 0) {
      fprintf(stderr, "helper: connect(%s) failed: %s\n", path.c_str(),
              strerror(errno));
      ::close(fd_);
      fd_ = -1;
      return false;
    }

    // Writes happen on the UI thread. A host that stops reading must never be
    // able to freeze Chromium, so a full send buffer costs 200 ms and the
    // event is dropped.
    const timeval timeout = {0, 200000};
    ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    fprintf(stderr, "helper: connected to %s\n", path.c_str());
    reader_ = std::thread([this] { ReadLoop(); });
    return true;
  }

  void Send(const std::string& line) {
    bool torn = false;
    {
      std::lock_guard<std::mutex> lock(write_mutex_);
      if (fd_ < 0)
        return;

      size_t sent = 0;
      while (sent < line.size()) {
        const ssize_t written =
            ::send(fd_, line.data() + sent, line.size() - sent, MSG_NOSIGNAL);
        if (written > 0) {
          sent += static_cast<size_t>(written);
          continue;
        }
        if (written < 0 && errno == EINTR)
          continue;
        if (sent == 0)
          fprintf(stderr, "helper: control write failed: %s\n", strerror(errno));
        else
          torn = true;
        break;
      }
    }

    /* A part of an event is already on the wire and cannot be unsent, so every
     * event after it would be read as the tail of that one. The framing of this
     * socket is gone: say so by hanging up, which is the shape both ends
     * already treat as the other one going away. Done outside the lock,
     * because Shutdown() takes it. */
    if (torn) {
      fprintf(stderr, "helper: an event went out half-written, hanging up\n");
      Shutdown();
    }
  }

  // Ends the reader thread. Must run before CefShutdown(): a task posted after
  // the message loop is gone would be a use-after-free.
  void Shutdown() {
    int fd = -1;
    {
      std::lock_guard<std::mutex> lock(write_mutex_);
      std::swap(fd, fd_);
    }
    if (fd >= 0) {
      ::shutdown(fd, SHUT_RDWR);
    }
    if (reader_.joinable())
      reader_.join();
    /* Only now: a descriptor closed while the reader is still awake is a
     * number the next open() in this process gets, and the reader would be
     * reading whatever that turned out to be. */
    if (fd >= 0)
      ::close(fd);
  }

 private:
  void ReadLoop() {
    /* Taken once, then never looked at again: fd_ belongs to Shutdown() from
     * here on, and a descriptor this thread keeps using but never closes is a
     * number another thread could be handed back as its own. */
    const int fd = fd_;
    std::string buffer;
    char chunk[4096];

    for (;;) {
      const ssize_t n = ::read(fd, chunk, sizeof(chunk));
      if (n > 0) {
        // Leftover bytes hold no newline — every complete line was taken before
        // the last read — so only what this chunk added needs scanning, and the
        // consumed lines are cut out by index with the tail moved once per
        // chunk. erase() per line memmoves everything after it at every
        // line-sized step of a long command.
        std::string::size_type scan = buffer.size();
        std::string::size_type head = 0;
        std::string::size_type newline;
        buffer.append(chunk, static_cast<size_t>(n));
        while ((newline = buffer.find('\n', scan)) != std::string::npos) {
          std::string line = buffer.substr(head, newline - head);
          head = newline + 1;
          scan = head;
          if (!line.empty() && line.back() == '\r')
            line.pop_back();
          if (!line.empty())
            CefPostTask(TID_UI, new CommandTask(std::move(line)));
        }
        if (head > 0)
          buffer.erase(0, head);
        if (buffer.size() > (1U << 20)) {
          fprintf(stderr, "helper: command line exceeded 1 MiB, discarding\n");
          buffer.clear();
        }
        continue;
      }
      if (n < 0 && errno == EINTR)
        continue;
      break;  // EOF or error: the host is gone.
    }

    CefPostTask(TID_UI, new HostGoneTask());
  }

  int fd_ = -1;
  std::mutex write_mutex_;
  std::thread reader_;
};

void Emit(CefRefPtr<CefDictionaryValue> dict) {
  if (!g_control)
    return;
  CefRefPtr<CefValue> root = CefValue::Create();
  root->SetDictionary(dict);
  const CefString json = CefWriteJSON(root, JSON_WRITER_DEFAULT);
  if (json.empty())
    return;
  std::string line = json.ToString();
  line.push_back('\n');
  g_control->Send(line);
}

CefRefPtr<CefDictionaryValue> NewEvent(const char* name) {
  CefRefPtr<CefDictionaryValue> dict = CefDictionaryValue::Create();
  dict->SetString("event", name);
  if (g_active_browser_id != 0)
    dict->SetInt("browser", g_active_browser_id);
  return dict;
}

void EmitStatus(const std::string& message) {
  CefRefPtr<CefDictionaryValue> dict = NewEvent("status");
  dict->SetString("message", message);
  Emit(dict);
}

static BrowserSlot* FindSlotForBrowser(CefRefPtr<CefBrowser> browser) {
  if (!browser)
    return nullptr;
  auto it = g_browser_slots.find(browser->GetIdentifier());
  return it == g_browser_slots.end() ? nullptr : &it->second;
}

/* ------------------------------------------------------------------ *
 * View Source: streaming one frame's HTML to the host                *
 *                                                                    *
 * The library's default for the menu id is CefFrame::ViewSource():   *
 * the DOM serialized into a temp file, then the desktop's text       *
 * viewer — the wrong shape for a component, and a temp file nobody   *
 * asked for. Instead the source is fetched with GetSource() and      *
 * streamed over the control channel: the session's line limit is    *
 * 4 MiB and a page's source passes it easily once JSON-escaped, so   *
 * the one CefString becomes begin / chunk* / end, 64 KiB to a        *
 * chunk and cut on UTF-8 boundaries, 16 MiB the hard cap.            *
 * What arrives is the DOM as GetSource() serializes it — script      *
 * changes included — not the network's original bytes; the host      *
 * documents that.                                                    *
 * ------------------------------------------------------------------ */
constexpr size_t kSourceChunkBytes = 64 * 1024;
constexpr size_t kSourceMaxBytes = 16 * 1024 * 1024;
int g_next_source_id = 1;

class SourceStream : public CefStringVisitor {
 public:
  SourceStream(int id, int browser_id, const std::string& url,
               const std::string& title)
      : id_(id), browser_id_(browser_id), url_(url), title_(title) {}

  void Visit(const CefString& source) override {
    CEF_REQUIRE_UI_THREAD();
    const std::string text = source.ToString();

    if (text.empty()) {
      CefRefPtr<CefDictionaryValue> dict = NewEvent("view-source-error");
      dict->SetInt("browser", browser_id_);
      dict->SetInt("id", id_);
      dict->SetString("message", "the frame has no source");
      Emit(dict);
      return;
    }

    const size_t total = std::min(text.size(), kSourceMaxBytes);
    const bool truncated = text.size() > kSourceMaxBytes;

    CefRefPtr<CefDictionaryValue> begin = NewEvent("view-source-begin");
    begin->SetInt("browser", browser_id_);
    begin->SetInt("id", id_);
    begin->SetString("url", url_);
    begin->SetString("title", title_);
    begin->SetInt("bytes", static_cast<int>(total));
    Emit(begin);

    for (size_t at = 0; at < total; ) {
      size_t take = std::min(kSourceChunkBytes, total - at);
      /* Never split a UTF-8 sequence: a leading byte is never 10xxxxxx, so
       * stepping the cut back while it lands on a continuation byte keeps
       * every chunk whole characters. take can only reach 0 on malformed
       * input, where one byte at a time still terminates. */
      while (take > 0 && at + take < total &&
             (static_cast<unsigned char>(text[at + take]) & 0xC0) == 0x80)
        take--;
      CefRefPtr<CefDictionaryValue> chunk = NewEvent("view-source-chunk");
      chunk->SetInt("browser", browser_id_);
      chunk->SetInt("id", id_);
      chunk->SetString("data", text.substr(at, take));
      Emit(chunk);
      at += take > 0 ? take : 1;
    }

    CefRefPtr<CefDictionaryValue> end = NewEvent("view-source-end");
    end->SetInt("browser", browser_id_);
    end->SetInt("id", id_);
    end->SetBool("truncated", truncated);
    Emit(end);
  }

 private:
  int id_;
  int browser_id_;
  std::string url_;
  std::string title_;

  IMPLEMENT_REFCOUNTING(SourceStream);
};

/* The menu id and the control command share this one path. Reports the
 * outcome itself (a gone frame becomes a view-source-error), so callers
 * only decide what "nothing to stream" means for them. */
static void
EmitFrameSource(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame) {
  if (!frame || !frame->IsValid()) {
    CefRefPtr<CefDictionaryValue> dict = NewEvent("view-source-error");
    dict->SetInt("browser", browser ? browser->GetIdentifier() : 0);
    dict->SetInt("id", g_next_source_id++);
    dict->SetString("message", "the frame is gone");
    Emit(dict);
    return;
  }

  const BrowserSlot* slot = FindSlotForBrowser(browser);
  const std::string title = slot ? slot->title : std::string();

  frame->GetSource(new SourceStream(g_next_source_id++,
                                    browser->GetIdentifier(),
                                    frame->GetURL().ToString(), title));
}

/* ------------------------------------------------------------------ *
 * Cookies, history, page text and PDF                                *
 * ------------------------------------------------------------------ *
 *
 * These four are the "host asks, component answers" batch: no policy and no
 * UI. Cookies are read from and written to the browser's own request context,
 * so a persistent data directory makes them persistent and an incognito
 * profile keeps them for the session. The PDF bytes leave CEF as a file (its
 * PrintToPDF API has no byte-returning form), so the helper prints into its
 * own ephemeral root, reads the file back, streams it as base64 and removes
 * it — the host never sees a path and never inherits a temp file. */

constexpr size_t kTextChunkBytes = 64 * 1024;
constexpr size_t kPageTextMaxBytes = 4 * 1024 * 1024;
constexpr size_t kPdfChunkBytes = 48 * 1024;
constexpr size_t kPdfMaxBytes = 16 * 1024 * 1024;
int g_next_pdf_id = 1;

/* CEF's wall clock is microseconds since the Windows epoch (1601); cookies
 * speak Unix seconds on the web and on the channel. */
constexpr int64_t kWindowsEpochSeconds = 11644473600LL;

class FnTask : public CefTask {
 public:
  explicit FnTask(std::function<void()> fn) : fn_(std::move(fn)) {}
  void Execute() override { fn_(); }

  IMPLEMENT_REFCOUNTING(FnTask);

 private:
  std::function<void()> fn_;
};

void PostDelayedUi(std::function<void()> fn, int64_t ms) {
  CefPostDelayedTask(TID_UI, new FnTask(std::move(fn)), ms);
}

CefRefPtr<CefDictionaryValue> CookieToEntry(const CefCookie& cookie) {
  CefRefPtr<CefDictionaryValue> entry = CefDictionaryValue::Create();
  entry->SetString("name", CefString(&cookie.name));
  entry->SetString("value", CefString(&cookie.value));
  entry->SetString("domain", CefString(&cookie.domain));
  entry->SetString("path", CefString(&cookie.path));
  entry->SetDouble("expires",
                   cookie.has_expires
                       ? static_cast<double>(cookie.expires.val / 1000000 -
                                             kWindowsEpochSeconds)
                       : 0.0);
  entry->SetBool("secure", cookie.secure ? true : false);
  entry->SetBool("httponly", cookie.httponly ? true : false);
  return entry;
}

void EmitCookiesError(int id, const std::string& message) {
  CefRefPtr<CefDictionaryValue> dict = NewEvent("cookies-result");
  dict->SetInt("id", id);
  dict->SetBool("ok", false);
  dict->SetString("error", message);
  Emit(dict);
}

/* Visit has no completion callback in CEF: it is called once per cookie and
 * never at all when the store holds nothing matching. The delayed answer is
 * the "no cookies" path, long enough for a real visit to have run and short
 * enough that a host does not sit on a spinner. A later Visit that finds the
 * query already answered only stops the walk. */
class CookieCollector : public CefCookieVisitor {
 public:
  explicit CookieCollector(int id)
      : id_(id), cookies_(CefListValue::Create()) {}

  bool Visit(const CefCookie& cookie,
             int count,
             int total,
             bool& delete_cookie) override {
    CEF_REQUIRE_UI_THREAD();
    if (answered_)
      return false;
    cookies_->SetDictionary(cookies_->GetSize(), CookieToEntry(cookie));
    if (total <= 0 || count + 1 >= total)
      Answer();
    return !answered_;
  }

  void AnswerIfSilent() {
    CEF_REQUIRE_UI_THREAD();
    if (!answered_)
      Answer();
  }

 private:
  void Answer() {
    if (answered_)
      return;
    answered_ = true;
    /* Read the size before SetList: a dictionary that takes ownership of a
     * value invalidates the reference it was handed, so counting afterwards
     * reads a moved-from list and reports 0. */
    const int count = static_cast<int>(cookies_->GetSize());
    CefRefPtr<CefDictionaryValue> dict = NewEvent("cookies-result");
    dict->SetInt("id", id_);
    dict->SetBool("ok", true);
    dict->SetList("cookies", cookies_);
    dict->SetInt("count", count);
    Emit(dict);
  }

  int id_;
  bool answered_ = false;
  CefRefPtr<CefListValue> cookies_;

  IMPLEMENT_REFCOUNTING(CookieCollector);
};

/* One SetCookie callback per cookie, all sharing the tally: the result is one
 * event after the last answer, the way the command's single id promises. A
 * rejected call (bad URL, disallowed characters) counts as a failed name and
 * never produces a callback, so the command loop settles it itself. */
struct CookieSetState {
  int id = 0;
  int total = 0;
  int done = 0;
  CefRefPtr<CefListValue> failed = CefListValue::Create();
};

void EmitCookieSetResult(const std::shared_ptr<CookieSetState>& state) {
  CefRefPtr<CefDictionaryValue> dict = NewEvent("cookies-result");
  dict->SetInt("id", state->id);
  dict->SetInt("count", state->total - static_cast<int>(state->failed->GetSize()));
  dict->SetBool("ok", state->failed->GetSize() == 0);
  dict->SetList("failed", state->failed);
  Emit(dict);
}

void NoteCookieSetDone(const std::shared_ptr<CookieSetState>& state,
                       const std::string& name, bool success) {
  CEF_REQUIRE_UI_THREAD();
  if (!success)
    state->failed->SetString(state->failed->GetSize(), name);
  if (++state->done == state->total)
    EmitCookieSetResult(state);
}

class CookieSetCallback : public CefSetCookieCallback {
 public:
  CookieSetCallback(std::shared_ptr<CookieSetState> state, std::string name)
      : state_(std::move(state)), name_(std::move(name)) {}

  void OnComplete(bool success) override {
    NoteCookieSetDone(state_, name_, success);
  }

 private:
  std::shared_ptr<CookieSetState> state_;
  std::string name_;

  IMPLEMENT_REFCOUNTING(CookieSetCallback);
};

class CookieDeleteCallback : public CefDeleteCookiesCallback {
 public:
  explicit CookieDeleteCallback(int id) : id_(id) {}

  void OnComplete(int num_deleted) override {
    CEF_REQUIRE_UI_THREAD();
    CefRefPtr<CefDictionaryValue> dict = NewEvent("cookies-result");
    dict->SetInt("id", id_);
    dict->SetBool("ok", true);
    dict->SetInt("deleted", num_deleted);
    Emit(dict);
  }

 private:
  int id_;

  IMPLEMENT_REFCOUNTING(CookieDeleteCallback);
};

bool EntryToCookie(CefRefPtr<CefDictionaryValue> entry,
                   CefCookie* cookie,
                   std::string* error) {
  const std::string name =
      entry->HasKey("name") ? entry->GetString("name").ToString() : std::string();
  if (name.empty()) {
    *error = "cookie-needs-name";
    return false;
  }
  CefString(&cookie->name).FromString(name);
  CefString(&cookie->value).FromString(
      entry->HasKey("value") ? entry->GetString("value").ToString()
                             : std::string());
  if (entry->HasKey("domain"))
    CefString(&cookie->domain).FromString(entry->GetString("domain").ToString());
  if (entry->HasKey("path"))
    CefString(&cookie->path).FromString(entry->GetString("path").ToString());
  cookie->secure = entry->HasKey("secure") && entry->GetBool("secure");
  cookie->httponly = entry->HasKey("httponly") && entry->GetBool("httponly");
  if (entry->HasKey("expires") &&
      entry->GetType("expires") == VTYPE_DOUBLE) {
    const double expires = entry->GetDouble("expires");
    if (expires > 0) {
      cookie->has_expires = 1;
      cookie->expires.val = static_cast<int64_t>(expires) * 1000000 +
                            kWindowsEpochSeconds * 1000000;
    }
  }
  return true;
}

class HistoryVisitor : public CefNavigationEntryVisitor {
 public:
  explicit HistoryVisitor(int id) : id_(id), entries_(CefListValue::Create()) {}

  bool Visit(CefRefPtr<CefNavigationEntry> entry,
             bool current,
             int index,
             int total) override {
    CEF_REQUIRE_UI_THREAD();
    if (answered_)
      return false;
    if (entry && entry->IsValid()) {
      CefRefPtr<CefDictionaryValue> one = CefDictionaryValue::Create();
      one->SetInt("index", index);
      one->SetString("url", entry->GetURL());
      one->SetString("title", entry->GetTitle());
      one->SetBool("current", current);
      entries_->SetDictionary(entries_->GetSize(), one);
      if (current)
        current_index_ = index;
    }
    if (total <= 0 || index + 1 >= total)
      Answer();
    return !answered_;
  }

  void AnswerIfSilent() {
    CEF_REQUIRE_UI_THREAD();
    if (!answered_)
      Answer();
  }

 private:
  void Answer() {
    if (answered_)
      return;
    answered_ = true;
    CefRefPtr<CefDictionaryValue> dict = NewEvent("history-result");
    dict->SetInt("id", id_);
    dict->SetInt("current_index", current_index_);
    dict->SetList("entries", entries_);
    Emit(dict);
  }

  int id_;
  int current_index_ = -1;
  bool answered_ = false;
  CefRefPtr<CefListValue> entries_;

  IMPLEMENT_REFCOUNTING(HistoryVisitor);
};

class PageTextStream : public CefStringVisitor {
 public:
  PageTextStream(int id, int browser_id) : id_(id), browser_id_(browser_id) {}

  void Visit(const CefString& text) override {
    CEF_REQUIRE_UI_THREAD();
    const std::string all = text.ToString();
    const size_t total = std::min(all.size(), kPageTextMaxBytes);
    const bool truncated = all.size() > kPageTextMaxBytes;

    CefRefPtr<CefDictionaryValue> begin = NewEvent("page-text-begin");
    begin->SetInt("id", id_);
    begin->SetInt("browser", browser_id_);
    begin->SetInt("bytes", static_cast<int>(total));
    begin->SetBool("truncated", truncated);
    Emit(begin);

    for (size_t at = 0; at < total;) {
      size_t take = std::min(kTextChunkBytes, total - at);
      while (take > 0 && at + take < total &&
             (static_cast<unsigned char>(all[at + take]) & 0xC0) == 0x80)
        take--;
      CefRefPtr<CefDictionaryValue> chunk = NewEvent("page-text-chunk");
      chunk->SetInt("id", id_);
      chunk->SetString("data", all.substr(at, take));
      Emit(chunk);
      at += take > 0 ? take : 1;
    }

    CefRefPtr<CefDictionaryValue> end = NewEvent("page-text-end");
    end->SetInt("id", id_);
    end->SetBool("truncated", truncated);
    Emit(end);
  }

 private:
  int id_;
  int browser_id_;

  IMPLEMENT_REFCOUNTING(PageTextStream);
};

/* PrintToPDF has no byte form: the callback names the file CEF wrote. The
 * file is this helper's own, under the ephemeral root (tmpfs when the host
 * has one), read once and removed before the answer goes out. */
class PdfPrintStream : public CefPdfPrintCallback {
 public:
  PdfPrintStream(int id, std::string path, size_t max_bytes)
      : id_(id), path_(std::move(path)), max_bytes_(max_bytes) {}

  void OnPdfPrintFinished(const CefString& path, bool ok) override {
    CEF_REQUIRE_UI_THREAD();
    std::string bytes;
    if (ok) {
      std::FILE* file = std::fopen(path_.c_str(), "rb");
      if (file) {
        char buffer[64 * 1024];
        size_t got;
        while ((got = std::fread(buffer, 1, sizeof buffer, file)) > 0)
          bytes.append(buffer, got);
        std::fclose(file);
      }
    }
    std::error_code error;
    std::filesystem::remove(path_, error);

    CefRefPtr<CefDictionaryValue> end = NewEvent("pdf-end");
    end->SetInt("id", id_);
    if (!ok || bytes.empty()) {
      end->SetInt("bytes", 0);
      end->SetBool("truncated", false);
      end->SetString("error", ok ? "empty-pdf" : "print-failed");
      Emit(end);
      return;
    }

    const size_t total = std::min(bytes.size(), max_bytes_);
    const bool truncated = bytes.size() > max_bytes_;

    CefRefPtr<CefDictionaryValue> begin = NewEvent("pdf-begin");
    begin->SetInt("id", id_);
    begin->SetInt("bytes", static_cast<int>(total));
    begin->SetBool("truncated", truncated);
    Emit(begin);

    for (size_t at = 0; at < total;) {
      const size_t take = std::min(kPdfChunkBytes, total - at);
      CefRefPtr<CefDictionaryValue> chunk = NewEvent("pdf-chunk");
      chunk->SetInt("id", id_);
      chunk->SetString("data", CefBase64Encode(bytes.data() + at, take));
      Emit(chunk);
      at += take;
    }

    end->SetInt("bytes", static_cast<int>(total));
    end->SetBool("truncated", truncated);
    Emit(end);
  }

 private:
  int id_;
  std::string path_;
  size_t max_bytes_;

  IMPLEMENT_REFCOUNTING(PdfPrintStream);
};

void SelectBrowserById(int id) {
  auto it = g_browser_slots.find(id);
  if (it == g_browser_slots.end() || !it->second.browser)
    {
      EmitStatus("no-such-browser");
      return;
    }

  BrowserSlot* slot = &it->second;
  /* A DevTools browser, however it is hosted, must not become the active
   * browser: that would point every command and every state event at the
   * inspector page, so a focus echo from its window is ignored. The same
   * holds for a background page, which has no window at all. */
  if (slot->devtools || slot->background)
    return;
  if (slot->browser == g_browser)
    return;   /* already active: a focus echo must not re-enter through
                 the RequestFocus below */

  BrowserSlot* old = FindSlotForBrowser(g_browser);
  if (old && old->view && old->window == slot->window)
    old->view->SetVisible(false);   /* tabs share one window, so only one of
                                       them can show; a browser in its own
                                       window stays visible when another
                                       window's browser activates */

  g_browser = slot->browser;
  g_window = slot->window;
  g_active_browser_id = id;
  if (slot->view)
    {
      slot->view->SetVisible(true);
      if (slot->window)
        slot->window->Layout();
      slot->view->RequestFocus();
    }

  CefRefPtr<CefDictionaryValue> event = NewEvent("browser-selected");
  event->SetInt("browser", id);
  event->SetBool("windowed", slot->windowed);
  Emit(event);
}

/* ------------------------------------------------------------------ *
 * Downloads                                                          *
 * ------------------------------------------------------------------ *
 *
 * A download is a decision the host has to make: nothing reaches the disk
 * until it says where. OnBeforeDownload parks the gate callback here and asks;
 * the answer comes back as download-accept (Continue) or download-reject.
 * Only CEF's own item ids are used as keys, and nothing but scalars and the
 * two callbacks outlives a handler call — CefDownloadItem must not be held
 * past the method it was handed to. */

struct Download {
  CefRefPtr<CefBeforeDownloadCallback> gate;    // spent once the host has answered
  CefRefPtr<CefDownloadItemCallback> item;      // the documented way to cancel
  bool asked = false;   // OnBeforeDownload has run, so the host knows this id
  int percent = -1;
  int64_t reported_bytes = -1;
  bool finished = false;
};

std::map<uint32_t, Download> g_downloads;  // UI thread only, like every CEF call here

void EmitDownloadProgress(CefRefPtr<CefDownloadItem> item) {
  CefRefPtr<CefDictionaryValue> dict = NewEvent("download-progress");
  dict->SetInt("id", static_cast<int>(item->GetId()));
  dict->SetDouble("received_bytes", static_cast<double>(item->GetReceivedBytes()));
  dict->SetDouble("total_bytes", static_cast<double>(item->GetTotalBytes()));
  dict->SetInt("percent", item->GetPercentComplete());
  dict->SetDouble("speed", static_cast<double>(item->GetCurrentSpeed()));
  Emit(dict);
}

void EmitDownloadEnd(CefRefPtr<CefDownloadItem> item) {
  const char* state = item->IsComplete() ? "complete"
                    : item->IsCanceled() ? "cancelled"
                                         : "interrupted";
  CefRefPtr<CefDictionaryValue> dict = NewEvent("download-end");
  dict->SetInt("id", static_cast<int>(item->GetId()));
  dict->SetString("state", state);
  dict->SetString("path", item->GetFullPath().ToString());
  dict->SetDouble("received_bytes", static_cast<double>(item->GetReceivedBytes()));
  Emit(dict);
}

class HelperDownloadHandler : public CefDownloadHandler {
 public:
  // Answering here would be a second policy the host cannot see; the gate with
  // a path is OnBeforeDownload, and everything the host needs to decide is in
  // that one event.
  bool CanDownload(CefRefPtr<CefBrowser> browser,
                   const CefString& url,
                   const CefString& request_method) override {
    return true;
  }

  bool OnBeforeDownload(CefRefPtr<CefBrowser> browser,
                        CefRefPtr<CefDownloadItem> download_item,
                        const CefString& suggested_name,
                        CefRefPtr<CefBeforeDownloadCallback> callback) override {
    CEF_REQUIRE_UI_THREAD();

    Download& entry = g_downloads[download_item->GetId()];
    entry.gate = callback;
    entry.asked = true;

    CefRefPtr<CefDictionaryValue> dict = NewEvent("download-begin");
    dict->SetInt("id", static_cast<int>(download_item->GetId()));
    dict->SetString("url", download_item->GetURL().ToString());
    dict->SetString("suggested_name", suggested_name.ToString());
    dict->SetString("mime_type", download_item->GetMimeType().ToString());
    dict->SetDouble("total_bytes",
                    static_cast<double>(download_item->GetTotalBytes()));
    Emit(dict);

    // True means this helper owns the answer. CEF cancels the download if the
    // callback is destroyed without Continue, so a host that never replies
    // leaves the file unwritten rather than guessing a path for it.
    return true;
  }

  void OnDownloadUpdated(CefRefPtr<CefBrowser> browser,
                         CefRefPtr<CefDownloadItem> download_item,
                         CefRefPtr<CefDownloadItemCallback> callback) override {
    CEF_REQUIRE_UI_THREAD();

    Download& entry = g_downloads[download_item->GetId()];
    entry.item = callback;

    /* Measured against the real browser: Chromium ticks this before
     * OnBeforeDownload as well — a 0 % update for an id the host has never been
     * told about. The item callback is worth keeping because it is how a
     * download gets cancelled, but there is nothing to report yet. */
    if (!entry.asked)
      return;

    if (download_item->IsInProgress()) {
      /* CEF calls this on every tick of its own clock, and the channel writes on
       * the UI thread with a 200 ms send timeout: reporting each whole percent
       * is the throttle. A download whose length the server never told has
       * percent stuck at -1, so the byte count has to carry it as well. */
      const int percent = download_item->GetPercentComplete();
      const int64_t received = download_item->GetReceivedBytes();
      if (percent != entry.percent ||
          received - entry.reported_bytes >= kProgressByteStep) {
        entry.percent = percent;
        entry.reported_bytes = received;
        EmitDownloadProgress(download_item);
      }
      return;
    }

    if (entry.finished)
      return;
    entry.finished = true;
    EmitDownloadEnd(download_item);
    g_downloads.erase(download_item->GetId());
  }

  IMPLEMENT_REFCOUNTING(HelperDownloadHandler);
};

/* ------------------------------------------------------------------ *
 * Find                                                               *
 * ------------------------------------------------------------------ *
 *
 * In-page search is one request and more than one answer when it finds
 * something: measured on real Chromium, a request with matches sends a counting
 * update and then the same numbers marked final, and a request with no matches
 * sends only the final one. The pattern travels with every request, so the
 * helper holds no find state at all — a step is the same text with next=true,
 * and nothing here can go stale the way a parked download callback can. */

class HelperFindHandler : public CefFindHandler {
 public:
  void OnFindResult(CefRefPtr<CefBrowser> browser,
                    int identifier,
                    int count,
                    const CefRect& selectionRect,
                    int activeMatchOrdinal,
                    bool finalUpdate) override {
    CEF_REQUIRE_UI_THREAD();
    CefRefPtr<CefDictionaryValue> dict = NewEvent("find-result");
    // Measured: the identifier goes up for every request, a step as much as a
    // new search, and both updates about one request carry the same number. So
    // it groups the replies a request can produce, not the searches a page has
    // run.
    dict->SetInt("identifier", identifier);
    dict->SetInt("count", count);
    dict->SetInt("active_ordinal", activeMatchOrdinal);
    dict->SetBool("final", finalUpdate);
    // selectionRect is deliberately not on the wire: the browser scrolls the
    // match it selects into view on its own, and a host that cannot draw inside
    // the page has no use for a window coordinate. Measured, a new search does
    // not move the view at all.
    Emit(dict);
  }

  IMPLEMENT_REFCOUNTING(HelperFindHandler);
};

/* ------------------------------------------------------------------ *
 * Context menu                                                       *
 * ------------------------------------------------------------------ *
 *
 * The menu Chromium draws for itself is a widget of its own style, in a surface
 * of its own, and the embedding app knows nothing about it. RunContextMenu is
 * CEF's documented way out: the model stays Cef's — every item it offers still
 * works, because Continue() hands the id back to the library's own command
 * handling — and only the drawing moves to the host.
 *
 * What goes over the wire is the model and the point, nothing else. The
 * CefContextMenuParams (link url, media state, edit flags) are deliberately
 * left in this process: the model is already the answer to them, and a host
 * that draws the model needs no second copy of the question. */

// One menu at a time, like CEF's own. The callback owns the whole request: when
// it goes out of scope unanswered the menu is treated as dismissed, which is the
// documented behaviour and the safe one for a host that stops replying.
CefRefPtr<CefRunContextMenuCallback> g_menu_callback;

/* Chromium selects the misspelled word before it hands the menu over. The
 * selection is useful to its suggestion commands but wrong after the menu is
 * gone: the next Ctrl+V replaces the word instead of inserting at the caret.
 * Keep the frame and collapse to the selection end after the answer. */
CefRefPtr<CefFrame> g_menu_restore_frame;
bool g_menu_restore_caret = false;
unsigned int g_menu_generation = 0;

void CollapseCaretInFrame(CefRefPtr<CefFrame> frame) {
  if (!frame || !frame->IsValid())
    return;

  // Direct script text, not eval: the same CSP-safe shape as js-expr. For an
  // input/textarea collapse its own range; for contenteditable collapse the
  // document selection. The word is deliberately left at the caret, not
  // selected.
  frame->ExecuteJavaScript(
      R"JS((function(){try{var e=document.activeElement;if(!e)return;var done=false;if(typeof e.setSelectionRange==="function"){try{var p=e.selectionEnd;e.setSelectionRange(p,p);done=true;}catch(x){}}if(!done){var s=window.getSelection();if(s&&s.rangeCount){var r=s.getRangeAt(0);r.collapse(false);s.removeAllRanges();s.addRange(r);}}}catch(e){}})();)JS",
      "cefnest-helper", 0);
  fprintf(stderr, "helper: collapsed the spellcheck word selection after the menu\n");
}

void RestoreCaretAfterSpellcheck() {
  if (!g_menu_restore_caret)
    return;

  CefRefPtr<CefFrame> frame = g_menu_restore_frame;
  g_menu_restore_caret = false;
  g_menu_restore_frame = nullptr;
  CollapseCaretInFrame(frame);
}

class RestoreCaretTask : public CefTask {
 public:
  RestoreCaretTask(CefRefPtr<CefFrame> frame, unsigned int generation)
      : frame_(frame), generation_(generation) {}

  void Execute() override {
    if (generation_ != g_menu_generation)
      return;
    CollapseCaretInFrame(frame_);
  }

  IMPLEMENT_REFCOUNTING(RestoreCaretTask);

 private:
  CefRefPtr<CefFrame> frame_;
  unsigned int generation_;
};

/* Post, don't run inline: a default command such as a spelling suggestion may
 * be queued by Continue() after this callback returns. Clearing the selection in
 * the same task could make CEF's replacement see no word. */
void ScheduleCaretRestoreAfterSpellcheck() {
  if (!g_menu_restore_caret)
    return;

  CefRefPtr<CefFrame> frame = g_menu_restore_frame;
  g_menu_restore_caret = false;
  g_menu_restore_frame = nullptr;
  if (!frame || !frame->IsValid())
    return;
  CefPostTask(TID_UI, new RestoreCaretTask(frame, g_menu_generation));
}

/* ------------------------------------------------------------------ *
 * Plugins: the scheme, the menu rows, the script tables               *
 *                                                                     *
 * Everything here is browser-process state fed by four control        *
 * commands: plugins-load (the whole truth about one plugin: its root  *
 * for the extension scheme, its content scripts for later),           *
 * plugins-unload, and the two menu verbs. The scheme factory reads    *
 * the table per request from the IO thread while HandleCommand        *
 * writes it on the UI thread, so the root lookup takes a mutex —      *
 * the one shared mutable thing on this path.                          *
 *                                                                     *
 * Content-script injection itself is not in this build yet: the       *
 * scripts arrive and are stored, and the renderer half needs its own  *
 * measured mechanism (browser → renderer table delivery before the    *
 * first context) before it can claim document_start. The plan puts    *
 * that with the Web frontend.                                         *
 * ------------------------------------------------------------------ */

// Chrome match-pattern subset: "<scheme>://<host>/<path>" with "*" wildcards,
// or "<all_urls>". Enough for a content script's matches and a menu row's
// "when"; the syntax and its limits are documented in docs/control-channel.md.
struct MatchPattern {
  bool all = false;
  bool any_scheme = false;
  std::string scheme;
  bool any_host = false;
  std::string host_suffix;   // "*.example.com" — matches example.com too
  std::string host_exact;
  std::string path;

  static MatchPattern Parse(const std::string& text) {
    MatchPattern pattern;
    if (text == "<all_urls>") {
      pattern.all = true;
      return pattern;
    }
    const std::string::size_type scheme_end = text.find("://");
    if (scheme_end == std::string::npos)
      return MatchPattern();
    std::string scheme = text.substr(0, scheme_end);
    std::string rest = text.substr(scheme_end + 3);
    const std::string::size_type path_start = rest.find('/');
    std::string host =
        path_start == std::string::npos ? rest : rest.substr(0, path_start);
    pattern.path = path_start == std::string::npos
                       ? std::string("/")
                       : rest.substr(path_start);
    std::transform(scheme.begin(), scheme.end(), scheme.begin(), ::tolower);
    if (scheme == "*")
      pattern.any_scheme = true;
    else if (!scheme.empty())
      pattern.scheme = scheme;
    else
      return MatchPattern();

    std::transform(host.begin(), host.end(), host.begin(), ::tolower);
    if (host == "*") {
      pattern.any_host = true;
    } else if (host.rfind("*.", 0) == 0) {
      pattern.host_suffix = host.substr(1);  // ".example.com"
      if (pattern.host_suffix.size() < 2)
        return MatchPattern();
    } else if (!host.empty()) {
      pattern.host_exact = host;
    } else {
      return MatchPattern();
    }
    return pattern;
  }

  bool Usable() const {
    return all || ((any_scheme || !scheme.empty()) &&
                   (any_host || !host_exact.empty() || !host_suffix.empty()));
  }

  bool Matches(const std::string& url) const {
    if (all)
      return true;

    const std::string::size_type scheme_end = url.find("://");
    if (scheme_end == std::string::npos)
      return false;
    std::string scheme = url.substr(0, scheme_end);
    std::string rest = url.substr(scheme_end + 3);
    const std::string::size_type path_start = rest.find('/');
    const std::string host =
        path_start == std::string::npos ? rest : rest.substr(0, path_start);
    std::string path = path_start == std::string::npos
                                 ? std::string("/")
                                 : rest.substr(path_start);
    if (path.empty())
      path = "/";

    std::transform(scheme.begin(), scheme.end(), scheme.begin(), ::tolower);
    if (!any_scheme && scheme != this->scheme)
      return false;

    std::string host_lower = host;
    std::transform(host_lower.begin(), host_lower.end(), host_lower.begin(),
                   ::tolower);
    if (!any_host) {
      if (!host_exact.empty()) {
        if (host_lower != host_exact)
          return false;
      } else if (!host_suffix.empty()) {
        // "*.example.com" matches the domain itself and any subdomain.
        if (!(host_lower == host_suffix.substr(1) ||
              (host_lower.size() > host_suffix.size() &&
               host_lower.compare(host_lower.size() - host_suffix.size(),
                                  host_suffix.size(), host_suffix) == 0)))
          return false;
      } else {
        return false;
      }
    }

    return WildcardMatch(path, this->path);
  }

  // '*' matches any run of characters, including nothing. Paths are short and
  // patterns are tiny, so the O(len*pattern) table costs nothing.
  static bool WildcardMatch(const std::string& text,
                            const std::string& pattern) {
    const size_t n = text.size(), m = pattern.size();
    std::vector<bool> row(m + 1, false), next(m + 1, false);
    row[0] = true;
    for (size_t j = 1; j <= m; ++j)
      row[j] = row[j - 1] && pattern[j - 1] == '*';
    for (size_t i = 1; i <= n; ++i) {
      next[0] = false;
      for (size_t j = 1; j <= m; ++j) {
        if (pattern[j - 1] == '*')
          next[j] = next[j - 1] || row[j];
        else
          next[j] = row[j - 1] && pattern[j - 1] == text[i - 1];
      }
      std::swap(row, next);
    }
    return row[m];
  }
};

struct PluginScript {
  std::vector<MatchPattern> matches;
  std::vector<std::string> js;       // file paths, as the manifest wrote them
  std::vector<std::string> js_text;  // contents, read at plugins-load for the
                                     // browser-side document_idle injection;
                                     // the renderer fetches its own copies
                                     // through the extension scheme, so the
                                     // table that travels stays path-sized
  std::string run_at;
};

struct PluginEntry {
  std::string root;
  std::vector<PluginScript> scripts;
  /* The manifest's background.service_worker path, relative to the root. The
   * background page itself is this factory's synthesized wrapper around it. */
  std::string background;
};

struct UserMenuItem {
  std::string label;
  bool has_when = false;
  MatchPattern when;
};

/* Both maps are written on the UI thread (HandleCommand) and the roots are
 * read per request from the factory's thread. */
std::mutex g_plugin_mutex;
std::map<std::string, PluginEntry> g_plugin_table;
std::map<int, UserMenuItem> g_user_menu_items;

std::string PluginRootFor(const std::string& id) {
  std::lock_guard<std::mutex> lock(g_plugin_mutex);
  auto it = g_plugin_table.find(id);
  return it == g_plugin_table.end() ? std::string() : it->second.root;
}

std::string PluginBackgroundFor(const std::string& id) {
  std::lock_guard<std::mutex> lock(g_plugin_mutex);
  auto it = g_plugin_table.find(id);
  return it == g_plugin_table.end() ? std::string() : it->second.background;
}

// §2.8's plugin-error event: a script file this side could not read. The row
// itself stays loaded — its pages still serve, its other scripts still run.
void EmitPluginError(const std::string& id, const std::string& message) {
  CefRefPtr<CefDictionaryValue> dict = NewEvent("plugin-error");
  dict->SetString("id", id);
  dict->SetString("message", message);
  Emit(dict);
}

// The same inside-the-root rule the extension scheme enforces per request,
// applied once at load to the files a manifest names. A URL layer is not to
// be trusted on its way in; a manifest path is, but both ends of the rule
// live here so neither depends on the other.
bool PluginPathInside(const std::string& root, const std::string& relative,
                      std::string* out) {
  std::error_code error;
  const std::filesystem::path wanted =
      std::filesystem::weakly_canonical(root + "/" + relative, error);
  const std::filesystem::path base =
      std::filesystem::weakly_canonical(root, error);
  if (error)
    return false;
  const std::string wanted_text = wanted.string();
  const std::string base_text = base.string() + "/";
  if (wanted_text.compare(0, base_text.size(), base_text) != 0)
    return false;
  *out = wanted_text;
  return true;
}

// Reads one content script. Returns false with a reason when the file is
// missing, escapes the root, or is past the per-file ceiling — the script is
// dropped and the reason goes to the host as a plugin-error.
bool ReadScriptFile(const std::string& id, const std::string& root,
                    const std::string& relative, std::string* out) {
  std::string path;
  if (!PluginPathInside(root, relative, &path)) {
    EmitPluginError(id, "script-path-escape:" + relative);
    return false;
  }
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error)) {
    EmitPluginError(id, "script-missing:" + relative);
    return false;
  }
  const uintmax_t bytes = std::filesystem::file_size(path, error);
  if (error || bytes > kPluginScriptMaxBytes) {
    EmitPluginError(id, "script-too-large:" + relative);
    return false;
  }
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) {
    EmitPluginError(id, "script-read-failed:" + relative);
    return false;
  }
  out->resize(static_cast<size_t>(bytes));
  const size_t got = fread(out->data(), 1, out->size(), f);
  fclose(f);
  if (got != out->size()) {
    out->clear();
    EmitPluginError(id, "script-read-failed:" + relative);
    return false;
  }
  return true;
}

// The renderer's copy of the table: no roots (the scheme is browser-side
// only), script texts instead of paths.
CefRefPtr<CefValue> BuildRendererTable() {
  CefRefPtr<CefListValue> plugins = CefListValue::Create();
  size_t n = 0;
  std::lock_guard<std::mutex> lock(g_plugin_mutex);
  for (const auto& entry : g_plugin_table) {
    CefRefPtr<CefDictionaryValue> plugin = CefDictionaryValue::Create();
    plugin->SetString("id", entry.first);
    CefRefPtr<CefListValue> scripts = CefListValue::Create();
    size_t m = 0;
    for (const auto& script : entry.second.scripts) {
      CefRefPtr<CefDictionaryValue> one = CefDictionaryValue::Create();
      CefRefPtr<CefListValue> matches = CefListValue::Create();
      // Patterns travel as text and the renderer re-parses them, so the two
      // ends cannot drift apart on what "*" meant.
      size_t k = 0;
      for (const auto& pattern : script.matches) {
        std::string text = pattern.all ? "<all_urls>" : "";
        if (!pattern.all) {
          text = (pattern.any_scheme ? "*" : pattern.scheme) + std::string("://") +
                 (pattern.any_host ? "*"
                                   : !pattern.host_suffix.empty()
                                       ? "*" + pattern.host_suffix
                                       : pattern.host_exact) +
                 pattern.path;
        }
        matches->SetString(k++, text);
      }
      one->SetList("matches", matches);
      CefRefPtr<CefListValue> files = CefListValue::Create();
      size_t j = 0;
      for (const auto& path : script.js)
        files->SetString(j++, path);
      one->SetList("js", files);
      one->SetString("run_at", script.run_at);
      scripts->SetDictionary(m++, one);
    }
    plugin->SetList("scripts", scripts);
    plugins->SetDictionary(n++, plugin);
  }
  CefRefPtr<CefDictionaryValue> root = CefDictionaryValue::Create();
  root->SetList("plugins", plugins);
  CefRefPtr<CefValue> value = CefValue::Create();
  value->SetDictionary(root);
  return value;
}

// One table push to one renderer. Called on the UI thread: browser creation,
// plugins-load, and the renderer's own request all land there.
void PushPluginsTable(CefRefPtr<CefBrowser> browser) {
  if (!browser || !browser->GetHost())
    return;
  CefRefPtr<CefFrame> frame = browser->GetMainFrame();
  if (!frame)
    return;
  CefRefPtr<CefValue> table = BuildRendererTable();
  CefRefPtr<CefProcessMessage> message =
      CefProcessMessage::Create(kPluginsTableProcess);
  const std::string json =
      CefWriteJSON(table, JSON_WRITER_DEFAULT).ToString();
  message->GetArgumentList()->SetString(0, json);
  // Frame-scoped on purpose (CEF 156 moved SendProcessMessage to CefFrame):
  // the table is process-wide state on the renderer side, and the main frame
  // is the one this process reliably has.
  frame->SendProcessMessage(PID_RENDERER, message);
}

void PushPluginsTableToAll() {
  for (auto& entry : g_browser_slots)
    PushPluginsTable(entry.second.browser);
}

// The renderer's own request: answer through the frame that asked.
void PushPluginsTableToBrowserFrame(CefRefPtr<CefBrowser> browser,
                                    CefRefPtr<CefFrame> frame) {
  if (!browser || !frame || !frame->IsValid())
    return;
  CefRefPtr<CefValue> table = BuildRendererTable();
  CefRefPtr<CefProcessMessage> message =
      CefProcessMessage::Create(kPluginsTableProcess);
  message->GetArgumentList()->SetString(
      0, CefWriteJSON(table, JSON_WRITER_DEFAULT).ToString());
  frame->SendProcessMessage(PID_RENDERER, message);
}

// A refusal has to answer with a status the page can read, so it is its own
// handler: CefStreamResourceHandler over a zero-length CefStreamReader crashes
// the browser process (measured — the 404 for a missing <img> took the helper
// out with SIGSEGV, with the socket closing under it and no core to read).
class ExtensionErrorResponse : public CefResourceHandler {
 public:
  ExtensionErrorResponse(int status, std::string reason)
      : status_(status), reason_(std::move(reason)),
        body_(reason_) {}

  bool Open(CefRefPtr<CefRequest> request,
            bool& handle_request,
            CefRefPtr<CefCallback> callback) override {
    handle_request = true;
    return true;
  }

  void GetResponseHeaders(CefRefPtr<CefResponse> response,
                          int64_t& response_length,
                          CefString& redirectUrl) override {
    response->SetStatus(status_);
    response->SetStatusText(reason_);
    response->SetMimeType("text/plain");
    CefResponse::HeaderMap headers;
    headers.emplace("Access-Control-Allow-Origin", "*");
    headers.emplace("Cache-Control", "no-store");
    response->SetHeaderMap(headers);
    response_length = static_cast<int64_t>(body_.size());
  }

  bool Read(void* data, int bytes_to_read, int& bytes_read,
            CefRefPtr<CefResourceReadCallback> callback) override {
    bytes_read = 0;
    if (offset_ < body_.size()) {
      const size_t wanted =
          std::min(static_cast<size_t>(bytes_to_read), body_.size() - offset_);
      memcpy(data, body_.data() + offset_, wanted);
      offset_ += wanted;
      bytes_read = static_cast<int>(wanted);
    }
    return bytes_read > 0;
  }

  void Cancel() override {}

 private:
  const int status_;
  const std::string reason_;
  const std::string body_;
  size_t offset_ = 0;

  IMPLEMENT_REFCOUNTING(ExtensionErrorResponse);
};

// The synthesized background page: status 200 with a body built in memory.
// The bytes live in the handler itself, so nothing handed to the resource
// loader outlives its owner.
class ExtensionDataResponse : public CefResourceHandler {
 public:
  ExtensionDataResponse(std::string mime, std::string body)
      : mime_(std::move(mime)), body_(std::move(body)) {}

  bool Open(CefRefPtr<CefRequest> request,
            bool& handle_request,
            CefRefPtr<CefCallback> callback) override {
    handle_request = true;
    return true;
  }

  void GetResponseHeaders(CefRefPtr<CefResponse> response,
                          int64_t& response_length,
                          CefString& redirectUrl) override {
    response->SetStatus(200);
    response->SetStatusText("OK");
    response->SetMimeType(mime_);
    CefResponse::HeaderMap headers;
    headers.emplace("Access-Control-Allow-Origin", "*");
    headers.emplace("Cache-Control", "no-store");
    response->SetHeaderMap(headers);
    response_length = static_cast<int64_t>(body_.size());
  }

  bool Read(void* data, int bytes_to_read, int& bytes_read,
            CefRefPtr<CefResourceReadCallback> callback) override {
    bytes_read = 0;
    if (offset_ < body_.size()) {
      const size_t wanted =
          std::min(static_cast<size_t>(bytes_to_read), body_.size() - offset_);
      memcpy(data, body_.data() + offset_, wanted);
      offset_ += wanted;
      bytes_read = static_cast<int>(wanted);
    }
    return bytes_read > 0;
  }

  void Cancel() override {}

 private:
  const std::string mime_;
  const std::string body_;
  size_t offset_ = 0;

  IMPLEMENT_REFCOUNTING(ExtensionDataResponse);
};

// Serves cefnest-extension://<id>/<path> out of the root plugins-load
// reported for <id>, and nothing else. The prefix check runs on every
// request, because the URL layer cannot be trusted to have done it: a literal
// ../ is normalised away before anything here sees it, while ..%2F arrives
// still holding the dots and has to be rejected by this side. Both failures
// answer with a real body — see ExtensionErrorResponse.
class ExtensionFactory : public CefSchemeHandlerFactory {
 public:
  CefRefPtr<CefResourceHandler> Create(CefRefPtr<CefBrowser> browser,
                                       CefRefPtr<CefFrame> frame,
                                       const CefString& scheme_name,
                                       CefRefPtr<CefRequest> request) override {
    const std::string url = request->GetURL().ToString();
    if (url.rfind(kExtensionSchemePrefix, 0) != 0) {
      fprintf(stderr, "helper: extension bad-url url=%s\n", url.c_str());
      return Refuse(400, "bad-url");
    }

    std::string rest = url.substr(sizeof(kExtensionSchemePrefix) - 1);
    const std::string::size_type slash = rest.find('/');
    std::string id = slash == std::string::npos ? rest : rest.substr(0, slash);
    std::string leaf =
        slash == std::string::npos ? std::string() : rest.substr(slash + 1);
    // The query is the page's cache buster, never part of the file name.
    const std::string::size_type cut = leaf.find_first_of("?#");
    if (cut != std::string::npos)
      leaf = leaf.substr(0, cut);
    // Decoded with path separators unescaped on purpose — %2f%2e%2e%2f is the
    // form that survives URL normalisation and still reaches "../", so this
    // is the case the canonical-prefix check below exists for.
    const cef_uri_unescape_rule_t rule =
        static_cast<cef_uri_unescape_rule_t>(UU_NORMAL | UU_PATH_SEPARATORS);
    leaf = CefURIDecode(leaf, false, rule).ToString();
    id = CefURIDecode(id, false, rule).ToString();

    const std::string root = PluginRootFor(id);
    if (root.empty()) {
      fprintf(stderr, "helper: extension unknown-plugin status=404 url=%s\n",
              url.c_str());
      return Refuse(404, "no-such-plugin");
    }

    /* The background page is synthesized, not a file: a manifest's
     * background.service_worker names a script, and a script alone would
     * render as text. The wrapper loads it as a page script, and everything
     * else about it (the shim, the runtime message route) is the ordinary
     * extension-page machinery. The name is reserved: a plugin shipping a
     * real file of this name would lose to the wrapper. */
    if (leaf == "__cefnest_background__.html") {
      const std::string background = PluginBackgroundFor(id);
      if (background.empty()) {
        fprintf(stderr,
                "helper: extension no-background status=404 url=%s\n",
                url.c_str());
        return Refuse(404, "no-background-page");
      }
      // The path came from the manifest and passed the inside-the-root check
      // at load, but it travels here inside an attribute, so quotes in it
      // still get escaped rather than trusted.
      std::string escaped;
      escaped.reserve(background.size());
      for (char c : background) {
        switch (c) {
          case '"': escaped += "&quot;"; break;
          case '<': escaped += "&lt;"; break;
          case '&': escaped += "&amp;"; break;
          default: escaped += c;
        }
      }
      const std::string body =
          "<!doctype html><html><head><meta charset=\"utf-8\">"
          "<title>cefnest background</title></head><body>"
          "<script src=\"" + escaped + "\"></script>"
          "</body></html>";
      fprintf(stderr,
              "helper: extension served background status=200 url=%s\n",
              url.c_str());
      return new ExtensionDataResponse("text/html", body);
    }

    std::error_code error;
    // The root plugins-load reports is the plugin's own directory, so the
    // leaf hangs directly under it. (The spike's root was the parent of all
    // plugins; the real one is per plugin, and the id only picks the root.)
    const std::filesystem::path wanted =
        std::filesystem::weakly_canonical(root + "/" + leaf, error);
    const std::filesystem::path base =
        std::filesystem::weakly_canonical(root, error);
    if (error) {
      fprintf(stderr, "helper: extension canonical-failed url=%s: %s\n",
              url.c_str(), error.message().c_str());
      return Refuse(500, "canonical-failed");
    }
    const std::string wanted_text = wanted.string();
    const std::string base_text = base.string() + "/";
    if (wanted_text.compare(0, base_text.size(), base_text) != 0) {
      fprintf(stderr,
              "helper: extension-path-escape status=403 url=%s path=%s\n",
              url.c_str(), wanted_text.c_str());
      EmitStatus("extension-path-escape");
      return Refuse(403, "outside-root");
    }

    if (!std::filesystem::is_regular_file(wanted, error)) {
      fprintf(stderr, "helper: extension missing status=404 url=%s path=%s\n",
              url.c_str(), wanted_text.c_str());
      return Refuse(404, "no-such-file");
    }

    CefRefPtr<CefStreamReader> stream =
        CefStreamReader::CreateForFile(wanted_text);
    if (!stream) {
      fprintf(stderr, "helper: extension open-failed status=500 url=%s\n",
              url.c_str());
      return Refuse(500, "open-failed");
    }
    fprintf(stderr, "helper: extension served status=200 url=%s\n", url.c_str());
    CefResponse::HeaderMap headers;
    headers.emplace("Access-Control-Allow-Origin", "*");
    headers.emplace("Cache-Control", "no-store");
    return new CefStreamResourceHandler(200, "OK",
                                        ExtensionMimeType(wanted_text),
                                        headers, stream);
  }

 private:
  static CefRefPtr<CefResourceHandler> Refuse(int status, const char* reason) {
    return new ExtensionErrorResponse(status, reason);
  }

  static const char* ExtensionMimeType(const std::string& path) {
    if (path.ends_with(".png")) return "image/png";
    if (path.ends_with(".svg")) return "image/svg+xml";
    if (path.ends_with(".js")) return "application/javascript";
    if (path.ends_with(".html")) return "text/html";
    if (path.ends_with(".css")) return "text/css";
    if (path.ends_with(".json")) return "application/json";
    return "text/plain";
  }

  IMPLEMENT_REFCOUNTING(ExtensionFactory);
};

const char* MenuItemTypeName(cef_menu_item_type_t type) {
  switch (type) {
    case MENUITEMTYPE_COMMAND:
      return "command";
    case MENUITEMTYPE_CHECK:
      return "check";
    case MENUITEMTYPE_RADIO:
      return "radio";
    case MENUITEMTYPE_SEPARATOR:
      return "separator";
    case MENUITEMTYPE_SUBMENU:
      return "submenu";
    default:
      return "none";
  }
}

/* CEF's Alloy menu only knows the generic cef_menu_id_t commands. These ids
 * are ours; the host draws the rows and sends the id back, and the command is
 * executed here while CefContextMenuParams still describes the clicked node. */
enum {
  kMenuOpenLinkWindow = MENU_ID_USER_FIRST + 1,
  kMenuCopyLinkAddress,
  kMenuOpenImageWindow,
  kMenuCopyImageAddress,
  kMenuSaveImage,
  kMenuOpenMediaWindow,
  kMenuCopyMediaAddress,
  kMenuInspect,
};

class HelperContextMenuHandler : public CefContextMenuHandler {
 public:
  bool RunContextMenu(CefRefPtr<CefBrowser> browser,
                      CefRefPtr<CefFrame> frame,
                      CefRefPtr<CefContextMenuParams> params,
                      CefRefPtr<CefMenuModel> model,
                      CefRefPtr<CefRunContextMenuCallback> callback) override {
    CEF_REQUIRE_UI_THREAD();

    // The inspector's own page keeps CEF's default menu: the host's menu is
    // built for the page's model, and this browser is never the active one.
    if (BrowserSlot* slot = FindSlotForBrowser(browser))
      if (slot->devtools)
        return false;

    // A menu that was still pending is replaced by this one; put the old
    // selection back before looking at the new params.
    RestoreCaretAfterSpellcheck();
    ++g_menu_generation;

    // Whatever the host left unanswered is over before this one starts; the ref
    // going away is how CEF is told so.
    g_menu_callback = callback;

    g_menu_restore_caret =
        params->IsEditable() && !params->GetMisspelledWord().empty();
    g_menu_restore_frame = frame;
    if (g_menu_restore_caret)
      fprintf(stderr, "helper: spellcheck menu will collapse the word selection\n");

    // Alloy's default model has no link/image/media actions. Add the browser
    // rows the node actually supports, and let OnContextMenuCommand execute
    // them while |params| is still valid.
    const uint32_t flags = params->GetTypeFlags();
    const bool is_link = (flags & CM_TYPEFLAG_LINK) != 0;
    const bool is_media = (flags & CM_TYPEFLAG_MEDIA) != 0;
    const cef_context_menu_media_type_t media = params->GetMediaType();
    const bool is_image = is_media && media == CM_MEDIATYPE_IMAGE;
    const bool is_video = is_media && (media == CM_MEDIATYPE_VIDEO ||
                                       media == CM_MEDIATYPE_AUDIO);

    int at = 0;
    if (is_link) {
      model->InsertItemAt(at++, kMenuOpenLinkWindow, "在新窗口打开链接");
      model->InsertItemAt(at++, kMenuCopyLinkAddress, "复制链接地址");
      model->InsertSeparatorAt(at++);
    }
    if (is_image) {
      model->InsertItemAt(at++, kMenuOpenImageWindow, "在新窗口打开图片");
      model->InsertItemAt(at++, kMenuCopyImageAddress, "复制图片地址");
      model->InsertItemAt(at++, kMenuSaveImage, "图片另存为…");
      model->InsertSeparatorAt(at++);
    } else if (is_video) {
      model->InsertItemAt(at++, kMenuOpenMediaWindow, "在新窗口打开媒体");
      model->InsertItemAt(at++, kMenuCopyMediaAddress, "复制媒体地址");
      model->InsertSeparatorAt(at++);
    }
    /* The host's own row goes last, the way Chrome puts "检查" at the bottom of
     * its menu. A DevTools browser gets no inspect row: that would open DevTools
     * on DevTools. */
    if (g_devtools_enabled) {
      BrowserSlot* slot = FindSlotForBrowser(browser);
      if (!slot || !slot->devtools) {
        model->AddSeparator();
        model->AddItem(kMenuInspect, "检查");
      }
    }

    /* Plugin rows come after everything else, which is where the plan puts
     * them ("插进末尾"). A row whose "when" pattern does not match this
     * frame's URL stays hidden; the frame URL, not the request's params, is
     * what a pattern is about — the menu is for the page. */
    if (!g_user_menu_items.empty()) {
      const std::string page_url =
          frame ? frame->GetURL().ToString() : std::string();
      bool added = false;
      for (const auto& entry : g_user_menu_items) {
        if (entry.second.has_when && !entry.second.when.Matches(page_url))
          continue;
        if (!added) {
          model->AddSeparator();
          added = true;
        }
        model->AddItem(kPluginMenuBase + entry.first, entry.second.label);
      }
    }

    CefRefPtr<CefListValue> items = CefListValue::Create();
    int written = 0;
    bool skip_separator = false;
    for (size_t i = 0; i < model->GetCount(); ++i) {
      if (!model->IsVisibleAt(i))
        continue;

      const int command_id = model->GetCommandIdAt(i);
      // A link/image context menu is about that node, not about where the
      // page's history happens to be; the browser menu we are matching does
      // not put Back/Forward above its link/image actions.
      if ((is_link || is_media) &&
          (command_id == MENU_ID_BACK || command_id == MENU_ID_FORWARD ||
           command_id == MENU_ID_PRINT || command_id == MENU_ID_VIEW_SOURCE)) {
        skip_separator = true;
        continue;
      }
      if (command_id == MENU_ID_ADD_TO_DICTIONARY) {
        // The dictionary row is a browser-profile feature this embedded host
        // does not expose. Drop it rather than render a dead entry.
        skip_separator = true;
        continue;
      }
      if (skip_separator && model->GetTypeAt(i) == MENUITEMTYPE_SEPARATOR) {
        skip_separator = false;
        continue;
      }
      skip_separator = false;

      std::string label = model->GetLabelAt(i).ToString();
      if (command_id == MENU_ID_NO_SPELLING_SUGGESTIONS) {
        // The CEF resource spells this one with a leading & in some locales;
        // it is not a shortcut and must not reach the GTK menu as a literal.
        label.erase(std::remove(label.begin(), label.end(), '&'), label.end());
      }

      CefRefPtr<CefDictionaryValue> item = CefDictionaryValue::Create();
      item->SetInt("id", command_id);
      item->SetString("label", label);
      item->SetString("type", MenuItemTypeName(model->GetTypeAt(i)));
      item->SetBool("enabled", model->IsEnabledAt(i));
      item->SetBool("checked", model->IsCheckedAt(i));
      items->SetDictionary(written++, item);
    }

    CefRefPtr<CefDictionaryValue> dict = NewEvent("context-menu");
    // The coordinates are the click's own, relative to the render view — which
    // for this widget is the surface the host already feeds the pointer to.
    dict->SetInt("x", params->GetXCoord());
    dict->SetInt("y", params->GetYCoord());
    dict->SetList("items", items);
    Emit(dict);

    fprintf(stderr, "helper: context menu with %d items at %d,%d\n", written,
            params->GetXCoord(), params->GetYCoord());

    // True: this helper will not draw anything and the host owes the answer.
    return true;
  }

  bool OnContextMenuCommand(CefRefPtr<CefBrowser> browser,
                            CefRefPtr<CefFrame> frame,
                            CefRefPtr<CefContextMenuParams> params,
                            int command_id,
                            EventFlags event_flags) override {
    CEF_REQUIRE_UI_THREAD();

    /* The inspector's default menu is CEF's; the ids are CEF's too, and the
     * stock ones (View source, Save image) must not be read as this helper's
     * page commands. A background page has no surface to right-click, so the
     * guard is only defensive there. */
    if (BrowserSlot* slot = FindSlotForBrowser(browser))
      if (slot->devtools || slot->background)
        return false;

    // Plugin rows are their own id space and their own route: the host
    // decides what the pick means, and the params describe the node the menu
    // was opened on — the same facts a copy-text row would act on.
    if (command_id >= kPluginMenuBase &&
        command_id <= kPluginMenuBase + kPluginMenuMaxSlot) {
      CefRefPtr<CefDictionaryValue> payload = CefDictionaryValue::Create();
      auto set_non_empty = [&](const char* key, const CefString& value) {
        const std::string text = value.ToString();
        if (!text.empty())
          payload->SetString(key, text);
      };
      set_non_empty("page_url", frame ? frame->GetURL() : CefString());
      set_non_empty("link_url", params->GetLinkUrl());
      set_non_empty("source_url", params->GetSourceUrl());
      set_non_empty("selection_text", params->GetSelectionText());

      CefRefPtr<CefDictionaryValue> dict = NewEvent(kPluginMenuCommandEvent);
      dict->SetInt("slot", command_id - kPluginMenuBase);
      dict->SetDictionary("params", payload);
      Emit(dict);
      return true;
    }

    CefString url;
    const char* event = nullptr;

    switch (command_id) {
      case kMenuOpenLinkWindow:
        url = params->GetLinkUrl();
        event = "open-url";
        break;
      case kMenuCopyLinkAddress:
        url = params->GetUnfilteredLinkUrl();
        if (url.empty())
          url = params->GetLinkUrl();
        event = "copy-text";
        break;
      case kMenuOpenImageWindow:
      case kMenuOpenMediaWindow:
        url = params->GetSourceUrl();
        event = "open-url";
        break;
      case kMenuCopyImageAddress:
      case kMenuCopyMediaAddress:
        url = params->GetSourceUrl();
        event = "copy-text";
        break;
      case kMenuSaveImage:
        url = params->GetSourceUrl();
        if (url.empty())
          return false;
        // Reuse the existing download path: the host gets download-begin and
        // chooses the file, exactly as it does for a page-initiated download.
        browser->GetHost()->StartDownload(url);
        return true;
      case kMenuInspect:
        if (!g_devtools_enabled || !browser->GetHost())
          return false;
        /* Chrome's own reading of "Inspect": open the panel with the node under
         * the cursor already selected. */
        ShowDevToolsFor(browser,
                        CefPoint(params->GetXCoord(), params->GetYCoord()));
        return true;
      case MENU_ID_VIEW_SOURCE: {
        /* Intercepted: the source streams to the host instead of CEF's
         * ViewSource() writing a temp file for the desktop's text viewer.
         * The page's own viewer (a loaded view-source: URL) has no source
         * worth fetching, and the recursion guard is the URL itself. */
        const std::string target = frame ? frame->GetURL().ToString()
                                         : params->GetPageUrl().ToString();
        if (target.empty() || target.rfind("view-source:", 0) == 0)
          return true;
        EmitFrameSource(browser, frame ? frame : browser->GetMainFrame());
        return true;
      }
      default:
        // Generic CEF ids: let the library's default implementation run.
        return false;
    }

    if (!event || url.empty())
      return false;

    CefRefPtr<CefDictionaryValue> dict = NewEvent(event);
    if (!strcmp(event, "copy-text")) {
      dict->SetString("text", url);
    } else {
      dict->SetString("url", url);
      dict->SetString("disposition", "new-window");
    }
    Emit(dict);
    return true;
  }

  void OnContextMenuDismissed(CefRefPtr<CefBrowser> browser,
                              CefRefPtr<CefFrame> frame) override {
    CEF_REQUIRE_UI_THREAD();

    // Only a menu this helper still holds a request for can be dismissed by
    // CEF; after the host's own answer the callback is gone and this is the
    // library closing up shop, which the host already knows about.
    if (!g_menu_callback)
      return;

    RestoreCaretAfterSpellcheck();
    g_menu_callback = nullptr;
    Emit(NewEvent("context-menu-dismissed"));
  }

  IMPLEMENT_REFCOUNTING(HelperContextMenuHandler);
};

/* ------------------------------------------------------------------ *
 * Permission prompts                                                  *
 * ------------------------------------------------------------------ *
 *
 * Measured against real Chromium: getUserMedia
 * arrives as OnRequestMediaAccessPermission, and geolocation, clipboard-read,
 * midi-sysex and notifications arrive as OnShowPermissionPrompt. The two kinds
 * run on queues of their own — a held media ask does not stop a prompt from
 * arriving — and each holds exactly one request: a second media ask is not
 * delivered until the first is answered, and the second prompt replaces the
 * first, which Chromium dismisses as "ignore" a moment before the new one shows.
 * A prompt the host sits on also costs the page the ask made right after it,
 * which reaches neither handler. What never reaches this process at all is
 * navigator.storage.persist(), permissions.query() and idle detection, so there
 * is no host that could be asked about those.
 *
 * What an unanswered ask costs the page is the reason nothing is left lying: with
 * Alloy style there is no UI to answer it with, and the page's promise simply
 * stays pending. So every request goes out as permission-request and comes back
 * as permission-respond, and the session answers one the host does not take with
 * a deny — which is both what a page sees today and the only answer that unblocks
 * it. */

struct PermissionRequest {
  bool is_media = false;
  CefRefPtr<CefMediaAccessCallback> media_callback;
  CefRefPtr<CefPermissionPromptCallback> prompt_callback;
  // Continue() has to be handed exactly the mask CEF asked for, so it is kept.
  uint32_t required = 0;
  // CEF's own id for a prompt, kept only to recognise its dismiss callback.
  uint64_t prompt_id = 0;
};

std::map<uint32_t, PermissionRequest> g_permissions;  // wire id, UI thread only
uint32_t g_permission_seq = 0;
std::map<uint64_t, uint32_t> g_prompt_wire;  // CEF's prompt id -> the wire id

const struct {
  uint32_t bit;
  const char* name;
} kPermissionTypes[] = {
    {CEF_PERMISSION_TYPE_AR_SESSION, "ar-session"},
    {CEF_PERMISSION_TYPE_CAMERA_PAN_TILT_ZOOM, "camera-pan-tilt-zoom"},
    {CEF_PERMISSION_TYPE_CAMERA_STREAM, "camera-stream"},
    {CEF_PERMISSION_TYPE_CAPTURED_SURFACE_CONTROL, "captured-surface-control"},
    {CEF_PERMISSION_TYPE_CLIPBOARD, "clipboard"},
    {CEF_PERMISSION_TYPE_TOP_LEVEL_STORAGE_ACCESS, "top-level-storage-access"},
    {CEF_PERMISSION_TYPE_DISK_QUOTA, "disk-quota"},
    {CEF_PERMISSION_TYPE_LOCAL_FONTS, "local-fonts"},
    {CEF_PERMISSION_TYPE_GEOLOCATION, "geolocation"},
    {CEF_PERMISSION_TYPE_HAND_TRACKING, "hand-tracking"},
    {CEF_PERMISSION_TYPE_IDENTITY_PROVIDER, "identity-provider"},
    {CEF_PERMISSION_TYPE_IDLE_DETECTION, "idle-detection"},
    {CEF_PERMISSION_TYPE_MIC_STREAM, "mic-stream"},
    {CEF_PERMISSION_TYPE_MIDI_SYSEX, "midi-sysex"},
    {CEF_PERMISSION_TYPE_MULTIPLE_DOWNLOADS, "multiple-downloads"},
    {CEF_PERMISSION_TYPE_NOTIFICATIONS, "notifications"},
    {CEF_PERMISSION_TYPE_KEYBOARD_LOCK, "keyboard-lock"},
    {CEF_PERMISSION_TYPE_POINTER_LOCK, "pointer-lock"},
    {CEF_PERMISSION_TYPE_PROTECTED_MEDIA_IDENTIFIER, "protected-media"},
    {CEF_PERMISSION_TYPE_REGISTER_PROTOCOL_HANDLER, "protocol-handler"},
    {CEF_PERMISSION_TYPE_STORAGE_ACCESS, "storage-access"},
    {CEF_PERMISSION_TYPE_VR_SESSION, "vr-session"},
    {CEF_PERMISSION_TYPE_WEB_APP_INSTALLATION, "web-app-installation"},
    {CEF_PERMISSION_TYPE_WINDOW_MANAGEMENT, "window-management"},
    {CEF_PERMISSION_TYPE_FILE_SYSTEM_ACCESS, "file-system-access"},
    {CEF_PERMISSION_TYPE_LOCAL_NETWORK, "local-network"},
    {CEF_PERMISSION_TYPE_LOOPBACK_NETWORK, "loopback-network"},
    {CEF_PERMISSION_TYPE_SENSORS, "sensors"},
};

std::vector<std::string> PermissionTypeNames(uint32_t types) {
  std::vector<std::string> names;
  for (const auto& entry : kPermissionTypes) {
    if (types & entry.bit)
      names.push_back(entry.name);
  }
  return names;
}

std::vector<std::string> MediaPermissionNames(uint32_t permissions) {
  std::vector<std::string> names;
  if (permissions & CEF_MEDIA_PERMISSION_DEVICE_AUDIO_CAPTURE)
    names.push_back("device-audio");
  if (permissions & CEF_MEDIA_PERMISSION_DEVICE_VIDEO_CAPTURE)
    names.push_back("device-video");
  if (permissions & CEF_MEDIA_PERMISSION_DESKTOP_AUDIO_CAPTURE)
    names.push_back("desktop-audio");
  if (permissions & CEF_MEDIA_PERMISSION_DESKTOP_VIDEO_CAPTURE)
    names.push_back("desktop-video");
  return names;
}

std::string JoinNames(const std::vector<std::string>& names) {
  std::string out;
  for (const auto& name : names) {
    if (!out.empty())
      out += "+";
    out += name;
  }
  return out.empty() ? "none" : out;
}

CefRefPtr<CefListValue> NameList(const std::vector<std::string>& names) {
  CefRefPtr<CefListValue> list = CefListValue::Create();
  for (size_t i = 0; i < names.size(); ++i)
    list->SetString(i, names[i]);
  return list;
}

void EmitPermissionRequest(uint32_t id,
                           bool is_media,
                           const std::string& origin,
                           uint32_t mask) {
  const std::vector<std::string> names =
      is_media ? MediaPermissionNames(mask) : PermissionTypeNames(mask);

  CefRefPtr<CefDictionaryValue> dict = NewEvent("permission-request");
  dict->SetInt("id", id);
  dict->SetString("kind", is_media ? "media" : "prompt");
  dict->SetString("origin", origin);
  dict->SetList("permissions", NameList(names));
  // The mask too: a name list is for a human, and a host that wants to match on
  // "exactly these bits" should not have to re-derive them from the names.
  dict->SetInt("raw", static_cast<int>(mask));
  Emit(dict);

  fprintf(stderr, "helper: permission %u %s from %s wants %s\n", id,
          is_media ? "media" : "prompt", origin.c_str(),
          JoinNames(names).c_str());
}

const char* PermissionResultName(cef_permission_request_result_t result) {
  switch (result) {
    case CEF_PERMISSION_RESULT_ACCEPT:
      return "accept";
    case CEF_PERMISSION_RESULT_DENY:
      return "deny";
    case CEF_PERMISSION_RESULT_DISMISS:
      return "dismiss";
    case CEF_PERMISSION_RESULT_IGNORE:
      return "ignore";
    default:
      return "unknown";
  }
}

class HelperPermissionHandler : public CefPermissionHandler {
 public:
  bool OnRequestMediaAccessPermission(
      CefRefPtr<CefBrowser> browser,
      CefRefPtr<CefFrame> frame,
      const CefString& requesting_origin,
      uint32_t requested_permissions,
      CefRefPtr<CefMediaAccessCallback> callback) override {
    CEF_REQUIRE_UI_THREAD();

    const uint32_t id = ++g_permission_seq;
    PermissionRequest& entry = g_permissions[id];
    entry.is_media = true;
    entry.media_callback = callback;
    entry.required = requested_permissions;

    EmitPermissionRequest(id, true, requesting_origin.ToString(),
                          requested_permissions);

    // True: this helper draws nothing and the host owes the answer. Default
    // handling here is a denial the page cannot be told about.
    return true;
  }

  bool OnShowPermissionPrompt(CefRefPtr<CefBrowser> browser,
                              uint64_t prompt_id,
                              const CefString& requesting_origin,
                              uint32_t requested_permissions,
                              CefRefPtr<CefPermissionPromptCallback> callback) override {
    CEF_REQUIRE_UI_THREAD();

    const uint32_t id = ++g_permission_seq;
    PermissionRequest& entry = g_permissions[id];
    entry.prompt_callback = callback;
    entry.prompt_id = prompt_id;
    g_prompt_wire[prompt_id] = id;

    EmitPermissionRequest(id, false, requesting_origin.ToString(),
                          requested_permissions);
    return true;
  }

  void OnDismissPermissionPrompt(CefRefPtr<CefBrowser> browser,
                                 uint64_t prompt_id,
                                 cef_permission_request_result_t result) override {
    CEF_REQUIRE_UI_THREAD();

    // Only a request this helper still holds can be dismissed under it. Once the
    // host has answered, CEF runs the same callback on the way out and the host
    // already knows what it said.
    auto wire = g_prompt_wire.find(prompt_id);
    if (wire == g_prompt_wire.end())
      return;
    /* The erase destroys the node `wire` points at: read the id it carries
     * first. Reading wire->second afterwards is a use of a freed iterator,
     * and the id it would return is whatever the allocator left there. */
    const uint32_t id = wire->second;
    g_prompt_wire.erase(wire);

    auto held = g_permissions.find(id);
    if (held == g_permissions.end())
      return;
    g_permissions.erase(held);

    CefRefPtr<CefDictionaryValue> dict = NewEvent("permission-dismissed");
    dict->SetInt("id", id);
    dict->SetString("result", PermissionResultName(result));
    Emit(dict);

    fprintf(stderr, "helper: permission %u dismissed as %s\n", id,
            PermissionResultName(result));
  }

  IMPLEMENT_REFCOUNTING(HelperPermissionHandler);
};

/* ------------------------------------------------------------------ *
 * JavaScript and file dialogs: questions the page blocks on          *
 *                                                                    *
 * A js dialog stops the page's script where it stands: nothing after  *
 * alert() runs until the callback is executed. Alloy has no dialog UI *
 * of its own that can survive in this process (the Chrome-style views *
 * path is the one measured to crash on Wayland), and a dialog the     *
 * host never hears about leaves the page parked forever — measured:   *
 * an alert() on load keeps the title from ever changing. So both      *
 * kinds are parked here and answered by the host, the same shape as   *
 * downloads and permission requests.                                  *
 *                                                                    *
 * The session is what answers a dialog no host took, with Chrome's    *
 * defaults (alert: ok, confirm: cancel, prompt: cancel with an empty  *
 * value, beforeunload: leave). That rule lives on the host side       *
 * because a helper that answered on its own would never give a host   *
 * the chance to draw the question.                                    *
 *                                                                    *
 * One js dialog is held per browser, which is Chromium's own rule: a  *
 * second request while one is open is suppressed outright, exactly    *
 * as the default CEF handler documents. File dialogs are keyed only   *
 * by their wire id; Chromium queues them per browser itself.          *
 * ------------------------------------------------------------------ */
struct JsDialog {
  int browser_id = 0;
  std::string kind;
  CefRefPtr<CefJSDialogCallback> callback;
};

std::map<uint32_t, JsDialog> g_js_dialogs;              // wire id
std::map<int, uint32_t> g_js_dialog_by_browser;         // browser id -> wire id
struct FileDialog {
  int browser_id = 0;
  CefRefPtr<CefFileDialogCallback> callback;
};
std::map<uint32_t, FileDialog> g_file_dialogs;
uint32_t g_dialog_seq = 0;

/* A certificate error the host has not answered yet, keyed by wire id, plus
 * the fingerprints the host has authorized for this helper's lifetime. The
 * table is memory only — persistence across restarts is the host's own
 * policy store, by the same rule as the data directory. */
struct CertDialog {
  int browser_id = 0;
  CefRefPtr<CefCallback> callback;
};
std::map<uint32_t, CertDialog> g_cert_dialogs;
std::set<std::string> g_cert_authorized;

/* 资源拦截一期, the helper half. Rules live only here: no registration is
 * the zero-overhead pass-through, and every matched request parks one
 * question on the channel unless the mode is passive. The id space is this
 * feature's own — request_id — so answers stay unambiguous no matter what
 * else is parked at the same time. */
struct InterceptRule {
  uint32_t id = 0;
  std::vector<MatchPattern> patterns;
  std::set<std::string> resource_types;   // empty means every type
};
std::vector<InterceptRule> g_intercept_rules;
bool g_intercept_passive = false;

struct InterceptPending {
  int browser_id = 0;
  CefRefPtr<CefRequest> request;
  CefRefPtr<CefCallback> callback;
};
std::map<uint32_t, InterceptPending> g_intercept_pending;
uint32_t g_intercept_seq = 0;
/* GetResourceRequestHandler and OnBeforeResourceLoad run on the IO thread;
 * the intercept-* commands and request-respond run on the UI thread. Both
 * sides take this one lock. The emission itself needs no thread hop: the
 * control channel's writer already serializes sends. */
std::mutex g_intercept_mutex;

const char* ResourceTypeText(cef_resource_type_t type) {
  switch (type) {
    case RT_MAIN_FRAME: return "main_frame";
    case RT_SUB_FRAME: return "sub_frame";
    case RT_STYLESHEET: return "stylesheet";
    case RT_SCRIPT: return "script";
    case RT_IMAGE: return "image";
    case RT_FONT_RESOURCE: return "font";
    case RT_MEDIA: return "media";
    case RT_WORKER: return "worker";
    case RT_SHARED_WORKER: return "shared_worker";
    case RT_PREFETCH: return "prefetch";
    case RT_FAVICON: return "favicon";
    case RT_XHR: return "xhr";
    case RT_PING: return "ping";
    case RT_NAVIGATION_PRELOAD_MAIN_FRAME: return "navigation_preload";
    case RT_NAVIGATION_PRELOAD_SUB_FRAME: return "navigation_preload";
    default: return "other";
  }
}

const InterceptRule* MatchInterceptRule(const std::string& url,
                                        const std::string& type) {
  // Callers hold g_intercept_mutex.
  for (const auto& rule : g_intercept_rules) {
    if (!rule.resource_types.empty() && !rule.resource_types.count(type))
      continue;
    for (const auto& pattern : rule.patterns)
      if (pattern.Usable() && pattern.Matches(url))
        return &rule;
  }
  return nullptr;
}

/* The DevTools protocol pipe. Off until cdp-enable: registering the observer
 * is the switch, because a page's full protocol — script, storage, network,
 * everything — is exactly what it hands the host. Method results map back to
 * the host's own id through g_cdp_pending; events pass through as they
 * arrive. Results past the channel's 4 MiB line are refused by name here,
 * before a line nobody can read goes out. */
constexpr size_t kCdpMaxBytes = 4 * 1024 * 1024;

std::map<int, int> g_cdp_pending;   // CEF message id -> the host's id
std::map<int, CefRefPtr<CefRegistration>> g_cdp_registrations;

class DevToolsObserver : public CefDevToolsMessageObserver {
 public:
  void OnDevToolsMethodResult(CefRefPtr<CefBrowser> browser,
                              int message_id, bool success,
                              const void* result, size_t result_size)
      override {
    auto pending = g_cdp_pending.find(message_id);
    if (pending == g_cdp_pending.end())
      return;   // a call this side did not make: not ours to report
    const int host_id = pending->second;
    g_cdp_pending.erase(pending);

    CefRefPtr<CefDictionaryValue> dict = NewEvent("cdp-result");
    dict->SetInt("id", host_id);
    dict->SetBool("ok", success);
    if (result_size > kCdpMaxBytes) {
      dict->SetString("json", "");
      dict->SetString("message", "cdp-too-large");
    } else {
      dict->SetString("json", std::string(
          static_cast<const char*>(result), result_size));
    }
    if (browser)
      dict->SetInt("browser", browser->GetIdentifier());
    Emit(dict);
  }

  void OnDevToolsEvent(CefRefPtr<CefBrowser> browser,
                       const CefString& method,
                       const void* params, size_t params_size) override {
    CefRefPtr<CefDictionaryValue> dict = NewEvent("cdp-event");
    if (browser)
      dict->SetInt("browser", browser->GetIdentifier());
    dict->SetString("method", method);
    if (params_size > kCdpMaxBytes) {
      dict->SetString("params", "");
      dict->SetString("message", "cdp-too-large");
    } else {
      dict->SetString("params", std::string(
          static_cast<const char*>(params), params_size));
    }
    Emit(dict);
  }

  IMPLEMENT_REFCOUNTING(DevToolsObserver);
};

CefRefPtr<DevToolsObserver> g_cdp_observer{new DevToolsObserver};

/* The per-request handler CEF asks for when a rule matches. It exists only
 * for the moment OnBeforeResourceLoad runs: either the event goes out and
 * the request continues (passive), or the decision parks here and the
 * answer comes back as request-respond. */
class RequestGate : public CefResourceRequestHandler {
 public:
  RequestGate(uint32_t request_id, int browser_id, std::string url,
              std::string method, std::string resource_type,
              bool is_main_frame, std::string headers_json)
      : request_id_(request_id), browser_id_(browser_id),
        url_(std::move(url)), method_(std::move(method)),
        resource_type_(std::move(resource_type)),
        is_main_frame_(is_main_frame), headers_json_(std::move(headers_json)) {}

  cef_return_value_t OnBeforeResourceLoad(
      CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
      CefRefPtr<CefRequest> request, CefRefPtr<CefCallback> callback)
      override {
    bool passive;
    {
      std::lock_guard<std::mutex> lock(g_intercept_mutex);
      passive = g_intercept_passive;
      if (!passive) {
        InterceptPending& pending = g_intercept_pending[request_id_];
        pending.browser_id = browser_id_;
        pending.request = request;
        pending.callback = callback;
      }
    }

    CefRefPtr<CefDictionaryValue> dict = NewEvent("request-intercept");
    dict->SetInt("request_id", static_cast<int>(request_id_));
    if (browser_id_)
      dict->SetInt("browser", browser_id_);
    dict->SetString("url", url_);
    dict->SetString("method", method_);
    dict->SetString("resource_type", resource_type_);
    dict->SetBool("is_main_frame", is_main_frame_);
    if (!headers_json_.empty())
      dict->SetString("headers", headers_json_);
    Emit(dict);
    fprintf(stderr,
            "helper: request-intercept %u browser=%d type=%s url=%s\n",
            request_id_, browser_id_, resource_type_.c_str(), url_.c_str());

    if (passive) {
      // Observed, not obstructed: the event is the product here.
      return RV_CONTINUE;
    }
    return RV_CONTINUE_ASYNC;
  }

 private:
  uint32_t request_id_;
  int browser_id_;
  std::string url_;
  std::string method_;
  std::string resource_type_;
  bool is_main_frame_;
  std::string headers_json_;

  IMPLEMENT_REFCOUNTING(RequestGate);
};

/* The certificate decision names, in the net-error vocabulary the host
 * already sees in load-error. */
const char* CertErrorName(cef_errorcode_t error) {
  switch (error) {
    case ERR_CERT_COMMON_NAME_INVALID: return "ERR_CERT_COMMON_NAME_INVALID";
    case ERR_CERT_DATE_INVALID: return "ERR_CERT_DATE_INVALID";
    case ERR_CERT_AUTHORITY_INVALID: return "ERR_CERT_AUTHORITY_INVALID";
    case ERR_CERT_CONTAINS_ERRORS: return "ERR_CERT_CONTAINS_ERRORS";
    case ERR_CERT_NO_REVOCATION_MECHANISM:
      return "ERR_CERT_NO_REVOCATION_MECHANISM";
    case ERR_CERT_UNABLE_TO_CHECK_REVOCATION:
      return "ERR_CERT_UNABLE_TO_CHECK_REVOCATION";
    case ERR_CERT_REVOKED: return "ERR_CERT_REVOKED";
    case ERR_CERT_INVALID: return "ERR_CERT_INVALID";
    case ERR_CERT_WEAK_SIGNATURE_ALGORITHM:
      return "ERR_CERT_WEAK_SIGNATURE_ALGORITHM";
    case ERR_CERT_NON_UNIQUE_NAME: return "ERR_CERT_NON_UNIQUE_NAME";
    case ERR_CERT_VALIDITY_TOO_LONG: return "ERR_CERT_VALIDITY_TOO_LONG";
    case ERR_CERT_NAME_CONSTRAINT_VIOLATION:
      return "ERR_CERT_NAME_CONSTRAINT_VIOLATION";
    case ERR_SSL_PINNED_KEY_NOT_IN_CERT_CHAIN:
      return "ERR_SSL_PINNED_KEY_NOT_IN_CERT_CHAIN";
    case ERR_CERTIFICATE_TRANSPARENCY_REQUIRED:
      return "ERR_CERTIFICATE_TRANSPARENCY_REQUIRED";
    default: return "ERR_CERT_UNKNOWN";
  }
}

/* SHA-256, one screen of the standard: the DER of the leaf becomes the
 * fingerprint the host compares and authorizes. The helper links no crypto
 * library of its own, and the input is 32 bytes of output over whatever the
 * certificate carries — a compact, well-known table implementation is the
 * honest shape for that here. */
class Sha256 {
 public:
  Sha256() { Reset(); }
  void Reset() {
    length_ = 0;
    h_[0] = 0x6a09e667; h_[1] = 0xbb67ae85; h_[2] = 0x3c6ef372;
    h_[3] = 0xa54ff53a; h_[4] = 0x510e527f; h_[5] = 0x9b05688c;
    h_[6] = 0x1f83d9ab; h_[7] = 0x5be0cd19;
    buffer_used_ = 0;
  }
  void Update(const unsigned char* data, size_t size) {
    length_ += size;
    while (size > 0) {
      const size_t take = std::min(size, sizeof(buffer_) - buffer_used_);
      memcpy(buffer_ + buffer_used_, data, take);
      buffer_used_ += take;
      data += take;
      size -= take;
      if (buffer_used_ == sizeof(buffer_)) {
        Transform(buffer_);
        buffer_used_ = 0;
      }
    }
  }
  std::string Hex() {
    uint64_t bits = length_ * 8;
    const unsigned char one = 0x80;
    Update(&one, 1);
    const unsigned char zero = 0;
    while (buffer_used_ != 56)
      Update(&zero, 1);
    unsigned char tail[8];
    for (int i = 0; i < 8; ++i)
      tail[i] = static_cast<unsigned char>(bits >> (56 - i * 8));
    Update(tail, 8);
    std::string out;
    out.reserve(64);
    for (int i = 0; i < 8; ++i)
      for (int b = 3; b >= 0; --b) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x",
                 (h_[i] >> (b * 8)) & 0xff);
        out += buf;
      }
    return out;
  }

 private:
  static uint32_t Rotr(uint32_t v, int n) { return (v >> n) | (v << (32 - n)); }
  void Transform(const unsigned char* chunk) {
    static const uint32_t k[64] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b,
      0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
      0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7,
      0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
      0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152,
      0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
      0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
      0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
      0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
      0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
      0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
      0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
      0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
      w[i] = (uint32_t)chunk[i * 4] << 24 | (uint32_t)chunk[i * 4 + 1] << 16 |
             (uint32_t)chunk[i * 4 + 2] << 8 | (uint32_t)chunk[i * 4 + 3];
    for (int i = 16; i < 64; ++i) {
      const uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^
                          (w[i - 15] >> 3);
      const uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^
                          (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3];
    uint32_t e = h_[4], f = h_[5], g = h_[6], hh = h_[7];
    for (int i = 0; i < 64; ++i) {
      const uint32_t s1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
      const uint32_t ch = (e & f) ^ (~e & g);
      const uint32_t t1 = hh + s1 + ch + k[i] + w[i];
      const uint32_t s0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
      const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      const uint32_t t2 = s0 + maj;
      hh = g; g = f; f = e; e = d + t1;
      d = c; c = b; b = a; a = t1 + t2;
    }
    h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d;
    h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += hh;
    memset(w, 0, sizeof(w));
  }

  uint32_t h_[8];
  uint64_t length_ = 0;
  unsigned char buffer_[64];
  size_t buffer_used_ = 0;
};

const char* JsDialogKindName(cef_jsdialog_type_t type) {
  switch (type) {
    case JSDIALOGTYPE_ALERT:
      return "alert";
    case JSDIALOGTYPE_CONFIRM:
      return "confirm";
    case JSDIALOGTYPE_PROMPT:
      return "prompt";
    default:
      return "unknown";
  }
}

const char* FileDialogModeName(cef_file_dialog_mode_t mode) {
  switch (mode) {
    case FILE_DIALOG_OPEN:
      return "open";
    case FILE_DIALOG_OPEN_MULTIPLE:
      return "open-multiple";
    case FILE_DIALOG_OPEN_FOLDER:
      return "open-folder";
    case FILE_DIALOG_SAVE:
      return "save";
    default:
      return "unknown";
  }
}

void EmitJsDialogClosed(int id, int browser_id, const char* why) {
  auto held = g_js_dialogs.find(static_cast<uint32_t>(id));
  if (held == g_js_dialogs.end())
    return;
  g_js_dialogs.erase(held);
  auto by_browser = g_js_dialog_by_browser.find(browser_id);
  if (by_browser != g_js_dialog_by_browser.end() &&
      by_browser->second == static_cast<uint32_t>(id))
    g_js_dialog_by_browser.erase(by_browser);

  CefRefPtr<CefDictionaryValue> dict = NewEvent("js-dialog-closed");
  dict->SetInt("browser", browser_id);
  dict->SetInt("id", id);
  Emit(dict);
  fprintf(stderr, "helper: js-dialog %d closed (%s)\n", id, why);
}

/* The callback is dropped, not executed: the dialog is already gone on CEF's
 * side when these arrive, and a Continue() into a dismissed request is at best
 * a no-op. The event is what tells the host to take its own UI down. */
void CloseJsDialogForBrowser(CefRefPtr<CefBrowser> browser, const char* why) {
  if (!browser)
    return;
  const int browser_id = browser->GetIdentifier();
  auto held = g_js_dialog_by_browser.find(browser_id);
  if (held == g_js_dialog_by_browser.end())
    return;
  EmitJsDialogClosed(static_cast<int>(held->second), browser_id, why);
}

/* A closing browser takes its dialog with it. The host has to hear that before
 * the browser-closed event, or its panel would keep waiting for an answer the
 * channel can no longer carry. */
void DropDialogsForBrowser(int browser_id) {
  auto held = g_js_dialog_by_browser.find(browser_id);
  if (held != g_js_dialog_by_browser.end())
    EmitJsDialogClosed(static_cast<int>(held->second), browser_id,
                       "browser closed");

  for (auto it = g_file_dialogs.begin(); it != g_file_dialogs.end();) {
    if (it->second.browser_id != browser_id) {
      ++it;
      continue;
    }
    const int id = static_cast<int>(it->first);
    it = g_file_dialogs.erase(it);
    CefRefPtr<CefDictionaryValue> dict = NewEvent("file-dialog-closed");
    dict->SetInt("browser", browser_id);
    dict->SetInt("id", id);
    Emit(dict);
    fprintf(stderr, "helper: file-dialog %d closed (browser closed)\n", id);
  }

  for (auto it = g_cert_dialogs.begin(); it != g_cert_dialogs.end();) {
    if (it->second.browser_id != browser_id) {
      ++it;
      continue;
    }
    const int id = static_cast<int>(it->first);
    it = g_cert_dialogs.erase(it);
    CefRefPtr<CefDictionaryValue> dict = NewEvent("certificate-error-closed");
    dict->SetInt("browser", browser_id);
    dict->SetInt("id", id);
    Emit(dict);
    fprintf(stderr, "helper: certificate-error %d closed (browser closed)\n",
            id);
  }

  std::lock_guard<std::mutex> intercept_lock(g_intercept_mutex);
  for (auto it = g_intercept_pending.begin(); it != g_intercept_pending.end();) {
    if (it->second.browser_id != browser_id) {
      ++it;
      continue;
    }
    const uint32_t request_id = it->first;
    if (it->second.callback)
      it->second.callback->Cancel();
    it = g_intercept_pending.erase(it);
    CefRefPtr<CefDictionaryValue> dict = NewEvent("request-intercept-closed");
    dict->SetInt("browser", browser_id);
    dict->SetInt("request_id", static_cast<int>(request_id));
    Emit(dict);
    fprintf(stderr,
            "helper: request-intercept %u closed (browser closed)\n",
            request_id);
  }
}

class HelperJSDialogHandler : public CefJSDialogHandler {
 public:
  bool OnJSDialog(CefRefPtr<CefBrowser> browser,
                  const CefString& origin_url,
                  JSDialogType dialog_type,
                  const CefString& message_text,
                  const CefString& default_prompt_text,
                  CefRefPtr<CefJSDialogCallback> callback,
                  bool& suppress_message) override {
    CEF_REQUIRE_UI_THREAD();

    /* The inspector is not the page. Its own frontend keeps its dialogs
     * internal; a question from it has no host UI worth waking up for, and
     * answering with the cancel default keeps the inspector's script moving.
     * A background page gets the same: its alert() must not park a question
     * on the channel, and cancel keeps its script running. */
    if (BrowserSlot* slot = FindSlotForBrowser(browser))
      if (slot->devtools || slot->background) {
        if (callback)
          callback->Continue(false, CefString());
        return true;
      }

    if (!browser || !callback)
      return false;

    /* Chromium serialises js dialogs per browser. A second one while this side
     * still holds the first is a page spamming alert(); the default handler's
     * answer is to suppress it, and suppressing is better than executing the
     * callback the page is not waiting to hear from. */
    if (g_js_dialog_by_browser.count(browser->GetIdentifier())) {
      suppress_message = true;
      fprintf(stderr, "helper: js-dialog suppressed for browser %d\n",
              browser->GetIdentifier());
      return false;
    }

    const uint32_t id = ++g_dialog_seq;
    JsDialog& entry = g_js_dialogs[id];
    entry.browser_id = browser->GetIdentifier();
    entry.kind = JsDialogKindName(dialog_type);
    entry.callback = callback;
    g_js_dialog_by_browser[entry.browser_id] = id;

    CefRefPtr<CefDictionaryValue> dict = NewEvent("js-dialog");
    dict->SetInt("browser", entry.browser_id);
    dict->SetInt("id", static_cast<int>(id));
    dict->SetString("kind", entry.kind);
    dict->SetString("origin", origin_url.ToString());
    dict->SetString("message", message_text.ToString());
    dict->SetString("default_value", default_prompt_text.ToString());
    Emit(dict);

    fprintf(stderr,
            "helper: js-dialog %u kind=%s browser=%d origin=%s\n", id,
            entry.kind.c_str(), entry.browser_id, origin_url.ToString().c_str());
    return true;
  }

  bool OnBeforeUnloadDialog(CefRefPtr<CefBrowser> browser,
                            const CefString& message_text,
                            bool is_reload,
                            CefRefPtr<CefJSDialogCallback> callback) override {
    CEF_REQUIRE_UI_THREAD();

    if (!browser || !callback)
      return false;

    if (g_js_dialog_by_browser.count(browser->GetIdentifier())) {
      /* Returning true without executing the callback would leave the unload
       * parked with no id the host could ever answer. Answer "stay", which is
       * what a page with a dialog still on screen means anyway. */
      fprintf(stderr, "helper: beforeunload suppressed by a held dialog for browser %d\n",
              browser->GetIdentifier());
      callback->Continue(false, CefString());
      return true;
    }

    const uint32_t id = ++g_dialog_seq;
    JsDialog& entry = g_js_dialogs[id];
    entry.browser_id = browser->GetIdentifier();
    entry.kind = "beforeunload";
    entry.callback = callback;
    g_js_dialog_by_browser[entry.browser_id] = id;

    CefRefPtr<CefDictionaryValue> dict = NewEvent("js-dialog");
    dict->SetInt("browser", entry.browser_id);
    dict->SetInt("id", static_cast<int>(id));
    dict->SetString("kind", entry.kind);
    dict->SetString("origin", "");
    dict->SetString("message", message_text.ToString());
    dict->SetString("default_value", "");
    Emit(dict);

    fprintf(stderr,
            "helper: js-dialog %u kind=beforeunload browser=%d reload=%d\n", id,
            entry.browser_id, is_reload ? 1 : 0);
    return true;
  }

  /* Navigation cancels whatever was parked, and OnDialogClosed fires when the
   * request is gone whichever way it went. Both only report a dialog this side
   * still holds: answering is what removes the entry, and a request already
   * answered has no host UI left to take down. */
  void OnResetDialogState(CefRefPtr<CefBrowser> browser) override {
    CEF_REQUIRE_UI_THREAD();
    CloseJsDialogForBrowser(browser, "reset by navigation");
  }

  void OnDialogClosed(CefRefPtr<CefBrowser> browser) override {
    CEF_REQUIRE_UI_THREAD();
    CloseJsDialogForBrowser(browser, "closed by the browser");
  }

  IMPLEMENT_REFCOUNTING(HelperJSDialogHandler);
};

class HelperDialogHandler : public CefDialogHandler {
 public:
  bool OnFileDialog(CefRefPtr<CefBrowser> browser,
                    FileDialogMode mode,
                    const CefString& title,
                    const CefString& default_file_path,
                    const std::vector<CefString>& accept_filters,
                    const std::vector<CefString>& accept_extensions,
                    const std::vector<CefString>& accept_descriptions,
                    CefRefPtr<CefFileDialogCallback> callback) override {
    CEF_REQUIRE_UI_THREAD();

    if (!browser || !callback)
      return false;

    /* The inspector's file chooser is the frontend's own business; refusing it
     * is better than parking a question no host panel is meant to answer. A
     * background page's chooser gets the same refusal. */
    if (BrowserSlot* slot = FindSlotForBrowser(browser))
      if (slot->devtools || slot->background) {
        callback->Cancel();
        return true;
      }

    const uint32_t id = ++g_dialog_seq;
    FileDialog& entry = g_file_dialogs[id];
    entry.browser_id = browser->GetIdentifier();
    entry.callback = callback;

    std::vector<std::string> accept;
    for (const auto& filter : accept_filters)
      accept.push_back(filter.ToString());

    CefRefPtr<CefDictionaryValue> dict = NewEvent("file-dialog");
    dict->SetInt("browser", entry.browser_id);
    dict->SetInt("id", static_cast<int>(id));
    dict->SetString("mode", FileDialogModeName(mode));
    dict->SetString("title", title.ToString());
    dict->SetString("default_path", default_file_path.ToString());
    dict->SetList("accept", NameList(accept));
    Emit(dict);

    fprintf(stderr, "helper: file-dialog %u mode=%s browser=%d accept=%s\n", id,
            FileDialogModeName(mode), entry.browser_id,
            JoinNames(accept).c_str());
    return true;
  }

  IMPLEMENT_REFCOUNTING(HelperDialogHandler);
};

/* PrintToPDF on Linux asks the client for a paper size before the page is laid
 * out; without a handler CEF refuses the job. Only GetPdfPaperSize is load-
 * bearing here: the helper has no printer UI, and window.print() from a page
 * cancels because OnPrintDialog says no rather than parking a question the
 * control channel has no event for. */
class HelperPrintHandler : public CefPrintHandler {
 public:
  void OnPrintStart(CefRefPtr<CefBrowser> browser) override {}

  void OnPrintSettings(CefRefPtr<CefBrowser> browser,
                       CefRefPtr<CefPrintSettings> settings,
                       bool get_defaults) override {}

  bool OnPrintDialog(CefRefPtr<CefBrowser> browser,
                     bool has_selection,
                     CefRefPtr<CefPrintDialogCallback> callback) override {
    return false;
  }

  bool OnPrintJob(CefRefPtr<CefBrowser> browser,
                  const CefString& document_name,
                  const CefString& pdf_file_path,
                  CefRefPtr<CefPrintJobCallback> callback) override {
    return false;
  }

  void OnPrintReset(CefRefPtr<CefBrowser> browser) override {}

  CefSize GetPdfPaperSize(CefRefPtr<CefBrowser> browser,
                          int device_units_per_inch) override {
    // A4, the size the CEF sample uses: 8.27 x 11.69 inches.
    return CefSize(device_units_per_inch * 827 / 100,
                   device_units_per_inch * 1169 / 100);
  }

  IMPLEMENT_REFCOUNTING(HelperPrintHandler);
};

// ExecuteJavaScript has no return value, so the code is wrapped: the page
// evaluates it, turns the result into JSON itself, and hands that one text to
// kReportFunction. Statements work as well as expressions because eval() yields
// the last value; a script that returns nothing reports null, not a failure.
std::string WrapScript(const std::string& code, int id, bool expression_only) {
  CefRefPtr<CefValue> quoted = CefValue::Create();
  quoted->SetString(code);
  // JSON string syntax is a subset of JavaScript string syntax, so the code can
  // be re-quoted here and never has to be escaped again.
  const std::string literal = CefWriteJSON(quoted, JSON_WRITER_DEFAULT).ToString();

  std::string out;
  out += "(function () {";
  out += "var __id = " + std::to_string(id) + ";";
  out += "function __report(ok, valueJson, message) {";
  out += "try { window." + std::string(kReportFunction) +
         "('{\"id\":' + __id + ',\"ok\":' + (ok ? 'true' : 'false') + "
         "',\"value\":' + valueJson + ',\"message\":' + JSON.stringify(message) + '}');";
  out += "} catch (e) {}";
  out += "}";
  out += "var __value;";
  if (expression_only) {
    // Direct embedding, not eval(): pages with a script-src CSP reject eval()
    // even when the embedder compiled this script itself. js-expr is for
    // inspection expressions (and IIFEs); js remains the statement/eval form.
    out += "try { __value = (" + code + "); }";
  } else {
    out += "try { __value = eval(" + literal + "); }";
  }
  out += "catch (e) { __report(false, 'null', String((e && e.message) ? e.message : e)); return; }";
  out += "if (typeof __value === 'undefined') { __report(true, 'null', ''); return; }";
  out += "var __text;";
  out += "try { __text = JSON.stringify(__value); }";
  out += "catch (e) { __report(true, JSON.stringify(String(__value)), "
        "'result was converted to a string'); return; }";
  out += "__report(true, __text === undefined ? 'null' : __text, '');";
  out += "})();";
  return out;
}

// The payload arrives as one JSON text built by WrapScript; it becomes the
// host's js-result event with the same fields.
void EmitJsResult(const CefString& payload) {
  CefString parse_error;
  CefRefPtr<CefValue> root =
      CefParseJSONAndReturnError(payload, JSON_PARSER_RFC, parse_error);
  if (!root || root->GetType() != VTYPE_DICTIONARY) {
    fprintf(stderr, "helper: unreadable js result: %s\n",
            parse_error.ToString().c_str());
    EmitStatus("js-result-unreadable");
    return;
  }

  CefRefPtr<CefDictionaryValue> result = root->GetDictionary();
  CefRefPtr<CefDictionaryValue> event = NewEvent("js-result");
  event->SetInt("id", result->HasKey("id") ? result->GetInt("id") : 0);
  event->SetBool("ok", result->HasKey("ok") ? result->GetBool("ok") : false);
  event->SetString("message",
                   result->HasKey("message")
                       ? result->GetString("message").ToString()
                       : std::string());

  CefRefPtr<CefValue> value =
      result->HasKey("value") ? result->GetValue("value") : nullptr;
  std::string value_json;
  if (value)
    value_json = CefWriteJSON(value, JSON_WRITER_DEFAULT).ToString();
  if (value_json.size() > kMaxResultBytes) {
    // ok once meant "the script ran", which made a dropped result look like a
    // script that returned null — a host checking ok=1 would believe it. ok
    // now means "the script ran and its result was delivered": a dropped
    // result is a failed call, value null, and the message says why.
    event->SetBool("ok", false);
    event->SetString("message", "result exceeded 64 KiB and was dropped");
    event->SetBool("truncated", true);
    value = nullptr;
    value_json.clear();
  }

  if (!g_control)
    return;

  /* The value is serialized once and the text that measured the ceiling is
   * the text that goes on the wire: the envelope is rendered without it and
   * the fragment spliced in before the closing brace — the same place the
   * dictionary writer puts a member set last, so the bytes match what
   * Emit(event-with-value) produced. A dropped value leaves "value":null in
   * the envelope and nothing to splice. */
  if (!value)
    event->SetNull("value");
  CefRefPtr<CefValue> envelope = CefValue::Create();
  envelope->SetDictionary(event);
  std::string line = CefWriteJSON(envelope, JSON_WRITER_DEFAULT).ToString();
  if (value) {
    line.insert(line.size() - 1, ",\"value\":" + value_json);
  }
  line.push_back('\n');
  g_control->Send(line);
}

void HandleCommand(const std::string& line) {
  CEF_REQUIRE_UI_THREAD();

  CefString parse_error;
  CefRefPtr<CefValue> value =
      CefParseJSONAndReturnError(line, JSON_PARSER_RFC, parse_error);
  if (!value || value->GetType() != VTYPE_DICTIONARY) {
    fprintf(stderr, "helper: bad command: %s\n", parse_error.ToString().c_str());
    EmitStatus("bad-command");
    return;
  }

  CefRefPtr<CefDictionaryValue> command = value->GetDictionary();
  const std::string name = command->GetString("cmd").ToString();

  if (name == "quit") {
    /* A multi-browser helper has to close every browser, not just the active
     * one: OnBeforeClose only ends the message loop when the map is empty. */
    CloseEveryBrowser();
    return;
  }

  if (name == "new-browser") {
    const std::string url = command->GetString("url").ToString();
    const std::string mode = command->HasKey("mode")
                                 ? command->GetString("mode").ToString()
                                 : "";
    const bool windowed = mode == "window";
    const bool background = mode == "background";
    CreateNewBrowser(url, windowed, background);
    return;
  }

  if (name == "select-browser") {
    SelectBrowserById(command->GetInt("browser"));
    return;
  }

  if (name == "close-browser") {
    const int id = command->HasKey("browser")
                       ? command->GetInt("browser")
                       : g_active_browser_id;
    auto it = g_browser_slots.find(id);
    if (it == g_browser_slots.end() || !it->second.browser) {
      EmitStatus("no-such-browser");
      return;
    }
    /* A tab is a child of the shared CefWindow. Alloy treats CloseBrowser on a
     * browser whose view is still owned by the window as a request to close
     * the window, which takes every other tab with it. Detaching the view and
     * releasing the slot's reference first makes the browser the only owner of
     * its lifetime, so CloseBrowser closes just that browser. The local
     * reference keeps the browser alive across the callback, which erases the
     * map entry this iterator points into. */
    CefRefPtr<CefBrowser> browser = it->second.browser;
    if (!it->second.windowed)
      {
        if (it->second.view && it->second.window)
          it->second.window->RemoveChildView(it->second.view);
        it->second.view = nullptr;
      }
    browser->GetHost()->CloseBrowser(true);
    return;
  }

  // Plugin commands answer before the no-browser guard: a plugin's tables
  // belong to the helper as a whole, not to whichever browser is active, and
  // a plugin can be loaded before any browser exists to show its pages.
  if (name == "plugins-load") {
    CefRefPtr<CefListValue> plugins =
        command->HasKey("plugins") ? command->GetList("plugins") : nullptr;
    if (!plugins) {
      EmitStatus("plugins-load-needs-plugins");
      return;
    }
    {
    std::lock_guard<std::mutex> lock(g_plugin_mutex);
    for (size_t i = 0; i < plugins->GetSize(); ++i) {
      CefRefPtr<CefDictionaryValue> entry = plugins->GetDictionary(i);
      if (!entry)
        continue;
      const std::string id = entry->GetString("id").ToString();
      const std::string root = entry->GetString("root").ToString();
      if (id.empty() || root.empty()) {
        EmitStatus("plugins-load-needs-id-and-root");
        return;
      }
      PluginEntry& slot = g_plugin_table[id];  // replace: the whole truth
      slot.root = root;
      slot.scripts.clear();
      slot.background.clear();
      if (entry->HasKey("background")) {
        // The same inside-the-root rule the content scripts get, applied at
        // load: a bad background path is refused by name before any page
        // could ask for it.
        std::string background_path;
        const std::string background_relative =
            entry->GetString("background").ToString();
        if (background_relative.empty())
          EmitPluginError(id, "background-empty");
        else if (!PluginPathInside(root, background_relative,
                                   &background_path))
          EmitPluginError(id, "background-path-escape:" + background_relative);
        else
          slot.background = background_relative;
      }
      if (entry->HasKey("scripts")) {
        CefRefPtr<CefListValue> scripts = entry->GetList("scripts");
        for (size_t s = 0; s < scripts->GetSize(); ++s) {
          CefRefPtr<CefDictionaryValue> script = scripts->GetDictionary(s);
          if (!script)
            continue;
          PluginScript parsed;
          if (script->HasKey("matches")) {
            CefRefPtr<CefListValue> matches = script->GetList("matches");
            for (size_t m = 0; m < matches->GetSize(); ++m) {
              MatchPattern pattern = MatchPattern::Parse(
                  matches->GetString(m).ToString());
              if (pattern.Usable())
                parsed.matches.push_back(pattern);
            }
          }
          if (script->HasKey("js")) {
            CefRefPtr<CefListValue> files = script->GetList("js");
            for (size_t j = 0; j < files->GetSize(); ++j)
              parsed.js.push_back(files->GetString(j).ToString());
          }
          parsed.run_at = script->HasKey("run_at")
                              ? script->GetString("run_at").ToString()
                              : "document_idle";
          // Read the files now: the renderer gets texts (its sandbox cannot
          // read the disk), and document_idle injection from this process
          // does not want to touch the file system per navigation either.
          for (const auto& file : parsed.js) {
            std::string text;
            if (ReadScriptFile(id, root, file, &text))
              parsed.js_text.push_back(text);
          }
          if (!parsed.js_text.empty())
            slot.scripts.push_back(parsed);
        }
      }
      fprintf(stderr, "helper: plugins load id=%s root=%s scripts=%zu\n",
              id.c_str(), root.c_str(), slot.scripts.size());
    }
    }
    // The push takes the same mutex inside BuildRendererTable — it runs
    // here, after the load's own lock has lifted. Inside the scope above it
    // would deadlock the UI thread against itself, and the whole helper
    // would go quiet mid-ready (measured 2026-10-26: every later command,
    // the page load included, never reached the message loop).
    PushPluginsTableToAll();
    return;
  }

  if (name == "plugins-unload") {
    const std::string id = command->GetString("id").ToString();
    {
      std::lock_guard<std::mutex> lock(g_plugin_mutex);
      g_plugin_table.erase(id);
      fprintf(stderr, "helper: plugins unload id=%s\n", id.c_str());
    }
    PushPluginsTableToAll();
    return;
  }

  if (name == "menu-add-user-item") {
    const int slot = command->GetInt("slot");
    const std::string label = command->GetString("label").ToString();
    if (slot < 0 || slot > kPluginMenuMaxSlot) {
      EmitStatus("plugin-menu-slot-out-of-range");
      return;
    }
    UserMenuItem& item = g_user_menu_items[slot];  // replace by slot
    item.label = label;
    item.has_when = false;
    if (command->HasKey("when")) {
      MatchPattern when = MatchPattern::Parse(
          command->GetString("when").ToString());
      if (when.Usable()) {
        item.when = when;
        item.has_when = true;
      } else {
        fprintf(stderr, "helper: plugin menu slot=%d has an unusable when=%s\n",
                slot, command->GetString("when").ToString().c_str());
      }
    }
    fprintf(stderr, "helper: plugin menu row slot=%d label=%s%s\n", slot,
            label.c_str(), item.has_when ? " (when)" : "");
    return;
  }

  if (name == "menu-remove-user-item") {
    const int slot = command->GetInt("slot");
    g_user_menu_items.erase(slot);
    fprintf(stderr, "helper: plugin menu row removed slot=%d\n", slot);
    return;
  }

  if (name == "plugin-respond") {
    // The host's answer to one plugin-message. Routed to the browser the
    // call came from; the renderer delivers it to every shim in that
    // browser, and the one holding the reply id resolves.
    const int browser_id = command->GetInt("browser");
    auto it = g_browser_slots.find(browser_id);
    if (it == g_browser_slots.end() || !it->second.browser) {
      EmitStatus("no-such-browser");
      return;
    }
    std::string value = command->HasKey("value")
                            ? command->GetString("value").ToString()
                            : std::string("null");
    if (value.empty())
      value = "null";
    if (value.size() > kPluginPayloadMaxBytes) {
      EmitStatus("plugin-payload-too-large");
      return;
    }
    CefRefPtr<CefProcessMessage> message =
        CefProcessMessage::Create(kPluginResponseProcess);
    message->GetArgumentList()->SetInt(0, command->GetInt("reply_id"));
    message->GetArgumentList()->SetBool(1, command->GetBool("ok"));
    message->GetArgumentList()->SetString(2, value);
    it->second.browser->GetMainFrame()->SendProcessMessage(PID_RENDERER,
                                                           message);
    return;
  }

  if (name == "browsers-list") {
    const int request_id = command->GetInt("id");
    CefRefPtr<CefListValue> list = CefListValue::Create();
    size_t n = 0;
    for (const auto& entry : g_browser_slots) {
      if (entry.second.devtools || entry.second.background)
        continue;  // the inspector is not a tab a plugin should see, and
                   // neither is a background page
      CefRefPtr<CefDictionaryValue> one = CefDictionaryValue::Create();
      one->SetInt("id", entry.first);
      one->SetString("url", entry.second.browser &&
                                    entry.second.browser->GetMainFrame()
                                        ? entry.second.browser->GetMainFrame()
                                              ->GetURL()
                                              .ToString()
                                        : entry.second.start_url);
      one->SetString("title", entry.second.title);
      one->SetBool("active", entry.first == g_active_browser_id);
      one->SetBool("windowed", entry.second.windowed);
      // Read from the host, not from a cache: mute and zoom are facts about
      // the browser as Chromium holds them right now.
      if (entry.second.browser && entry.second.browser->GetHost()) {
        one->SetBool("muted",
                     entry.second.browser->GetHost()->IsAudioMuted());
        one->SetDouble("zoom",
                       entry.second.browser->GetHost()->GetZoomLevel());
      }
      list->SetDictionary(n++, one);
    }
    CefRefPtr<CefDictionaryValue> dict = NewEvent("browsers");
    dict->SetInt("id", request_id);
    dict->SetList("browsers", list);
    Emit(dict);
    return;
  }

  if (!g_browser) {
    EmitStatus("no-browser");
    return;
  }

  if (name == "load") {
    const std::string url = command->GetString("url").ToString();
    if (url.empty()) {
      EmitStatus("load-needs-url");
      return;
    }
    // tabs.update names the browser it wants; absent, the active one. Same
    // shape the close-browser command uses.
    CefRefPtr<CefBrowser> target = g_browser;
    if (command->HasKey("browser")) {
      auto it = g_browser_slots.find(command->GetInt("browser"));
      if (it == g_browser_slots.end() || !it->second.browser) {
        EmitStatus("no-such-browser");
        return;
      }
      target = it->second.browser;
    }
    CefRefPtr<CefFrame> frame = target->GetMainFrame();
    if (!frame) {
      EmitStatus("no-main-frame");
      return;
    }
    frame->LoadURL(url);
    return;
  }

  if (name == "view-source") {
    /* The menu id and this command share one path: the active browser's
     * main frame streams to the host. */
    if (!g_browser) {
      EmitStatus("no-browser");
      return;
    }
    EmitFrameSource(g_browser, g_browser->GetMainFrame());
    return;
  }

  if (name == "cookies-get" || name == "cookies-set" ||
      name == "cookies-delete") {
    const int id = command->HasKey("id") ? command->GetInt("id") : 0;
    CefRefPtr<CefRequestContext> context =
        g_browser->GetHost() ? g_browser->GetHost()->GetRequestContext()
                             : nullptr;
    CefRefPtr<CefCookieManager> manager =
        context ? context->GetCookieManager(nullptr) : nullptr;
    if (!manager) {
      EmitCookiesError(id, "no-cookie-manager");
      return;
    }

    if (name == "cookies-get") {
      const std::string url = command->GetString("url").ToString();
      if (url.empty()) {
        EmitCookiesError(id, "cookies-get-needs-url");
        return;
      }
      CefRefPtr<CookieCollector> collector(new CookieCollector(id));
      if (!manager->VisitUrlCookies(url, true, collector)) {
        EmitCookiesError(id, "cookie-store-unavailable");
        return;
      }
      PostDelayedUi([collector]() { collector->AnswerIfSilent(); }, 1500);
      return;
    }

    if (name == "cookies-delete") {
      const std::string url = command->GetString("url").ToString();
      const std::string cookie_name =
          command->HasKey("name") ? command->GetString("name").ToString()
                                  : std::string();
      if (url.empty() && cookie_name.empty()) {
        EmitCookiesError(id, "cookies-delete-needs-url-or-name");
        return;
      }
      if (!manager->DeleteCookies(url, cookie_name,
                                  new CookieDeleteCallback(id))) {
        EmitCookiesError(id, "cookie-store-unavailable");
        return;
      }
      return;
    }

    CefRefPtr<CefListValue> cookies =
        command->HasKey("cookies") && command->GetType("cookies") == VTYPE_LIST
            ? command->GetList("cookies")
            : nullptr;
    if (!cookies || cookies->GetSize() == 0) {
      EmitCookiesError(id, "cookies-set-needs-cookies");
      return;
    }
    const std::string url =
        command->HasKey("url") ? command->GetString("url").ToString()
                               : std::string();
    auto state = std::make_shared<CookieSetState>();
    state->id = id;
    state->total = static_cast<int>(cookies->GetSize());
    for (size_t i = 0; i < cookies->GetSize(); ++i) {
      CefCookie cookie;
      std::string error;
      CefRefPtr<CefDictionaryValue> entry = cookies->GetDictionary(i);
      if (!entry || !EntryToCookie(entry, &cookie, &error)) {
        NoteCookieSetDone(state, entry ? entry->GetString("name").ToString()
                                       : std::string(),
                          false);
        continue;
      }
      if (!manager->SetCookie(url, cookie,
                              new CookieSetCallback(
                                  state, CefString(&cookie.name).ToString()))) {
        NoteCookieSetDone(state, CefString(&cookie.name).ToString(), false);
      }
    }
    return;
  }

  if (name == "history") {
    const int id = command->HasKey("id") ? command->GetInt("id") : 0;
    CefRefPtr<CefBrowser> target = g_browser;
    if (command->HasKey("browser")) {
      auto it = g_browser_slots.find(command->GetInt("browser"));
      if (it == g_browser_slots.end() || !it->second.browser) {
        EmitStatus("no-such-browser");
        return;
      }
      target = it->second.browser;
    }
    CefRefPtr<HistoryVisitor> visitor(new HistoryVisitor(id));
    target->GetHost()->GetNavigationEntries(visitor, false);
    PostDelayedUi([visitor]() { visitor->AnswerIfSilent(); }, 1500);
    return;
  }

  if (name == "page-text") {
    const int id = command->HasKey("id") ? command->GetInt("id") : 0;
    CefRefPtr<CefBrowser> target = g_browser;
    if (command->HasKey("browser")) {
      auto it = g_browser_slots.find(command->GetInt("browser"));
      if (it == g_browser_slots.end() || !it->second.browser) {
        EmitStatus("no-such-browser");
        return;
      }
      target = it->second.browser;
    }
    CefRefPtr<CefFrame> frame = target->GetMainFrame();
    if (!frame) {
      EmitStatus("no-main-frame");
      return;
    }
    frame->GetText(new PageTextStream(id, target->GetIdentifier()));
    return;
  }

  if (name == "print-pdf") {
    const int id = command->HasKey("id") ? command->GetInt("id") : 0;
    CefRefPtr<CefBrowser> target = g_browser;
    if (command->HasKey("browser")) {
      auto it = g_browser_slots.find(command->GetInt("browser"));
      if (it == g_browser_slots.end() || !it->second.browser) {
        EmitStatus("no-such-browser");
        return;
      }
      target = it->second.browser;
    }

    CefPdfPrintSettings settings;
    settings.landscape =
        command->HasKey("landscape") && command->GetBool("landscape");
    settings.print_background = command->HasKey("print_background") &&
                                command->GetBool("print_background");
    if (command->HasKey("scale") && command->GetDouble("scale") > 0)
      settings.scale = command->GetDouble("scale");
    if (command->HasKey("page_ranges"))
      CefString(&settings.page_ranges)
          .FromString(command->GetString("page_ranges").ToString());

    /* The caller names a ceiling only to be able to prove the truncation
     * path; the channel's own 16 MiB is the default and the hard maximum. */
    size_t max_bytes = kPdfMaxBytes;
    if (command->HasKey("max_bytes") && command->GetInt("max_bytes") > 0)
      max_bytes = static_cast<size_t>(
          std::min(command->GetInt("max_bytes"), static_cast<int>(kPdfMaxBytes)));

    const std::string path =
        RuntimeBaseDir() + "/cefnest-pdf-" + std::to_string(getpid()) + "-" +
        std::to_string(g_next_pdf_id++) + ".pdf";
    target->GetHost()->PrintToPDF(
        path, settings, new PdfPrintStream(id, path, max_bytes));
    fprintf(stderr, "helper: print-pdf id=%d -> %s\n", id, path.c_str());
    return;
  }

  if (name == "back") {
    g_browser->GoBack();
    return;
  }
  if (name == "forward") {
    g_browser->GoForward();
    return;
  }
  if (name == "reload") {
    if (command->HasKey("hard") && command->GetBool("hard"))
      g_browser->ReloadIgnoreCache();
    else
      g_browser->Reload();
    return;
  }
  // Page zoom: the host drives, the level is a CEF zoom level (0 = 100 %,
  // each +0.5 is one Chrome UI step ±). Chromium persists zoom per origin,
  // which is the browser-like behavior an embedded component wants.
  if (name == "zoom-in" || name == "zoom-out" || name == "zoom-reset" ||
      name == "zoom-set") {
    CefRefPtr<CefBrowserHost> host = g_browser->GetHost();
    if (!host) {
      EmitStatus("no-host");
      return;
    }

    double level = host->GetZoomLevel();
    if (name == "zoom-in")
      level += 0.5;
    else if (name == "zoom-out")
      level -= 0.5;
    else if (name == "zoom-reset")
      level = 0.0;
    else
      level = command->HasKey("level") ? command->GetDouble("level") : 0.0;
    host->SetZoomLevel(level);

    CefRefPtr<CefDictionaryValue> dict = NewEvent("zoom");
    dict->SetDouble("level", host->GetZoomLevel());
    Emit(dict);
    return;
  }

  if (name == "auto-resize") {
    CefRefPtr<CefBrowser> target = g_browser;
    if (command->HasKey("browser")) {
      auto it = g_browser_slots.find(command->GetInt("browser"));
      if (it == g_browser_slots.end() || !it->second.browser) {
        EmitStatus("no-such-browser");
        return;
      }
      target = it->second.browser;
    }
    CefRefPtr<CefBrowserHost> host = target->GetHost();
    if (!host) {
      EmitStatus("no-host");
      return;
    }
    const bool enabled =
        command->HasKey("enabled") && command->GetBool("enabled");
    const CefSize min_size(
        command->HasKey("min_width") ? command->GetInt("min_width") : 0,
        command->HasKey("min_height") ? command->GetInt("min_height") : 0);
    const CefSize max_size(
        command->HasKey("max_width") ? command->GetInt("max_width") : 0,
        command->HasKey("max_height") ? command->GetInt("max_height") : 0);
    host->SetAutoResizeEnabled(enabled, min_size, max_size);
    fprintf(stderr,
            "helper: auto-resize %s min=%dx%d max=%dx%d (browser %d)\n",
            enabled ? "on" : "off", min_size.width, min_size.height,
            max_size.width, max_size.height, target->GetIdentifier());
    return;
  }

  if (name == "stop") {
    g_browser->StopLoad();
    return;
  }

  if (name == "mute") {
    // tabs.update names the browser it wants muted; absent, the active one.
    // Same shape the load command uses.
    CefRefPtr<CefBrowser> target = g_browser;
    if (command->HasKey("browser")) {
      auto it = g_browser_slots.find(command->GetInt("browser"));
      if (it == g_browser_slots.end() || !it->second.browser) {
        EmitStatus("no-such-browser");
        return;
      }
      target = it->second.browser;
    }
    CefRefPtr<CefBrowserHost> host = target ? target->GetHost() : nullptr;
    if (!host) {
      EmitStatus("no-host");
      return;
    }
    const bool mute =
        command->HasKey("on") ? command->GetBool("on") : !host->IsAudioMuted();
    host->SetAudioMuted(mute);
    // Reported back rather than assumed: the page's sound leaves this process
    // through an unsandboxed audio service, and whether it
    // is silenced is a fact about that service, not about this call. The
    // browser field names the browser actually muted — muting a background
    // tab must not read as if the active one changed.
    CefRefPtr<CefDictionaryValue> dict = NewEvent("muted");
    dict->SetInt("browser", target->GetIdentifier());
    dict->SetBool("muted", host->IsAudioMuted());
    Emit(dict);
    return;
  }

  if (name == "find") {
    CefRefPtr<CefBrowserHost> host = g_browser->GetHost();
    if (!host) {
      EmitStatus("no-host");
      return;
    }
    const std::string pattern = command->GetString("pattern").ToString();
    if (pattern.empty()) {
      /* CEF reads an empty search text as "stop searching", which is what
       * find-stop is for; passing it through would give two names for one
       * command and no way for the host to tell them apart. */
      EmitStatus("find-needs-pattern");
      return;
    }
    const bool forward =
        command->HasKey("forward") ? command->GetBool("forward") : true;
    const bool match_case =
        command->HasKey("match_case") ? command->GetBool("match_case") : false;
    const bool next =
        command->HasKey("next") ? command->GetBool("next") : false;
    host->Find(pattern, forward, match_case, next);
    // Nothing answered here: the results are find-result events, and the first
    // one can wait for the whole page to be scanned.
    return;
  }

  if (name == "find-stop") {
    CefRefPtr<CefBrowserHost> host = g_browser->GetHost();
    if (!host) {
      EmitStatus("no-host");
      return;
    }
    const bool clear =
        command->HasKey("clear") ? command->GetBool("clear") : true;
    host->StopFinding(clear);
    return;
  }

  if (name == "download-accept") {
    const uint32_t id = static_cast<uint32_t>(command->GetInt("id"));
    auto it = g_downloads.find(id);
    if (it == g_downloads.end() || !it->second.gate) {
      EmitStatus("no-such-download");
      return;
    }
    /* No empty path here on purpose: CEF reads that as "suggested name in the
     * default temp directory", and a file the host never asked for appearing
     * somewhere it does not know about is worse than no file at all. */
    const std::string path = command->GetString("path").ToString();
    if (path.empty()) {
      EmitStatus("download-accept-needs-path");
      return;
    }
    it->second.gate->Continue(path, false);
    it->second.gate = nullptr;
    return;
  }

  if (name == "download-pause" || name == "download-resume") {
    const uint32_t id = static_cast<uint32_t>(command->GetInt("id"));
    auto it = g_downloads.find(id);
    if (it == g_downloads.end() || !it->second.item) {
      EmitStatus("download-not-in-flight");
      return;
    }
    if (name == "download-pause")
      it->second.item->Pause();
    else
      it->second.item->Resume();
    fprintf(stderr, "helper: download %u %s\n", id,
            name == "download-pause" ? "paused" : "resumed");
    return;
  }

  if (name == "download-reject") {
    const uint32_t id = static_cast<uint32_t>(command->GetInt("id"));
    auto it = g_downloads.find(id);
    if (it == g_downloads.end()) {
      EmitStatus("no-such-download");
      return;
    }
    if (it->second.item) {
      /* Cancelling an in-flight download is the item callback's job, and the
       * terminal download-end comes from OnDownloadUpdated afterwards. */
      it->second.item->Cancel();
      return;
    }
    /* The gate is the only thing here, so dropping it is the cancellation CEF
     * documents. No update follows one that never started, so this is also
     * where the host's terminal event has to come from. */
    it->second.gate = nullptr;
    CefRefPtr<CefDictionaryValue> dict = NewEvent("download-end");
    dict->SetInt("id", static_cast<int>(id));
    dict->SetString("state", "cancelled");
    dict->SetString("path", "");
    dict->SetDouble("received_bytes", 0.0);
    Emit(dict);
    g_downloads.erase(it);
    return;
  }

  if (name == "context-menu-select") {
    if (!g_menu_callback) {
      /* The host answered a menu that is already gone — it lost a race with a
       * second right click, or answered after its own window closed. Nothing to
       * continue, and saying so is the whole reply. */
      EmitStatus("no-context-menu");
      return;
    }
    CefRefPtr<CefRunContextMenuCallback> callback;
    callback.swap(g_menu_callback);
    const int flags = command->HasKey("flags") ? command->GetInt("flags") : 0;
    const int command_id = command->GetInt("id");
    // Paste is the one command whose result depends on the selection it sees.
    // Collapse first so it inserts at the caret, not over the spellcheck word.
    // Select All is the opposite: the selection is the result, so leave it.
    if (command_id == MENU_ID_PASTE ||
        command_id == MENU_ID_PASTE_MATCH_STYLE)
      RestoreCaretAfterSpellcheck();
    if (command_id == MENU_ID_SELECT_ALL) {
      g_menu_restore_caret = false;
      g_menu_restore_frame = nullptr;
    }
    callback->Continue(command_id, static_cast<cef_event_flags_t>(flags));
    ScheduleCaretRestoreAfterSpellcheck();
    return;
  }

  if (name == "context-menu-dismiss") {
    if (!g_menu_callback)
      return;
    CefRefPtr<CefRunContextMenuCallback> callback;
    callback.swap(g_menu_callback);
    // Cancel() is the answer that closes the request without a command; CEF
    // still calls OnContextMenuDismissed afterwards, by which time the
    // callback is gone and nothing more is emitted.
    callback->Cancel();
    RestoreCaretAfterSpellcheck();
    return;
  }

  if (name == "permission-respond") {
    const uint32_t id = static_cast<uint32_t>(command->GetInt("id"));
    const std::string result = command->GetString("result").ToString();

    auto it = g_permissions.find(id);
    if (it == g_permissions.end()) {
      EmitStatus("no-permission-request");
      return;
    }

    // Drop the slot before answering: the dismiss callback CEF runs on the way
    // out has to find nothing held, which is how it knows the host already spoke.
    PermissionRequest entry = std::move(it->second);
    g_permissions.erase(it);
    if (entry.prompt_id)
      g_prompt_wire.erase(entry.prompt_id);

    // Anything that is not accept or dismiss is a deny — the safe reading of a
    // word this side does not know. Media access has no third answer: Cancel()
    // closes a dismissed ask the same way it closes a refused one.
    if (entry.is_media && entry.media_callback) {
      if (result == "accept")
        entry.media_callback->Continue(entry.required);
      else
        entry.media_callback->Cancel();
    } else if (entry.prompt_callback) {
      entry.prompt_callback->Continue(result == "accept"
                                          ? CEF_PERMISSION_RESULT_ACCEPT
                                      : result == "dismiss"
                                          ? CEF_PERMISSION_RESULT_DISMISS
                                          : CEF_PERMISSION_RESULT_DENY);
    }

    fprintf(stderr, "helper: permission %u answered %s\n", id, result.c_str());
    return;
  }

  if (name == "js-dialog-respond") {
    const uint32_t id = static_cast<uint32_t>(command->GetInt("id"));
    auto it = g_js_dialogs.find(id);
    if (it == g_js_dialogs.end()) {
      /* The browser navigated or closed under the host's panel. Nothing left to
       * continue, and saying so is the whole reply. */
      EmitStatus("no-js-dialog");
      return;
    }

    JsDialog entry = std::move(it->second);
    g_js_dialogs.erase(it);
    auto by_browser = g_js_dialog_by_browser.find(entry.browser_id);
    if (by_browser != g_js_dialog_by_browser.end() && by_browser->second == id)
      g_js_dialog_by_browser.erase(by_browser);

    const bool ok = command->HasKey("ok") && command->GetBool("ok");
    const std::string value =
        command->HasKey("value") ? command->GetString("value").ToString()
                                 : std::string();
    if (entry.callback)
      entry.callback->Continue(ok, value);
    fprintf(stderr, "helper: js-dialog %u answered ok=%d value_bytes=%zu\n", id,
            ok ? 1 : 0, value.size());
    return;
  }

  if (name == "cdp-enable") {
    CefRefPtr<CefBrowser> target = g_browser;
    if (command->HasKey("browser")) {
      auto it = g_browser_slots.find(command->GetInt("browser"));
      if (it == g_browser_slots.end() || !it->second.browser) {
        EmitStatus("no-such-browser");
        return;
      }
      target = it->second.browser;
    }
    if (!target || !target->GetHost()) {
      EmitStatus("no-browser");
      return;
    }
    const int id = target->GetIdentifier();
    if (!g_cdp_registrations.count(id)) {
      g_cdp_registrations[id] =
          target->GetHost()->AddDevToolsMessageObserver(g_cdp_observer);
      fprintf(stderr, "helper: cdp-enable browser=%d\n", id);
    }
    return;
  }

  if (name == "cdp-disable") {
    CefRefPtr<CefBrowser> target = g_browser;
    if (command->HasKey("browser")) {
      auto it = g_browser_slots.find(command->GetInt("browser"));
      if (it == g_browser_slots.end() || !it->second.browser) {
        EmitStatus("no-such-browser");
        return;
      }
      target = it->second.browser;
    }
    if (target) {
      const int id = target->GetIdentifier();
      // Destroying the registration is the unregister; answers still owed
      // by this browser's backend simply never match a pending entry again.
      g_cdp_registrations.erase(id);
      fprintf(stderr, "helper: cdp-disable browser=%d\n", id);
    }
    return;
  }

  if (name == "cdp-execute") {
    CefRefPtr<CefBrowser> target = g_browser;
    if (command->HasKey("browser")) {
      auto it = g_browser_slots.find(command->GetInt("browser"));
      if (it == g_browser_slots.end() || !it->second.browser) {
        EmitStatus("no-such-browser");
        return;
      }
      target = it->second.browser;
    }
    if (!target || !target->GetHost()) {
      EmitStatus("no-browser");
      return;
    }
    if (!g_cdp_registrations.count(target->GetIdentifier())) {
      // The switch is the observer: without it a host could talk the whole
      // protocol by accident. Enabling is one explicit command.
      EmitStatus("cdp-not-enabled");
      return;
    }
    const std::string method = command->GetString("method").ToString();
    if (method.empty()) {
      EmitStatus("cdp-needs-method");
      return;
    }
    CefRefPtr<CefDictionaryValue> params;
    if (command->HasKey("params")) {
      if (command->GetType("params") == VTYPE_STRING) {
        CefString parse_error;
        CefRefPtr<CefValue> parsed = CefParseJSONAndReturnError(
            command->GetString("params"), JSON_PARSER_RFC, parse_error);
        if (!parsed || parsed->GetType() != VTYPE_DICTIONARY) {
          EmitStatus("cdp-params-unreadable");
          return;
        }
        params = parsed->GetDictionary();
      } else if (command->GetType("params") == VTYPE_DICTIONARY) {
        params = command->GetDictionary("params");
      }
    }
    const int host_id = command->HasKey("id") ? command->GetInt("id") : 0;
    const int message_id =
        target->GetHost()->ExecuteDevToolsMethod(0, method, params);
    if (message_id == 0) {
      EmitStatus("cdp-submit-failed");
      return;
    }
    g_cdp_pending[message_id] = host_id;
    fprintf(stderr, "helper: cdp-execute %s host_id=%d message_id=%d\n",
            method.c_str(), host_id, message_id);
    return;
  }

  if (name == "certificate-respond") {
    const uint32_t id = static_cast<uint32_t>(command->GetInt("id"));
    auto it = g_cert_dialogs.find(id);
    if (it == g_cert_dialogs.end()) {
      EmitStatus("no-certificate-error");
      return;
    }

    CertDialog entry = std::move(it->second);
    g_cert_dialogs.erase(it);
    const bool allow = command->HasKey("allow") && command->GetBool("allow");
    if (entry.callback) {
      // Continue() is the allow answer, Cancel() the deny: this CEF shape
      // carries the decision in which method runs, not in an argument.
      if (allow)
        entry.callback->Continue();
      else
        entry.callback->Cancel();
    }
    fprintf(stderr, "helper: certificate-error %u answered allow=%d\n", id,
            allow ? 1 : 0);
    return;
  }

  if (name == "certificate-authorize") {
    /* The host's policy store in one line: this fingerprint may continue for
     * as long as this helper lives. Nothing here outlives the process. */
    const std::string fingerprint = command->GetString("fingerprint").ToString();
    if (fingerprint.empty()) {
      EmitStatus("certificate-authorize-needs-fingerprint");
      return;
    }
    g_cert_authorized.insert(fingerprint);
    fprintf(stderr, "helper: certificate-authorize %s\n", fingerprint.c_str());
    return;
  }

  if (name == "intercept-add") {
    std::lock_guard<std::mutex> intercept_lock(g_intercept_mutex);
    // One rule replaces whatever the same id held: the host's list is the
    // whole truth per id, the way plugins-load is per plugin.
    const uint32_t id = static_cast<uint32_t>(command->GetInt("id"));
    InterceptRule rule;
    rule.id = id;
    if (command->GetType("patterns") == VTYPE_LIST) {
      CefRefPtr<CefListValue> patterns = command->GetList("patterns");
      for (size_t i = 0; i < patterns->GetSize(); ++i)
        rule.patterns.push_back(
            MatchPattern::Parse(patterns->GetString(i).ToString()));
    }
    if (command->GetType("resource_types") == VTYPE_LIST) {
      CefRefPtr<CefListValue> types = command->GetList("resource_types");
      for (size_t i = 0; i < types->GetSize(); ++i)
        rule.resource_types.insert(types->GetString(i).ToString());
    }
    if (rule.patterns.empty()) {
      EmitStatus("intercept-add-needs-patterns");
      return;
    }
    g_intercept_rules.erase(
        std::remove_if(g_intercept_rules.begin(), g_intercept_rules.end(),
                       [id](const InterceptRule& existing) {
                         return existing.id == id;
                       }),
        g_intercept_rules.end());
    g_intercept_rules.push_back(std::move(rule));
    fprintf(stderr, "helper: intercept-add %u rules=%zu\n", id,
            g_intercept_rules.size());
    return;
  }

  if (name == "intercept-remove") {
    std::lock_guard<std::mutex> intercept_lock(g_intercept_mutex);
    const uint32_t id = static_cast<uint32_t>(command->GetInt("id"));
    g_intercept_rules.erase(
        std::remove_if(g_intercept_rules.begin(), g_intercept_rules.end(),
                       [id](const InterceptRule& existing) {
                         return existing.id == id;
                       }),
        g_intercept_rules.end());
    fprintf(stderr, "helper: intercept-remove %u rules=%zu\n", id,
            g_intercept_rules.size());
    return;
  }

  if (name == "intercept-mode") {
    std::lock_guard<std::mutex> intercept_lock(g_intercept_mutex);
    g_intercept_passive =
        command->HasKey("passive") && command->GetBool("passive");
    fprintf(stderr, "helper: intercept-mode %s\n",
            g_intercept_passive ? "passive" : "blocking");
    return;
  }

  if (name == "request-respond") {
    std::lock_guard<std::mutex> intercept_lock(g_intercept_mutex);
    const uint32_t request_id =
        static_cast<uint32_t>(command->GetInt("request_id"));
    auto it = g_intercept_pending.find(request_id);
    if (it == g_intercept_pending.end()) {
      EmitStatus("no-such-request");
      return;
    }

    InterceptPending entry = std::move(it->second);
    g_intercept_pending.erase(it);
    const std::string action = command->HasKey("action")
                                   ? command->GetString("action").ToString()
                                   : "allow";
    if (action == "cancel") {
      if (entry.callback)
        entry.callback->Cancel();
    } else if (action == "redirect") {
      if (command->HasKey("url"))
        entry.request->SetURL(command->GetString("url"));
      if (entry.callback)
        entry.callback->Continue();
    } else {
      if (entry.callback)
        entry.callback->Continue();
    }
    fprintf(stderr, "helper: request-intercept %u answered %s\n", request_id,
            action.c_str());
    return;
  }

  if (name == "file-dialog-respond") {
    const uint32_t id = static_cast<uint32_t>(command->GetInt("id"));
    auto it = g_file_dialogs.find(id);
    if (it == g_file_dialogs.end()) {
      EmitStatus("no-file-dialog");
      return;
    }

    FileDialog entry = std::move(it->second);
    g_file_dialogs.erase(it);

    std::vector<CefString> paths;
    if (command->HasKey("paths") &&
        command->GetType("paths") == VTYPE_LIST) {
      CefRefPtr<CefListValue> list = command->GetList("paths");
      for (size_t i = 0; i < list->GetSize(); ++i) {
        if (list->GetType(i) == VTYPE_STRING)
          paths.push_back(list->GetString(i));
      }
    }

    /* An empty selection is the cancel answer, which is the one CEF's own
     * documentation gives it. */
    if (entry.callback) {
      if (paths.empty())
        entry.callback->Cancel();
      else
        entry.callback->Continue(paths);
    }
    fprintf(stderr, "helper: file-dialog %u answered %zu path(s)\n", id,
            paths.size());
    return;
  }

  if (name == "a11y") {
    const bool enabled = command->HasKey("enabled") && command->GetBool("enabled");
    if (g_browser && g_browser->GetHost()) {
      g_browser->GetHost()->SetAccessibilityState(
          enabled ? STATE_ENABLED : STATE_DEFAULT);
      fprintf(stderr, "helper: browser accessibility = %s (host)\n",
              enabled ? "enabled" : "default");
    }
    return;
  }

  if (name == "devtools") {
    g_devtools_enabled =
        command->HasKey("enabled") && command->GetBool("enabled");
    fprintf(stderr, "helper: devtools %s\n",
            g_devtools_enabled ? "enabled" : "disabled");
    return;
  }

  if (name == "js" || name == "js-expr") {
    const std::string code = command->GetString("code").ToString();
    if (code.empty()) {
      EmitStatus("js-needs-code");
      return;
    }
    CefRefPtr<CefBrowser> target = g_browser;
    if (command->HasKey("browser")) {
      auto it = g_browser_slots.find(command->GetInt("browser"));
      if (it == g_browser_slots.end() || !it->second.browser) {
        EmitStatus("no-such-browser");
        return;
      }
      target = it->second.browser;
    }
    CefRefPtr<CefFrame> frame = target->GetMainFrame();
    if (!frame) {
      EmitStatus("no-main-frame");
      return;
    }
    const int id = command->HasKey("id") ? command->GetInt("id") : 0;
    frame->ExecuteJavaScript(WrapScript(code, id, name == "js-expr"),
                             "cefnest-helper", 0);
    // The answer comes back from the renderer as a js-result event. Nothing is
    // emitted here: a navigation away from the frame can lose the script, and
    // an ack would promise a delivery the page has not confirmed.
    return;
  }

  fprintf(stderr, "helper: unknown command '%s'\n", name.c_str());
  EmitStatus("unknown-command");
}

// Runs in the renderer subprocess. The report function is one-way on purpose:
// the host wants the value, not a callback inside the page.
class ReportHandler : public CefV8Handler {
 public:
  bool Execute(const CefString& name,
               CefRefPtr<CefV8Value> object,
               const CefV8ValueList& arguments,
               CefRefPtr<CefV8Value>& retval,
               CefString& exception) override {
    if (arguments.size() != 1 || !arguments[0]->IsString()) {
      exception = "expected the result as one JSON string";
      return true;
    }

    CefRefPtr<CefV8Context> context = CefV8Context::GetCurrentContext();
    CefRefPtr<CefFrame> frame = context ? context->GetFrame() : nullptr;
    if (!frame) {
      exception = "no frame to report the js result from";
      return true;
    }

    CefRefPtr<CefProcessMessage> message =
        CefProcessMessage::Create(kJsResultMessage);
    message->GetArgumentList()->SetString(0, arguments[0]->GetStringValue());
    frame->SendProcessMessage(PID_BROWSER, message);

    retval = CefV8Value::CreateUndefined();
    return true;
  }

  IMPLEMENT_REFCOUNTING(ReportHandler);
};

// The page → host half of the message channel: a page calls
// cefnestPostMessage(JSON.stringify(msg)) and the string rides a
// CefProcessMessage to the browser process, which forwards it to the host as
// a page-message event. The payload stays one opaque JSON string — the host
// owns whatever schema lives inside it.
class PostMessageHandler : public CefV8Handler {
 public:
  bool Execute(const CefString& name, CefRefPtr<CefV8Value> object,
               const CefV8ValueList& arguments, CefRefPtr<CefV8Value>& retval,
               CefString& exception) override {
    if (arguments.size() != 1 || !arguments[0]->IsString()) {
      exception = "expected the message as one JSON string";
      return true;
    }

    CefRefPtr<CefV8Context> context = CefV8Context::GetCurrentContext();
    CefRefPtr<CefFrame> frame = context ? context->GetFrame() : nullptr;
    if (!frame) {
      exception = "no frame to send the message from";
      return true;
    }

    CefRefPtr<CefProcessMessage> message =
        CefProcessMessage::Create(kPageMessageProcess);
    message->GetArgumentList()->SetString(0, arguments[0]->GetStringValue());
    frame->SendProcessMessage(PID_BROWSER, message);

    retval = CefV8Value::CreateUndefined();
    return true;
  }

  IMPLEMENT_REFCOUNTING(PostMessageHandler);
};

class PrimarySelectionHandler : public CefV8Handler {
 public:
  bool Execute(const CefString& name, CefRefPtr<CefV8Value> object,
               const CefV8ValueList& arguments, CefRefPtr<CefV8Value>& retval,
               CefString& exception) override {
    if (arguments.size() != 1 || !arguments[0]->IsString()) {
      exception = "expected the selected text as one string";
      return true;
    }

    CefRefPtr<CefV8Context> context = CefV8Context::GetCurrentContext();
    CefRefPtr<CefFrame> frame = context ? context->GetFrame() : nullptr;
    if (!frame) {
      exception = "no frame to send the selection from";
      return true;
    }

    CefRefPtr<CefProcessMessage> message =
        CefProcessMessage::Create(kPrimarySelectionProcess);
    message->GetArgumentList()->SetString(0, arguments[0]->GetStringValue());
    frame->SendProcessMessage(PID_BROWSER, message);
    retval = CefV8Value::CreateUndefined();
    return true;
  }

  IMPLEMENT_REFCOUNTING(PrimarySelectionHandler);
};

int64_t NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// ---------------------------------------------------------------- render --
// The renderer's copy of the plugin table. One renderer process, one main
// thread: the table arrives on it, contexts are created on it, and nothing
// here is touched from anywhere else.
struct RendererScript {
  std::vector<MatchPattern> matches;
  std::vector<std::string> js;  // file paths; the text is fetched on demand
  std::string run_at;
};
struct RendererPlugin {
  std::string id;
  std::vector<RendererScript> scripts;
};
std::vector<RendererPlugin> g_renderer_plugins;
bool g_table_requested = false;  // per renderer process

void LoadRendererTable(const std::string& json) {
  g_renderer_plugins.clear();
  g_table_requested = true;  // whatever asked is answered
  CefString parse_error;
  CefRefPtr<CefValue> value =
      CefParseJSONAndReturnError(json, JSON_PARSER_RFC, parse_error);
  if (!value || value->GetType() != VTYPE_DICTIONARY) {
    fprintf(stderr, "helper: plugin table unreadable: %s\n",
            parse_error.ToString().c_str());
    return;
  }
  CefRefPtr<CefListValue> plugins =
      value->GetDictionary()->GetList("plugins");
  for (size_t i = 0; i < plugins->GetSize(); ++i) {
    CefRefPtr<CefDictionaryValue> plugin = plugins->GetDictionary(i);
    if (!plugin)
      continue;
    RendererPlugin parsed;
    parsed.id = plugin->GetString("id").ToString();
    CefRefPtr<CefListValue> scripts = plugin->GetList("scripts");
    for (size_t j = 0; j < scripts->GetSize(); ++j) {
      CefRefPtr<CefDictionaryValue> script = scripts->GetDictionary(j);
      if (!script)
        continue;
      RendererScript one;
      CefRefPtr<CefListValue> matches = script->GetList("matches");
      for (size_t k = 0; k < matches->GetSize(); ++k) {
        MatchPattern pattern =
            MatchPattern::Parse(matches->GetString(k).ToString());
        if (pattern.Usable())
          one.matches.push_back(pattern);
      }
      CefRefPtr<CefListValue> files = script->GetList("js");
      for (size_t k = 0; k < files->GetSize(); ++k)
        one.js.push_back(files->GetString(k).ToString());
      one.run_at = script->GetString("run_at").ToString();
      if (!one.js.empty())
        parsed.scripts.push_back(one);
    }
    g_renderer_plugins.push_back(parsed);
  }
}

// The shim, one instance per plugin. The id rides in the text: it is what
// chrome.runtime.id answers and what every bridge call carries back.
std::string MakePluginShim(const std::string& id) {
  std::string text = kPluginShimScript;
  const std::string marker = "__ID__";
  const std::string::size_type at = text.find(marker);
  if (at != std::string::npos)
    text.replace(at, marker.size(), id);
  return text;
}

// The bridge: one host function on the global, taking one JSON text. What is
// inside — which plugin asks, what for — is the shim's business; this side
// only carries it to the browser process, which re-checks the size before
// the host ever sees it.
class PluginBridgeHandler : public CefV8Handler {
 public:
  bool Execute(const CefString& name, CefRefPtr<CefV8Value> object,
               const CefV8ValueList& arguments, CefRefPtr<CefV8Value>& retval,
               CefString& exception) override {
    if (arguments.size() < 1 || !arguments[0]->IsString()) {
      exception = "__cefnestPlugin takes one JSON string";
      return false;
    }
    // One handler serves every context that installed the bridge, so the
    // message leaves through whichever frame is executing right now — the
    // handler runs inside the caller's context.
    CefRefPtr<CefV8Context> context = CefV8Context::GetCurrentContext();
    if (!context || !context->GetFrame()) {
      exception = "__cefnestPlugin has no frame to send from";
      return false;
    }
    CefRefPtr<CefProcessMessage> message =
        CefProcessMessage::Create(kPluginMessageProcess);
    message->GetArgumentList()->SetString(0, arguments[0]->GetStringValue());
    context->GetFrame()->SendProcessMessage(PID_BROWSER, message);
    retval = CefV8Value::CreateBool(true);
    return true;
  }

  IMPLEMENT_REFCOUNTING(PluginBridgeHandler);
};

CefRefPtr<PluginBridgeHandler> g_plugin_bridge{new PluginBridgeHandler};

// What this context gets, and why: an extension page of a plugin gets the
// shim (chrome.* exists there the way Chrome puts it in extension contexts);
// a page one of a plugin's content scripts matches gets the shim plus that
// plugin's document_start scripts. A page nothing matches gets nothing — no
// bridge, no chrome global, no machinery a page could poke at. The timing is
// the P0-measured one: everything here lands before the page's first inline
// script, because OnContextCreated runs before any of it.
void InjectPlugins(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                   CefRefPtr<CefV8Context> context) {
  if (g_renderer_plugins.empty()) {
    // The backstop: a renderer this side never pushed to asks for the table
    // itself. The answer cannot land in time for THIS context's
    // document_start — it arms the process for every page after — and it is
    // the reason a table too big for the command line degrades rather than
    // disappears.
    if (!g_table_requested && browser) {
      g_table_requested = true;
      CefRefPtr<CefProcessMessage> request =
          CefProcessMessage::Create(kPluginsRequestProcess);
      browser->GetMainFrame()->SendProcessMessage(PID_BROWSER, request);
    }
    return;
  }
  const int64_t start = NowUs();
  const std::string url = frame->GetURL().ToString();
  const bool extension_page = url.rfind(kExtensionSchemePrefix, 0) == 0;
  std::string page_plugin_id;
  if (extension_page) {
    std::string rest = url.substr(sizeof(kExtensionSchemePrefix) - 1);
    const std::string::size_type slash = rest.find('/');
    page_plugin_id = slash == std::string::npos ? rest : rest.substr(0, slash);
  }

  context->GetGlobal()->SetValue(
      kPluginBridgeFunction,
      CefV8Value::CreateFunction(kPluginBridgeFunction, g_plugin_bridge),
      static_cast<cef_v8_propertyattribute_t>(
          V8_PROPERTY_ATTRIBUTE_READONLY | V8_PROPERTY_ATTRIBUTE_DONTENUM |
          V8_PROPERTY_ATTRIBUTE_DONTDELETE));
  int count = 0;
  for (const auto& plugin : g_renderer_plugins) {
    std::vector<const RendererScript*> hits;
    if (!extension_page) {
      for (const auto& script : plugin.scripts) {
        bool hit = false;
        for (const auto& pattern : script.matches)
          if (pattern.Matches(url)) { hit = true; break; }
        if (hit)
          hits.push_back(&script);
      }
      if (hits.empty())
        continue;
    }
    {
      // Eval, not ExecuteJavaScript: the queued form races the page's own
      // parser and can lose. Eval runs the text synchronously inside the
      // context OnContextCreated just handed us — the same timing the
      // P0-measured bridge installs used. An exception in a plugin's script
      // is that plugin's problem, not the page's — logged, never thrown on.
      CefRefPtr<CefV8Value> retval;
      CefRefPtr<CefV8Exception> exception;
      if (!context->Eval(MakePluginShim(plugin.id), "cefnest-plugin-shim", 0,
                         retval, exception)) {
        fprintf(stderr, "helper: plugin shim failed: %s\n",
                exception ? exception->GetMessage().ToString().c_str()
                          : "unknown");
      }
      count++;
    }
    if (extension_page)
      continue;
    for (const RendererScript* script : hits) {
      if (script->run_at != "document_start")
        continue;  // document_idle and document_end ride OnLoadEnd
      for (const auto& path : script->js) {
        // The script's bytes come from the scheme handler, synchronously:
        // the renderer's sandbox cannot read the disk, the table that
        // reaches this process stays path-sized (a 50 KiB switch once
        // SIGTRAP'd the zygote's fork — measured), and a fetch from the
        // context being created is the one shape that keeps document_start
        // honest at any script size. The scheme's CORS flags and the
        // factory's Access-Control-Allow-Origin are what make this reachable
        // from a page origin (P0 metric 1).
        const std::string url = std::string(kExtensionSchemePrefix) +
                                plugin.id + "/" + path;
        CefRefPtr<CefV8Value> fetched;
        {
          const std::string loader =
              std::string("(function(){var x=new XMLHttpRequest();") +
              "x.open(\"GET\",\""
                  "cefnest-extension://" + plugin.id + "/" + path +
              "\",false);x.send();return x.responseText;})()";
          CefRefPtr<CefV8Exception> exception;
          if (!context->Eval(loader, "cefnest-plugin-fetch", 0, fetched,
                             exception)) {
            fprintf(stderr,
                    "helper: cs fetch failed (%s): %s\n", plugin.id.c_str(),
                    exception ? exception->GetMessage().ToString().c_str()
                              : "unknown");
            continue;
          }
        }
        if (!fetched || !fetched->IsString() ||
            fetched->GetStringValue().empty()) {
          fprintf(stderr, "helper: cs fetch empty (%s): %s\n",
                  plugin.id.c_str(), url.c_str());
          continue;
        }
        CefRefPtr<CefV8Value> script_retval;
        CefRefPtr<CefV8Exception> exception;
        if (!context->Eval(fetched->GetStringValue(), url, 0, script_retval,
                           exception))
          fprintf(stderr, "helper: cs eval failed (%s): %s\n",
                  plugin.id.c_str(),
                  exception ? exception->GetMessage().ToString().c_str()
                            : "unknown");
        count++;
      }
    }
  }
  if (count > 0)
    fprintf(stderr,
            "helper: cs inject browser=%d frame=%s scripts=%d us=%lld\n",
            browser ? browser->GetIdentifier() : 0,
            frame->GetIdentifier().ToString().c_str(), count,
            static_cast<long long>(NowUs() - start));
}

class HelperRenderProcessHandler : public CefRenderProcessHandler {
 public:
  void OnWebKitInitialized() override {
    /* A zygote-forked renderer never re-runs main() or the command-line
       hooks — but its fork command line does carry what the browser
       attached at OnBeforeChildProcessLaunch, and WebKit initializes here,
       per process, before any page context exists. This is where
       document_start is won or lost. */
    CefRefPtr<CefCommandLine> command_line =
        CefCommandLine::GetGlobalCommandLine();
    if (command_line->HasSwitch(kPluginsTableSwitch)) {
      CefRefPtr<CefBinaryValue> decoded =
          CefBase64Decode(command_line->GetSwitchValue(kPluginsTableSwitch));
      if (decoded) {
        std::vector<char> bytes(decoded->GetSize());
        decoded->GetData(bytes.data(), bytes.size(), 0);
        LoadRendererTable(std::string(bytes.data(), bytes.size()));
      }
    }
  }

  void OnContextCreated(CefRefPtr<CefBrowser> browser,
                        CefRefPtr<CefFrame> frame,
                        CefRefPtr<CefV8Context> context) override {
    // DONTENUM keeps the function out of for-in loops and Object.keys(), so a
    // page walking its own globals does not trip over the host. READONLY and
    // DONTDELETE make the channel reliable: a page that replaced or deleted the
    // function would turn every js command into silence, and a missing answer is
    // indistinguishable from a script that never finished.
    context->GetGlobal()->SetValue(
        kReportFunction,
        CefV8Value::CreateFunction(kReportFunction, handler_),
        static_cast<cef_v8_propertyattribute_t>(
            V8_PROPERTY_ATTRIBUTE_READONLY | V8_PROPERTY_ATTRIBUTE_DONTENUM |
            V8_PROPERTY_ATTRIBUTE_DONTDELETE));
    context->GetGlobal()->SetValue(
        kPostMessageFunction,
        CefV8Value::CreateFunction(kPostMessageFunction, post_handler_),
        static_cast<cef_v8_propertyattribute_t>(
            V8_PROPERTY_ATTRIBUTE_READONLY | V8_PROPERTY_ATTRIBUTE_DONTENUM |
            V8_PROPERTY_ATTRIBUTE_DONTDELETE));
    context->GetGlobal()->SetValue(
        kPrimarySelectionFunction,
        CefV8Value::CreateFunction(kPrimarySelectionFunction, primary_handler_),
        static_cast<cef_v8_propertyattribute_t>(
            V8_PROPERTY_ATTRIBUTE_READONLY | V8_PROPERTY_ATTRIBUTE_DONTENUM |
            V8_PROPERTY_ATTRIBUTE_DONTDELETE));

    InjectPlugins(browser, frame, context);
  }

  bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                CefRefPtr<CefFrame> frame,
                                CefProcessId source_process,
                                CefRefPtr<CefProcessMessage> message) override {
    if (source_process == PID_BROWSER &&
        message->GetName() == kPluginsTableProcess) {
      LoadRendererTable(message->GetArgumentList()->GetString(0).ToString());
      return true;
    }

    if (source_process == PID_BROWSER &&
        message->GetName() == kPluginResponseProcess) {
      // The host answered one call. Delivered to the main frame: the shim
      // holding the reply id is usually there. A shim inside a subframe
      // waits forever for its reply — CEF 156 has no GetFrame(id) to reach
      // one, and docs/plugins.md records that as the known limit it is.
      const auto& args = message->GetArgumentList();
      CefRefPtr<CefValue> value = CefValue::Create();
      value->SetString(args->GetString(2));
      const std::string js =
          std::string("window.__cefnestDeliver&&__cefnestDeliver(") +
          std::to_string(args->GetInt(0)) + "," +
          (args->GetBool(1) ? "true" : "false") + "," +
          CefWriteJSON(value, JSON_WRITER_DEFAULT).ToString() + ");";
      browser->GetMainFrame()->ExecuteJavaScript(js, "cefnest-plugin", 0);
      return true;
    }

    if (source_process == PID_BROWSER &&
        message->GetName() == kPluginsRequestProcess)
      return false;  // never sent renderer-ward; nothing to do

    return false;
  }

 private:
  CefRefPtr<CefV8Handler> handler_{new ReportHandler};
  CefRefPtr<CefV8Handler> post_handler_{new PostMessageHandler};
  CefRefPtr<CefV8Handler> primary_handler_{new PrimarySelectionHandler};

  IMPLEMENT_REFCOUNTING(HelperRenderProcessHandler);
};

class HelperApp : public CefApp,
                  public CefBrowserProcessHandler {
 public:
  CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override {
    return render_handler_;
  }

  CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override {
    return this;
  }

  /* Runs in every process — browser, renderer, everything CefExecuteProcess
   * hands back a code for — and the registration set has to match across
   * them, which is CEF's own hard rule: a renderer that registers a
   * different set than the browser process shows the failure as "scripts
   * inject fine but every extension resource 404s". The option set is the
   * one the plugin plan measured (docs/plans/plugin-plan-2026-10-25.md §〇):
   * XHR needs CORS_ENABLED or the request never reaches the handler at all,
   * fetch() needs FETCH_ENABLED or it throws before that, and
   * STANDARD|SECURE is what gives extension pages a normal origin.
   * CSP_BYPASSING is deliberately absent — reaching its own resources is
   * reachability, not a licence to ignore a page's CSP. */
  void OnRegisterCustomSchemes(
      CefRawPtr<CefSchemeRegistrar> registrar) override {
    const int options = CEF_SCHEME_OPTION_STANDARD | CEF_SCHEME_OPTION_SECURE |
                        CEF_SCHEME_OPTION_CORS_ENABLED |
                        CEF_SCHEME_OPTION_FETCH_ENABLED;
    if (!registrar->AddCustomScheme(kExtensionScheme, options))
      fprintf(stderr, "helper: extension scheme registration failed\n");
  }

  /* Browser side, at child spawn: every renderer starts knowing the table.
   * The gpu and utility children get nothing — the switch is only for
   * renderers, and zygote-forked renderers carry it the same way their other
   * switches travel. */
  void OnBeforeChildProcessLaunch(
      CefRefPtr<CefCommandLine> command_line) override {
    if (command_line->GetSwitchValue("type").ToString() != "renderer")
      return;
    CefRefPtr<CefValue> table = BuildRendererTable();
    const std::string json =
        CefWriteJSON(table, JSON_WRITER_DEFAULT).ToString();
    const CefString encoded = CefBase64Encode(json.data(), json.size());
    if (encoded.ToString().size() > kPluginsTableSwitchMax) {
      fprintf(stderr,
              "helper: plugin table of %zu bytes too big for the renderer "
              "command line; new renderers ride the first-context request\n",
              json.size());
      return;
    }
    command_line->AppendSwitchWithValue(kPluginsTableSwitch, encoded);
  }

  /* Renderer side, at process start — before any context exists, which is
   * exactly what document_start needs. */
  void OnBeforeCommandLineProcessing(
      const CefString& process_type,
      CefRefPtr<CefCommandLine> command_line) override {
    if (!process_type.empty())
      return;  // Browser process only.

    // No --no-sandbox here on purpose. CefSettings.no_sandbox alone reaches the
    // whole process tree: with CHROMIUM_HELPER_NO_SANDBOX=1 all seven
    // subprocesses of a hosted run read Seccomp=0 in /proc. Appending the
    // switch a second time would only give the two paths a chance to drift.

    // Native Wayland, unless the host picked a platform on purpose.
    if (!command_line->HasSwitch("ozone-platform")) {
      command_line->AppendSwitchWithValue("ozone-platform", "wayland");
      fprintf(stderr, "helper: forcing --ozone-platform=wayland\n");
    }

    // Assistive tech: Chromium enables its accessibility tree when it sees an
    // AT-SPI client, but detection in an embedded helper is unreliable — make
    // it deterministic for hosts that know a screen reader is in play.
    if (EnvOrEmpty("CHROMIUM_A11Y") == "1")
      {
        command_line->AppendSwitch("force-renderer-accessibility");
        fprintf(stderr, "helper: accessibility forced on\n");
      }

    if (EnvOrEmpty("CHROMIUM_HELPER_DISABLE_GPU") == "1") {
      command_line->AppendSwitch("disable-gpu");
      fprintf(stderr, "helper: --disable-gpu\n");
    }

    // CEF has no runtime proxy API: the switch has to be in place before
    // CefInitialize, and this callback is the one hook with the command line
    // in hand. The host unsets both variables when it has no proxy to name.
    const std::string proxy = EnvOrEmpty("CHROMIUM_HELPER_PROXY");
    if (!proxy.empty()) {
      command_line->AppendSwitchWithValue("proxy-server", proxy);
      fprintf(stderr, "helper: --proxy-server=%s\n", proxy.c_str());
    }
    const std::string proxy_bypass = EnvOrEmpty("CHROMIUM_HELPER_PROXY_BYPASS");
    if (!proxy_bypass.empty()) {
      command_line->AppendSwitchWithValue("proxy-bypass-list", proxy_bypass);
      fprintf(stderr, "helper: --proxy-bypass-list=%s\n", proxy_bypass.c_str());
    }

    if (EnvOrEmpty("CHROMIUM_SPELLCHECK") != "1") {
      command_line->AppendSwitch("disable-spell-checking");
      fprintf(stderr, "helper: spellcheck disabled (CHROMIUM_SPELLCHECK=1 enables)\n");
    }
  }

 private:
  CefRefPtr<CefRenderProcessHandler> render_handler_{
      new HelperRenderProcessHandler};

  IMPLEMENT_REFCOUNTING(HelperApp);
};

// document_idle and document_end content scripts run from here: the load has
// committed, the DOM is there, and the browser process is the side that knows
// OnLoadEnd. Extension pages get nothing — a plugin does not inject into its
// own pages.
void InjectIdleScripts(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame) {
  const std::string url = frame->GetURL().ToString();
  if (url.rfind(kExtensionSchemePrefix, 0) == 0)
    return;
  std::lock_guard<std::mutex> lock(g_plugin_mutex);
  for (const auto& entry : g_plugin_table) {
    for (const auto& script : entry.second.scripts) {
      if (script.run_at == "document_start")
        continue;  // rode OnContextCreated in the renderer
      bool hit = false;
      for (const auto& pattern : script.matches)
        if (pattern.Matches(url)) { hit = true; break; }
      if (!hit)
        continue;
      for (const auto& text : script.js_text)
        frame->ExecuteJavaScript(text, entry.first.c_str(), 0);
    }
  }
}

class HelperClient : public CefClient,
                     public CefLifeSpanHandler,
                     public CefLoadHandler,
                     public CefDisplayHandler,
                     public CefFocusHandler,
                     public CefKeyboardHandler,
                     public CefRequestHandler {
 public:
  explicit HelperClient(int slot) : slot_(slot) {}

  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override {
    return this;
  }
  CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
  CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
  CefRefPtr<CefFocusHandler> GetFocusHandler() override { return this; }
  CefRefPtr<CefKeyboardHandler> GetKeyboardHandler() override { return this; }
  CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }
  CefRefPtr<CefDownloadHandler> GetDownloadHandler() override {
    return download_handler_;
  }
  CefRefPtr<CefFindHandler> GetFindHandler() override { return find_handler_; }
  CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override {
    return context_menu_handler_;
  }
  CefRefPtr<CefPermissionHandler> GetPermissionHandler() override {
    return permission_handler_;
  }
  CefRefPtr<CefJSDialogHandler> GetJSDialogHandler() override {
    return jsdialog_handler_;
  }
  CefRefPtr<CefDialogHandler> GetDialogHandler() override {
    return dialog_handler_;
  }
  CefRefPtr<CefPrintHandler> GetPrintHandler() override {
    return print_handler_;
  }

  void OnGotFocus(CefRefPtr<CefBrowser> browser) override {
    /* Clicking a page gives its view focus: make that browser the active one,
     * so host-drawn menus and plain commands follow the window the user is
     * looking at. The host sees ::browser-selected before the ::context-menu
     * that the same click goes on to request. Idempotent by the guard in
     * SelectBrowserById. */
    SelectBrowserById(browser->GetIdentifier());
  }

  void OnTakeFocus(CefRefPtr<CefBrowser> browser, bool next) override {
    /* The inspector's own focus walk is not the page's; a host moving its
     * widget focus for it would fight the DevTools frontend. */
    if (BrowserSlot* slot = FindSlotForBrowser(browser))
      if (slot->devtools)
        return;

    /* Blink's sequential traversal ran off the edge of the document: the
     * walk belongs to the host now, and |next| says which way it was going.
     * The host answers by moving focus to the widget after (backwards:
     * before) the view; CEF restarts the page's traversal when the focus
     * comes back. */
    CefRefPtr<CefDictionaryValue> dict = NewEvent("take-focus");
    dict->SetInt("browser", browser->GetIdentifier());
    dict->SetBool("next", next);
    Emit(dict);
  }

  bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                CefRefPtr<CefFrame> frame,
                                CefProcessId source_process,
                                CefRefPtr<CefProcessMessage> message) override {
    if (source_process == PID_RENDERER &&
        message->GetName() == kPageMessageProcess)
      {
        CefRefPtr<CefDictionaryValue> dict = NewEvent(kPageMessageEvent);
        dict->SetString("json", message->GetArgumentList()->GetString(0));
        Emit(dict);
        return true;
      }

    if (source_process == PID_RENDERER &&
        message->GetName() == kPrimarySelectionProcess)
      {
        CefRefPtr<CefDictionaryValue> dict = NewEvent(kPrimarySelectionEvent);
        dict->SetString("text", message->GetArgumentList()->GetString(0));
        Emit(dict);
        return true;
      }

    if (source_process == PID_RENDERER &&
        message->GetName() == kPluginsRequestProcess) {
      // The safety net: this renderer's first context just arrived and it
      // wants the table in case the push raced its channel. Answer with the
      // whole table — the same bytes a change would push.
      if (frame)
        PushPluginsTableToBrowserFrame(browser, frame);
      return true;
    }

    if (source_process == PID_RENDERER &&
        message->GetName() == kPluginMessageProcess) {
      const std::string payload =
          message->GetArgumentList()->GetString(0).ToString();
      if (payload.size() > kPluginPayloadMaxBytes) {
        fprintf(stderr,
                "helper: plugin message of %zu bytes refused (payload ceiling)\n",
                payload.size());
        return true;
      }
      CefString parse_error;
      CefRefPtr<CefValue> parsed =
          CefParseJSONAndReturnError(payload, JSON_PARSER_RFC, parse_error);
      if (!parsed || parsed->GetType() != VTYPE_DICTIONARY) {
        fprintf(stderr, "helper: plugin message not an object: %s\n",
                parse_error.ToString().c_str());
        return true;
      }
      CefRefPtr<CefDictionaryValue> call = parsed->GetDictionary();
      CefRefPtr<CefDictionaryValue> dict = NewEvent("plugin-message");
      if (browser)
        dict->SetInt("browser", browser->GetIdentifier());
      dict->SetInt("reply_id", call->GetInt("reply"));
      dict->SetString("id", call->GetString("id"));
      dict->SetString("api", call->GetString("api"));
      CefRefPtr<CefValue> args =
          call->HasKey("args") ? call->GetValue("args") : nullptr;
      dict->SetString("args", args ? CefWriteJSON(args, JSON_WRITER_DEFAULT).ToString()
                                   : "null");
      Emit(dict);
      return true;
    }

    if (source_process != PID_RENDERER || message->GetName() != kJsResultMessage)
      return false;

    EmitJsResult(message->GetArgumentList()->GetString(0));
    return true;
  }

  void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
    CEF_REQUIRE_UI_THREAD();
    auto pending = g_slot_to_browser.find(slot_);
    BrowserSlot* slot = nullptr;
    if (pending != g_slot_to_browser.end()) {
      auto it = g_browser_slots.find(pending->second);
      if (it != g_browser_slots.end())
        slot = &it->second;
      g_slot_to_browser.erase(pending);
    }
    /* ShowDevTools content: CEF created this browser itself, so there is no
     * request slot for it. The frontend URL is the one reliable mark. Give it
     * a slot anyway — without one its close would still reach the host, but
     * nothing could find or describe it in the meantime. */
    if (!slot) {
      CefRefPtr<CefFrame> frame = browser->GetMainFrame();
      const std::string url = frame ? frame->GetURL().ToString() : std::string();
      const bool pending = g_devtools_creating > 0;
      if (url.rfind("devtools://", 0) == 0 || pending) {
        if (pending)
          --g_devtools_creating;
        BrowserSlot entry;
        entry.slot = browser->GetIdentifier();
        entry.windowed = true;
        entry.devtools = true;
        entry.browser = browser;
        if (!url.empty())
          entry.start_url = url;
        g_browser_slots[entry.slot] = entry;
        slot = &g_browser_slots[entry.slot];
        fprintf(stderr, "helper: devtools browser %d adopted\n",
                browser->GetIdentifier());
      }
    }
    /* Compare the request-side slot with what the host asked to select before
     * the entry is re-keyed: after the re-key, `slot->slot` is a CEF browser
     * id, and the two counters only stay equal until the first creation
     * failure. Reading it afterwards leaves every later browser unselected.
     * A DevTools browser is never what a host asked to select, and neither is
     * a background page. */
    bool select_this = slot && !slot->devtools && !slot->background &&
                       slot->slot == g_select_slot;
    if (slot) {
      /* The map starts under the request slot so commands can find the view
       * before CEF has finished creating the browser. Copy before erasing:
       * the erase destroys the value that `slot` points at. */
      BrowserSlot entry = *slot;
      g_browser_slots.erase(entry.slot);
      entry.slot = browser->GetIdentifier();
      entry.browser = browser;
      g_browser_slots[entry.slot] = entry;
      slot = &g_browser_slots[entry.slot];
    }
    if (!g_browser || select_this)
      SelectBrowserById(browser->GetIdentifier());
    else if (slot && slot->view)
      slot->view->SetVisible(false);
    if (select_this)
      g_select_slot = 0;

    fprintf(stderr,
            "helper: browser created, id=%d slot=%d windowed=%d devtools=%d\n",
            browser->GetIdentifier(), slot ? slot->slot : 0,
            slot ? slot->windowed : 0, slot ? slot->devtools : 0);

    CefRefPtr<CefDictionaryValue> created = NewEvent("browser-created");
    created->SetInt("browser", browser->GetIdentifier());
    created->SetString("url", slot && !slot->start_url.empty()
                                  ? slot->start_url
                                  : browser->GetMainFrame()->GetURL().ToString());
    created->SetBool("windowed", slot ? slot->windowed : false);
    created->SetBool("devtools", slot ? slot->devtools : false);
    created->SetBool("background", slot ? slot->background : false);
    Emit(created);

    if (slot && slot->background && browser->GetHost()) {
      /* Chromium's own hidden-page bit: timers and network keep running,
       * rendering settles to the hidden-tab schedule. This is the "existing
       * mechanism, no new one" shape the plan asked the background page to
       * take on this side. */
      browser->GetHost()->WasHidden(true);
    }

    /* The first browser still carries the original ready handshake; later
     * browser-created events are enough for a host that already knows the
     * helper. */
    if (g_browser_slots.size() == 1) {
      CefRefPtr<CefDictionaryValue> dict = NewEvent("ready");
      dict->SetString("cef", CEF_VERSION);
      dict->SetInt("pid", static_cast<int>(::getpid()));
      dict->SetString("url", browser->GetMainFrame()->GetURL().ToString());
      Emit(dict);
    }

    // Deterministic accessibility enablement: a windowed browser in
    // STATE_ENABLED runs Chromium's complete mode — all platform (AT-SPI)
    // accessibility objects are created and managed by Chromium itself. The
    // client only has to detect a screen reader and call this; hosts can do
    // that from their side, the env knob makes it explicit here.
    if (EnvOrEmpty("CHROMIUM_A11Y") == "1")
      {
        browser->GetHost()->SetAccessibilityState(STATE_ENABLED);
        fprintf(stderr, "helper: browser accessibility = enabled\n");
      }

    // The new renderer gets the table now, before its first navigation can
    // commit: OnContextCreated may fire in a renderer this push has not
    // reached, which is what the renderer's own request is the net for.
    PushPluginsTable(browser);

    if (g_auto_quit_ms > 0) {
      fprintf(stderr, "helper: auto-quit in %ld ms\n", g_auto_quit_ms);
      CefPostDelayedTask(TID_UI, new CloseTask(browser), g_auto_quit_ms);
    }
  }

  // A single-view component cannot show a second toplevel: a popup the page
  // opens (target=_blank, window.open, middle click) navigates this view to
  // the target instead — the kiosk-style in-place answer. Background tabs are
  // the one exception: they asked not to take over the view, so they are only
  // reported. The host hears about every request either way.
  bool OnBeforePopup(CefRefPtr<CefBrowser> browser,
                     CefRefPtr<CefFrame> frame,
                     int popup_id,
                     const CefString& target_url,
                     const CefString& target_frame_name,
                     cef_window_open_disposition_t target_disposition,
                     bool user_gesture,
                     const CefPopupFeatures& popupFeatures,
                     CefWindowInfo& windowInfo,
                     CefRefPtr<CefClient>& client,
                     CefBrowserSettings& settings,
                     CefRefPtr<CefDictionaryValue>& extra_info,
                     bool* no_javascript_access) override {
    fprintf(stderr, "helper: popup requested -> %s (disposition %d)\n",
            target_url.ToString().c_str(), (int) target_disposition);

    CefRefPtr<CefDictionaryValue> dict = NewEvent("popup-requested");
    dict->SetString("url", target_url);
    dict->SetInt("disposition", (int) target_disposition);
    dict->SetBool("gesture", user_gesture ? true : false);
    /* NewEvent() names the active browser. A popup can be requested by a
     * background one, and the host needs to know which tab opened it. */
    if (browser)
      dict->SetInt("browser", browser->GetIdentifier());
    Emit(dict);

    /* Navigate the browser that asked, not whichever browser is active: a
     * window.open() in a background tab must not replace the page the user is
     * looking at. The parameter is guaranteed by CEF, but its frame is not,
     * and dereferencing a NULL frame here would take the helper down. */
    if (target_disposition != CEF_WOD_NEW_BACKGROUND_TAB && browser) {
      CefRefPtr<CefFrame> frame = browser->GetMainFrame();
      if (frame)
        frame->LoadURL(target_url);
    }

    return true;
  }

  /* The default answer is "no", Chrome's own: a certificate the platform does
   * not trust is the user's call, and nobody's automation. The host hears one
   * parked event per error — browser, id, the net-error name, the URL, and
   * the leaf's SHA-256 fingerprint — and answers with certificate-respond.
   * A fingerprint the host has authorized (certificate-authorize) continues
   * without asking, for this helper's lifetime only. */
  bool OnCertificateError(CefRefPtr<CefBrowser> browser,
                          ErrorCode cert_error,
                          const CefString& request_url,
                          CefRefPtr<CefSSLInfo> ssl_info,
                          CefRefPtr<CefCallback> callback) override {
    CEF_REQUIRE_UI_THREAD();

    if (!callback)
      return false;

    std::string fingerprint;
    std::string subject;
    std::string issuer;
    if (ssl_info && ssl_info->GetX509Certificate()) {
      CefRefPtr<CefX509Certificate> cert = ssl_info->GetX509Certificate();
      CefRefPtr<CefBinaryValue> der = cert->GetDEREncoded();
      if (der) {
        const size_t size = der->GetSize();
        std::vector<unsigned char> bytes(size);
        if (size > 0 && der->GetData(bytes.data(), size, 0) == size) {
          Sha256 hash;
          hash.Update(bytes.data(), bytes.size());
          fingerprint = hash.Hex();
        }
      }
      if (cert->GetSubject())
        subject = cert->GetSubject()->GetDisplayName().ToString();
      if (cert->GetIssuer())
        issuer = cert->GetIssuer()->GetDisplayName().ToString();
    }

    if (!fingerprint.empty() && g_cert_authorized.count(fingerprint)) {
      fprintf(stderr,
              "helper: certificate-error %s authorized (%s) for %s\n",
              fingerprint.c_str(), CertErrorName(cert_error),
              request_url.ToString().c_str());
      callback->Continue();
      return true;
    }

    const uint32_t id = ++g_dialog_seq;
    CertDialog& entry = g_cert_dialogs[id];
    entry.browser_id = browser ? browser->GetIdentifier() : 0;
    entry.callback = callback;

    CefRefPtr<CefDictionaryValue> dict = NewEvent("certificate-error");
    if (entry.browser_id)
      dict->SetInt("browser", entry.browser_id);
    dict->SetInt("id", static_cast<int>(id));
    dict->SetString("error", CertErrorName(cert_error));
    dict->SetString("url", request_url.ToString());
    dict->SetString("subject", subject);
    dict->SetString("issuer", issuer);
    dict->SetString("fingerprint", fingerprint);
    Emit(dict);

    fprintf(stderr,
            "helper: certificate-error %u %s browser=%d url=%s subject=%s "
            "fingerprint=%s\n",
            id, CertErrorName(cert_error), entry.browser_id,
            request_url.ToString().c_str(), subject.c_str(),
            fingerprint.c_str());
    return true;
  }

  /* No rule registered is the zero-cost answer: no handler object, no
   * callback, the request flows as if the feature did not exist. A match
   * creates one RequestGate for exactly this request. */
  CefRefPtr<CefResourceRequestHandler> GetResourceRequestHandler(
      CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
      CefRefPtr<CefRequest> request, bool is_navigation, bool is_download,
      const CefString& request_initiator,
      bool& disable_default_handling) override {
    const std::string url = request->GetURL().ToString();
    const std::string type = ResourceTypeText(request->GetResourceType());
    std::lock_guard<std::mutex> lock(g_intercept_mutex);
    fprintf(stderr, "helper: get-resource-handler url=%s type=%s rules=%zu\n",
            url.c_str(), type.c_str(), g_intercept_rules.size());
    if (g_intercept_rules.empty())
      return nullptr;

    const InterceptRule* rule = MatchInterceptRule(url, type);
    if (!rule)
      return nullptr;

    CefRequest::HeaderMap headers;
    request->GetHeaderMap(headers);
    std::string headers_json = "{";
    bool first = true;
    for (const auto& header : headers) {
      // One flat JSON object text, built here because the header map is the
      // one structure the event carries that is not a plain string field.
      std::string name = header.first.ToString();
      std::string value = header.second.ToString();
      std::string quoted;
      for (char c : value) {
        if (c == '"' || c == '\\')
          quoted += '\\';
        quoted += c;
      }
      if (!first)
        headers_json += ",";
      first = false;
      headers_json += "\"" + name + "\":\"" + quoted + "\"";
    }
    headers_json += "}";

    const uint32_t request_id = ++g_intercept_seq;
    return new RequestGate(request_id,
                           browser ? browser->GetIdentifier() : 0, url,
                           request->GetMethod().ToString(), type,
                           request->GetResourceType() == RT_MAIN_FRAME,
                           headers_json);
  }

  void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
    CEF_REQUIRE_UI_THREAD();
    const int id = browser->GetIdentifier();
    fprintf(stderr, "helper: browser %d closing\n", id);

    // The observer registration dies with its CefRegistration, and answers
    // this side still owed go with the browser: nothing can route them.
    g_cdp_registrations.erase(id);
    g_cdp_pending.clear();

    /* Before the slot goes: a parked dialog belonged to this browser, and its
     * panel has to hear that there is nothing left to answer. */
    DropDialogsForBrowser(id);

    auto it = g_browser_slots.find(id);
    if (it != g_browser_slots.end())
      g_browser_slots.erase(it);

    CefRefPtr<CefDictionaryValue> closed = NewEvent("browser-closed");
    closed->SetInt("browser", id);
    Emit(closed);

    if (g_browser_slots.empty()) {
      g_browser = nullptr;
      g_window = nullptr;
      g_active_browser_id = 0;
      Emit(NewEvent("closed"));
      QuitOnce();
      return;
    }

    if (g_active_browser_id == id) {
      g_browser = nullptr;
      /* A DevTools entry can never become active, so the next live page
       * browser takes over instead. A background page is not a candidate:
       * handing the active slot to a hidden page would point every
       * host-facing state event at it. */
      for (const auto& other : g_browser_slots)
        if (!other.second.devtools && !other.second.background) {
          SelectBrowserById(other.first);
          break;
        }
    }
  }

  void OnLoadingStateChange(CefRefPtr<CefBrowser> browser,
                            bool is_loading,
                            bool can_go_back,
                            bool can_go_forward) override {
    /* Same rule as the title and address: the host's navigation state is the
     * page's, and the inspector's own loads are not navigation the user made.
     * A background page's loads belong to nobody visible, either. */
    if (BrowserSlot* slot = FindSlotForBrowser(browser))
      if (slot->devtools || slot->background)
        return;
    CefRefPtr<CefDictionaryValue> dict = NewEvent("loading");
    dict->SetBool("loading", is_loading);
    dict->SetBool("can_go_back", can_go_back);
    dict->SetBool("can_go_forward", can_go_forward);
    Emit(dict);
  }

  /* A number between 0 and 1 for a host that draws a progress bar. It is
   * fire-and-forget: the value is advisory and the next one supersedes it, so
   * nothing here waits for an answer. */
  void OnLoadingProgressChange(CefRefPtr<CefBrowser> browser,
                               double progress) override {
    if (BrowserSlot* slot = FindSlotForBrowser(browser))
      if (slot->devtools || slot->background)
        return;
    CefRefPtr<CefDictionaryValue> dict = NewEvent("loading-progress");
    if (browser)
      dict->SetInt("browser", browser->GetIdentifier());
    dict->SetDouble("progress", progress);
    Emit(dict);
    fprintf(stderr, "helper: loading progress %.3f\n", progress);
  }

  /* The page's icons, in the order it declared them. Fire-and-forget: which
   * one to download and where to show it is the host's call. */
  void OnFaviconURLChange(CefRefPtr<CefBrowser> browser,
                          const std::vector<CefString>& icon_urls) override {
    if (BrowserSlot* slot = FindSlotForBrowser(browser))
      if (slot->devtools || slot->background)
        return;
    CefRefPtr<CefDictionaryValue> dict = NewEvent("favicon");
    if (browser)
      dict->SetInt("browser", browser->GetIdentifier());
    CefRefPtr<CefListValue> urls = CefListValue::Create();
    for (size_t i = 0; i < icon_urls.size(); ++i)
      urls->SetString(static_cast<int>(i), icon_urls[i]);
    dict->SetList("urls", urls);
    Emit(dict);
    fprintf(stderr, "helper: favicon %zu url(s)\n", icon_urls.size());
  }

  // The page's preferred size, once auto-resize is on. Reporting it here and
  // returning true keeps CEF from applying a default it does not have for a
  // views-hosted browser; the host owns the toplevel those pixels land in.
  bool OnAutoResize(CefRefPtr<CefBrowser> browser,
                    const CefSize& new_size) override {
    CefRefPtr<CefDictionaryValue> dict = NewEvent("auto-resize");
    if (browser)
      dict->SetInt("browser", browser->GetIdentifier());
    dict->SetInt("width", new_size.width);
    dict->SetInt("height", new_size.height);
    Emit(dict);
    fprintf(stderr, "helper: auto-resize %dx%d (browser %d)\n", new_size.width,
            new_size.height, browser ? browser->GetIdentifier() : 0);
    return true;
  }

  // Alloy leaves the platform fullscreen transition to the client: the page
  // says what it wants, and the window does it. This is the pairing the
  // CefDisplayHandler documents for windowed Alloy.
  void OnFullscreenModeChange(CefRefPtr<CefBrowser> browser,
                              bool fullscreen) override {
    /* A background page cannot offer its document fullscreen (no user
       gesture, no window), and the event carries no per-browser reading the
       host could sort out itself — so it never reaches the channel. */
    if (BrowserSlot* slot = FindSlotForBrowser(browser))
      if (slot->background)
        return;
    fprintf(stderr, "helper: page fullscreen mode = %d\n", fullscreen);
    /* Reported before the window moves: the host may own the toplevel (the
     * CEF window is a child surface in the nested compositor), and it has to
     * hear about the page's request whichever way it decides to honour it. */
    CefRefPtr<CefDictionaryValue> dict = NewEvent("fullscreen");
    if (browser)
      dict->SetInt("browser", browser->GetIdentifier());
    dict->SetBool("fullscreen", fullscreen);
    Emit(dict);
    /* The browser that asked owns the window to move. Using the globally
     * active browser here would fullscreen another tab's window when a
     * background browser receives the request. */
    auto it = g_browser_slots.find(browser->GetIdentifier());
    if (it != g_browser_slots.end() && it->second.window)
      it->second.window->SetFullscreen(fullscreen);
  }

  void OnTitleChange(CefRefPtr<CefBrowser> browser,
                     const CefString& title) override {
    if (BrowserSlot* slot = FindSlotForBrowser(browser)) {
      slot->title = title.ToString();
      /* The host's title/address state belongs to the page it shows. A DevTools
       * title must not replace it; nothing on the host renders it. A
       * background page's title likewise names nothing the host shows. */
      if (slot->devtools || slot->background)
        return;
    }
    CefRefPtr<CefDictionaryValue> dict = NewEvent("title");
    dict->SetString("title", title.ToString());
    Emit(dict);
  }

  void OnAddressChange(CefRefPtr<CefBrowser> browser,
                       CefRefPtr<CefFrame> frame,
                       const CefString& url) override {
    if (!frame->IsMain())
      return;
    if (BrowserSlot* slot = FindSlotForBrowser(browser))
      if (slot->devtools || slot->background)
        return;
    CefRefPtr<CefDictionaryValue> dict = NewEvent("address");
    dict->SetString("url", url.ToString());
    Emit(dict);
  }

  void OnLoadEnd(CefRefPtr<CefBrowser> browser,
                 CefRefPtr<CefFrame> frame,
                 int http_status_code) override {
    if (!frame->IsMain())
      return;
    if (BrowserSlot* slot = FindSlotForBrowser(browser))
      if (slot->devtools || slot->background)
        return;
    CefRefPtr<CefDictionaryValue> dict = NewEvent("load-end");
    dict->SetString("url", frame->GetURL().ToString());
    dict->SetInt("http_code", http_status_code);
    Emit(dict);

    // Chromium keeps zoom per origin, so a navigation can change the level
    // without a zoom command. Report what the new page actually has instead of
    // leaving the host's cached value on the previous origin.
    if (browser && browser->GetHost()) {
      CefRefPtr<CefDictionaryValue> zoom = NewEvent("zoom");
      zoom->SetDouble("level", browser->GetHost()->GetZoomLevel());
      Emit(zoom);
    }

    if (EnvOrEmpty("CHROMIUM_SPELLCHECK") != "1") {
      frame->ExecuteJavaScript(kDisableSpellcheckScript, "cefnest-helper", 0);
      fprintf(stderr, "helper: spellcheck disabled in the page\n");
    }

    frame->ExecuteJavaScript(kPrimarySelectionScript, "cefnest-helper", 0);

    InjectIdleScripts(browser, frame);
  }

  void OnLoadError(CefRefPtr<CefBrowser> browser,
                   CefRefPtr<CefFrame> frame,
                   ErrorCode errorCode,
                   const CefString& errorText,
                   const CefString& failedUrl) override {
    if (!frame->IsMain())
      return;
    if (BrowserSlot* slot = FindSlotForBrowser(browser))
      if (slot->devtools || slot->background)
        return;
    fprintf(stderr, "helper: load error %d for %s\n", static_cast<int>(errorCode),
            failedUrl.ToString().c_str());
    CefRefPtr<CefDictionaryValue> dict = NewEvent("load-error");
    dict->SetInt("error_code", static_cast<int>(errorCode));
    dict->SetString("message", errorText.ToString());
    dict->SetString("url", failedUrl.ToString());
    Emit(dict);
  }

  // The renderer is the process the sandbox exists for,
  // and its death looks exactly like a page that stopped answering: the surface
  // keeps showing the last frame, every command after it goes unanswered. So it
  // has to reach the host as its own event rather than as a silent gap.
  void OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser,
                                 TerminationStatus status,
                                 int error_code,
                                 const CefString& error_string) override {
    /* An inspector crash is not the page's crash: the surface on the host still
     * shows the page, and ::renderer-exited would say it is gone. Neither is a
     * background page's crash a view crash. */
    if (BrowserSlot* slot = FindSlotForBrowser(browser))
      if (slot->devtools || slot->background)
        return;
    fprintf(stderr, "helper: render process terminated (status=%d error=%d)\n",
            static_cast<int>(status), error_code);
    CefRefPtr<CefDictionaryValue> dict = NewEvent("renderer-exited");
    dict->SetInt("status", static_cast<int>(status));
    dict->SetInt("error_code", error_code);
    dict->SetString("message", error_string.ToString());
    Emit(dict);
  }

  bool OnPreKeyEvent(CefRefPtr<CefBrowser> browser,
                     const CefKeyEvent& event,
                     CefEventHandle os_event,
                     bool* is_keyboard_shortcut) override {
    if (event.type != KEYEVENT_KEYDOWN)
      return false;

    /* The DevTools window keeps its own keys: Esc closes its drawer and F12
     * belongs to the inspector, not to this helper's shortcuts. */
    if (BrowserSlot* slot = FindSlotForBrowser(browser))
      if (slot->devtools)
        return false;

    if (g_devtools_enabled) {
      const bool f12 = event.windows_key_code == 0x7B;
      const bool ctrl_shift_i =
          event.windows_key_code == 'I' &&
          (event.modifiers & EVENTFLAG_CONTROL_DOWN) &&
          (event.modifiers & EVENTFLAG_SHIFT_DOWN);
      if (f12 || ctrl_shift_i) {
        fprintf(stderr, "helper: devtools on %s\n",
                f12 ? "F12" : "Ctrl+Shift+I");
        ShowDevToolsFor(browser, CefPoint());
        if (is_keyboard_shortcut)
          *is_keyboard_shortcut = true;
        return true;
      }
    }

    // Standalone convenience only; a host that loses the socket kills the
    // process anyway.
    const bool escape = event.windows_key_code == 0x1B;
    const bool ctrl_q = event.windows_key_code == 'Q' &&
                        (event.modifiers & EVENTFLAG_CONTROL_DOWN);
    if (!escape && !ctrl_q)
      return false;

    // In fullscreen the user means "leave the page's fullscreen", not
    // "close the browser": the CefBrowserHost documentation names Esc as the
    // Alloy exit path and says exiting window fullscreen should also exit
    // browser fullscreen.
    if (escape && browser->GetHost()->IsFullscreen())
      {
        fprintf(stderr, "helper: leaving fullscreen on Esc\n");
        browser->GetHost()->ExitFullscreen(true);
        if (g_window)
          g_window->SetFullscreen(false);
        return true;
      }

    fprintf(stderr, "helper: closing on %s\n", escape ? "Esc" : "Ctrl+Q");
    if (is_keyboard_shortcut)
      *is_keyboard_shortcut = true;
    browser->GetHost()->CloseBrowser(true);
    return true;
  }

  IMPLEMENT_REFCOUNTING(HelperClient);

 private:
  int slot_ = 0;
  CefRefPtr<CefDownloadHandler> download_handler_{new HelperDownloadHandler};
  CefRefPtr<CefFindHandler> find_handler_{new HelperFindHandler};
  CefRefPtr<CefContextMenuHandler> context_menu_handler_{
      new HelperContextMenuHandler};
  CefRefPtr<CefPermissionHandler> permission_handler_{
      new HelperPermissionHandler};
  CefRefPtr<CefJSDialogHandler> jsdialog_handler_{
      new HelperJSDialogHandler};
  CefRefPtr<CefPrintHandler> print_handler_{new HelperPrintHandler};
  CefRefPtr<CefDialogHandler> dialog_handler_{new HelperDialogHandler};
};

/* ShowDevTools opens an associated browser CEF windows itself; because every
 * helper browser is wrapped in a CefBrowserView, CefWindowInfo cannot change
 * that. The client is ours — slot 0, since this is not a request slot — so the
 * created/closed events and the URL report come back over the control channel
 * and the browser can be adopted into a slot. */
void ShowDevToolsFor(CefRefPtr<CefBrowser> browser, const CefPoint& point) {
  if (!browser || !browser->GetHost())
    return;

  /* An already-open panel is only focused; no browser is created, so the
   * pending mark would never be consumed. */
  if (!browser->GetHost()->HasDevTools())
    ++g_devtools_creating;

  CefRefPtr<HelperClient> client(new HelperClient(0));
  browser->GetHost()->ShowDevTools(CefWindowInfo(), client, CefBrowserSettings(),
                                   point);
  fprintf(stderr, "helper: devtools requested for browser %d\n",
          browser->GetIdentifier());
}

class HelperWindowDelegate : public CefWindowDelegate {
 public:
  explicit HelperWindowDelegate(CefRefPtr<CefBrowserView> browser_view,
                                bool primary = true,
                                cef_runtime_style_t style =
                                    CEF_RUNTIME_STYLE_ALLOY)
      : browser_view_(browser_view), primary_(primary), style_(style) {}

  void OnWindowCreated(CefRefPtr<CefWindow> window) override {
    CEF_REQUIRE_UI_THREAD();
    /* A DevTools shell is not the primary window: the global tracks the one
     * the Esc-fullscreen handling applies to. */
    if (primary_)
      g_window = window;
    window->AddChildView(browser_view_);
    window->Show();
    browser_view_->RequestFocus();
    /* The window owns the view from AddChildView on, so this reference is a
     * second owner — and a second owner is what makes CloseBrowser on *this*
     * browser read as "close the window" to Alloy instead of "close the
     * browser". Measured: the tab window's delegate kept its view alive, so the
     * first tab could never be closed (no OnBeforeClose, no ::browser-closed)
     * while every later tab, whose view only the window and the slot held,
     * closed fine. OnWindowCreated runs before CreateTopLevelWindow returns, so
     * the reference has done its job by here. */
    browser_view_ = nullptr;
    fprintf(stderr, "helper: window created\n");
  }

  void OnWindowDestroyed(CefRefPtr<CefWindow> window) override {
    CEF_REQUIRE_UI_THREAD();
    if (g_window == window)
      g_window = nullptr;
    if (g_tab_window == window)
      g_tab_window = nullptr;
    fprintf(stderr, "helper: window destroyed\n");
    browser_view_ = nullptr;
    /* A tab's window outlives individual browser views; only the last window
     * may end the message loop. */
    if (g_browser_slots.empty())
      QuitOnce();
  }

  void OnWindowActivationChanged(CefRefPtr<CefWindow> window,
                                 bool active) override {
    CEF_REQUIRE_UI_THREAD();
    /* Activating a standalone window makes its browser the active one, so the
     * host's menus and commands follow even when the click lands on window
     * chrome instead of the page. The tab window needs no look-up: it shows
     * the active browser by definition. */
    if (!active || window == g_tab_window)
      return;
    for (auto& entry : g_browser_slots) {
      if (entry.second.window.get() == window.get() && entry.second.browser) {
        SelectBrowserById(entry.second.browser->GetIdentifier());
        break;
      }
    }
  }

  // The page is the whole window: the GTK host draws the chrome.
  bool IsFrameless(CefRefPtr<CefWindow> window) override { return true; }
  bool CanResize(CefRefPtr<CefWindow> window) override { return true; }

  // The window left fullscreen by a user action (WM shortcut, title bar):
  // Alloy does not exit browser fullscreen on its own, so the page follows
  // the window only if we ask it to.
  void OnWindowFullscreenTransition(CefRefPtr<CefWindow> window,
                                    bool is_completed) override {
    if (!is_completed || window->IsFullscreen())
      return;
    /* Exit fullscreen on the browser that lives in this window, not on the
     * active one: the user may have left fullscreen in a window that is not
     * the tab currently selected. */
    for (auto& entry : g_browser_slots) {
      if (entry.second.window.get() == window.get() && entry.second.browser &&
          entry.second.browser->GetHost()) {
        entry.second.browser->GetHost()->ExitFullscreen(true);
        break;
      }
    }
  }

  // Ignored once the compositor configures us; the widget's size wins.
  CefRect GetInitialBounds(CefRefPtr<CefWindow> window) override {
    return CefRect(100, 100, 1000, 700);
  }

  /* Alloy everywhere — except the DevTools adopt window: a DevTools popup
   * view is Chrome style by CEF's own rule (browser_view_impl.cc CHECKs it,
   * and an Alloy window around one crashes), so its shell must claim Chrome. */
  cef_runtime_style_t GetWindowRuntimeStyle() override { return style_; }

  IMPLEMENT_REFCOUNTING(HelperWindowDelegate);

 private:
  CefRefPtr<CefBrowserView> browser_view_;
  bool primary_ = true;
  cef_runtime_style_t style_ = CEF_RUNTIME_STYLE_ALLOY;
};

class HelperBrowserViewDelegate : public CefBrowserViewDelegate {
 public:
  explicit HelperBrowserViewDelegate(bool chrome = false) : chrome_(chrome) {}
  cef_runtime_style_t GetBrowserRuntimeStyle() override {
    /* A DevTools popup view is Chrome style by CEF's own rule —
     * browser_view_impl.cc refuses Alloy for DevTools popups, and an Alloy
     * window around one segfaults. */
    return chrome_ ? CEF_RUNTIME_STYLE_CHROME : CEF_RUNTIME_STYLE_ALLOY;
  }

  /* The DevTools popup's view is ours to place: returning our own Chrome
   * style delegate keeps the view's style legal, and
   * OnPopupBrowserViewCreated then builds the frameless shell for it instead
   * of letting CEF build its framed default window. Measured dead ends, both
   * segfault-adjacent: moving the popup view after creation never maps
   * before its first frame and segfaults after it, and a Chrome-style window
   * hosting a fresh view dies in StatusBubble's WaylandBubble — the popup
   * hook is the one route that maps, paints and stays alive. */
  CefRefPtr<CefBrowserViewDelegate> GetDelegateForPopupBrowserView(
      CefRefPtr<CefBrowserView> browser_view,
      const CefBrowserSettings& settings,
      CefRefPtr<CefClient> client,
      bool is_devtools) override {
    if (is_devtools && g_devtools_enabled)
      return new HelperBrowserViewDelegate(/*chrome=*/true);
    return this;
  }

  bool OnPopupBrowserViewCreated(CefRefPtr<CefBrowserView> browser_view,
                                 CefRefPtr<CefBrowserView> popup_browser_view,
                                 bool is_devtools) override {
    if (!is_devtools || !g_devtools_enabled)
      return false;
    CefRefPtr<HelperWindowDelegate> delegate(
        new HelperWindowDelegate(popup_browser_view,
                                 /*primary=*/false,
                                 /*style=*/CEF_RUNTIME_STYLE_CHROME));
    CefRefPtr<CefWindow> window = CefWindow::CreateTopLevelWindow(delegate);
    if (!window) {
      fprintf(stderr, "helper: devtools frameless window failed\n");
      return false;   /* let CEF build its default window instead */
    }
    fprintf(stderr, "helper: devtools popup in a frameless window\n");
    return true;   /* the view hierarchy is ours now */
  }

  IMPLEMENT_REFCOUNTING(HelperBrowserViewDelegate);

 private:
  bool chrome_ = false;
};

int CreateNewBrowser(const std::string& url, bool windowed, bool background) {
  const int slot = g_next_browser_slot++;
  CefRefPtr<HelperClient> client(new HelperClient(slot));
  CefRefPtr<HelperBrowserViewDelegate> view_delegate(
      new HelperBrowserViewDelegate);

  /* CefBrowserView::CreateBrowserView can reach OnAfterCreated synchronously;
   * the slot must already be discoverable by the request-side id. */
  BrowserSlot entry;
  entry.slot = slot;
  entry.windowed = windowed;
  entry.background = background;
  entry.start_url = url;
  g_browser_slots[slot] = entry;
  g_slot_to_browser[slot] = slot;
  /* Set before CreateBrowserView: OnAfterCreated can run synchronously and then
   * it is this flag that makes the new tab active. A background page never
   * raises the flag: it must not take the active slot even when it is the
   * first browser this helper creates. */
  g_select_slot = background ? 0 : slot;

  CefBrowserSettings browser_settings;
  browser_settings.background_color = 0xff101828;
  CefRefPtr<CefBrowserView> view = CefBrowserView::CreateBrowserView(
      client, url, browser_settings, nullptr, nullptr, view_delegate);
  if (!view) {
    fprintf(stderr, "helper: CreateBrowserView failed for slot %d\n", slot);
    g_browser_slots.erase(slot);
    g_slot_to_browser.erase(slot);
    EmitStatus("browser-create-failed");
    return 0;
  }
  g_browser_slots[slot].view = view;

  if (background) {
    /* Attached to the existing tab window as an invisible child — never its
     * own CefWindow. Measured 2026-10-26: a browser view attached to no
     * window is a browser that never comes into existence (OnAfterCreated
     * waits for the view to be hosted), while a hidden child of the already
     * shown tab window creates the browser without adding a toplevel, which
     * is what keeps the host's FIFO pairing untouched. WasHidden in
     * OnAfterCreated then puts the page on Chromium's hidden-tab schedule. */
    if (g_tab_window) {
      g_browser_slots[slot].window = g_tab_window;
      g_tab_window->AddChildView(view);
      view->SetVisible(false);
      fprintf(stderr, "helper: new background page slot %d\n", slot);
    } else {
      fprintf(stderr,
              "helper: background page slot %d idle — no tab window to hide "
              "in yet\n",
              slot);
    }
    return slot;
  }

  if (!windowed && g_tab_window) {
    g_browser_slots[slot].window = g_tab_window;
    g_tab_window->AddChildView(view);
    const bool active = g_active_browser_id == slot || g_select_slot == slot;
    if (active) {
      BrowserSlot* old = FindSlotForBrowser(g_browser);
      if (old && old->view && old->slot != slot)
        old->view->SetVisible(false);
      view->SetVisible(true);
      g_tab_window->Layout();
      view->RequestFocus();
    } else {
      view->SetVisible(false);
    }
    fprintf(stderr, "helper: new tab slot %d in tab window%s\n", slot,
            active ? " (active)" : "");
    return slot;
  }

  CefRefPtr<HelperWindowDelegate> window_delegate(
      new HelperWindowDelegate(view));
  CefRefPtr<CefWindow> window =
      CefWindow::CreateTopLevelWindow(window_delegate);
  if (!window) {
    fprintf(stderr, "helper: CreateTopLevelWindow failed for slot %d\n", slot);
    g_browser_slots.erase(slot);
    g_slot_to_browser.erase(slot);
    EmitStatus("window-create-failed");
    return 0;
  }

  g_browser_slots[slot].window = window;
  if (!windowed && !g_tab_window)
    g_tab_window = window;
  fprintf(stderr, "helper: new %s slot %d\n",
          windowed ? "window" : "tab", slot);
  return slot;
}

void PrintEnvironment() {
  // GDK_BACKEND belongs here with the display variables: GTK picks its backend
  // from it before it looks at DISPLAY at all, so "x11" leaking in from the host
  // is enough to make the helper die before it has a window.
  //
  // The sandbox is printed for whoever reads a transcript of a run that did not
  // start: Chromium dies with "No usable sandbox!" when it has nothing to
  // sandbox with, and the switch named in that message is not the knob this
  // helper reads.
  fprintf(stderr,
          "helper: CEF %s\n"
          "helper: WAYLAND_DISPLAY=%s DISPLAY=%s XDG_SESSION_TYPE=%s\n"
          "helper: GDK_BACKEND=%s\n"
          "helper: %s=%s\n"
          "helper: sandbox=%s\n",
          CEF_VERSION, EnvOrEmpty("WAYLAND_DISPLAY").c_str(),
          EnvOrEmpty("DISPLAY").c_str(),
          EnvOrEmpty("XDG_SESSION_TYPE").c_str(),
          EnvOrEmpty("GDK_BACKEND").c_str(), kSocketEnv,
          EnvOrEmpty(kSocketEnv).empty() ? "(unset, no control)"
                                         : EnvOrEmpty(kSocketEnv).c_str(),
          SandboxDisabled() ? "OFF (CHROMIUM_HELPER_NO_SANDBOX=1)" : "on");
}

}  // namespace

int main(int argc, char** argv) {
  CefMainArgs main_args(argc, argv);

  CefRefPtr<HelperApp> app(new HelperApp);
  const int exit_code = CefExecuteProcess(main_args, app, nullptr);
  if (exit_code >= 0) {
    // This was a Chromium subprocess, and it must not reach main's epilogue.
    // CEF's subprocesses here are the zygote's fork-without-exec children: they
    // inherit this stack, including this frame with the canary its prologue
    // copied out of TLS, but %fs:0x28 holds something else by the time
    // CefExecuteProcess returns. Measured on CEF 154 with the helper printing
    // both sides in its own main(): three runs of a bing search, two such
    // children each, every one reporting %fs:0x28 unchanged at main's entry and
    // changed — by a value specific to that child — on the way out, and
    // `*** stack smashing detected ***` immediately after each of those lines.
    // Nothing was smashed: glibc's message cannot tell an overwritten frame slot
    // from a canary that moved, and a plain fork() moves nothing (both checked).
    // CefExecuteProcess blocks until the process should exit and hands back
    // its exit code, so there is nothing left to unwind here — _exit takes the
    // code out without the check.
    _exit(exit_code);
  }

  PrintEnvironment();

  // Only the browser process reaches this line: CefExecuteProcess above has
  // already returned for every Chromium subprocess, whose signal handling stays
  // Chromium's own. Start() is below, after CefInitialize — see the note there.
  SignalWatcher signals;

  Control control;
  g_control = &control;
  const std::string socket_path = EnvOrEmpty(kSocketEnv);
  if (!socket_path.empty() && !control.Connect(socket_path))
    return 1;

  // Test hook: lets a smoke run end by itself instead of waiting for a human.
  const std::string auto_quit = EnvOrEmpty("CHROMIUM_HELPER_AUTO_QUIT_MS");
  if (!auto_quit.empty())
    g_auto_quit_ms = std::strtol(auto_quit.c_str(), nullptr, 10);

  // A data directory is the host's or the standalone user's decision, and
  // nothing here invents one: without CHROMIUM_HELPER_DATA_DIR there is no
  // persistence at all, only a tmpfs root for what CEF insists on writing.
  // The host names the directory for its own session; the environment variable
  // is also the standalone entry point.
  const std::string given = EnvOrEmpty("CHROMIUM_HELPER_DATA_DIR");
  const bool ephemeral = given.empty();
  std::string root;
  std::string profile;
  if (ephemeral) {
    root = MakeEphemeralRoot();
    if (root.empty())
      return 1;
  } else {
    // A misspelt path must fail loudly: quietly falling back to an ephemeral
    // profile would turn "persistent" into "silently lost on exit".
    std::error_code error;
    if (given.front() != '/' || !std::filesystem::is_directory(given, error)) {
      fprintf(stderr, "helper: data directory not usable: %s\n", given.c_str());
      return 1;
    }
    if (!LockDataDirectory(given)) {
      fprintf(stderr, "helper: data directory is in use: %s\n", given.c_str());
      return 1;
    }
    root = given;
    profile = given + "/browser";
  }
  if (ephemeral)
    fprintf(stderr, "helper: cache mode=ephemeral root=%s\n", root.c_str());
  else
    fprintf(stderr, "helper: cache mode=persistent root=%s profile=%s\n",
            root.c_str(), profile.c_str());

  CefSettings settings;
  // Sandbox on unless the host takes it off on purpose, and no fallback: a host
  // with neither unprivileged user namespaces nor a setuid chrome-sandbox dies
  // at zygote_host_impl_linux.cc with "No usable sandbox!" (SIGTRAP, and the
  // session reports "helper killed by signal 5") rather than starting wide open.
  // CHROMIUM_HELPER_NO_SANDBOX=1 is the way to say it deliberately.
  settings.no_sandbox = SandboxDisabled();
  settings.windowless_rendering_enabled = false;
  settings.multi_threaded_message_loop = false;
  settings.external_message_pump = false;
  // INFO on stderr (and debug.log) is per-event noise a running session emits
  // continuously; warnings and errors are the ones that mean something. Keep
  // the setting greppable: this is the only place it is chosen.
  settings.log_severity = LOGSEVERITY_WARNING;
  settings.persist_session_cookies = false;
  // A host-wide default User-Agent. CEF bakes CefSettings.user_agent in at
  // CefInitialize — the one chance to set it — so the host opts in by putting
  // CHROMIUM_USER_AGENT in its own environment before chromium_session_start();
  // the spawned helper inherits it. Changing the UA later means stopping the
  // session and starting it again with a new value (see
  // examples/useragent.c).
  {
    const std::string ua = EnvOrEmpty("CHROMIUM_USER_AGENT");
    if (!ua.empty())
      CefString(&settings.user_agent).FromString(ua);
  }
  // Accept-Language is fixed for the context's lifetime, so this is the one
  // place to set it: the renderer's navigator.language and the HTTP header
  // both read it back from here.
  {
    const std::string accept_lang =
        EnvOrEmpty("CHROMIUM_HELPER_ACCEPT_LANG");
    if (!accept_lang.empty()) {
      CefString(&settings.accept_language_list).FromString(accept_lang);
      fprintf(stderr, "helper: accept-language=%s\n", accept_lang.c_str());
    }
  }
  // root_cache_path is never empty: that is the structural half of "no data
  // directory means no disk", because a CEF with nothing here falls back to
  // ~/.config/cef_user_data. cache_path stays empty in ephemeral mode, which
  // is what makes the profile in-memory (Chromium's incognito).
  CefString(&settings.root_cache_path).FromString(root);
  if (!profile.empty())
    CefString(&settings.cache_path).FromString(profile);

  const std::string resources = EnvOrEmpty("CEF_RESOURCES_PATH");
  if (!resources.empty()) {
    CefString(&settings.resources_dir_path).FromString(resources);
    CefString(&settings.locales_dir_path).FromString(resources + "/locales");
    fprintf(stderr, "helper: resources=%s\n", resources.c_str());
  }

  if (!CefInitialize(main_args, settings, app, nullptr)) {
    fprintf(stderr, "helper: CefInitialize failed\n");
    if (ephemeral)
      RemoveTree(root);
    return 1;
  }

  // After CefInitialize, not before: that call puts SIGHUP, SIGINT and SIGTERM
  // back to SIG_DFL on the way through (strace on CEF 154: the three are reset
  // about 2 ms after an install made before it). A handler installed before it
  // is simply gone by the time there is a message loop to leave.
  signals.Start();

  // The factory serves only the roots plugins-load has reported, one prefix
  // check per request — a URL is not to be trusted on its way in (a literal
  // ../ is normalised away, ..%2F is not), so this side re-runs the rule the
  // host-side builder already applied.
  if (!CefRegisterSchemeHandlerFactory(kExtensionScheme, "",
                                       new ExtensionFactory)) {
    fprintf(stderr, "helper: extension scheme factory registration failed\n");
  }

  // Removing a dead helper's tmpfs root is unbounded filesystem work (entries
  // to stat, whole trees to remove), so it runs on its own thread: the reader
  // is already answering the host, and CefInitialize and the message loop must
  // not queue behind a scan of the runtime directory. Standalone runs sweep
  // too — they leave the same kind of root behind. Every exit path below joins
  // it: a thread outliving main() would race the C runtime's teardown.
  std::thread sweep([] { SweepEphemeralRoots(RuntimeBaseDir()); });

  // Hosted: no initial navigation. The empty document a browser starts with is
  // not a history entry of its own — the host's first load replaces it, which is
  // why the first page reports can_go_back false — but a URL passed here *does*
  // become an entry, so starting on about:blank would leave the user one Back
  // press away from a page they never asked for. Standalone mode takes argv.
  const std::string url =
      socket_path.empty() ? PickUrl(argc, argv) : std::string();
  fprintf(stderr, "helper: url=%s\n", url.c_str());

  const int first_slot = CreateNewBrowser(url, false);
  if (first_slot <= 0) {
    signals.Stop();
    control.Shutdown();
    CefShutdown();
    if (sweep.joinable())
      sweep.join();
    if (ephemeral)
      RemoveTree(root);
    return 1;
  }
  g_select_slot = first_slot;

  CefRunMessageLoop();
  signals.Stop();
  control.Shutdown();
  g_control = nullptr;
  CefShutdown();
  if (sweep.joinable())
    sweep.join();
  if (ephemeral)
    RemoveTree(root);
  fprintf(stderr, "helper: exited\n");
  return 0;
}
