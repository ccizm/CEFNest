# 打包：装到哪里、helper 怎么找、两条回退方向相反

日期：2026-10-21
问题：P4 的「打包（helper 路径按架构/安装前缀参数化）」。做之前组件只能在自己被构建的那棵树里跑：
`src/meson.build` 没有 install 规则，本体是一个 `executable` 而不是 `shared_library`，没有装出去的头文件，
也没有 `.pc`；helper 要么由 `CHROMIUM_HELPER` 现场指出来，要么就没有第二条路。这一篇把它变成一个装进
prefix 之后不需要任何环境变量也能启动真 Chromium 的东西。

## 1. 装出来的东西

实测装到 `$HOME/.local/cefnest-stage`（`-Dprefix=$HOME/.local/cefnest-stage -Dinstall-helper=true`）：

```
bin/cefnest-demo
include/cefnest/{nested,chromium-view,chromium-session}.h
lib/libcefnest.so.0.0.1        (+ libcefnest.so.0、libcefnest.so 两个符号链接)
lib/pkgconfig/cefnest.pc
libexec/cefnest/shm-client
libexec/cefnest/x86_64/         cefnest-helper + libcef.so + …，一共 1.5 G
```

三条规则各有理由：

- **helper 装在 `<libexecdir>/cefnest/<host_machine.cpu_family()>/` 下面，和它必须并排的 CEF 运行时一起。**
  架构名做目录，是因为两个架构共用一个 prefix 时不该互相覆盖对方的 `libcef.so`；组件后来问的也是自己
  宿主机的这个值，和安装规则用的是同一个变量，不是两份各写一遍的字符串。
- **只有三个头文件进 `includedir/cefnest`。** `nested-internal.h` 是 compositor 各模块之间共享的内部件，
  生成的协议头点名的是宿主没有理由去实现的接口——两者都不该出现在安装树里。
- **`install-helper` 默认 off。** 那 1.5 G 是整个 Chromium，没人想在一次默认的 `ninja install` 里搬它。

## 2. helper 路径：一条规则，不是一个默认值

`chromium_session_new (NULL)` 之后，路径由 `session_find_helper()`（`src/chromium-session.c`）按这个顺序定：

| 顺位 | 来源 | 算数的条件 |
| --- | --- | --- |
| 1 | `CEFNEST_HELPER` | 值里含 `/` |
| 2 | `CHROMIUM_HELPER` | 值里含 `/` |
| 3 | 编译进来的 `HELPER_PATH` | 存在且可执行 |
| 4 | 编译进来的 `HELPER_DEVELOPMENT` | 存在且可执行 |
| 5 | `HELPER_PATH` 再一次 | — |

「含 `/`」是前两条唯一的门槛，为的是让一个只写了程序名的值不当路径用：那样做等于把「请去 `PATH` 上找
`cefnest-helper`」实现成一个 `exec` 前的字符串拼接，而这一层的契约是**报出宿主自己写下来的那串字**——
一个 `start()` 失败时的错误里，应该出现宿主给的路径，哪怕它不存在。第 3、4 位都要求真的可执行，
所以一个装好的组件不会因为旁边还留着开发树就悄悄改用开发树那份 Chromium：

```c
/* Nothing to start. Name the path this build was configured with, because
 * that is the one a packager can compare against where they actually put it. */
return g_strdup (HELPER_PATH);
```

`chromium_session_new (path)` 给的显式路径压过前两位——它是构造函数的参数，不需要再被环境变量否掉。
`test/helper-path.c` 把这五档全部钉住（不需要显示器、不需要窗口、不需要浏览器），它是这个套件里第一个
`suite: 'unit'` 的用例。

## 3. 踩到的那个 bug：`get_option('libexecdir')` 是相对 prefix 的

第一版把 `get_option ('libexecdir') / 'cefnest' / helper_arch` 直接烧进了 `-DHELPER_PATH`，
展开出来是：

```
-DHELPER_PATH="libexec/cefnest/x86_64/cefnest-helper"
```

对安装规则来说这完全正确，对烧进二进制的默认值来说完全错：组件会去**浏览器当时所在的那个目录底下**
找 helper。 staging 的探测程序于是回落到第 4 档，报出开发树的路径——看起来像装成功了，实际一次都没碰
过安装位置。

更难办的是**这条 unit test 当时是绿的**：`HELPER_DEVELOPMENT` 存在、绝对、可执行，把前一位的错完全遮住了。
补的断言是针对那个值本身，不是针对解析结果：

```c
g_assert_true (g_path_is_absolute (HELPER_PATH));
```

反向对照：`-Dhelper-path=relative/helper/cefnest-helper` 重跑，`helper-path` 用例 abort（exit 134）；
改回 `-Dhelper-path=` 就绿。修法是 `get_option ('prefix') / get_option ('libexecdir') / …`，
并在 `meson.build` 里把这件事写成注释，因为下一次读到 `get_option` 的人仍会默认它是绝对路径。

## 4. `$ORIGIN` 出现在两处，理由不同

- **helper 那边**：它靠 `$ORIGIN` 找 `libcef.so`，没有别的机制。所以 `install_subdir (helper_build_dir, …)`
  搬的是整个 cmake 产物目录而不是只有那个可执行文件——运行时文件必须跟它并排落地。
- **demo 那边**：`install_rpath: '$ORIGIN/' + fs.relative_to (prefix/libdir, prefix/bindir)`。用 `$ORIGIN`
  而不是绝对 libdir，是因为打包机构建时会换一个 `DESTDIR`，写死的路径跟着 prefix 一起失效。
  参数顺序反了会静默得到 `$ORIGIN/../bin`，`ldd` 报 `libcefnest.so.0 => not found`；
  `fs.relative_to (a, b)` 给的是「a 相对 b」，所以传的是 `(libdir, bindir)`。

`fs.relative_to` 要 meson ≥ 1.3，`meson_version` 从 1.1.0 抬到 1.3.0。抬的依据不是猜的——meson 自己会报：

```
WARNING: Project specifies a minimum meson_version '>= 1.1.0' but uses features which were added in newer versions:
 * 1.3.0: {'fs.relative_to'}
```

## 5. demo 的模式选择，和它的另一条回退方向相反

demo 原来只在 `CHROMIUM_HELPER` 被设过时才进 session 模式。装好之后这件事不该再需要环境变量，于是
判据变成「要么有人指了个 helper，要么组件自己找得到一个可执行的」：

```c
const gboolean use_session = g_getenv ("CHROMIUM_HELPER") != NULL ||
                             helper_is_available ();
```

`helper_is_available()` 造的是一次性的 session 对象、只看它给出的路径能不能执行——找路径的规则因此
只有 `chromium-session.c` 一份，demo 里没有第二套。

同一个文件里 `shm-client`（没有浏览器时 demo 驱动的那个客户端）也做两档回退，**但顺序是反的**：先开发
树，再安装位置。两者的失败模式不一样：装好的组件去跑开发树那份 Chromium，是在跑错的浏览器；demo 去跑
安装位置那份 `shm-client`，是在跑自己测试驱动的旧副本，而每一个 gate 脚本都假设跑的是当前这棵树刚编出来的
那一份。树不在了才没有「旧的」可优先，这时安装位置才是唯一能用的。两条分支都实测过：

```
nested compositor on WAYLAND_DISPLAY=wayland-1, client: /home/…/builddir-stage/test/shm-client
nested compositor on WAYLAND_DISPLAY=wayland-1, client: /home/…/.local/cefnest-stage/libexec/cefnest/shm-client
```

后者是把树里那份临时改名得到的。`shm-client` 的 `install:` 因此跟着 `install-demo` 走：装了 demo 就得带上
它，否则一台没有 Chromium 的机器上，安装版 demo 开出一个空窗。

## 6. 当成库用

`.pc` 是这么生成的（`pkgconfig.generate`）：`requires:` 列了宿主 include 头文件时一定会连带 include 的
六个包，`extra_cflags: [ '-I${includedir}' ]` 把目录本身也带上——宿主写的是 `<cefnest/chromium-session.h>`，
而 `nested.h` 这种光秃秃的名字太容易在别人树里被先找到。装出来的内容：

```
Requires: gtk4 >= 4.14, json-glib-1.0, wayland-server, xkbcommon, gbm, libdrm
Libs: -L${libdir} -lcefnest
Cflags: -I${includedir}/cefnest -I${includedir}
```

`cflags:` 不是 `pkgconfig.generate` 的关键字，meson 1.7 会直接拒绝：`got unknown keyword arguments "cflags"`。
实测一个只有这三行的探测程序：

```c
ChromiumSession *s = chromium_session_new (NULL);
printf ("%s\n", chromium_session_get_helper_path (s));
```

```
$ PKG_CONFIG_PATH=$HOME/.local/cefnest-stage/lib/pkgconfig cc probe.c -o probe $(pkg-config --cflags --libs cefnest)
$ env -u CHROMIUM_HELPER -u CEFNEST_HELPER LD_LIBRARY_PATH=$HOME/.local/cefnest-stage/lib ./probe
/home/siem/.local/cefnest-stage/libexec/cefnest/x86_64/cefnest-helper
```

一个环境变量都没设，报的是安装位置而不是仍在旁边的开发树——第 2 节的第 3 位压过第 4 位，这条实测就是它。
（探测程序在 prefix 外面，所以需要 `LD_LIBRARY_PATH` 或 `ldconfig`；安装进 `bin/` 的 demo 不需要，因为
第 4 节那条 `$ORIGIN/../lib`。）

## 7. 四个选项，三种典型配置

| 选项 | 默认 | 作用 |
| --- | --- | --- |
| `helper-path` | 空 = 由 prefix 和架构推 | 烧进库的默认 helper 路径 |
| `helper-build-dir` | `helper/build/Release` | cmake 产物目录：既是开发回退，也是 `install-helper` 搬的东西 |
| `install-helper` | `false` | 把 helper 和它一整层 CEF 运行时装进 prefix |
| `install-demo` | `true` | 装 `cefnest-demo`，连带它回退用的 `shm-client` |

```bash
# 开发树：什么都不设，demo 用 CHROMIUM_HELPER 或树里的 helper
meson setup builddir && ninja -C builddir

# 自己机器上装一份能跑的
meson setup builddir -Dprefix=$HOME/.local/cefnest-stage -Dinstall-helper=true
ninja -C builddir install

# 打包机：helper 由包管理器放到别处，demo 不装
meson setup builddir -Dprefix=/usr -Dhelper-path=/usr/lib/cefnest/x86_64/cefnest-helper \
  -Dinstall-demo=false
DESTDIR=$PKG ninja -C builddir install
```

第三种配置里 `helper-path` 必须是绝对路径——`test/helper-path.c` 的第一条断言就是它，而第 3 节说明这条
断言为什么值得留在这里。

## 8. 装好的东西确实跑起了一页

上面那套 staging 安装之后，把 `CHROMIUM_HELPER` 和 `CEFNEST_HELPER` 都清掉，直接跑安装位置的二进制：

```bash
env -u CHROMIUM_HELPER -u CEFNEST_HELPER \
  CHROMIUM_URL=file://$PWD/test/dnd-page.html \
  CHROMIUM_SESSION_SCRIPT=/tmp/stage.session \
  $HOME/.local/cefnest-stage/bin/cefnest-demo
```

脚本只有 `load` / `wait` / `js document.title + '|' + …draggable` / `quit` 四行。实测这一行：

```
nested compositor on WAYLAND_DISPLAY=wayland-1, helper: /home/siem/.local/cefnest-stage/libexec/cefnest/x86_64/cefnest-helper
demo: loaded …/test/dnd-page.html (HTTP 200)
demo: script 3 ok value="DND-PAGE|true"
```

`helper:` 报的是安装位置而不是仍在旁边的开发树，`script … ok` 是一次走完的 JS 往返——它经过的第 3 位
就是编译进来的那个默认值，这一 run 没有任何环境变量可以怪。横幅里还有 `helper: sandbox=on`、
`CEF 154.0.34+g14c5a08+chromium-154.0.8037.98`，退出码 0。
重构之后的回归：`meson test -C builddir` 13/13，`./test/dnd-gate.sh` 六次真 Chromium 运行、59 条断言全过。

## 9. 还没有

- helper 本身仍由 cmake 构建，meson 不驱动它，`install-helper` 只是搬 cmake 留下的东西——「先 cmake、
  再 meson install」这个顺序还是硬要求。这条顺序本来会静默失败：`install_subdir()` 搬一个不存在的目录
  不是错误，meson 打印一行自信的 `Installing subdir …/helper/build/Nope to …/libexec/cefnest/x86_64`，
  造出一个**空目录**，退出码 0。一个装出空 helper 目录的包正是这个选项唯一要防的事，所以现在
  `src/meson.build` 在配置阶段就要 `fs.is_file (helper_build_path)`，而且问的是那个可执行文件而不是
  目录（cmake 可能被指到了别处，目录在、东西不在）：

  ```
  src/meson.build:70:4: ERROR: Problem encountered: install-helper is on but
  /home/siem/文档/ChromiumGTK/helper/build/Nope/cefnest-helper does not exist.
  Build the helper with cmake first, or point helper-build-dir at the cmake
  output directory that holds it.
  ```

  反向对照就是 `-Dhelper-build-dir=helper/build/Nope` 配置一次（上面那行），再用真正的
  `helper/build/Release` 配置一次（通过）。
- 没有 soname/ABI 门禁：`soversion: '0'` 是手写的，导出清单没有生成（没有 `.symbols` 文件），
  `version:` 直接跟 `project` 的 0.0.1。
- 没有 deb/tar 的打包脚本，没有 desktop 文件；`third_party/cef/dist` 那份分发仍假定与可执行文件并排。
