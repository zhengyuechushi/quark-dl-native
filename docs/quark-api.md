# 夸克网盘接口：哪些是实测的，哪些是推断的

> 这一节原本在 README 里。它不是背景知识，而是代码里每个字段映射的依据：
> 改字段名、加接口、换域名之前，先看这里为什么这么写。
>
> `pan.quark.cn` 没有公开文档。下面的结论分成「实测确认」和「推断」两类——
> 因为两者的故障表现完全不同：路径错会 404/405，字段名错则是「目录是空的」。

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
