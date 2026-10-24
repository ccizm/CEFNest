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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_command_line.h"
#include "include/cef_context_menu_handler.h"
#include "include/cef_display_handler.h"
#include "include/cef_download_handler.h"
#include "include/cef_download_item.h"
#include "include/cef_find_handler.h"
#include "include/cef_keyboard_handler.h"
#include "include/cef_life_span_handler.h"
#include "include/cef_load_handler.h"
#include "include/cef_parser.h"
#include "include/cef_permission_handler.h"
#include "include/cef_process_message.h"
#include "include/cef_render_process_handler.h"
#include "include/cef_request_handler.h"
#include "include/cef_task.h"
#include "include/cef_v8.h"
#include "include/cef_values.h"
#include "include/cef_version.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_browser_view_delegate.h"
#include "include/views/cef_window.h"
#include "include/views/cef_window_delegate.h"
#include "include/wrapper/cef_helpers.h"

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
CefRefPtr<CefBrowser> g_browser;
CefRefPtr<CefWindow> g_window;

/* One helper can own several browsers. A tab and a standalone window differ
 * only in which CefWindow the CefBrowserView is parented to; everything on the
 * control channel is keyed by the browser id CEF assigns. */
struct BrowserSlot {
  int slot = 0;                 /* request-side key, until CEF assigns id */
  bool windowed = false;
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

int CreateNewBrowser(const std::string& url, bool windowed);
void SelectBrowserById(int id);

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

std::string CacheDirectory(const std::string& name) {
  std::string base = EnvOrEmpty("XDG_CACHE_HOME");
  if (base.empty()) {
    std::string home = EnvOrEmpty("HOME");
    base = home.empty() ? "/tmp" : home + "/.cache";
  }
  std::string dir = base + "/" + name;
  std::error_code error;
  std::filesystem::create_directories(dir, error);
  return dir;
}

// Chromium allows one browser process per cache directory: a second helper
// pointed at the same profile prints "Opening in an existing browser session"
// and exits — and the collision has also been observed to take the first
// helper down with it. Hosts each listen on their own socket, so the socket
// path is a stable per-instance identity: hash it into the directory name and
// concurrent hosts stop sharing a profile. The same host keeps the same
// directory across runs, so cookies and the GPU cache are still reused.
std::string SocketCacheSuffix(const std::string& socket_path) {
  uint64_t hash = 1469598103934665603ull;   // FNV-1a
  for (unsigned char c : socket_path) {
    hash ^= c;
    hash *= 1099511628211ull;
  }
  char out[17];
  snprintf(out, sizeof(out), "%016llx",
           static_cast<unsigned long long>(hash));
  return out;
}

// Hosts come and go — crashed ones especially — so their per-instance
// directories would pile up forever without a sweep. Anything not touched for
// two weeks goes. The current instance's directory is created moments later
// and is brand new, so it is never its own victim; a failure to remove is
// ignored: the sweep is best effort and must never block startup.
void PruneStaleCacheDirs(const std::string& keep) {
  std::string base = EnvOrEmpty("XDG_CACHE_HOME");
  if (base.empty()) {
    std::string home = EnvOrEmpty("HOME");
    base = home.empty() ? "/tmp" : home + "/.cache";
  }
  std::error_code error;
  const auto cutoff = std::filesystem::file_time_type::clock::now() -
                      std::chrono::hours(24 * 14);
  for (const auto& entry :
       std::filesystem::directory_iterator(base, error)) {
    const std::string name = entry.path().filename().string();
    if (name.rfind("cefnest-helper-hosted-", 0) != 0 ||
        entry.path().string() == keep)
      continue;
    auto last = std::filesystem::last_write_time(entry.path(), error);
    if (!error && last < cutoff)
      std::filesystem::remove_all(entry.path(), error);
  }
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

void SelectBrowserById(int id) {
  auto it = g_browser_slots.find(id);
  if (it == g_browser_slots.end() || !it->second.browser)
    {
      EmitStatus("no-such-browser");
      return;
    }

  BrowserSlot* slot = &it->second;
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
};

class HelperContextMenuHandler : public CefContextMenuHandler {
 public:
  bool RunContextMenu(CefRefPtr<CefBrowser> browser,
                      CefRefPtr<CefFrame> frame,
                      CefRefPtr<CefContextMenuParams> params,
                      CefRefPtr<CefMenuModel> model,
                      CefRefPtr<CefRunContextMenuCallback> callback) override {
    CEF_REQUIRE_UI_THREAD();

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
  if (value && CefWriteJSON(value, JSON_WRITER_DEFAULT).ToString().size() >
                  kMaxResultBytes) {
    event->SetString("message", "result exceeded 64 KiB and was dropped");
    event->SetBool("truncated", true);
    value = nullptr;
  }

  if (value)
    event->SetValue("value", value);
  else
    event->SetNull("value");

  Emit(event);
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
    const bool windowed =
        command->HasKey("mode") &&
        command->GetString("mode").ToString() == "window";
    CreateNewBrowser(url, windowed);
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
    CefRefPtr<CefFrame> frame = g_browser->GetMainFrame();
    if (!frame) {
      EmitStatus("no-main-frame");
      return;
    }
    frame->LoadURL(url);
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
  if (name == "zoom-in" || name == "zoom-out" || name == "zoom-reset") {
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
    else
      level = 0.0;
    host->SetZoomLevel(level);

    CefRefPtr<CefDictionaryValue> dict = NewEvent("zoom");
    dict->SetDouble("level", host->GetZoomLevel());
    Emit(dict);
    return;
  }

  if (name == "stop") {
    g_browser->StopLoad();
    return;
  }

  if (name == "mute") {
    CefRefPtr<CefBrowserHost> host = g_browser->GetHost();
    if (!host) {
      EmitStatus("no-host");
      return;
    }
    const bool mute =
        command->HasKey("on") ? command->GetBool("on") : !host->IsAudioMuted();
    host->SetAudioMuted(mute);
    // Reported back rather than assumed: the page's sound leaves this process
    // through an unsandboxed audio service, and whether it
    // is silenced is a fact about that service, not about this call.
    CefRefPtr<CefDictionaryValue> dict = NewEvent("muted");
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

  if (name == "js" || name == "js-expr") {
    const std::string code = command->GetString("code").ToString();
    if (code.empty()) {
      EmitStatus("js-needs-code");
      return;
    }
    CefRefPtr<CefFrame> frame = g_browser->GetMainFrame();
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

class HelperRenderProcessHandler : public CefRenderProcessHandler {
 public:
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
  }

 private:
  CefRefPtr<CefV8Handler> handler_{new ReportHandler};
  CefRefPtr<CefV8Handler> post_handler_{new PostMessageHandler};
  CefRefPtr<CefV8Handler> primary_handler_{new PrimarySelectionHandler};

  IMPLEMENT_REFCOUNTING(HelperRenderProcessHandler);
};

class HelperApp : public CefApp {
 public:
  CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override {
    return render_handler_;
  }

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

  void OnGotFocus(CefRefPtr<CefBrowser> browser) override {
    /* Clicking a page gives its view focus: make that browser the active one,
     * so host-drawn menus and plain commands follow the window the user is
     * looking at. The host sees ::browser-selected before the ::context-menu
     * that the same click goes on to request. Idempotent by the guard in
     * SelectBrowserById. */
    SelectBrowserById(browser->GetIdentifier());
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
    /* Compare the request-side slot with what the host asked to select before
     * the entry is re-keyed: after the re-key, `slot->slot` is a CEF browser
     * id, and the two counters only stay equal until the first creation
     * failure. Reading it afterwards leaves every later browser unselected. */
    bool select_this = slot && slot->slot == g_select_slot;
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

    fprintf(stderr, "helper: browser created, id=%d slot=%d windowed=%d\n",
            browser->GetIdentifier(), slot ? slot->slot : 0,
            slot ? slot->windowed : 0);

    CefRefPtr<CefDictionaryValue> created = NewEvent("browser-created");
    created->SetInt("browser", browser->GetIdentifier());
    created->SetString("url", slot && !slot->start_url.empty()
                                  ? slot->start_url
                                  : browser->GetMainFrame()->GetURL().ToString());
    created->SetBool("windowed", slot ? slot->windowed : false);
    Emit(created);

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

  void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
    CEF_REQUIRE_UI_THREAD();
    const int id = browser->GetIdentifier();
    fprintf(stderr, "helper: browser %d closing\n", id);

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
      auto next = g_browser_slots.begin();
      SelectBrowserById(next->first);
    }
  }

  void OnLoadingStateChange(CefRefPtr<CefBrowser> browser,
                            bool is_loading,
                            bool can_go_back,
                            bool can_go_forward) override {
    CefRefPtr<CefDictionaryValue> dict = NewEvent("loading");
    dict->SetBool("loading", is_loading);
    dict->SetBool("can_go_back", can_go_back);
    dict->SetBool("can_go_forward", can_go_forward);
    Emit(dict);
  }

  // Alloy leaves the platform fullscreen transition to the client: the page
  // says what it wants, and the window does it. This is the pairing the
  // CefDisplayHandler documents for windowed Alloy.
  void OnFullscreenModeChange(CefRefPtr<CefBrowser> browser,
                              bool fullscreen) override {
    fprintf(stderr, "helper: page fullscreen mode = %d\n", fullscreen);
    /* The browser that asked owns the window to move. Using the globally
     * active browser here would fullscreen another tab's window when a
     * background browser receives the request. */
    auto it = g_browser_slots.find(browser->GetIdentifier());
    if (it != g_browser_slots.end() && it->second.window)
      it->second.window->SetFullscreen(fullscreen);
  }

  void OnTitleChange(CefRefPtr<CefBrowser> browser,
                     const CefString& title) override {
    CefRefPtr<CefDictionaryValue> dict = NewEvent("title");
    dict->SetString("title", title.ToString());
    Emit(dict);
  }

  void OnAddressChange(CefRefPtr<CefBrowser> browser,
                       CefRefPtr<CefFrame> frame,
                       const CefString& url) override {
    if (!frame->IsMain())
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
  }

  void OnLoadError(CefRefPtr<CefBrowser> browser,
                   CefRefPtr<CefFrame> frame,
                   ErrorCode errorCode,
                   const CefString& errorText,
                   const CefString& failedUrl) override {
    if (!frame->IsMain())
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
};

class HelperBrowserViewDelegate : public CefBrowserViewDelegate {
 public:
  cef_runtime_style_t GetBrowserRuntimeStyle() override {
    return CEF_RUNTIME_STYLE_ALLOY;
  }

  IMPLEMENT_REFCOUNTING(HelperBrowserViewDelegate);
};

class HelperWindowDelegate : public CefWindowDelegate {
 public:
  explicit HelperWindowDelegate(CefRefPtr<CefBrowserView> browser_view)
      : browser_view_(browser_view) {}

  void OnWindowCreated(CefRefPtr<CefWindow> window) override {
    CEF_REQUIRE_UI_THREAD();
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

  cef_runtime_style_t GetWindowRuntimeStyle() override {
    return CEF_RUNTIME_STYLE_ALLOY;
  }

  IMPLEMENT_REFCOUNTING(HelperWindowDelegate);

 private:
  CefRefPtr<CefBrowserView> browser_view_;
};

int CreateNewBrowser(const std::string& url, bool windowed) {
  const int slot = g_next_browser_slot++;
  CefRefPtr<HelperClient> client(new HelperClient(slot));
  CefRefPtr<HelperBrowserViewDelegate> view_delegate(
      new HelperBrowserViewDelegate);

  /* CefBrowserView::CreateBrowserView can reach OnAfterCreated synchronously;
   * the slot must already be discoverable by the request-side id. */
  BrowserSlot entry;
  entry.slot = slot;
  entry.windowed = windowed;
  entry.start_url = url;
  g_browser_slots[slot] = entry;
  g_slot_to_browser[slot] = slot;
  /* Set before CreateBrowserView: OnAfterCreated can run synchronously and then
   * it is this flag that makes the new tab active. */
  g_select_slot = slot;

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

  std::string cache_dir;
  if (socket_path.empty()) {
    // Standalone mode: one fixed profile, one instance at a time by design.
    cache_dir = CacheDirectory("cefnest-helper");
  } else {
    cache_dir = CacheDirectory("cefnest-helper-hosted-" +
                               SocketCacheSuffix(socket_path));
    PruneStaleCacheDirs(cache_dir);
  }
  fprintf(stderr, "helper: cache=%s\n", cache_dir.c_str());

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
  settings.log_severity = LOGSEVERITY_INFO;
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
  CefString(&settings.root_cache_path).FromString(cache_dir);
  CefString(&settings.cache_path).FromString(cache_dir + "/browser");

  const std::string resources = EnvOrEmpty("CEF_RESOURCES_PATH");
  if (!resources.empty()) {
    CefString(&settings.resources_dir_path).FromString(resources);
    CefString(&settings.locales_dir_path).FromString(resources + "/locales");
    fprintf(stderr, "helper: resources=%s\n", resources.c_str());
  }

  if (!CefInitialize(main_args, settings, app, nullptr)) {
    fprintf(stderr, "helper: CefInitialize failed\n");
    return 1;
  }

  // After CefInitialize, not before: that call puts SIGHUP, SIGINT and SIGTERM
  // back to SIG_DFL on the way through (strace on CEF 154: the three are reset
  // about 2 ms after an install made before it). A handler installed before it
  // is simply gone by the time there is a message loop to leave.
  signals.Start();

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
    return 1;
  }
  g_select_slot = first_slot;

  CefRunMessageLoop();
  signals.Stop();
  control.Shutdown();
  g_control = nullptr;
  CefShutdown();
  fprintf(stderr, "helper: exited\n");
  return 0;
}
