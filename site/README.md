# CEFNest 项目页

一个单页项目站：Vite + Vue 3 + Bootstrap 5，无路由、无后端，构建产物是纯静态文件。

```bash
npm install
npm run dev      # http://localhost:5173
npm run build    # dist/
npm run preview  # 预览 dist/
```

`vite.config.js` 里 `base: './'`，所以 `dist/` 可以放在任意子路径下（CNB Pages、
GitHub Pages、nginx 的任意目录），不需要改构建配置。

## 结构

```
index.html              入口与 meta
public/cefnest.svg      图标与 favicon
src/main.js             引入 Bootstrap CSS、图标与样式
src/App.vue             页面内容（简介、快速开始、文档入口）
src/style.css           版式、代码块、卡片等自定义样式
src/components/         导航栏与带复制按钮的代码块
src/data/content.js     文案、代码片段与文档链接
```

内容与 README、`docs/control-channel.md`、`examples/README.md` 保持一致；
仓库链接都指向 CNB。
