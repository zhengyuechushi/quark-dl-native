# Quark DL — PS5 上的夸克网盘下载器

一个越狱 PS5 的 **payload**：加载后在主机上跑一个小型 HTTP 服务，用浏览器（主机自带或同一
局域网的手机/电脑）扫码登录夸克网盘、浏览目录、勾选文件或文件夹，下载到主机磁盘。
产物是**单个 ELF**，没有安装包、没有依赖、没有 zip。

- 当前版本：**0.12.6**（版本演进见 [CHANGELOG.md](CHANGELOG.md)）
- 形态：`quark-dl_v0.12.6.elf`，约 1 MB（其中 ~200 KB 是根证书库）
- 主屏幕图标：由 payload 自己注册，点图标直接开控制页

## 一分钟装机

1. **加载**：Payload Manager 导入，或 `nc <PS5-IP> 9021 < quark-dl_v0.12.6.elf`；
2. **看通知**：屏幕右上出现 `Quark DL 0.12.6 / 端口: 8520 / 进程: quark-dl.elf` —— 出现即服务已就绪；
3. **点图标**：退回主屏幕等 2~3 秒，出现「夸克网盘下载」；点它，系统自己打开控制页。
   图标没出来也不影响使用，浏览器手输 `http://<PS5-IP>:8520` 一样。

> 装过 0.11.0 的磁贴？先删掉 `/data/homebrew/QDLK85200/` 再重启，否则会有两个同名图标。

详细的实机验收步骤（含每条失败现象怎么读）在 [docs/verification.md](docs/verification.md)。

## 能做什么

**登录**：扫码（默认）或粘贴 Cookie（四种粘法都认：裸值 / `Cookie:` 整行 / 整块请求头 /
`Copy as cURL`）。扫码四个接口的实测记录见 [docs/quark-api.md](docs/quark-api.md)。

**浏览与下载**：

- 面包屑导航；勾选文件或整个文件夹；单次上限 2000 个文件、递归 32 层；
- 文件夹按网盘层级还原到本地目录树，不是平铺；
- 队列可暂停 / 继续 / 取消 / 重试，每个文件和每个文件夹都有实时速度，文件夹分组可折叠；
- **队列只有一份，页面是两个视图**：网盘页列从网盘加的，直链页列粘链接加的；
- **并发上限各页独立**（默认各 2 个）：网盘排 40 个文件时，直链那边照样能立刻开下自己的 2 个。

**下载引擎**：

- 先写 `<名字>.part`，`fsync` 后改名 —— 中断只会留下可续传的 `.part`；
- 大文件（≥64 MiB）切 32 MiB 段、8 连接并行，突破 CDN 单连接限速；
- 直链懒加载：队列只存 fid，下载开始才换链，排多后都不会 412；重试/重新下载都会换新链；
- 同名文件已存在则跳过；剩余空间 < 64 MB 在开始前就报错。

**设置**：默认保存目录（默认 `/data/homebrew/Quarkdownload`）、两页各自的并发数，都写进
`settings.txt`，重启保留。

## 页面与接口

端口 **8520**（被占则向后找，最多 32 个；主屏幕条目里的地址会自动跟着改）。

| 方法 | 路径 | 作用 |
|---|---|---|
| GET | `/` | 控制页 |
| GET | `/api/health` | 存活 |
| GET | `/api/diag` | 诊断；浏览器看网页版，脚本拿 JSON（`?format=json`）/ `.txt` |
| GET | `/api/log` | `boot.log` 原文（页面「日志」页签） |
| GET | `/api/net/selftest?url=` | 网络自检：DNS / TCP / TLS / 证书 / HTTP 每一步与耗时 |
| GET | `/api/dl/list` | 队列快照（`src` 标明 drive/link，`limit_*` 与 `running_*` 分来源） |
| POST | `/api/dl/add` | 表单 `url=`（可多行）`&dir=` `&name=` |
| POST | `/api/dl/cancel?id=` | 取消一个（`id=all` 全部） |
| POST | `/api/dl/pause?id=` | 暂停（保留 `.part`） |
| POST | `/api/dl/resume?id=` | 继续（从断点） |
| POST | `/api/dl/retry?id=` | 终态任务重新排队（会换新直链） |
| POST | `/api/dl/clear?src=` | 清掉已结束任务；`drive` / `link` 只清一半 |
| GET/POST | `/api/settings` | 目录与并发。POST 的 `dir` / `limit` 都可选，`limit` 用 `src=drive\|link` 指明哪一份 |
| GET | `/api/fs/list?path=` | 列目录（最多 200 条，`truncated` 标记截断） |
| GET | `/api/quark/qr` `status` `list` `probe` | 扫码取图、会话状态、目录列表、原始 JSON |
| POST | `/api/quark/login` `logout` `add` | 粘贴 Cookie 登录、退出、把勾选项入队 |
| POST | `/api/open-ui` | 让主机浏览器打开控制页（**只认 127.0.0.1**） |
| POST | `/api/exit` | 进程自己退出，重新加载 ELF 即可再起 |

## 项目结构

```
quark-dl-native/
├── src/                    纯 C，约 9900 行；除 MHD 与 mbedTLS 外无第三方依赖
│   ├── main.c              启动顺序、选端口、回收旧实例
│   ├── websrv.c            MHD 封装、accept 循环、请求体收集（4 MB 上限）
│   ├── api.c               全部 /api/*、表单解析、诊断页
│   ├── dl.c                队列、并发槽、暂停/取消、分段下载、落盘
│   ├── quark.c             夸克接口（扫码、目录、换直链）、会话持久化
│   ├── net.c               socket、自解析 DNS、TLS 会话与证书信息
│   ├── httpc.c             HTTP 客户端（Range / 重定向 / chunked）
│   ├── json.c  cookie.c    极简 JSON 与 cookie jar
│   ├── asset.c             内存资源表（页面从内存出，不碰文件系统）
│   ├── launcher.c          主屏幕条目注册（后台线程）
│   └── boot.c  notify.c  kern.c  shell.c   boot.log 与平台层
├── assets/                 index.html、qrcode.js、icon0.png、param.json、ca.pem
├── tools/                  test-stage2.sh（引擎）、test-page.mjs（页面）、mockdl.py、C 单测
├── ps5/                    Dockerfile + build.sh（唯一的正式构建入口）
├── docs/                   设计说明、接口事实、验证手册、归档
├── .github/workflows/      build-elf.yml —— 构建与全部测试都在这里
├── Makefile                版本号（APP_VERSION）与两套目标（PS5 / Linux host）
├── dist/                   最近一次构建的产物（不进 git）
└── CHANGELOG.md  README.md  LICENSE  NOTICE.md
```

## 构建

正式构建只能走 GitHub Actions —— PS5 SDK 只在 Linux 上，且需要 Docker：

```bash
bash ps5/build.sh          # 产物 quark-dl_v<版本>.elf，无其他副产品
```

镜像里做三件事：装 ps5-payload-sdk v0.43、交叉编译 libmicrohttpd、交叉编译 mbedTLS 2.28.8。
`push` 会自动触发 CI，产物在对应 run 的 artifacts 里。

在 Linux 上跑同一份代码（CI 就是这么测的）：

```bash
MBEDTLS_PREFIX=$HOME/mbedtls MBEDTLS_CC=cc MBEDTLS_AR=ar MBEDTLS_RANLIB=ranlib \
  bash install-mbedtls.sh
make linux BUILD_ID=smoke TLS_PREFIX=$HOME/mbedtls
bash tools/test-stage2.sh
```

只改页面时，本地就能跑一部分测试，不必等构建：

```bash
node tools/test-page.mjs   # 用 stub DOM 跑 assets/index.html 的交互
```

**版本号只有一处**：`Makefile` 的 `APP_VERSION`。页面里的 `PAGE_V` 必须与它一致，
CI 会比对，不一致直接失败。

## 测试与 CI

顺序是「编译 → 静态断言 → 起服务器跑真实请求 → 引擎测试 → 单测」，任一失败即阻断：

| 步骤 | 覆盖 |
|---|---|
| Verify ELF shape | 体积、`.incbin` 资源、无遗留磁贴代码 |
| Home-screen entry payload | `param.json` 的 titleId / deeplinkUri / 分类 |
| Home-screen entry is registered at runtime | 启动顺序（通知必须在注册之前），禁止同步注册复活 |
| Default download directory | 默认目录、旧值迁移、页面 placeholder 一致 |
| Download directory is checked by writing into it | 探针文件判断，不用 `access(W_OK)` |
| Pause and cancel are distinct | 暂停保留 `.part`、取消删除 |
| Big files download over parallel ranges | 分段与并发连接 |
| Links are minted lazily | 队列里没有预签直链 |
| Page behaviour / Direct-link pane layout | 页面交互与按钮位置（stub DOM） |
| Queue is one list, two views | 两页分视图、并发按来源分预算、`?src=` 清一半 |
| Endpoints and clean exit | 真实 HTTP 全链路：诊断、页面、超长路径 404、无 CORS、干净退出 |
| Download engine and TLS | 明文 / TLS / 坏证书三个本地服务器：续传、chunked、逐字节一致 |
| JSON parser / Cookie paste / Log on failure | 单测 |

## 安全边界（务必读）

服务监听 `0.0.0.0`，**没有登录校验**：任何能访问 8520 的设备都能加下载任务（可指定任意目录）、
改默认目录、看队列和日志、关掉服务。设计前提是「家用局域网里的可信设备」。
**不要把这个端口映射到公网。** 细节与其它约束见 [docs/design-notes.md](docs/design-notes.md)。

## 文档索引

| 文档 | 内容 |
|---|---|
| [CHANGELOG.md](CHANGELOG.md) | 从 0.1.x 到 0.12.6 的演进，以及升级注意 |
| [docs/verification.md](docs/verification.md) | 实机验收手册：每一步该看到什么、失败怎么读 |
| [docs/design-notes.md](docs/design-notes.md) | 接手必读：线程模型、锁、落盘、为什么这么写 |
| [docs/quark-api.md](docs/quark-api.md) | 夸克接口哪些是实测的、哪些是推断的 |
| [docs/archive/plan-stage3.md](docs/archive/plan-stage3.md) | 历史归档：0.3.0 之前的方案稿，已过时 |

## 许可

GPL-3.0-or-later。`src/main.c`、`src/websrv.c`、`src/asset.c` 的骨架来自
[owendswang/ps5-web-file-manager](https://github.com/owendswang/ps5-web-file-manager)，
第三方组件与出处见 [NOTICE.md](NOTICE.md)。
