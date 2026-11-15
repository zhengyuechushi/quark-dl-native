#ifndef SHELL_H
#define SHELL_H

/* Asks the console to open its browser on `url`. Used by POST /api/open-ui so a
 * device on the same network can bring the UI up on the TV. The home-screen
 * icon does not go through here: it is a web-app entry the system launches on
 * its own (src/launcher.c).
 *
 * 0 = the request was accepted, -1 = nothing could be asked (host build, or the
 * system refused). A caller that gets -1 has not been told the browser is shut;
 * it has been told the console did not take the request. */
int shell_open_url(const char *url);

#endif
