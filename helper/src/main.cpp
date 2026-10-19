// ChromiumGTK helper — a windowed Chromium, driven over a socket.
//
// The GTK widget owns the Wayland compositor; this process owns Chromium. It
// creates one frameless CefWindow holding a CefBrowserView, so the page is
// rendered by Chromium into a real Wayland surface — no off-screen rendering,
// no XWayland — and it speaks one JSON object per line on the unix socket the
// host named in CHROMIUM_HELPER_SOCKET:
//
//   host   -> helper   {"cmd":"load","url":"https://example.com"}
//   helper -> host     {"event":"address","url":"https://example.com"}
//
// Commands are parsed off the socket on a reader thread and re-dispatched to
// the CEF UI thread; events are serialised to the socket under a mutex. If the
// host closes the socket the helper quits, so a crashed widget never leaves an
// orphaned Chromium behind.
//
// Without CHROMIUM_HELPER_SOCKET this is just the Gate 1 probe: a window that
// loads the URL from argv and quits on Esc.

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_command_line.h"
#include "include/cef_display_handler.h"
#include "include/cef_keyboard_handler.h"
#include "include/cef_life_span_handler.h"
#include "include/cef_load_handler.h"
#include "include/cef_parser.h"
#include "include/cef_task.h"
#include "include/cef_values.h"
#include "include/cef_version.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_browser_view_delegate.h"
#include "include/views/cef_window.h"
#include "include/views/cef_window_delegate.h"
#include "include/wrapper/cef_helpers.h"

namespace {

constexpr char kSocketEnv[] = "CHROMIUM_HELPER_SOCKET";

const char kDefaultUrl[] =
    "data:text/html;charset=utf-8,"
    "%3Cbody%20style%3D%22margin%3A0%3Bbackground%3A%23101828%3B"
    "color%3A%23e8eef8%3Bfont%3A16px%20sans-serif%22%3E"
    "%3Ctitle%3EChromiumGTK%20helper%3C%2Ftitle%3E"
    "%3Ch1%20style%3D%22padding%3A40px%22%3E"
    "CEF%20helper%20is%20running%3C%2Fh1%3E"
    "%3Cp%20style%3D%22padding%3A0%2040px%22%3E"
    "Start%20this%20process%20with%20CHROMIUM_HELPER_SOCKET%20set%20"
    "to%20let%20the%20GTK%20host%20navigate%20it.%3C%2Fp%3E"
    "%3C%2Fbody%3E";

bool g_quit_requested = false;
long g_auto_quit_ms = 0;
CefRefPtr<CefBrowser> g_browser;

void QuitOnce() {
  if (g_quit_requested)
    return;
  g_quit_requested = true;
  CefQuitMessageLoop();
}

std::string EnvOrEmpty(const char* name) {
  const char* value = std::getenv(name);
  return value ? value : "";
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
    if (g_browser && g_browser->GetHost())
      g_browser->GetHost()->CloseBrowser(true);
    else
      QuitOnce();
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
      fprintf(stderr, "helper: control write failed: %s\n", strerror(errno));
      return;
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
      ::close(fd);
    }
    if (reader_.joinable())
      reader_.join();
  }

 private:
  void ReadLoop() {
    std::string buffer;
    char chunk[4096];

    for (;;) {
      const ssize_t n = ::read(fd_, chunk, sizeof(chunk));
      if (n > 0) {
        buffer.append(chunk, static_cast<size_t>(n));
        std::string::size_type newline;
        while ((newline = buffer.find('\n')) != std::string::npos) {
          std::string line = buffer.substr(0, newline);
          buffer.erase(0, newline + 1);
          if (!line.empty() && line.back() == '\r')
            line.pop_back();
          if (!line.empty())
            CefPostTask(TID_UI, new CommandTask(std::move(line)));
        }
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
  return dict;
}

void EmitStatus(const std::string& message) {
  CefRefPtr<CefDictionaryValue> dict = NewEvent("status");
  dict->SetString("message", message);
  Emit(dict);
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
    if (g_browser && g_browser->GetHost())
      g_browser->GetHost()->CloseBrowser(true);
    else
      QuitOnce();
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
  if (name == "stop") {
    g_browser->StopLoad();
    return;
  }

  // The result of the script comes back through the renderer, which is not
  // wired up yet: this only guarantees the code reached the main frame.
  if (name == "js") {
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
    frame->ExecuteJavaScript(code, "chromium-helper", 0);
    CefRefPtr<CefDictionaryValue> dict = NewEvent("js-ack");
    dict->SetInt("id", command->HasKey("id") ? command->GetInt("id") : 0);
    Emit(dict);
    return;
  }

  fprintf(stderr, "helper: unknown command '%s'\n", name.c_str());
  EmitStatus("unknown-command");
}

class HelperApp : public CefApp {
 public:
  void OnBeforeCommandLineProcessing(
      const CefString& process_type,
      CefRefPtr<CefCommandLine> command_line) override {
    if (!process_type.empty())
      return;  // Browser process only.

    // The component still has no sandbox decision. Settings.no_sandbox covers
    // the browser process; this switch keeps the zygote and renderer
    // subprocesses from trying to set one up.
    command_line->AppendSwitch("no-sandbox");

    // Native Wayland, unless the host picked a platform on purpose.
    if (!command_line->HasSwitch("ozone-platform")) {
      command_line->AppendSwitchWithValue("ozone-platform", "wayland");
      fprintf(stderr, "helper: forcing --ozone-platform=wayland\n");
    }

    if (EnvOrEmpty("CHROMIUM_HELPER_DISABLE_GPU") == "1") {
      command_line->AppendSwitch("disable-gpu");
      fprintf(stderr, "helper: --disable-gpu\n");
    }
  }

  IMPLEMENT_REFCOUNTING(HelperApp);
};

class HelperClient : public CefClient,
                     public CefLifeSpanHandler,
                     public CefLoadHandler,
                     public CefDisplayHandler,
                     public CefKeyboardHandler {
 public:
  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override {
    return this;
  }
  CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
  CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
  CefRefPtr<CefKeyboardHandler> GetKeyboardHandler() override { return this; }

  void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
    CEF_REQUIRE_UI_THREAD();
    g_browser = browser;
    fprintf(stderr, "helper: browser created, id=%d\n",
            browser->GetIdentifier());

    CefRefPtr<CefDictionaryValue> dict = NewEvent("ready");
    dict->SetString("cef", CEF_VERSION);
    dict->SetInt("pid", static_cast<int>(::getpid()));
    dict->SetString("url", browser->GetMainFrame()->GetURL().ToString());
    Emit(dict);

    if (g_auto_quit_ms > 0) {
      fprintf(stderr, "helper: auto-quit in %ld ms\n", g_auto_quit_ms);
      CefPostDelayedTask(TID_UI, new CloseTask(browser), g_auto_quit_ms);
    }
  }

  void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
    CEF_REQUIRE_UI_THREAD();
    fprintf(stderr, "helper: browser closing\n");
    g_browser = nullptr;
    Emit(NewEvent("closed"));
    QuitOnce();
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

    fprintf(stderr, "helper: closing on %s\n", escape ? "Esc" : "Ctrl+Q");
    if (is_keyboard_shortcut)
      *is_keyboard_shortcut = true;
    browser->GetHost()->CloseBrowser(true);
    return true;
  }

  IMPLEMENT_REFCOUNTING(HelperClient);
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
    window->AddChildView(browser_view_);
    window->Show();
    browser_view_->RequestFocus();
    fprintf(stderr, "helper: window created\n");
  }

  void OnWindowDestroyed(CefRefPtr<CefWindow> window) override {
    CEF_REQUIRE_UI_THREAD();
    fprintf(stderr, "helper: window destroyed\n");
    browser_view_ = nullptr;
    QuitOnce();
  }

  // The page is the whole window: the GTK host draws the chrome.
  bool IsFrameless(CefRefPtr<CefWindow> window) override { return true; }
  bool CanResize(CefRefPtr<CefWindow> window) override { return true; }

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

void PrintEnvironment() {
  fprintf(stderr,
          "helper: CEF %s\n"
          "helper: WAYLAND_DISPLAY=%s DISPLAY=%s XDG_SESSION_TYPE=%s\n"
          "helper: %s=%s\n",
          CEF_VERSION, EnvOrEmpty("WAYLAND_DISPLAY").c_str(),
          EnvOrEmpty("DISPLAY").c_str(),
          EnvOrEmpty("XDG_SESSION_TYPE").c_str(), kSocketEnv,
          EnvOrEmpty(kSocketEnv).empty() ? "(unset, no control)"
                                         : EnvOrEmpty(kSocketEnv).c_str());
}

}  // namespace

int main(int argc, char** argv) {
  CefMainArgs main_args(argc, argv);

  CefRefPtr<HelperApp> app(new HelperApp);
  const int exit_code = CefExecuteProcess(main_args, app, nullptr);
  if (exit_code >= 0)
    return exit_code;  // This was a Chromium subprocess.

  PrintEnvironment();

  Control control;
  g_control = &control;
  const std::string socket_path = EnvOrEmpty(kSocketEnv);
  if (!socket_path.empty() && !control.Connect(socket_path))
    return 1;

  // Test hook: lets a smoke run end by itself instead of waiting for a human.
  const std::string auto_quit = EnvOrEmpty("CHROMIUM_HELPER_AUTO_QUIT_MS");
  if (!auto_quit.empty())
    g_auto_quit_ms = std::strtol(auto_quit.c_str(), nullptr, 10);

  const std::string cache_dir =
      CacheDirectory(socket_path.empty() ? "chromium-helper"
                                         : "chromium-helper-hosted");
  fprintf(stderr, "helper: cache=%s\n", cache_dir.c_str());

  CefSettings settings;
  settings.no_sandbox = true;  // Pending the component's sandbox decision.
  settings.windowless_rendering_enabled = false;
  settings.multi_threaded_message_loop = false;
  settings.external_message_pump = false;
  settings.log_severity = LOGSEVERITY_INFO;
  settings.persist_session_cookies = false;
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

  // Hosted: the host navigates us, so start on a blank page rather than
  // keeping a placeholder in the history. Standalone: take the URL from argv.
  const std::string url =
      socket_path.empty() ? PickUrl(argc, argv) : std::string("about:blank");
  fprintf(stderr, "helper: url=%s\n", url.c_str());

  CefRefPtr<HelperClient> client(new HelperClient);
  CefRefPtr<HelperBrowserViewDelegate> view_delegate(
      new HelperBrowserViewDelegate);

  CefBrowserSettings browser_settings;
  browser_settings.background_color = 0xff101828;

  CefRefPtr<CefBrowserView> browser_view = CefBrowserView::CreateBrowserView(
      client, url, browser_settings, nullptr, nullptr, view_delegate);
  if (!browser_view) {
    fprintf(stderr, "helper: CreateBrowserView failed\n");
    control.Shutdown();
    CefShutdown();
    return 1;
  }

  CefRefPtr<HelperWindowDelegate> window_delegate(
      new HelperWindowDelegate(browser_view));
  CefRefPtr<CefWindow> window =
      CefWindow::CreateTopLevelWindow(window_delegate);
  if (!window) {
    fprintf(stderr, "helper: CreateTopLevelWindow failed\n");
    control.Shutdown();
    CefShutdown();
    return 1;
  }

  CefRunMessageLoop();
  control.Shutdown();
  g_control = nullptr;
  CefShutdown();
  fprintf(stderr, "helper: exited\n");
  return 0;
}
