# PDF 查看器配方

[English](pdf-viewer.md) | 简体中文

点击 PDF 链接时页面空白——实测（2026-10-26）：发行包没有任何 PDF viewer
扩展，`application/pdf` 提交后页面里没有 viewer 的 `embed`，正文为空。CEF
没有查看器面，所以组件层接不了这一块；但**宿主用既有 API 就能拼出完整闭
环**：拦截 + 自定义 scheme + 脚本注入。这是一份宿主侧配方，组件零改动。

## 三个零件

1. **拦截**：`chromium_session_intercept_add(id, patterns, NULL)`，pattern
   `"*://*/*.pdf"`（或你收窄的域）。命中时 `::request-intercept` 到达，
   宿主以 `chromium_session_request_respond(request_id,
   CHROMIUM_REQUEST_REDIRECT, "viewer://pdf?src=<原 URL>")` 重定向到自己的
   查看器 scheme。
2. **查看器 scheme**：`chromium_session_scheme_register("viewer", …)`
   （start 前设置 flags，见 [control-channel](control-channel.zh-CN.md) 的
   `CHROMIUM_HELPER_SCHEMES`），用 `::scheme-request` /
   `scheme_response` / `scheme_write` / `scheme_end` 服务查看器壳页。壳页
   由你打包——[pdf.js](https://mozilla.github.io/pdf.js/) 的发行文件
   （约 2 MiB）从上游取得，放进你自己的资源目录；组件不携带它，体积是
   你们的取舍。
3. **取回 PDF 字节**：壳页里的 pdf.js 需要拿到原始 PDF。两条路：
   - 壳页直接 `fetch(原 URL)`——跨 scheme 时给 `viewer` 注册
     `cors` flag，或让原站自己可读；
   - 或者由宿主代取：拦截命中的那次请求顺手用你自己的网络栈取回字节，
     经 `scheme_write` 喂给壳页（原 URL 只作参数，不再由页面发起）。
     这条路让"仅 Cookie 可见的内网 PDF"照常工作，因为宿主进程本来就在
     拦截点上。

## 下载与打印不受影响

- 拦截 pattern 只匹配 `.pdf`：不想重定向的（`Content-Disposition:
  attachment`、下载意图）在 `::request-intercept` 里自己判——重定向或放
  行（放行走既有下载流程）。
- 页内打印：`window.print()` 以 `::print-requested` 到达，`print_pdf` 给
  你字节；查看器自己的打印按钮同样走这条路，不经过 Chromium 的打印管线。

## 顺序

```c
/* start() 之前 */
chromium_session_scheme_register (self, "viewer", flags);
/* start() 之后、第一次导航之前 */
chromium_session_intercept_add (self, 1, patterns, NULL);
```

`scheme` 的 flags 只在进程启动时生效（与 proxy 同型）；拦截规则运行期
随时可加。两件事都各有门禁（`scheme-gate`、`intercept-gate`），本配方
没有新代码路径需要测——测的是你的壳页。
