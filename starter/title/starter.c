/*
 * Quark DL 的桌面磁贴（starter）—— 编译成原生 PS5 标题 eboot.bin。
 *
 * 主屏幕上那块图标点下去跑的就是这个程序。PS5 没有"给任意 payload 一个图标"
 * 的办法：磁贴必须是一个真正的原生标题（process-parameter 块 + 签名），所以
 * 这里是一个独立的小程序，它只做三件事：
 *
 *   1. 后台的 quark-dl 已经在跑（8520 起的一段端口）→ 让它打开页面；
 *   2. 没在跑 → 把内嵌在本 eboot.bin 里的 payload.elf 发给越狱的 ELF 加载器
 *      （127.0.0.1:9021），等它起来；
 *   3. 再让它打开页面，然后本进程退出。
 *
 * 页面由谁打开：**payload 自己**。磁贴不知道 payload 最终绑到哪个端口
 * （8520 被占会往后挪），也不该知道——它只发 POST /api/open-ui，payload 用
 * 自己实际监听的端口拼出 URL 去调系统浏览器。这样两边只有一条契约。
 *
 * 为什么是 freestanding：PS5 的应用加载器要一个真正的 process-parameter 块和
 * 匹配的签名，普通 ELF 起不来。这里只用 libkernel 和 libSceSystemService 的
 * 几个函数，不链 C 库（所以 -ffreestanding、没有 memset/memcpy；目录里的
 * sce_module/libc.prx 只是加载器强制要求，见该目录的 NOTICE.txt）。
 *
 * 方法学自 ps5-native-app-boilerplate（BlackBearReloaded, GPL-3.0-or-later）
 * 与 PS5 Cooling & System Center 的 browser 磁贴：freestanding 启动器 +
 * 链接脚本 + ps5-native-tool + sce_module/libc.prx。
 */
#include <stddef.h>
#include <stdint.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>

/* 与 src/main.c 的 DEFAULT_PORT / PORT_SPAN 对应：payload 在 8520 起最多往后
 * 找 32 个端口。这里全试一遍，本机 refused 是立刻返回的，试满也只要几毫秒。 */
#define PORT_FIRST  8520
#define PORT_LAST   8552
#define PORT_LOADER 9021

/* libkernel / libSceSystemService（SDK 的 target/lib 里的 stub） */
int close(int fd);
int sceKernelUsleep(unsigned int microseconds);
int sceKernelDebugOutText(int channel, const char *text);
int sceKernelSendNotificationRequest(int device, void *request, size_t size, int blocking);
int sceSystemServiceLoadExec(const char *path, const char *const *argv);

/* payload.elf 由 build_title.sh 复制进来，.incbin 直接把它贴进 eboot.bin：
 * 磁贴和它拉起的程序永远是同一版，不会出现"磁贴升级了 payload 还是旧的"。 */
__asm__(".section .rodata\n"
        ".global payload_elf\n"
        "payload_elf:\n"
        ".incbin \"payload.elf\"\n"
        ".global payload_elf_end\n"
        "payload_elf_end:\n"
        ".previous\n");
extern const unsigned char payload_elf[], payload_elf_end[];

/* 来自链接脚本（title.ld）：PS5 的加载器不清 BSS，这里自己清。 */
extern char __bss_start[], __bss_end[];

static struct {
    char reserved[45];
    char message[3075];
} notify_request;

static size_t length(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

/* 屏幕上的通知 + 内核日志。磁贴没有页面可看，起不来时这是唯一的线索。 */
static void report(const char *msg) {
    volatile char *dst = notify_request.message;
    size_t i = 0;
    for (; msg[i] && i + 1 < sizeof(notify_request.message); i++) dst[i] = msg[i];
    dst[i] = 0;
    sceKernelDebugOutText(0, msg);
    sceKernelDebugOutText(0, "\n");
    sceKernelSendNotificationRequest(0, &notify_request, sizeof(notify_request), 0);
}

/* x86_64 小端上手工换字节序，省掉对 glibc __bswap_16 的引用（-ffreestanding
 * 下那个符号不存在）。 */
static unsigned short net16(unsigned short v) {
    return (unsigned short)(((v & 0x00ffu) << 8) | ((v & 0xff00u) >> 8));
}

static int connect_local(int port) {
    struct sockaddr_in sa;
    volatile unsigned char *p = (volatile unsigned char *)&sa;
    for (size_t i = 0; i < sizeof(sa); i++) p[i] = 0;
    sa.sin_len = sizeof(sa);
    sa.sin_family = AF_INET;
    sa.sin_port = net16((unsigned short)port);
    sa.sin_addr.s_addr = 0x0100007fu; /* 127.0.0.1，网络序 */
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    if (connect(s, (const struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(s);
        return -1;
    }
    return s;
}

static int send_all(int s, const unsigned char *p, size_t n) {
    while (n) {
        ssize_t w = send(s, p, n, 0);
        if (w <= 0) return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static int answered_ok(const char *buf, int n) {
    static const char ok[] = "HTTP/1.1 200";
    if (n < (int)sizeof(ok) - 1) return 0;
    for (size_t i = 0; ok[i]; i++)
        if (buf[i] != ok[i]) return 0;
    return 1;
}

/* 问一个端口上的服务把页面打开。1 = 它答应了，0 = 这个端口不是它。
 *
 * 用 POST /api/open-ui 而不是探测某个 GET：这个路径只有 quark-dl 有，别人的
 * 插件会回 404，于是"连上了但不是它"和"它答应了"能区分开。请求体必须是合法
 * JSON（payload 那边只挡住了路径和方法，不解析内容），{} 就够。
 *
 * 超时必须给：接受连接却永不回话的服务（卡死）不能把磁贴永远吊在这里。 */
static int ask_open_ui(int port) {
    static const char req[] = "POST /api/open-ui HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                              "Content-Type: application/json\r\nContent-Length: 2\r\n"
                              "Connection: close\r\n\r\n{}";
    char buf[128];
    int s = connect_local(port);
    if (s < 0) return 0;
    struct timeval tv = { 3, 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (send_all(s, (const unsigned char *)req, length(req)) != 0) {
        close(s);
        return 0;
    }
    ssize_t n = recv(s, buf, sizeof(buf) - 1, 0);
    close(s);
    if (n <= 0) return 0;
    buf[n] = 0;
    return answered_ok(buf, (int)n);
}

static int open_ui(void) {
    for (int port = PORT_FIRST; port <= PORT_LAST; port++)
        if (ask_open_ui(port)) return 1;
    return 0;
}

/* 把内嵌的 payload.elf 交给越狱的 ELF 加载器。0 = 已发出。 */
static int send_payload(void) {
    int s = connect_local(PORT_LOADER);
    if (s < 0) {
        report("Quark DL：找不到 ELF 加载器（9021）。越狱加载了吗？");
        return -1;
    }
    int rc = send_all(s, payload_elf, (size_t)(payload_elf_end - payload_elf));
    close(s);
    if (rc != 0) report("Quark DL：发送 payload 到加载器失败");
    return rc;
}

static void run(void) {
    if (open_ui()) return; /* 服务已经在跑：页面已经开了，什么都不用做 */
    if (send_payload() != 0) return;
    for (int i = 0; i < 120; i++) { /* 最多 30 秒 */
        sceKernelUsleep(250 * 1000);
        if (open_ui()) return;
    }
    report("Quark DL：后台服务没有响应，看 /data/quark-dl/boot.log");
}

/* 标题入口。结束时走 sceSystemServiceLoadExec("exit")：原生标题里 exit() 会
 * 抛 SIGSYS，而普通应用没有权限自杀。退出前先等几秒——磁贴恰好在浏览器被叫
 * 起来的那一刻结束时，它的进程会在拆卸过程中启动，沙箱可能留下残余。 */
__attribute__((noreturn, visibility("default"))) void _start(void *process_parameters,
                                                             void (*teardown)(void)) {
    (void)process_parameters;
    (void)teardown;
    for (volatile char *p = __bss_start; p < __bss_end; p++) *p = 0;
    run();
    sceKernelUsleep(3000 * 1000);
    sceSystemServiceLoadExec("exit", NULL);
    for (;;) sceKernelUsleep(100 * 1000);
}
