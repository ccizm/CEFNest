# CEFNest 缓存与数据目录落地方案（2026-10-25）

前两轮讨论（现状诊断 + 用户裁定）收敛出的设计，本文档是可直接执行的落地版本：
所有改动点标注到已核实的 `文件:行号`，含代码骨架、锁的归属决策、测试门禁与
实施顺序。执行时按第十节的顺序推进，每步用现有门禁回归。

---

## 〇、设计原则（不变量，后续一切由它推导）

1. **落盘数据只存在于开发者显式指定的目录**。持久模式下
   `root_cache_path = P`、`cache_path = P/browser`，HTTP 缓存、cookies、
   localStorage、IndexedDB、组件数据全部在 P 之下。
2. **不存在 CEFNest 管理的统一缓存目录**。`~/.cache/cefnest-helper-*`
   派生逻辑整体删除；`root_cache_path` 永远非空且永远由我们指定，
   从结构上杜绝 CEF 回退到 `~/.config/cef_user_data`。
3. **缓存纯 opt-in**。唯一公开入口是
   `chromium_session_set_data_directory()`；不调用即无痕（incognito），
   无环境变量后门（宿主残留的同名变量会被显式 unset）。
4. **不指定 = 不落盘**。incognito + CEF 必须写的安装级数据进
   `$XDG_RUNTIME_DIR` 下的 0700 临时目录：正常退出即删，崩溃残留因在
   tmpfs 上、重启自动清零。

对开发者目录的唯一态度：**只创建（0700）、只校验，永不自作主张删除**；
唯一例外是开发者显式调用的 `chromium_session_clear_data()`。

---

## 一、现状锚点（2026-10-25 已核实）

| 位置 | 内容 |
|---|---|
| `src/chromium-session.c:2337-2345` | socket 名每次 start 带新 UUID → helper 派生出新目录，"同 host 复用" 的注释意图从未实现 |
| `src/chromium-session.c:2353-2369` | GSubprocessLauncher 的 env 设置块（WAYLAND_DISPLAY / GDK_BACKEND / CHROMIUM_HELPER_SOCKET / unsetenv DISPLAY、WAYLAND_SOCKET） |
| `src/chromium-session.c:71-77` | 属性枚举，目前仅 `PROP_AUTO_SOURCE_WINDOW`（73-75 行注释自称 "one and only property"，需一并更新） |
| `src/chromium-session.c:99-181` | `struct _ChromiumSession` 字段区 |
| `src/chromium-session.c:3048-3061 / 3064-3093 / 3157-3170` | init / get_property / set_property / finalize |
| `helper/src/main.cpp:219-229` | `CacheDirectory()`：`XDG_CACHE_HOME` 派生（待删） |
| `helper/src/main.cpp:231-248` | `SocketCacheSuffix()` 及其注释（待删） |
| `helper/src/main.cpp:250-274` | `PruneStaleCacheDirs()`：只清 14 天前的（待删） |
| `helper/src/main.cpp:2736-2744` | hosted/standalone 的 cache_dir 决策 + 日志行 |
| `helper/src/main.cpp:2772-2773` | `root_cache_path = cache_dir`、`cache_path = cache_dir + "/browser"`（已验证形状，保留） |
| `helper/src/main.cpp:2799-2801` | prune 后台线程（改造成 tmpfs 清扫） |
| `helper/src/main.cpp:2812-2831` | 两条退出路径（2816 失败路径 / 2827 正常路径），`CefShutdown()` 之后是临时目录删除点 |
| `src/main.c:519-560` | demo 新窗口的 XDG_CACHE_HOME workaround（551 行 setenv，待删） |
| `meson.build:4` | version `0.0.1`；新增符号无需版本_gate |

---

## 二、最终行为矩阵

| start() 前的开发者动作 | root_cache_path | cache_path | 结果 |
|---|---|---|---|
| 不设置（默认） | `$XDG_RUNTIME_DIR/cefnest-helper-<pid>-XXXXXX`（tmpfs，0700） | 空 | incognito，内存缓存；退出删临时目录，重启清 tmpfs 残留 |
| `set_data_directory(session, P)` | P | P/browser | 持久 profile，重启后 cookies/localStorage 保留 |

- 无 `$XDG_RUNTIME_DIR` 时退回 `/dev/shm`；不退 `~/.cache`、不退 `/tmp`。
- 同进程两个活动 session 同路径 → 第二个 `start()` 直接 GError（登记表）。
- 跨进程同路径 → 第二个 helper 在 flock 上失败，打印
  `helper: data directory is in use: <P>` 并 exit 1，宿主收到
  ::closed("process exited")。这正是现状 `main.cpp:231-237` 注释描述的
  相撞场景，但从"互相拖垮"变成"干净、早、可读的失败"。
- helper standalone 直跑同样读环境变量获得持久化（见五）。

---

## 三、API（命名裁定：data 而非 cache）

目录里装的是完整用户数据（cookies、localStorage），不是纯缓存，且推荐的
落点是 `XDG_DATA_HOME`（见第七节），所以命名用 **data**：

```c
void        chromium_session_set_data_directory (ChromiumSession *self,
                                                 const char *path);
const char *chromium_session_get_data_directory (ChromiumSession *self);
gboolean    chromium_session_clear_data         (ChromiumSession *self,
                                                 GError **error);
```

- GObject 属性 `"data-directory"`（string，READWRITE，默认 NULL），
  `g_object_new` 构造时亦可给。头文件按现有风格写长注释块，
  插在 `chromium_session_stop()`（`chromium-session.h:22`）之后。
- 语义要点（写进头文件注释与 README）：
  - 必须绝对路径（相对路径、`~` 开头都在 `start()` 被拒，`~` 天然过不了
    绝对路径检查，不做波浪号展开）；
  - start() 之后设置只对下一次 start() 生效（env 在 spawn 时快照，天然成立）；
  - `persist_session_cookies` 维持 false（`main.cpp:2760`），session cookie
    即便持久模式也不跨重启 —— 这是 Chromium 语义，文档写明；
  - 该目录是完整 profile，无法"磁盘缓存落盘但 cookie 只在内存"。

环境变量（host→helper 内部契约 + standalone 入口）：
`CHROMIUM_HELPER_DATA_DIR`，跟随现有 `CHROMIUM_HELPER_SOCKET /
CHROMIUM_HELPER_NO_SANDBOX / CHROMIUM_HELPER_AUTO_QUIT_MS` 命名族。

---

## 四、Host 侧改动（src/chromium-session.c / chromium-session.h）

### 4.1 字段与属性

- struct（99-181）在 `startup_url` 一带加 `char *data_directory;`；
  finalize（3162-3166）补 `g_free`。
- 属性枚举（71-77）加 `PROP_DATA_DIRECTORY`；73-75 行
  "one and only property" 注释改写为枚举两条。
- pspec（`class_init` 2880 一带）：

```c
g_object_class_install_property
  (object_class, PROP_DATA_DIRECTORY,
   g_param_spec_string ("data-directory", "Data directory", NULL, NULL,
                        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
```

- `set_data_directory()`：NULL 或空串视为清除，否则 `g_strdup` 保存；
  设置时不校验（校验全部前置到 start()，错误带 GError 冒给开发者）。

### 4.2 start() 的接入

顺序（插入点在 start() 开头、重启拆除块 2295 之前——校验失败不应拆掉
正在跑的旧会话）：

1. `data_directory` 非空时：`g_path_is_absolute()` 否则
   `G_IO_ERROR_INVALID_ARGUMENT`，`"data directory must be an absolute path: %s"`。
2. 进程内登记表查重（静态 `GHashTable`，`g_str_hash`，按需创建）：
   命中 → `G_IO_ERROR_FAILED`，`"data directory already in use by another session: %s"`。
3. `g_mkdir_with_parents (P, 0700)` 失败 → GError；
   `g_file_test (P, G_FILE_TEST_IS_DIR)` 否则
   `"data directory is not a directory: %s"`（防"父目录存在但 P 是文件"）。
4. flock 预检（见第六节）：open `P/.cefnest.lock`（O_CREAT|O_RDWR，0600）、
   `flock (LOCK_EX | LOCK_NB)` 失败 → GError
   `"data directory is in use by another session: %s"`；成功后**立即释放
   并关闭**（权威锁在 helper）。
5. spawn 成功（2378-2388）后登记表插入；`running = TRUE`（2393）前后皆可。
6. env 块（2363-2368）：

```c
if (self->data_directory && *self->data_directory)
  g_subprocess_launcher_setenv (launcher, "CHROMIUM_HELPER_DATA_DIR",
                                self->data_directory, TRUE);
else
  g_subprocess_launcher_unsetenv (launcher, "CHROMIUM_HELPER_DATA_DIR");
```

7. 登记表移除：新增静态 `release_data_directory (self)`，在 child 结束的
   每个点调用——`chromium_session_stop()`（339 一带 running=FALSE 处）、
   `child_waited` 回调、start() 顶部重启块（2305-2309）、dispose（3118 一带）。
   幂等，重复调用无害。
8. 头部 include 补 `<sys/file.h>`、`<fcntl.h>`、`<unistd.h>`（若缺）。

### 4.3 clear_data

```c
gboolean
chromium_session_clear_data (ChromiumSession *self, GError **error)
{
  if (self->running)                          → GError "session is running"
  if (!self->data_directory || !*self->data_directory)
                                               → GError "no data directory set"
  if (!g_file_test (dir, G_FILE_TEST_EXISTS))  → return TRUE   /* 没东西可清 */
  fd = open (dir + "/.cefnest.lock", O_RDWR | O_CLOEXEC);      /* 不 O_CREAT */
  if (fd >= 0 && flock (fd, LOCK_EX | LOCK_NB) == -1 && errno == EWOULDBLOCK)
                                               → GError "in use by another session"
  /* ENOENT：锁文件只有活动 helper 会创建，不存在即无持有者 */
  remove_tree (dir, error);                    /* 递归删除整个 P，含锁文件 */
}
```

- `remove_tree()` 为新增静态辅助：`GFileEnumerator` 递归 `g_file_delete`
  （GLib 无 `g_rm_rf`，这几十行不应让每个宿主自己写）。
- 清完后 `data_directory` 字段保持不变：下次 start() 重建目录，
  开发者无需重设。

---

## 五、Helper 侧改动（helper/src/main.cpp）

### 5.1 删除

`CacheDirectory()`（219-229）、`SocketCacheSuffix()` 及注释（231-248）、
`PruneStaleCacheDirs()`（250-274）整体删除；`EnvOrEmpty()`（200-203）保留。

### 5.2 新增（放在原 CacheDirectory 的位置）

```cpp
// tmpfs 基址：XDG_RUNTIME_DIR，退 /dev/shm（都是 tmpfs，重启即清）。
std::string RuntimeBaseDir();

// "<base>/cefnest-helper-<pid>-XXXXXX"，mkdtemp 天生 0700。
// 失败 → stderr + 返回空串，调用方 return 1。
std::string MakeEphemeralRoot();

// 目录名解析 pid；pid 非本进程且 kill(pid,0)==-1&&errno==ESRCH 才 remove_all。
// 只认 "cefnest-helper-" 前缀，绝不碰其他任何东西；失败忽略（best effort）。
void SweepEphemeralRoots(const std::string& base);

// flock P/.cefnest.lock（O_CREAT|O_RDWR，0600，LOCK_EX|LOCK_NB）。
// fd 故意不 close：进程退出/崩溃由内核释放，无陈旧锁。
bool LockDataDirectory(const std::string& dir);

void RemoveTree(const std::string& path);     // std::filesystem::remove_all
```

include 补 `<sys/file.h>`、`<signal.h>`（kill）、`<unistd.h>`（getpid）。

### 5.3 决策点（替换 2736-2744）

```cpp
const std::string given = EnvOrEmpty("CHROMIUM_HELPER_DATA_DIR");
const bool ephemeral = given.empty();
std::string root, profile;
if (ephemeral) {
  root = MakeEphemeralRoot();
  if (root.empty()) return 1;
} else {
  // 拼写错误必须响亮失败：静默降级到 ephemeral 会把"持久化"变成悄悄丢失。
  if (given.front() != '/' ||
      !std::filesystem::is_directory(given)) {
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
fprintf(stderr, ephemeral
        ? "helper: cache mode=ephemeral root=%s\n"
        : "helper: cache mode=persistent root=%s profile=%s\n", ...);
```

### 5.4 设置接线（替换 2772-2773）

```cpp
CefString(&settings.root_cache_path).FromString(root);
if (!profile.empty())
  CefString(&settings.cache_path).FromString(profile);
/* profile 为空 → cache_path 不设置 → incognito（内存缓存） */
```

`root_cache_path` 永远非空：这是杜绝 `~/.config/cef_user_data` 回退的结构保证。

### 5.5 清扫线程（替换 2799-2801）

prune 线程改为 `SweepEphemeralRoots(RuntimeBaseDir())`，hosted 与
standalone 都跑（standalone 也会留下自己的临时目录）；本进程的 pid 天然
被"判活"跳过。仍是独立线程、全退出路径 join 的结构不变。

### 5.6 退出清理（2814-2831 两条路径）

两条路径在 `CefShutdown()` 之后、join 之前/后（与现有顺序一致即可）：

```cpp
if (ephemeral)
  RemoveTree(root);
```

---

## 六、锁的设计（flock 归属——关键决策）

**唯一持有者是 helper**，hosted 与 standalone 一致：

- GLib `GSubprocess` 默认在子进程关闭多余 fd，host 无法靠 fd 继承把锁
  "传"给 helper；做 `take_fd` 管道又引入 fd 号协商，复杂度不值。
- host 两侧都各自 open 会得到两个独立 open file description，第二个
  flock 必然失败——所以 host **不持锁**，只在 start() 做一次非阻塞
  预检并立即释放（第四节 4.2 第 4 步）：把最常见错误提前成 start() 的
  GError。预检释放与 helper 上锁之间存在竞态窗口，无害——helper 的
  flock 才是权威，撞上即 exit 1，宿主 ::closed。
- 同进程查重由登记表兜底（预检 flock 对同进程第二个 session 也会命中，
  双保险）。
- `clear_data` 用非阻塞 flock 探测"无任何活动使用方"：活动 helper（任意
  进程）持有则拒绝；锁文件不存在（ENONENT）即无持有者。
- 崩溃安全：锁随进程消失（内核回收），无陈旧锁；残留的空
  `.cefnest.lock` 文件无害（flock 不看内容）。
- CEF 自己的 SingletonLock 在 `P/browser` 内部，与本锁互不冲突。

---

## 七、demo / examples / 文档

### 7.1 demo（src/main.c）

- **删除** XDG_CACHE_HOME workaround（519-560 的 uuid/cache/config 拼装，
  551 行 setenv）：新默认下每个 helper 天生独立临时 profile，多窗口不再
  抢目录。`open_url_in_new_window()` 缩成"spawn 自身 + URL"，
  519-523 注释同步改写。
- **新增** `--data-dir=<绝对路径>` 参数：传给 `set_data_directory()`；
  不传即默认 incognito。demo 的现有参数解析处接入。

### 7.2 示例

新增 `examples/persistent.c`（以 `examples/browser.c` 为模板）：
用 `g_get_user_data_dir()` 拼应用子目录 + `set_data_directory()`，
演示 localStorage 跨重启保留与 `clear_data()`；注册进
`examples/meson.build` 与两份 examples README。

### 7.3 README（README.md + README.zh-CN.md，双语对齐）

新增"数据目录 / Data directory"一节：

- 行为矩阵（第二节那张表）；
- **放置指引（硬规则）**：推荐 `g_get_user_data_dir()/<app>/`
  （`~/.local/share/myapp/…`）；**不要放 `~/.cache`**——XDG 语义里 cache
  是清理器随时可删的目录，而这里装着登录态（cookies/localStorage），
  放 `~/.cache` 会被 BleachBit 之类静默清掉，用户看到的是"随机掉登录"；
  CEF 要求 cache_path 位于 root_cache_path 之下，所以无法按 XDG 拆成
  data/cache 两处；
- 卸载跟随表：Flatpak（沙箱内 XDG 目录即 `~/.var/app/<id>/`，
  `flatpak uninstall --delete-data` 原生带走）/ Snap（`snap remove --purge`）/
  deb-rpm（Linux 惯例用户数据不随卸载删除，与 Chromium/Firefox 一致）；
  清理数据用 `clear_data()` 或随应用卸载策略；
- standalone helper 的 `CHROMIUM_HELPER_DATA_DIR` 用法；
- 遗留目录手动清理一条命令（代码与包内都不自动删）：

```sh
rm -rf ~/.cache/cefnest-helper ~/.cache/cefnest-helper-hosted-*
```

---

## 八、测试与验收

### 8.1 新增单元测试 `test/data-dir.c`（无合成器，headless）

1. 相对路径 → `start()` FALSE，错误消息含 "absolute"
   （校验在最前，不需要 view/合成器即触发）；
2. `clear_data`：未设目录 → FALSE；目录不存在 → TRUE；
   有垃圾文件的目录 → 删除后不存在；
3. fork 一个持 flock 的子进程 → `clear_data` FALSE 且错误含 "in use"。

注册进 `test/meson.build` 与 CI 列表。

### 8.2 新增门禁 `test/data-dir-gate.sh`（沿用 dnd-gate.sh 驱动 demo 的模式）

HOME 与 XDG_RUNTIME_DIR 重定向到沙箱目录，断言：

1. 默认运行：`~/.cache` 无 `cefnest-helper*` 新增；运行期间
   `$XDG_RUNTIME_DIR/cefnest-helper-*` 存在；退出后消失；helper 日志含
   `cache mode=ephemeral`；
2. `--data-dir` 两次运行（同目录）：日志含 `cache mode=persistent`；
   `P/browser` 存在；localStorage 值跨重启可读回；
3. 相对路径 `--data-dir` → demo 报错退出，含 "absolute"；
4. 第一个 demo 存活时第二个同目录启动 → 第二个 helper 日志含
   "data directory is in use" 且会话 closed（跨进程锁证明）；
5. `kill -9` helper 后临时目录被下次任一 helper 启动的清扫带走
   （pid 判活）。

### 8.3 手动验收清单（对应第四节原则）

1. 连续起停 20 个默认会话：`~/.cache` 零新增、磁盘占用恒定；
2. 全程监视 `~/.config/cef_user_data` 从不出现；
3. 持久模式 cookies/localStorage 跨重启保留；
4. 宿主预置 `CHROMIUM_HELPER_DATA_DIR=...` 但未调 setter → 依然 ephemeral
   （unsetenv 生效）；
5. 现有门禁回归：helper-path.sh、multi-browser-gate.sh、dnd/download/
   find-gate.sh（真实会话类）+ `test/ci-session.sh`。

---

## 九、兼容与迁移

- 此前无公开缓存 API，无调用方破坏；**默认行为变更**（每次新落盘
  profile → incognito）在 README 显著标注。standalone helper 用户若依赖
  `~/.cache/cefnest-helper` 持久化，改设 `CHROMIUM_HELPER_DATA_DIR`。
- 旧目录（`~/.cache/cefnest-helper-hosted-*`、`~/.cache/cefnest-helper`，
  本机实测 175 个会话目录约 1.1 GB）成为孤儿：README 给手动清理命令，
  代码与包脚本都不自动删（破坏性操作，属用户决定）。
- 新符号 `set_data_directory` / `get_data_directory` / `clear_data` /
  属性 `data-directory` 进入公共 API；版本 0.0.1 阶段无需 deprecation 周期。

---

## 十、实施顺序（每步可编译、可回归）

| 步 | 内容 | 完成标志 |
|---|---|---|
| 1 | Host：字段/属性/setter/getter/start 接线（校验、mkdir、登记表、env、预检）+ 头文件文档 | 编译过；data-dir.c 第 1、2 项断言过 |
| 2 | Helper：删旧三函数、决策点、ephemeral root、设置接线、日志、退出清理、清扫线程 | 手动清单 1、2、4；日志断言 |
| 3 | 锁：helper `LockDataDirectory` + host 预检 + `clear_data` + `remove_tree` | data-dir.c 全过；gate 第 4 项 |
| 4 | demo：`--data-dir` + 删 workaround；`examples/persistent.c` | gate 第 2、3 项 |
| 5 | README 双语 + 遗留清理说明 | 文档评审 |
| 6 | `data-dir-gate.sh` + 现有门禁回归 + 手动清单全项 | 全绿 |

风险与开放点（实施时验证，不改变设计）：

- incognito 下临时 root 的实际写入内容与体积（预期仅少量安装级数据）——
  第 2 步实测并记录；
- `browser` 子目录形状保留（已验证）；扁平 `cache_path = P` 不在本方案内，
  如要做需单独 A/B；
- demo 多窗口原先靠独立 XDG_CACHE_HOME 隔离，新默认天然独立，
  由 multi-browser-gate.sh 回归确认等价。
