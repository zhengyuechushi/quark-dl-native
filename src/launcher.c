/* 主屏幕图标的来源：运行时把本 payload 注册成一个"网页应用"条目，
 * 而不是预打包一个签名 eboot.bin 装到 homebrew 目录。
 *
 * 链路抄自 owendswang/ps5-web-file-manager 的 src/app_installer.c
 * （GPL-3.0-or-later），它本身又是从 ps5-payload-dev/ftpsrv 的
 * home screen launcher / install flow 来的：
 *
 *   写 /user/app/<TITLEID>/sce_sys/{param.json,icon0.png}
 *   → sceAppInstUtilAppInstallTitleDir(<TITLEID>, "/user/app/", 0)
 *
 * 系统据此在主屏幕生成条目，点它由系统自己开浏览器到 deeplinkUri，
 * 不需要再把 payload 拉起一次，也不需要 sce_module/libc.prx。
 *
 * 上一版走的是另一条路（starter/ 里 freestanding C 编 eboot.bin，
 * ps5-native-tool 包成签名 FSELF，装 /data/homebrew/ 由 ShadowMount+ 挂载，
 * 点图标后由 starter 探端口再 POST /api/open-ui 拉起浏览器）。真机上
 * payload 正常、图标不出现，说明失败的正是"签名 SELF + 挂载"这一环。
 * 这条路用的是系统自己的安装通道，可验证的部分只有 /user/app 下有没有文件。
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

#ifdef __SCE__
/* libSceAppInstUtil 的两个导出。它们在 SDK 的 sce_stubs/libSceAppInstUtil.c
 * 里都有静态 stub（官方 samples/install_app 就是这么调的），所以直接链接
 * -lSceAppInstUtil 即可，不需要按 NID 去解析内核里的符号 ——
 * 那条路要内核读写权限，取不到就什么都装不上。 */
int sceAppInstUtilInitialize(void);
int sceAppInstUtilAppInstallTitleDir(const char *, const char *, void *);
#endif

#ifndef TITLE_ID
#define TITLE_ID "QDLK85200"
#endif

#define LAUNCH_BASE "/user/app"
#define LAUNCH_DIR LAUNCH_BASE "/" TITLE_ID
#define LAUNCH_SYS LAUNCH_DIR "/sce_sys"

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

/* 已存在就不覆盖：用户可能在系统里改过名字，重写等于把设置抹掉。 */
static int
install_file(const char *path, const void *data, size_t size) {
  struct stat st;
  FILE *f;

  if(!stat(path, &st)) {
    return 0;
  }
  if(errno != ENOENT) {
    return -1;
  }
  if(!(f = fopen(path, "wb"))) {
    return -1;
  }
  if(fwrite(data, size, 1, f) != 1) {
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
#endif /* __SCE__ */

int
launcher_install_if_needed(unsigned short port) {
#ifdef __SCE__
  struct stat st;
  char param[192];
  char icon[192];
  static int icon_served;
  int err;

  if(!icon_served) {
    /* assets/icon0.png 不在 ASSETS 里（不当作页面发布），所以 /favicon.ico
     * 一直是 404。既然图标已经嵌进来了，顺手补上。 */
    asset_register("/icon0.png", qdl_icon_png, qdl_icon_png_size, "image/png", 0);
    icon_served = 1;
  }

  snprintf(param, sizeof(param), "%s/param.json", LAUNCH_SYS);
  snprintf(icon, sizeof(icon), "%s/icon0.png", LAUNCH_SYS);

  /* 条目已经装过：只校准端口，不再走一次安装，省掉系统扫描。 */
  if(!stat(param, &st) && !stat(icon, &st)) {
    err = update_launcher_port(param, port);
    if(err <= 0) {
      if(err) {
        snprintf(g_status, sizeof(g_status), "条目已存在，改端口失败: %s",
                 strerror(errno));
      } else {
        snprintf(g_status, sizeof(g_status), "条目已存在，端口 %u",
                 (unsigned)port);
      }
      return err;
    }
  }

  if((err = sceAppInstUtilInitialize())) {
    snprintf(g_status, sizeof(g_status), "sceAppInstUtilInitialize 失败 0x%08X",
             (unsigned)err);
    return -1;
  }
  if(mkdir(LAUNCH_BASE, 0700) && errno != EEXIST) {
    snprintf(g_status, sizeof(g_status), "建 %s 失败: %s", LAUNCH_BASE,
             strerror(errno));
    return -1;
  }
  if(mkdir(LAUNCH_DIR, 0755) && errno != EEXIST) {
    snprintf(g_status, sizeof(g_status), "建 %s 失败: %s", LAUNCH_DIR,
             strerror(errno));
    return -1;
  }
  if(mkdir(LAUNCH_SYS, 0755) && errno != EEXIST) {
    snprintf(g_status, sizeof(g_status), "建 %s 失败: %s", LAUNCH_SYS,
             strerror(errno));
    return -1;
  }
  if(install_file(param, qdl_param_json, qdl_param_json_size) ||
     install_file(icon, qdl_icon_png, qdl_icon_png_size) ||
     update_launcher_port(param, port) < 0) {
    snprintf(g_status, sizeof(g_status), "写 param.json/icon0.png 失败: %s",
             strerror(errno));
    return -1;
  }

  /* 先把条目描述和图标落到 /user/app/<TID>/sce_sys，再让系统的安装器去扫。
   * 目录顺序不能反：安装器扫不到文件就等于没装。 */
  err = sceAppInstUtilAppInstallTitleDir(TITLE_ID, LAUNCH_BASE "/", 0);
  if(err) {
    snprintf(g_status, sizeof(g_status), "注册条目失败 0x%08X", (unsigned)err);
    return -1;
  }

  snprintf(g_status, sizeof(g_status), "条目已注册，端口 %u", (unsigned)port);
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
