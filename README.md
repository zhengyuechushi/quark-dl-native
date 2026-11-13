# Quark DL — 骨架版（阶段一）

这一版**不含登录、不含下载**。它只做三件事，并必须在实机上先确认：

1. ELF 能被加载并跑起来；
2. 进程列表里的名字是 `quark-dl.elf`；
3. 能主动退出，且退出后不再自己冒出来。

前七轮在 Rust 工程上反复失败，根因是**启动路径太复杂**：写日志、查禁用标记、读 1.6 MB
文件、可能 `execve`，任何一步提前退出，改名就不执行，而设备端什么都看不到。
所以这次换做法：**先把一个已知能跑的最小骨架搬过来，确认它在这个环境里成立，再往上加功能。**

## 骨架从哪来

`owendswang/ps5-web-file-manager`（GPL-3.0-or-later，实机确认可用、可关闭）。它的 `main()`
第一句就是：

```c
syscall(SYS_thr_set_name, -1, "web-file-mgr.elf");
```

接着用 `sysctl({1,14,8,0})` 遍历进程表，按 `ki_tdname`（`kinfo_proc` 偏移 447）找同名进程并
`SIGKILL`，然后 `find_available_port(8765)` + `websrv_listen()`。关闭靠 `/api/exit` →
`websrv_stop()` → 主循环 `break` → `main` 返回。

本工程把这段**原样保留**，只替换：进程名 → `quark-dl.elf`，业务路由 → `api.c`，
页面 → `assets/index.html`。HTTP 层仍是它的 `websrv.c` + 交叉编译的 libmicrohttpd，
构建脚本仍是它的 `install-libmicrohttpd.sh` + `prospero.mk`。

## 构建

需要 Docker（或让 GitHub Actions 跑）：

```sh
BUILD_ID=$(git rev-parse HEAD) bash ps5/build.sh
```

产物：`quark-dl.elf`。镜像内已按 SDK v0.43 装好 `prospero-clang` 与 libmicrohttpd，
构建过程不需要联网下载依赖。

## 在 PS5 上验证

加载 ELF（Toolbox 菜单 / `nc` 发 `file:...` / 网页 sender 都行），然后看三处：

**A. 主机通知**：`Quark DL <构建号> / 端口: 8765 / 进程: quark-dl.elf`

**B. 进程列表**：应显示 `quark-dl.elf`。名字由 `SYS_thr_set_name` 写入，是启动第一句。

**C. 浏览器打开 `http://<PS5-IP>:8765`**：

页面会显示「进程名（进程列表显示）」一行，值取自内核 `ki_tdname` —— 与进程列表同一字段，
所以这一行就是答案，不用靠截图比对。

命令行等价：

```sh
curl http://<PS5-IP>:8765/api/diag
```

```json
{
  "app": "quark-dl",
  "build": "…",
  "pid": 123, "ppid": 1, "port": 8765,
  "name": { "expected": "quark-dl.elf", "kernel": "quark-dl.elf",
            "readable": true, "ok": true, "instances": 1 },
  "exiting": false
}
```

`name.ok` 为 `true` ⇒ 改名成立且已自证。

**关不掉的问题**：点页面上的「结束运行」。它会 `POST /api/exit`，250 ms 后进程自己
`exit`，不写任何标记，重新加载即可再起。判断是否真的退出，只看 **PID 变不变**——
主机界面那个「终止」对 payload 会弹成功提示但信号没送到，所以之前一直"关不掉"。

## 接口

| 方法 | 路径 | 作用 |
|---|---|---|
| GET | `/api/health` | `ok/build/pid/port/uptime_ms/name` |
| GET | `/api/diag` | 上面的全部 + `ppid`、`name.readable/ok/instances`、`exiting` |
| POST | `/api/exit` | 250 ms 后退出进程（无标记文件） |

## 阶段二（骨架确认后接）

- 扫码登录与 Cookie 持久化（`/data/quark-dl/`）
- 网盘目录浏览、多选、文件夹递归
- 下载队列：Range 续传、`.part` 原子改名、并发 1–6、失败重试
- 默认下载目录与 ProsperoStore 一致
- 设置页：目录选择、并发数、空间预检

## 已经验证到哪一步

`linux-smoke` 作业用同一份源码（不定义 `__SCE__`）在 CI 里真的启动了服务，并断言：

- `GET /api/health` → `ok:true`、`build` 等于编译时的提交号、`port:8765`
- `GET /api/diag` → `name.ok:true`、`instances:1`
- `GET /` → 返回内嵌页面
- `POST /api/exit` → 进程在 250 ms 后消失（`kill -0` 失败）

也就是说**路由、JSON、静态资源、退出主循环**这几条在真实进程里跑通过。
只有 `SYS_thr_set_name`、`sysctl` 内核进程表读取、通知弹窗这三处属于主机专有，
无法离线验证——它们正是从参考项目逐行搬过来的部分。

## 许可

GPL-3.0-or-later。来源与逐文件对应关系见 `NOTICE.md`。
