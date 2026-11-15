# Quark DL — 登录、浏览、下载网盘文件

阶段一（能加载、进程名 `quark-dl.elf`、能主动退出）和阶段二（TLS、下载引擎、断点续传、
队列持久化、网络自检）都已在实机通过。业务层这一版有两种登录方式：

1. **扫码登录（默认）**：页面上出一张二维码，手机上的夸克 App 扫一下、点确认，进程自己去换 Cookie；
2. **粘贴 Cookie（兜底）**：扫不了码时，从电脑浏览器取一次 Cookie 粘进来——四种粘法都认
   （裸值 / `Cookie:` 整行 / 整块请求头 / `Copy as cURL` 的整段命令）；
3. **浏览**：面包屑 + 目录列表，字段名与路径来自实测；
4. **下载**：勾选文件或整个文件夹，批量换直链后交给阶段二的引擎——引擎一行没改。

两种登录最后都落到同一份 `cookie.txt`，所以浏览、下载、续传的代码完全不区分来源。

## 扫码是怎么实现的

四个接口全部从 `pan.quark.cn` 自己的前端 bundle 里读出来，不是猜的，并且逐条对本机实测过：

| 步骤 | 请求 | 实测 |
|---|---|---|
| 取一次性 token | `GET uop.quark.cn/cas/ajax/getTokenForQrcodeLogin` | `{"status":2000000,"data":{"members":{"token":"sta…"}}}` |
| 二维码内容 | `https://su.quark.cn/4_eMHBJ?token=<token>&client_id=532&ssb=weblogin&uc_biz_str=…` | 302（正常跳到落地页） |
| 轮询（2 秒一次） | `GET uop.quark.cn/cas/ajax/getServiceTicketByQrcodeToken?token=<token>` | 没人扫时 `{"status":50004001,"message":"Query result is empty"}` |
| 票据换会话 | `GET pan.quark.cn/account/info?st=<ticket>&lw=scan` | 会话在 `Set-Cookie` 里，且必须含 `__pus` / `__puus` |

等待码 `50004001 80005000 80005001 80005002`、过期码 `50004002 80005003 80005004`、
成功码 `2000000`，都取自同一份 bundle。**没见过的状态码当失败处理**，不留在「等待扫码」里干等。

二维码本身由 `assets/qrcode.js`（byte mode / 纠错 M，零依赖）在页面里画到 canvas 上，
不是去求第三方接口生成图片。这份编码器用 OpenCV 解码反验过：187 字节的真实 URL 能原样解出。

## 当前构建

| 项 | 值 |
|---|---|
| 版本 | `Makefile` 的 `APP_VERSION`（0.6.0）；页面里的 `PAGE_V` 必须与它一致，CI 校验。产物文件名带版本号 `quark-dl_v0.6.0.elf`，Payload Manager 会把它显示成标题 `quark dl` + 版本 `v0.6.0` |
| build id | 见 CI 产物（ELF 里可 `grep` 到完整 sha，用来确认刷进去的是哪一版） |
| 大小 | 约 850 KB（mbedTLS + 200 KB 根证书库 + 业务层 + 二维码编码器） |
| CI | `elf` 与 `linux-smoke` 全绿，含 JSON 解析器、Cookie 解析与 Set-Cookie 归并单测、页面行为测试 |

阶段一、二在 CI 里长期成立的那几项：https 用自签 CA 通过、换一个未知 CA 被拒**并报出被拒的那张证书**、
300 KB 逐字节一致、预置 `.part` 产生 `Range: bytes=120000-` 并续传成功、chunked 无 `Content-Length`、
404 直接判失败不重试、TLS 下载、重复链接复用同一任务、**删掉文件后重新加同一链接会真的重下**、
重试接口把终态任务重新排队、队列落盘、**重启后被删文件的任务读作 `missing` 而不是 `failed`**。

## 夸克接口：哪些是实测的，哪些是推断的

夸克网盘没有公开文档。这一节把「已经确认」和「猜的」分开写清楚，因为两者的故障表现完全不同。

**实测确认**（不带 cookie 直接打）：

| 请求 | 返回 | 结论 |
|---|---|---|
| `GET drive-pc.quark.cn/1/clouddrive/file/sort?...` | `401 {"code":31001,"require login [guest]"}` | 路径对，只差登录；`31001` 即会话失效 |
| `GET /1/clouddrive/file/download?...` | `405 Method Not Allowed` | 必须 POST |
| `GET /1/clouddrive/member?...` | `401 {"code":31001}` | 路径对 |
| `GET /1/clouddrive/config?...` | `401 {"code":31001}` | 路径对 |
| `GET pan.quark.cn/account/usercode?st=x` | `{"code":"GET_SERVICE_CODE_ERROR"}` | 扫码链路的起点，缺 `st` 票据 |

配置来源是 `pan.quark.cn` 首页引用前端 bundle 里的运行时常量：
`bizHost = https://pan.quark.cn`、API 域 `https://drive-pc.quark.cn`、固定参数 `pr=ucpro&fr=pc`。

**推断的**（只在有 cookie 时才能验证）：返回体里的字段名——`data.list[].file_name`、`dir`、
`size`、`updated_at`、`metadata._total`、`data[].download_url`。代码对每个字段都试了历史上用过的几种
写法，全部取不到时会**明确报错并附上原始响应**，而不是显示一个空目录。

`/api/quark/probe?path=...` 就是为此存在的：任意路径，原样返回。
猜错的时候，看到的是真实字节，而不是「你的网盘是空的」。

## 端口与接口

端口 **8520**（被占则向后找，最多 32 个）。`8888` 是参考 payload 的默认值，实机上已被别的插件占用。

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
| POST | `/api/open-ui` | 让主机浏览器打开控制页（只允许 127.0.0.1，桌面磁贴用） |
| POST | `/api/exit` | 进程自己退出（可重新加载） |

## 主屏幕图标（桌面磁贴）

只发 payload ELF 时，主机上没有任何图标：你只能从 Payload Manager 加载它，再自己把地址
敲进浏览器。`starter/` 把这件事补上——它编译成一个**原生 PS5 标题**，装到
`/data/homebrew/QDLK85200/`，主屏幕上就有一块「夸克下载」的磁贴。

点磁贴发生三件事：

1. 后台的 quark-dl 已经在跑（8520 起的一段端口）→ 直接让它打开控制页；
2. 没在跑 → 把**内嵌在 eboot.bin 里**的 payload ELF 发给越狱的 ELF 加载器
   （`127.0.0.1:9021`），等它起来；
3. 再让它打开控制页，然后磁贴自己退出。

所以点图标 = 启动服务 + 打开界面，不用记地址、不用敲任何东西。页面还是同一份
`assets/index.html`，功能一点没变。

构建（CI 自动做，本机需要 Docker）：

```bash
bash ps5/build.sh                       # payload ELF
QDL_BUILD_TILE=1 bash ps5/build.sh      # 顺带出磁贴
```

产物 `quark-dl-tile_v<版本>.zip`，解压得到 `QDLK85200/`，整个目录放到 `/data/homebrew/`。

> 磁贴内嵌的 payload 和 zip 里那份 ELF 来自**同一次构建**，版本永远一致；
> 换新版就重新装一次磁贴，别只换 ELF。


## 先做这一步：网络自检

加载 ELF，浏览器打开 `http://<PS5-IP>:8520`，在「网络自检」里点**开始自检**。
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
产物 `quark-dl_v0.6.0.elf` 约 1 MB（根证书 200 KB + mbedTLS）。

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
