# Quark DL — 阶段二：传输层与下载引擎

阶段一（能加载、进程名 `quark-dl.elf`、能主动退出）已在实机通过。
这一版往上加**真正下载所需的地基**，并且每一项都在 CI 里用真实进程验证过：

1. **TLS**：自带 mbedTLS（静态编进 ELF）+ 118 个根证书的信任库，能做 HTTPS 并校验证书；
2. **下载引擎**：多任务队列、`.part` 断点续传、原子改名、失败分类与重试；
3. **网络自检**：一个按钮，把「域名解析 / TCP / TLS / 证书 / HTTP」分开报出来；
4. **下载界面**：粘链接（可多行）、选目录、看进度、取消、清空。

**还没有**夸克登录与网盘浏览。它们只是这一层之上的调用方，等传输层在实机确认后再接——
否则登录失败时无法区分「协议写错了」和「这台主机根本连不上网」。

## 当前构建

| 项 | 值 |
|---|---|
| 版本 | 0.2.2 |
| build id | `74ad6f0`（ELF 里可 `grep` 到完整 sha，用来确认刷进去的是哪一版） |
| 大小 | 818 KB（mbedTLS 静态链接 + 200 KB 根证书库） |
| CI | run `43447095666`，`elf` 与 `linux-smoke` 两个作业全绿 |

`linux-smoke` 在主机上跑的是同一份 `src/`：https 用自签 CA 通过、换一个未知 CA 被拒**并报出被拒的那张证书**、
300 KB 逐字节一致、预置 `.part` 产生 `Range: bytes=120000-` 并续传成功、chunked 无 `Content-Length`、
404 直接判失败不重试、TLS 下载、重复链接复用同一任务、**删掉文件后重新加同一链接会真的重下**、
重试接口把终态任务重新排队、队列落盘、**重启后被删文件的任务读作 `missing` 而不是 `failed`**。

## 端口与接口

端口 **8765**（被占则向后找，最多 32 个）。`8888` 是参考 payload 的默认值，实机上已被别的插件占用。

| 方法 | 路径 | 作用 |
|---|---|---|
| GET | `/` | 控制页 |
| GET | `/api/health` | 存活 |
| GET | `/api/diag` | 诊断；浏览器看网页版，脚本拿 JSON（`?format=json`、`.txt`） |
| GET | `/api/net/selftest?url=` | 网络自检，返回每一步的结果与耗时 |
| POST | `/api/dl/add` | 表单 `url=`（可多行）`&dir=` `&name=` |
| GET | `/api/dl/list` | 队列快照（含进度、错误） |
| POST | `/api/dl/cancel?id=` | 取消一个（`id=all` 全部） |
| POST | `/api/dl/clear` | 清掉已结束的任务 |
| GET/POST | `/api/settings` | 读取/设置默认保存目录 |
| GET | `/api/fs/list?path=` | 列目录 |
| POST | `/api/exit` | 进程自己退出（可重新加载） |

## 先做这一步：网络自检

加载 ELF，浏览器打开 `http://<PS5-IP>:8765`，在「网络自检」里点**开始自检**。
默认目标是 `https://pan.quark.cn/`，结果逐行给出：

```
✓ pan.quark.cn → 203.119.174.54（getaddrinfo，12 ms）
  TLS TLSv1.2 / TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256
  服务器证书 C=CN, ST=Zhejiang, L=Hangzhou, O=Alibaba (China) Technology Co., Ltd., CN=*.alibaba.com · 到期 2027-03-26
  签发者 C=BE, O=GlobalSign nv-sa, CN=GlobalSign GCC R46 OV TLS CA 2025
  HTTP 200 · 42461 字节 · 341 ms
```

这四行是唯一能同时说清四件事的地方：

- **解析失败** → 主机 DNS 不通（`net.c` 会退回 `/etc/resolv.conf` 与 223.5.5.5 自行查询）；
- **TLS 握手失败** → 网络被劫持/需要代理，或主机时间不对；
- **证书校验失败** → 同上，这也是它必须单独报错的原因；
- **有 HTTP 状态码** → 传输层通了，之后都是应用层的事。

> **`api.quark.cn` 不要拿来当探针**（0.2.1 的默认值是它，选错了）。该域名解析得到
> `203.119.174.x`，但那边 ingress 只吐 `Kubernetes Ingress Controller Fake
> Certificate` 自签证书、HTTP 层 404，是台废弃入口。拿它自检等于每次开机先报一个红叉，
> 而主机其实完全正常。真正可用的域名 `pan.quark.cn`、`drive-pc.quark.cn` 都挂在
> `*.quark.cn` 这张 GlobalSign 泛域名证书下。

## 下载行为

- 先写 `<名字>.part`，`fsync` 成功后才改名成正式文件——中断只留下可续传的 `.part`，
  不会留下一个看起来完整、其实截断的文件；
- 重新加同一链接时，若 `.part` 已有一半，就带 `Range: bytes=N-` 接着下；服务器若忽略 Range
  返回 200，则清空重下（而不是把第二份追加在第一份后面）；
- 链接过期（400/401/403/404/410）**不重试**，直接失败并写明 HTTP 状态；网络中断才重试，
  最多 4 次，间隔 1/2/4 秒；
- 同名文件已存在则跳过；重复添加同一 URL 返回同一个任务，不会下两遍；
- 同时最多 3 个（`DL_LIMIT`），队列落盘到 `/data/quark-dl/downloads.txt`，重启后继续；
- 剩余空间不足 64 MB 时在开始前就报错。

默认目录 `/data/homebrew`，与 ProsperoStore 的安装位置一致（`src/app/store.hpp:86`）。

## 构建

```sh
BUILD_ID=$(git rev-parse HEAD) bash ps5/build.sh     # 需要 Docker
```

镜像内做三件事：装 SDK v0.43、交叉编译 libmicrohttpd、交叉编译 mbedTLS 2.28.8。
产物 `quark-dl.elf` 约 1 MB（根证书 200 KB + mbedTLS）。

主机上跑同一份代码（CI 就是这么测的）：

```sh
MBEDTLS_PREFIX=$HOME/mbedtls MBEDTLS_CC=cc MBEDTLS_AR=ar MBEDTLS_RANLIB=ranlib \
  bash install-mbedtls.sh
make linux BUILD_ID=smoke TLS_PREFIX=$HOME/mbedtls
bash tools/test-stage2.sh
```

`tools/test-stage2.sh` 起三个本地服务器（明文、受信 CA 的 TLS、不受信 CA 的 TLS），逐条断言：
HTTPS 自检成功、未知 CA 被拒、300 KB 下载逐字节一致、预置 `.part` 触发 `Range: bytes=120000-`、
chunked 解码、404 报告、TLS 下载、重复链接不重复下载、队列落盘。

## 两个构建/设计决定

- **TLS 为什么不用系统的 `libSceSsl`**：SDK 里确实有它的存根，但存根只对「该模块已加载进本进程」
  有效；payload 无法预先知道这一点，失败时只会得到一句没有信息量的错误。自带 mbedTLS 是唯一
  可离线验证的选择，代价是体积。
- **HTTP 客户端为什么自己写**：只需要 GET/POST、Range、重定向、chunked 四件事；再引入一个
  autoconf 的 libcurl，就是给交叉编译多开一条会失败的路。`Connection: close` 每请求一条连接，
  没有连接池要维护，卡住的下载也不会留下半复用的 socket。

## 下一步

夸克登录（扫码 / Cookie）、网盘目录浏览、多选与文件夹递归。它们只调用这里的
`http_perform()` 与 `dl_add()`，不再碰已经验证过的传输层。

## 许可

GPL-3.0-or-later。来源与逐文件对应关系见 `NOTICE.md`；第三方组件为 mbedTLS（Apache-2.0）
与 Mozilla CA 根证书包（MPL-2.0），均未修改。
