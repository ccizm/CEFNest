# site/

站点的文档正文**不在这里**。markdown 只有一份，就在仓库的 `docs/` 与根 `README*.md`；
这一层只做目录、渲染与构建产物表。所以改文档就是改 markdown，不需要动站点代码。

## 页面

| 地址 | 是什么 |
| --- | --- |
| `/` | 文档索引：按分组列出每一篇，每行就是仓库里的一个 markdown 文件 |
| `/builds` | 构建产物：每个版本 tag 一行，deb / rpm 两张表，带版本子串过滤与状态列 |
| `/docs/<slug>` | 一篇 markdown 的渲染结果（侧边栏是全量目录，正文下方是上一篇 / 下一篇） |
| `/api/<section>` | 调用文档：方法、信号与参数的结构化对照表，数据在 `src/data/apiData.js` |

旧地址 `/downloads` 与 `/dev/*` 保留为跳转。路由是 hash 模式，构建产物可以直接丢进任何静态
目录——`base` 是 `./`，也因此小节链接不能用 `#锚点`（第二个 `#` 会被 vue-router 吃掉），
本页小节表是脚本滚动。

## 目录与 slug

`src/docs/manifest.js` 记「分组、顺序、侧边栏短标签」。slug 规则：`docs/` 下的去掉前缀，
仓库根的那两份加 `root/`——于是 `docs/README.md` 是 `README`，根 `README.md` 是
`root/README`，两者不会撞在同一个地址上。

新增或改名 markdown 后必须同步这里：`npm run build` 前会跑 `scripts/check-docs.js`，
两边数不上就红，而不是等浏览器里出现一页空白。

文档之间以行内代码写出对方完整路径（`` `docs/guides/packaging.md` ``）——仓库里读起来正好，
`src/docs/markdown.js` 在渲染时把对得上号的行内代码变成站内链接，对不上的照旧是代码。

## 本地跑

```bash
npm install
npm run dev        # http://localhost:5173
npm run build      # 先 check-docs，再产出 dist/
npm run preview
```

dev server 要越过 `site/` 这一层去读仓库的 markdown，所以 `vite.config.js` 里有
`server.fs.allow: ['..']`；构建期没有这个限制，`import.meta.glob` 直接把文件读成按需加载的
分片，一篇文档一个 chunk。

## 数据

- `src/data/release.js`：版本号只写一次，包名规则与 Release 直链都从它推出来。
- `src/data/builds.js`：一行一个 tag，日期取 tag 的创建时间，说明取它指向的提交。
  `status` 只写被观察到的事实——冒烟阶段排在创建 Release 之前，一条红的冒烟意味着那一版
  根本没发包，直链会 404。判定过程写在 `../docs/testing/cnb-ci.md`。
