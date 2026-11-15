/* 主屏幕图标的来源：运行时把本 payload 注册成一个"网页应用"条目，
 * 而不是预打包一个签名 eboot.bin 装到 homebrew 目录。
 *
 * 链路抄自 owendswang/ps5-web-file-manager 的 src/app_installer.c
 * （GPL-3.0-or-later），它本身又是从 ps5-payload-dev/ftpsrv 的
 * home screen launcher / install flow 来的：
 *
 *   sceAppInstUtilInitialize()
 *   → 写 /user/app/<TITLEID>/sce_sys/{param.json,icon0.png}
 *   → AppInstallTitleDir(<TITLEID>, "/user/app/", 0)   ← 按 NID 运行时解析
 *
 * AppInstallTitleDir 刻意不走静态 stub，而是用 kernel_dynlib_handle +
 * kernel_dynlib_resolve 按 NID 取：参考项目就是这么做的，实测可用；按 NID
 * 取还能绕开不同固件导出表不一致的问题。取不到就退到 AppInstallAll。
 *
 * 上一版走的是另一条路（starter/ 里 freestanding C 编 eboot.bin，
 * ps5-native-tool 包成签名 FSELF，装 /data/homebrew/ 由 ShadowMount+ 挂载），
 * 真机上 payload 正常、图标不出现，说明失败的正是"签名 SELF + 挂载"这一环。
 *
 * 0.12.0 把下面这些调用同步放在了 HTTP 服务之前，真机上直接导致整个 payload
 * 起不来（无通知、端口不通）。现在全部搬进后台线程，并且：
 *   1. 每一步都写 boot.log，失败能看出停在哪一行；
 *   2. 任何一步失败都只是"没有图标"，服务照常；
 *   3. 线程在服务已经开始接请求之后才动手。
 */

#include "launcher.h"

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "asset.h"
#include "boot.h"

#ifdef __SCE__
#include <pthread.h>
#include <unistd.h>
/* kernel_dynlib_handle / kernel_dynlib_resolve / kernel_dynlib_dlsym：
 * payload SDK 自带（<ps5/kernel.h>），参考项目也直接用这个头。 */
#include <ps5/kernel.h>

/* 只留两个确实从静态 stub 拿的符号（参考项目同样只链这两个）：
 * Initialize 是安装子系统初始化，AppInstallAll 是解析不到 TitleDir 时的兜底
 * （它会重扫 /user/app 下所有目录）。 */
int sceAppInstUtilInitialize(void);
int sceAppInstUtilAppInstallAll(void *);
#endif

#ifndef TITLE_ID
#define TITLE_ID "QDLK85200"
#endif

#define LAUNCH_BASE "/user/app"
#define LAUNCH_DIR LAUNCH_BASE "/" TITLE_ID
#define LAUNCH_SYS LAUNCH_DIR "/sce_sys"

/* sceAppInstUtilAppInstallTitleDir 的 NID，取自参考项目实测可用的值。 */
#define LAUNCH_NID "Wudg3Xe3heE"

/* 服务开张之后等这么久再动手注册：让 MHD 先把端口接起来，注册过程中的
 * 系统调用再慢，用户那边页面已经能开了。 */
#define LAUNCH_DELAY_US (1500 * 1000)

static char g_status[192] = "尚未注册";

#ifdef __SCE__
/* 图标和 param.json 直接嵌进 ELF：条目内容和 payload 永远同版本，
 * 不会出现"图标是新的、服务是旧的"这种对不上的情况。 */
#define QDL_INCBIN(name, file)                                 \
  __asm__(".section .rodata\n"                                 \
          ".global " #name "\n"                                \
          ".global " #name "_end\n"                            \
          ".global " #name "_size\n"                           \
          ".align 16\n"                                        \
          #name ":\n"                                          \
          ".incbin \"" file "\"\n"                             \
          #name "_end:\n"                                      \
          #name "_size:\n"                                     \
          ".quad " #name "_end - " #name "\n"                  \
          ".previous\n");                                      \
  extern const uint8_t name[];                                 \
  extern const size_t name##_size;

QDL_INCBIN(qdl_param_json, "assets/param.json");
QDL_INCBIN(qdl_icon_png, "assets/icon0.png");

typedef int (*install_title_dir_fn)(const char *, const char *, void *);

/* 只解析一次。失败就记住失败，不要每次重试都去啃内核表。 */
static install_title_dir_fn
resolve_install_title_dir(void) {
  static install_title_dir_fn fn;
  static int done;
  uint32_t handle = 0;

  if(done) {
    return fn;
  }
  done = 1;

  if(kernel_dynlib_handle(-1, "libSceAppInstUtil.sprx", &handle) || !handle) {
    boot_trace("注册条目: 取不到 libSceAppInstUtil.sprx 句柄, errno %d", errno);
    return 0;
  }
  fn = (install_title_dir_fn)kernel_dynlib_resolve(-1, handle, LAUNCH_NID);
  if(fn) {
    boot_trace("注册条目: 按 NID 解析到 AppInstallTitleDir");
    return fn;
  }
  fn = (install_title_dir_fn)kernel_dynlib_dlsym(-1, handle,
                                                "sceAppInstUtilAppInstallTitleDir");
  boot_trace("注册条目: 按名字解析 AppInstallTitleDir %s",
             fn ? "成功" : "失败, 退到 AppInstallAll");
  return fn;
}

/* 已存在就不覆盖：用户可能在系统里改过名字，重写等于把设置抹掉。 */
static int
install_file(const char *path, const void *data, size_t size) {
  struct stat st;
  FILE *f;

  if(!stat(path, &st)) {
    return 0;
  }
  if(errno != ENOENT) {
    boot_trace("注册条目: stat %s 失败: %s", path, strerror(errno));
    return -1;
  }
  if(!(f = fopen(path, "wb"))) {
    boot_trace("注册条目: 打不开 %s: %s", path, strerror(errno));
    return -1;
  }
  if(fwrite(data, size, 1, f) != 1) {
    boot_trace("注册条目: 写 %s 失败: %s", path, strerror(errno));
    fclose(f);
    return -1;
  }
  return fclose(f) ? -1 : 0;
}

/* 端口被别的 payload 挤走时，只改 deeplinkUri 里的数字，别整份重写。
 * 返回 1 = 改了，0 = 没变，-1 = 出错。 */
static int
update_launcher_port(const char *path, unsigned short port) {
  static const char prefix[] = "http://127.0.0.1:";
  struct stat st;
  FILE *f = NULL;
  char *data = NULL;
  char tmp[512];
  char num[16];
  char *at, *end;
  size_t head, tail;
  int rc = -1;

  if(stat(path, &st) || st.st_size <= 0 || st.st_size > 65536) {
    return -1;
  }
  if(!(data = malloc((size_t)st.st_size + 1))) {
    return -1;
  }
  if(!(f = fopen(path, "rb"))) {
    goto out;
  }
  if(fread(data, 1, (size_t)st.st_size, f) != (size_t)st.st_size) {
    goto out;
  }
  fclose(f);
  f = NULL;
  data[st.st_size] = 0;

  snprintf(num, sizeof(num), "%u", (unsigned)port);
  if(!(at = strstr(data, prefix))) {
    goto out;
  }
  at += sizeof(prefix) - 1;
  end = at;
  while(isdigit((unsigned char)*end)) {
    end++;
  }
  if(end == at) {
    goto out;
  }
  if((size_t)(end - at) == strlen(num) && !memcmp(at, num, strlen(num))) {
    rc = 0;
    goto out;
  }

  head = (size_t)(at - data);
  tail = (size_t)st.st_size - (size_t)(end - data);
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  if(!(f = fopen(tmp, "wb"))) {
    goto out;
  }
  if(fwrite(data, 1, head, f) != head ||
     fwrite(num, 1, strlen(num), f) != strlen(num) ||
     fwrite(end, 1, tail, f) != tail) {
    fclose(f);
    f = NULL;
    remove(tmp);
    goto out;
  }
  if(fclose(f)) {
    f = NULL;
    remove(tmp);
    goto out;
  }
  f = NULL;
  if(rename(tmp, path)) {
    remove(tmp);
    goto out;
  }
  rc = 1;

out:
  if(f) {
    fclose(f);
  }
  free(data);
  return rc;
}

/* 真正干活的那一段。只在后台线程里跑，不碰 HTTP，不碰网络。 */
static int
install_entry(unsigned short port) {
  struct stat st;
  char param[192];
  char icon[192];
  install_title_dir_fn install;
  int err;

  snprintf(param, sizeof(param), "%s/param.json", LAUNCH_SYS);
  snprintf(icon, sizeof(icon), "%s/icon0.png", LAUNCH_SYS);

  /* 条目已经装过：只校准端口，不再走一次安装。
   * 这一步是纯文件操作，先做，省掉系统扫描。 */
  if(!stat(param, &st) && st.st_size > 0 && !stat(icon, &st)) {
    err = update_launcher_port(param, port);
    if(err <= 0) {
      if(err) {
        snprintf(g_status, sizeof(g_status), "条目已存在，改端口失败: %s",
                 strerror(errno));
        boot_trace("注册条目: %s", g_status);
      } else {
        snprintf(g_status, sizeof(g_status), "条目已存在，端口 %u",
                 (unsigned)port);
        boot_trace("注册条目: %s", g_status);
      }
      return err;
    }
  }

  if((err = sceAppInstUtilInitialize())) {
    snprintf(g_status, sizeof(g_status), "sceAppInstUtilInitialize 失败 0x%08X",
             (unsigned)err);
    boot_trace("注册条目: %s", g_status);
    return -1;
  }
  boot_trace("注册条目: Initialize 完成");

  if(mkdir(LAUNCH_BASE, 0700) && errno != EEXIST) {
    snprintf(g_status, sizeof(g_status), "建 %s 失败: %s", LAUNCH_BASE,
             strerror(errno));
    boot_trace("注册条目: %s", g_status);
    return -1;
  }
  if(mkdir(LAUNCH_DIR, 0755) && errno != EEXIST) {
    snprintf(g_status, sizeof(g_status), "建 %s 失败: %s", LAUNCH_DIR,
             strerror(errno));
    boot_trace("注册条目: %s", g_status);
    return -1;
  }
  if(mkdir(LAUNCH_SYS, 0755) && errno != EEXIST) {
    snprintf(g_status, sizeof(g_status), "建 %s 失败: %s", LAUNCH_SYS,
             strerror(errno));
    boot_trace("注册条目: %s", g_status);
    return -1;
  }
  if(install_file(param, qdl_param_json, qdl_param_json_size) ||
     install_file(icon, qdl_icon_png, qdl_icon_png_size) ||
     update_launcher_port(param, port) < 0) {
    snprintf(g_status, sizeof(g_status), "写 param.json/icon0.png 失败: %s",
             strerror(errno));
    boot_trace("注册条目: %s", g_status);
    return -1;
  }
  boot_trace("注册条目: 已写 %s (%u 字节) + icon0.png (%u 字节)", param,
             (unsigned)qdl_param_json_size, (unsigned)qdl_icon_png_size);

  /* 目录和文件都在了，最后才让系统的安装器去扫。顺序不能反：
   * 安装器扫不到文件就等于没装。 */
  install = resolve_install_title_dir();
  err = install ? install(TITLE_ID, LAUNCH_BASE "/", 0)
                : sceAppInstUtilAppInstallAll(0);
  if(err) {
    snprintf(g_status, sizeof(g_status), "注册条目失败 0x%08X", (unsigned)err);
    boot_trace("注册条目: %s", g_status);
    return -1;
  }

  snprintf(g_status, sizeof(g_status), "条目已注册，端口 %u", (unsigned)port);
  boot_trace("注册条目: %s", g_status);
  return 0;
}

static void *
launch_worker(void *arg) {
  unsigned short port = (unsigned short)(uintptr_t)arg;

  /* 先让端口接起来再动手。这个线程里全是系统调用，慢是它的事。 */
  usleep(LAUNCH_DELAY_US);
  boot_trace("注册条目线程启动, 端口 %u", (unsigned)port);
  (void)install_entry(port);
  return 0;
}
#endif /* __SCE__ */

void
launcher_serve_icon(void) {
#ifdef __SCE__
  /* 只插一个链表节点，没有系统调用，可以安全地放在 websrv 之前。
   * assets/icon0.png 不在 ASSETS 里（不当作页面发布），所以 /favicon.ico
   * 一直是 404，既然图标已经嵌进来了，顺手补上。 */
  asset_register("/icon0.png", qdl_icon_png, qdl_icon_png_size, "image/png", 0);
#endif
}

int
launcher_start(unsigned short port) {
#ifdef __SCE__
  pthread_t th;
  pthread_attr_t attr;
  int err;

  snprintf(g_status, sizeof(g_status), "待注册, 端口 %u", (unsigned)port);

  if(pthread_attr_init(&attr)) {
    snprintf(g_status, sizeof(g_status), "线程属性初始化失败");
    return -1;
  }
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  err = pthread_create(&th, &attr, launch_worker, (void *)(uintptr_t)port);
  pthread_attr_destroy(&attr);
  if(err) {
    snprintf(g_status, sizeof(g_status), "注册线程起不来: %d", err);
    return -1;
  }
  return 0;
#else
  (void)port;
  snprintf(g_status, sizeof(g_status), "host 构建不注册条目");
  return 0;
#endif
}

const char *
launcher_status(void) {
  return g_status;
}

const char *
launcher_title_id(void) {
  return TITLE_ID;
}
