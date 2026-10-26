# 资源拦截二期：响应过滤（接口壳，2026-10-26）

本文档立项**响应过滤**（拦截二期的名字），是 `work-plan-2026-10-26.md`
§4.4 留下的接口位。一期（咨询式请求拦截）已落地并有自己的门禁；二期在
此独立推进，不阻塞任何已发布面。

## 落地记录（2026-10-26）

两个 op 落地，第三个 op 实测定案为无面、被点名拒绝。门禁
`test/filter-gate.sh`（5 跑 / 16 断言，已进 `ci-all` 的 `filter` 行）。

**形状**：`filter-add {id, patterns, ops}` / `filter-remove {id}` 走控制
通道；Session API `chromium_session_filter_add/remove`；demo 脚本步骤
`filter-add <id> <patterns>[;…] <ops-b64>`（ops 走 base64 JSON，与
`script-add` 的 code 同一逃逸）。规则表与互斥纪律与一期同处
（`g_filter_rules` + `g_intercept_mutex`）；无规则注册 =
`GetResourceRequestHandler` 照旧早退，零开销直通。

**replace 与跨块拼接（本文 §"多块拼接策略实测"的答案）**：引擎按规则
顺序串流水，每级 holdback = find 长度 − 1 字节，跨块匹配照常命中——
"超界即错过"的措辞不需要了，字面替换的拼接不是深水区。实测：380 KiB
探针页过引擎，`chunks=7 in=389403 out=389418 spans=1`（spans 计的是
只靠拼接才完成的匹配，7 块 389 KB 全量过流，门禁断言页内零残留
marker 且替换计数精确）。流式协议的一条实测发现值得记录：CEF 的
`Filter()` 循环里，**`data_out_written == data_out_size` 是换取下一次
调用的唯一货币**——空输入调用后写不足整缓冲就直接丢失流出权。引擎的
纪律是 flush 前永远只写满减一字节（于是空输入调用只可能是响应结束，
无歧义），flush 后响应已定，写满整缓冲换下一次调用把尾部排干。

**inject-css**：`text/html` 判定用响应 mime（忽略参数大小写前缀），
`</head>` ASCII 大小写不敏感扫描、同样跨块，流里没有标记就附加在流
末尾。`filter-gate` 的 css 场景以 `getComputedStyle` 读回
`rgb(1, 2, 3)` 断言。

**drop-header：实测定案，面不存在**。CEF 156 的头文件在每个携带
response 的回调上都标注"cannot be modified"；实测把响应头改写
（`GetHeaderMap` → 擦除 → `SetHeaderMap`）先后挂在
`OnResourceResponse` 与 `GetResourceResponseFilter` 两处，改写都执行了
（helper 日志在），**页面的 `fetch()` 读回响应头原样返回——均不上线**。
按总计划"造数即谎报"的裁定，`drop-header` op 在 `filter-add` 处被点名
拒绝（`filter-add-drop-header-unsupported`），不留一条静默无效的死路；
CSP/HSTS 不碰的裁定由此自然成立。

**限额**：find/replace 各 ≤ 64 KiB（与 script-result 行一致）、css
≤ 1 MiB、≤ 32 op；超限或未知 op 拒绝整条规则
（`filter-add-op-too-large` / `filter-add-bad-op` / 
`filter-add-needs-patterns` / `filter-add-needs-ops`），拒绝的 id 不注册
任何东西。

## CEF 面

`CefResourceRequestHandler::GetResourceResponseFilter` +
`CefResponseFilter`：每个匹配请求挂一个过滤器，响应字节流过滤器的
`Filter` 流过——分块到达、可改写、可膨胀（有 offset/size 限制语义需要
实测）。

## 形状：声明式集成，规则长在组件/-helper 里

```
filter-add {id, patterns, ops: [{type: replace, find, replace}],
                              [{type: inject-css, css}],
                              [{type: drop-header, name}]}
filter-remove {id}
```

宿主只声明规则不写回调。规则引擎长在 helper 里（与一期的规则表同一
位置、同一互斥纪律）；引擎按 ops 顺序对流做字面替换/注入/头剔除。

## 本批明确不做（裁定沿用总计划 §六）

- **通用脚本响应过滤不做**：流式跨 chunk 正则的边界语义（一个 match
  跨两个 32 KiB 块）是深水区，声明式的 `replace` 也有同样问题——所以
  replace 的 `find` 限定为**单块内可完成**的匹配（helper 侧块拼接策略
  立项后实测再定，超界即错过的语义要写死在文档里）。
- inject-css 只对 `text/html` 响应生效（content-type 判定）；drop-header
  只允许剔除响应头，不允许伪造 security 相关头（CSP、HSTS 由
  Chromium 自己的管线管，helper 不碰）。

## 门禁形状（立项时细化）

`test/filter-gate.sh`：本地 HTTP 服务器出一页带标记字符串的 HTML →
`replace` 后页面读不到标记、`inject-css` 后 `getComputedStyle` 见样式、
`drop-header` 用自定义头回显断言；无注册=零开销直通沿用一期的形状。

## 实施顺序建议

1. helper：`GetResourceResponseFilter` 工厂 + 单 op（replace 单块）+
   filter-gate。
2. 多块拼接策略实测（一个 match 跨块的行为数据），据实裁定 replace
   语义的文档措辞。
3. inject-css / drop-header。
4. Session API（`chromium_session_filter_add/remove`）与双语文档。
