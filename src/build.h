#pragma once

/* Injected by the Makefile as -DBUILD_ID=\"<commit sha>\". A file named
 * quark-dl.elf that answers /api/health with the commit it was built from is
 * the only way to tell a fresh build from a stale copy on the console. */
#ifndef BUILD_ID
#define BUILD_ID "dev"
#endif

#define APP_NAME "quark-dl"
#ifndef APP_VERSION
#define APP_VERSION "0.1.0"
#endif
