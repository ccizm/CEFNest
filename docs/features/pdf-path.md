# PDF 播放验证（2026-10-21）

## 1. 结果：CEF 的 PDF viewer 可以工作

启动 demo，加载本机：

```
file:///usr/share/doc/shared-mime-info/shared-mime-info-spec.pdf
```

CEF 把页面交给内置 PDF viewer：侧边缩略图、页码 `1 / 19`、缩放、旋转、下载、打印按钮
都在；`dump` 得到的是完整 PDF 页面，不是空白或下载提示。

## 2. 边界

- 这是 **CEF 内置 PDF viewer**（HTML/JS 扩展）的画法，不是另写一个 GTK PDF 控件；
  页面里的 PDF 选择/复制、缩放和打印仍走 Chromium 自己的 UI。
- 下载按钮仍经过本项目的下载通道（`::download-begin` / accept / reject）。
- 没有测试加密 PDF、PDF 表单、批注或打印到打印机；本次只验证“能打开、能画、能翻页”。
- 真实 PDF viewer 的完整行为依赖 CEF 分发包里携带的 PDF 资源；换成不带该资源的
  minimal 构建时要重新跑这一条。
