#ifndef SHELL_H
#define SHELL_H

/* Asks the console to open its browser on `url`. On the console the home-screen
 * tile (starter/) relies on this: the tile only starts the payload, the payload
 * is the one that knows which port it actually ended up on.
 *
 * 0 = the request was accepted, -1 = nothing could be asked (host build, or the
 * system refused). A caller that gets -1 has not been told the browser is shut;
 * it has been told the console did not take the request. */
int shell_open_url(const char *url);

#endif
