# 数据目录

[English](data-directory.md) | 简体中文

页面自己的数据——cookies、localStorage、IndexedDB、HTTP 缓存——放在哪里，
什么时候才会落盘。这是 `chromium_session_set_data_directory()` /
`chromium_session_get_data_directory()` / `chromium_session_clear_data()` 与
`data-directory` 属性的完整契约；方法与信号速览见 [api.zh-CN.md](api.zh-CN.md)。

**不指定目录时，session 不向磁盘写任何东西。** helper 以 Chromium 的
incognito 模式运行：cookies、localStorage、IndexedDB 和 HTTP 缓存都在内存里；
CEF 必须写的那一个安装级目录是 `$XDG_RUNTIME_DIR` 下（未设置时是 `/dev/shm`）
的 0700 临时目录，helper 正常退出时删除，被杀死留下的残根由下一次启动清扫。
`~/.cache` 下不会出现任何东西，CEF 也不会回退到 `~/.config/cef_user_data`。

要让页面数据跨重启保留，在 `start()` 之前指定一个绝对路径：

```c
chromium_session_set_data_directory (session, "/home/me/.local/share/myapp/cefnest");
chromium_session_start (session, "https://example.com", &error);
```

| `start()` 之前 | `root_cache_path` | `cache_path` | 结果 |
| --- | --- | --- | --- |
| 不设置（默认） | `$XDG_RUNTIME_DIR/cefnest-helper-<pid>-XXXXXX`，0700 | 空 | incognito，内存缓存；退出删临时目录，残留由下次启动清扫 |
| `set_data_directory (session, P)` | `P` | `P/browser` | 持久 profile：cookies 与存储跨重启保留 |

规则：

- 必须是绝对路径。`~` 不会展开，相对路径会让 `start()` 以
  `G_IO_ERROR_INVALID_ARGUMENT` 失败。
- 目录不存在时按 0700 创建。CEFNest 从不自行删除它；唯一的删除入口是
  `chromium_session_clear_data()`，它保留属性值（下一次 `start()` 会把目录
  重新建成空的）。
- 一个目录只归一个 helper：第二个 session 指向已在使用的目录会尽早失败。
  同进程由登记表返回 `G_IO_ERROR_BUSY`；跨进程时 helper 的 `flock` 拒绝，
  该 session 以 "process exited" 关闭。
- 这是一个完整的 profile。CEF 要求 `cache_path` 位于 `root_cache_path`
  之下，因此不存在“缓存落盘、cookies 只在内存”的拆法。
- 即便是持久模式，session cookie 仍随 helper 一起消失
  （`persist_session_cookies` 为 false）。
- session 运行期间设置该属性只对下一次 `start()` 生效，不影响当前 helper。

**放在哪里。** 用 `g_get_user_data_dir()`——通常是
`~/.local/share/<应用>/…`。**不要放 `~/.cache`**：XDG 把那个目录定义为可随时
清理的，BleachBit 之类的工具会静默删掉登录态，用户看到的是“随机掉登录”。
数据是否随卸载删除由平台决定，而不是 CEFNest：

| 安装形态 | 数据目录的结局 |
| --- | --- |
| Flatpak | 沙箱内该路径落在 `~/.var/app/<id>/…`；`flatpak uninstall --delete-data` 会带走 |
| Snap | `snap remove --purge` 删除 |
| deb / rpm | 与 Chromium、Firefox 的 Linux 惯例一致：用户数据随卸载保留；请自行删除或调用 `clear_data()` |

demo 接受 `--data-dir=<绝对路径>`；不传时 demo 同样是 incognito。

standalone helper 用同一个契约，只是入口是环境变量：
`CHROMIUM_HELPER_DATA_DIR=/绝对路径 ./cefnest-helper` 即持久模式，不设置即
incognito。宿主始终替子进程做决定：库在 spawn 之前会 unset
`CHROMIUM_HELPER_DATA_DIR`，所以宿主环境里残留的同名变量不会把 session
变成落盘模式。

旧版本会派生 `~/.cache/cefnest-helper*` 缓存目录。现在没有任何代码再写那里，
也没有任何代码会替你删除它们；升级后手动清理：

```sh
rm -rf ~/.cache/cefnest-helper ~/.cache/cefnest-helper-hosted-*
```
