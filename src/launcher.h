#pragma once

/* 主屏幕条目的 titleId，必须与 assets/param.json 里的 titleId 一致。 */
const char *launcher_title_id(void);

/* 最近一次注册的结果，中文，可直接写 boot.log。 */
const char *launcher_status(void);

/* 把内嵌的 icon0.png 挂到 /icon0.png（页面 favicon）。
 *
 * 必须**在 websrv 起来之前**调一次：asset 表是一个没有锁的链表，服务开始
 * 接请求之后再往里插节点，就是在和 MHD 的请求线程抢指针。这一步只往链表里
 * 插一个节点，没有系统调用，放前面不会拖慢启动。 */
void launcher_serve_icon(void);

/* 在后台线程里注册主屏幕条目，**立刻返回**。
 *
 * 注册要走系统的安装器（libSceAppInstUtil + 内核符号解析），那是几十毫秒
 * 到几十秒不定的系统调用，也可能根本不返回。0.12.0 把它同步放在了
 * websrv_listen() 之前，真机上的结果就是：屏幕没有任何通知、8520 也不通 —
 * 服务从来没起来过。图标是加分项，服务是命，两者不能同归于尽。
 *
 * host 构建下什么都不做。返回值只表示线程有没有起来。 */
int launcher_start(unsigned short port);
