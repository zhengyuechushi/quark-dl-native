/* Opening the console's own browser.
 *
 * A payload loaded through the ELF loader has no tile of its own: the user sees
 * a notification with an address and has to type it into the browser. The
 * home-screen tile (starter/) fixes the entry point, but the tile is a separate
 * process that knows neither the payload's port nor its session - the payload
 * does. So the tile asks, and this file is the only thing in the payload that
 * talks to the system UI.
 *
 * The SDK ships sceSystemServiceLaunchWebBrowser as a stub in
 * libSceSystemService; the Makefile links that library for the console target
 * only. The host build answers -1, which keeps `make linux` and the smoke job
 * free of console-only machinery. */

#include "shell.h"

#ifdef __SCE__
/* Declared by hand for the same reason sceKernelSendNotificationRequest is
 * (notify.c): the header for this library is not part of the payload include
 * set. Signature and return convention match the SDK stub and the reference
 * apps that call it: int (const char *uri, void *userdata), 0 = accepted. */
int sceSystemServiceLaunchWebBrowser(const char *uri, void *userdata);

int
shell_open_url(const char *url) {
  if(!url || !*url) {
    return -1;
  }
  return sceSystemServiceLaunchWebBrowser(url, 0) == 0 ? 0 : -1;
}
#else
int
shell_open_url(const char *url) {
  (void)url;
  return -1;
}
#endif
