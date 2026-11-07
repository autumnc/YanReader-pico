# vendor/ —— 第三方文件

## `esptool-js.bundle.js`

浏览器端刷机用的 [esptool-js](https://github.com/espressif/esptool-js)（Espressif 官方，
Apache-2.0），**原样 vendored、一个字节都没改**：

| | |
|---|---|
| 版本 | `0.7.0` |
| 来源 | `https://unpkg.com/esptool-js@0.7.0/bundle.js`（即 npm 包里的 `bundle.js`）|
| 大小 | 313171 字节 |
| sha256 | `00ebe6fb0b202976d3e2a3c6b9a6bd438c26a81d2af6739c95ac276a64c1a84e` |

上面这个 sha256 与 npm registry 公布的 integrity `sha256-AOvm+wsgKXbT4qPGuaa9Q4wmqB0q9nOclawnamTBqE4=`
**逐字节一致**（base64 同一串），所以这份副本可以被独立复核：

```sh
sha256sum docs/vendor/esptool-js.bundle.js
curl -s https://registry.npmjs.org/esptool-js/0.7.0 | python3 -m json.tool | grep -A2 bundle.js
```

**为什么 vendored 而不是走 CDN / npm**：

1. 这个文件是 **rollup 打出来的自包含 ESM**（`import`/`export` 齐全，无 `require`、
   无 `module.exports`，各型号的 stub flasher 固件已经内联在里面），所以刷机页
   **不需要任何构建步骤** —— 没有 `package.json`、没有打包器、没有 import map，
   `<script type="module">` 直接 `import` 就行。
2. 刷机是把固件写进 flash 的操作，**不愿意让关键路径依赖第三方 CDN 的可用性与完整性**
   （本项目也没有任何其他外部运行时依赖）。同源托管在项目自己的 Pages 上。

## `esptool-js.LICENSE`

上面那个文件对应的 Apache-2.0 许可证全文（取自同一 npm 包的 `LICENSE`，原样放这里）。
