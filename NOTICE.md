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

`src/api.c`、`src/kern.c`、`src/build.h`、`assets/index.html`、`assets/param.json`、
`assets/icon0.png`、`Makefile`、`ps5/`、`.github/` 为本工程新写。

因此本工程整体以 **GPL-3.0-or-later** 分发。分发二进制时须同时提供对应源码。
