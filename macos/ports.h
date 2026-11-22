#pragma once

/*
 * Another machine's ports, in its windows: in a window on another
 * machine (a session there, over SSH), "localhost" is that machine.
 * What's opened from it, a web server's URL or a database's, is opened
 * here, through a port of this Mac that footd passes on to the one
 * there (session.h's sessions_open_port()). And Shell > Open Port of
 * lists the ports programs listen on there, to open one.
 *
 * It takes no settings: the port here is the same if it's free (else
 * another, and the URL has that one), for this Mac only, and it's
 * closed by itself with the connection to the machine.
 */

#include <stdbool.h>

struct terminal;

/*
 * For url-mode.c, which opens all URLs: one of the machine itself, in a
 * window on another machine (http://localhost:3000; also 127.0.0.1,
 * [::1], and 0.0.0.0, which servers say they listen on), is opened once
 * its port is open here, with [url]'s launch. If it can't be, a sheet
 * says why. False: it isn't one (or the window is on this Mac, or the
 * URL has a launcher of its own).
 */
bool ports_open_url(struct terminal *term, const char *url);
