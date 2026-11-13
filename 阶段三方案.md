# 阶段三方案：登录、浏览、多选下载

状态：**待确认**。确认后我才动手写代码。

阶段二的传输层已经过实机验证（下载、续传、取消后重加、重启后队列、deepseek 全链路 https 全绿），
所以这一层之上可以直接接业务。下面是已实测的事实、要做的东西、以及我打算怎么兜底。

---

## 一、已经实测确认的事实（不是猜的）

从 `pan.quark.cn` 首页的前端 bundle（`g.alicdn.com/quark-cloud-drive/.../index.js`，1.6 MB）
里读出来的运行时配置：

```
t: "https://drive-pc.quark.cn"      ← 业务 API 域
paramInfo: { pr: "ucpro", fr: "pc" } ← 每个业务请求要带的固定参数
bizHost: "https://pan.quark.cn"     ← 登录/账号域的宿主
loginTypes: [ {key:"scan", text:"扫码登录"}, {key:"mobile"} ]
```

接口族（全部在 `bizHost` 下）：

| 路径 | 作用 | 实测结果 |
|---|---|---|
| `GET /account/usercode?st=` | 取二维码里的那段 code | 路径存在，缺 `st` 时返回 `{"success":false,"code":"GET_SERVICE_CODE_ERROR"}` |
| `GET /account/info?st=` | 轮询扫码结果 / 登录取 cookie | 同上族，未登录测不了 |
| `GET /account/mobileinfo?st=` | 手机号登录态查询 | — |
| `GET /api/config` | 前端配置 | — |
| `GET /api/client_version` | 客户端版本 | — |

业务接口前缀确认为 **`/1/clouddrive/`**（bundle 里出现 `/1/clouddrive/config`、`/1/clouddrive/member`）。

**为什么 `api.quark.cn` 是错的**：它和 `pan.quark.cn` 不是一回事。前者那台机器只回 K8s 占位证书，
后者才是 `bizHost`。这也解释了 0.2.1 自检为什么必然红。

---

## 二、登录流程（要实现的）

```
1. GET {bizHost}/account/usercode?st=<ticket>
     → 拿到 code 字符串
2. 本地把 code 编码成二维码，画在网页上
3. 手机夸克 App 扫码并确认
4. 轮询 {bizHost}/account/info?st=<ticket>&lw=<...>
     → success=true 时，Set-Cookie 里带登录态
5. cookie 落盘 /data/quark-dl/cookie.txt（权限 600），之后每个业务请求带上
```

**第 1 步之前的那个 `st` 从哪来**，是这条链路上唯一剩下的未知。前端是从
`uop.quark.cn/cas/...` 拿的（bundle 里有 `https://uop.quark.cn/cas/custom/login?custom_login_type=...`）。
这一步我要先用真实请求跑通——**跑不通就不写界面**。

### 二维码怎么画

需要一个 QR 编码器。选择：**nayuki `qrcodegen.c`**（单文件、无依赖、MIT、约 1200 行、久经检验）。

理由：自己写 QR 编码是纯算法体力活，容易在对齐图案（alignment pattern）和掩码评分上出错，
而且错了表现为"手机扫不出来"这种极难定位的故障。这个库我读过，质量比我能现写的高。
和 mbedTLS 的取舍逻辑一致：依赖一个**单文件、可静态链接、可审计**的东西，而不是引入整个框架。

---

## 三、界面（在现有页面加两张卡）

**卡一：登录**

- 未登录：一个二维码 + 「刷新」「改用手机号」两个按钮 + 一段倒计时（二维码通常 120 秒过期）
- 已登录：显示昵称、容量（`/1/clouddrive/member`）、「退出登录」
- 登录态失效时（业务接口回鉴权失败码）：显示「登录已过期，重新扫码」，不清空下载队列

**卡二：网盘浏览**

- 面包屑路径 + 目录列表（名称 / 大小 / 类型 / 修改时间）
- 每行一个复选框；目录行的复选框表示"递归整棵树"
- 底部：「加入下载队列」按钮，显示已选 N 个文件、合计 M GB
- 只做**下载**，不做移动/删除/重命名/上传——这些和"下载器"无关，而且误操作代价高

点「加入下载队列」之后：对每个文件调 `/1/clouddrive/file/download` 换直链，
把直链塞进现有的 `dl_add`。**下载引擎一行不改**，这是阶段二把它做得那么厚的原因。

---

## 四、数据模型（定稿后不改）

```c
/* src/quark.h */
typedef struct {
  char cookie[4096];      /* 原样保存，不解析 —— 夸克的 cookie 字段名不稳定 */
  char nickname[64];
  long long total_bytes;  /* 0 表示未知 */
  long long used_bytes;
  time_t saved_at;
} quark_session;

/* 目录项。字段只留下载需要的，其余丢弃。 */
typedef struct {
  char fid[40];          /* 文件 id，换直链要用 */
  char name[256];
  char dir;              /* 'f' 文件 / 'd' 目录 */
  long long size;
  long long updated_at;
} quark_entry;

int  quark_load_session(quark_session *s, const char *data_dir);
int  quark_save_session(const quark_session *s, const char *data_dir);
int  quark_qr_start(char *code, size_t cap, char *st, size_t stcap);   /* 第 1 步 */
int  quark_qr_poll(const char *st, quark_session *out, int *pending);   /* 第 4 步 */
int  quark_list(const quark_session *s, const char *pdir, int page,
                quark_entry *out, int cap, int *total);
int  quark_direct_url(const quark_session *s, const char *fid,
                      char *url, size_t cap);                          /* 换直链 */
```

新增文件：`src/quark.c`（业务）、`src/qr.c`（qrcodegen 封装）、`src/json.c`（最小 JSON 解析）。

**为什么要自己写 JSON 解析**：夸克所有接口都回 JSON，但工程里目前没有解析器。
不引 cJSON（多一层依赖），写一个只支持"对象/数组/字符串/数字/布尔"的递归下降解析器，
约 300 行，够用且能读懂。这条要写清楚，因为它是这个方案里最容易被低估的工作量。

---

## 五、风险与退出条件

| 风险 | 概率 | 兜底 |
|---|---|---|
| `st` 获取流程逆不出来（有签名/风控） | 中 | **退到「手动粘 cookie」**：用户在电脑浏览器登录 `pan.quark.cn`，F12 复制 cookie，粘进页面。功能照样完整，只是首次麻烦一次 |
| `/1/clouddrive/file/download` 有防盗链/签名时效 | 中 | 直链拿到后立刻入队；失效就重新换一次（重试逻辑已有） |
| 接口字段和 bundle 推断的不一致 | 高 | 阶段三第一步就是**探针页**：不写界面，先用真 cookie 把 4 个接口各打一次，把原始 JSON 显示出来。字段对不上就在这一步发现，不是在写完全部界面之后 |
| 夸克改了接口 | 长期必然 | 把接口路径集中在一个头文件里，改一处即可 |

**退出条件写死**：如果探针页在 3 轮内调不通 `usercode` + `info`，我直接切「手动粘 cookie」方案，
不再纠缠扫码。理由：扫码登录是体验优化，**能下载才是功能**。为了一个更漂亮的登录方式卡住整个项目，
是典型的把顺序做反。

---

## 六、验收标准（每条都可判定）

1. 打开页面点「登录」，出现二维码；用夸克 App 扫，页面在 120 秒内变成"已登录"并显示昵称；
2. 重启 ELF，页面仍是已登录状态（cookie 持久化）；
3. 浏览到任意目录，列表条目数与手机 App 里同目录一致（抽查一层即可）；
4. 勾选一个 100 MB 以上的文件加入队列，下载完成后 `md5sum` 与手机端下载的一致；
5. 勾选一个含 3 个子目录的文件夹，队列里出现全部叶子文件，逐个下完；
6. 断电重启，未下完的任务自动继续（阶段二已有，回归验证）；
7. 登录过期后，页面提示重新扫码，**下载队列不丢**。

---

## 七、工作量（单人，含联调）

| 部分 | 估时 | 说明 |
|---|---|---|
| 探针：跑通 `st` + `usercode` + `info` | 1–2 天 | 不确定性最大，可能反复 |
| `json.c` + 单测 | 1 天 | 纯本地，可控 |
| `qr.c`（接 qrcodegen）+ 页面渲染 | 1 天 | |
| `quark.c`：登录态 + 列表 + 直链 | 2–3 天 | 依赖探针结论 |
| 界面两张卡 + 多选交互 | 1–2 天 | |
| 文件夹递归 + 队列批量加入 | 1 天 | |
| 实机联调 | 2 天 | 至少两轮 |

**合计 9–12 天**（单人，包含联调；不含夸克改接口导致的返工）。
区间下沿是"探针一次就通"，上沿是"退到手动 cookie"。我不给单点数字，因为 `st` 那一步我现在真的不知道。

---

## 八、我要你现在决定的两件事

1. **扫码登录跑不通时，是否接受「手动粘 cookie」作为交付标准？** 我建议接受——它 100% 能用，
   而且不依赖任何逆向成果。不接受的话，需要你提供一台能正常登录 `pan.quark.cn` 的机器的抓包。
2. **要不要一起做「分享链接解析」**（把别人分享的夸克链接里的文件转存/直下）？
   它不在你最初的需求里，但它和下载是同一套东西，边际成本低，很多人真正想要的是这个。
   默认**不做**，除非你说要。
