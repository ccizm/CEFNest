# 资源拦截二期：响应过滤（接口壳，2026-10-26）

本文档立项**响应过滤**（拦截二期的名字），是 `work-plan-2026-10-26.md`
§4.4 留下的接口位。一期（咨询式请求拦截）已落地并有自己的门禁；二期在
此独立推进，不阻塞任何已发布面。

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
