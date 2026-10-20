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
//   host   -> helper   {"cmd":"js","code":"document.title","id":7}
//   helper -> host     {"event":"js-result","id":7,"ok":true,"value":"Page"}
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
constexpr char kJsResultMessage[] = "chromium-helper-js-result";

/* Events are written on the UI thread with a 200 ms send timeout, so a script
 * that returns the whole DOM would stall Chromium and then be dropped anyway.
 * Say so instead. */
constexpr size_t kMaxResultBytes = 64 * 1024;

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

// ExecuteJavaScript has no return value, so the code is wrapped: the page
// evaluates it, turns the result into JSON itself, and hands that one text to
// kReportFunction. Statements work as well as expressions because eval() yields
// the last value; a script that returns nothing reports null, not a failure.
std::string WrapScript(const std::string& code, int id) {
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
  out += "try { __value = eval(" + literal + "); }";
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
    const int id = command->HasKey("id") ? command->GetInt("id") : 0;
    frame->ExecuteJavaScript(WrapScript(code, id), "chromium-helper", 0);
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
  }

 private:
  CefRefPtr<CefV8Handler> handler_{new ReportHandler};

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
    // subprocesses of a hosted run read Seccomp=0 in /proc
    // (docs/sandbox-policy.md). Appending the switch a second time would only
    // give the two paths a chance to drift.

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

 private:
  CefRefPtr<CefRenderProcessHandler> render_handler_{
      new HelperRenderProcessHandler};

  IMPLEMENT_REFCOUNTING(HelperApp);
};

class HelperClient : public CefClient,
                     public CefLifeSpanHandler,
                     public CefLoadHandler,
                     public CefDisplayHandler,
                     public CefKeyboardHandler,
                     public CefRequestHandler {
 public:
  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override {
    return this;
  }
  CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
  CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
  CefRefPtr<CefKeyboardHandler> GetKeyboardHandler() override { return this; }
  CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }

  bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                CefRefPtr<CefFrame> frame,
                                CefProcessId source_process,
                                CefRefPtr<CefProcessMessage> message) override {
    if (source_process != PID_RENDERER || message->GetName() != kJsResultMessage)
      return false;

    EmitJsResult(message->GetArgumentList()->GetString(0));
    return true;
  }

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

  // The renderer is the process the sandbox exists for (docs/sandbox-policy.md),
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

  // Hosted: no initial navigation. The empty document a browser starts with is
  // not a history entry of its own — the host's first load replaces it, which is
  // why the first page reports can_go_back false — but a URL passed here *does*
  // become an entry, so starting on about:blank would leave the user one Back
  // press away from a page they never asked for. Standalone mode takes argv.
  const std::string url =
      socket_path.empty() ? PickUrl(argc, argv) : std::string();
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
