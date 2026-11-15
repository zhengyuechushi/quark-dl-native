#pragma once

/* 把本 payload 注册成 PS5 主屏幕上的一个网页应用条目。
 *
 * 只在 PS5 构建里做实际工作；host 构建恒返回 0，方便 CI 的 linux 冒烟测试
 * 照常跑完整条启动链。
 *
 * port 是本次实际监听的端口，会写进条目的 deeplinkUri，所以端口被别的
 * payload 挤到 8521 时，点图标照样落到正确地址。 */

int launcher_install_if_needed(unsigned short port);

/* 最近一次注册的结果，中文，可直接写 boot.log。 */
const char *launcher_status(void);

/* 主屏幕条目使用的 titleId，必须与 assets/param.json 里的 titleId 一致。 */
const char *launcher_title_id(void);
