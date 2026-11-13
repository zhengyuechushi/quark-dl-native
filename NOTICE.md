# 来源与许可

本工程是 PS5 payload（`.elf`），骨架、HTTP 服务层与网页资源加载方式来自：

- **owendswang/ps5-web-file-manager** — GPL-3.0-or-later
  <https://github.com/owendswang/ps5-web-file-manager>

直接沿用并保留原版权头的文件：

| 本工程文件 | 来源 | 改动 |
|---|---|---|
| `src/websrv.c` / `src/websrv.h` | `src/websrv.c`、`src/websrv.h` | 去掉文件管理与上传流式处理，路由改指向 `api.c` |
| `src/asset.c` / `src/asset.h` | `src/asset.c`、`src/asset.h` | 未改动 |
| `src/notify.c` / `src/notify.h` | `src/notify.c`、`src/notify.h` | 未改动 |
| `gen-asset-module.py` | 同名文件 | 未改动 |
| `install-libmicrohttpd.sh` | 同名文件 | 未改动 |
| `src/main.c` 的启动序列 | `src/main.c` | 进程名、端口探测、同名实例清理、监听循环保留原逻辑 |
| `LICENSE` | 项目 LICENSE | GPL-3.0 全文 |

阶段二新增、为本工程新写：`src/net.c`（socket/DNS/TLS 封装）、`src/httpc.c`（HTTP/1.1 客户端）、
`src/dl.c`（下载队列）、`src/ca.h` 与 `gen-ca-module.py`（根证书嵌入）、`install-mbedtls.sh`、
`tools/mockdl.py` 与 `tools/test-stage2.sh`（本地验收）、`assets/index.html` 的下载界面。
另有 `src/kern.c`、`src/build.h`、`src/api.c`、`assets/param.json`、`assets/icon0.png`、
`Makefile`、`ps5/`、`.github/` 自阶段一起即为本工程新写。

## 第三方组件

| 组件 | 许可 | 使用方式 |
|---|---|---|
| mbedTLS 2.28.8（Mbed-TLS/mbedtls） | Apache-2.0 | 由 `install-mbedtls.sh` 以默认配置编译为静态库并链入，未修改源码 |
| Mozilla CA 根证书包（经 <https://curl.se/ca/cacert.pem>） | MPL-2.0 | 原样嵌入为 `assets/ca.pem`，未修改 |

两者都可再分发；`assets/ca.pem` 本身即 MPL-2.0 数据的原样副本，未做任何变更。

因此本工程整体以 **GPL-3.0-or-later** 分发。分发二进制时须同时提供对应源码。
