#pragma once

/*
 * Sockets over a stream: footd and footd --bridge, over SSH (see
 * session-protocol.h). What's written to a socket on one side comes out
 * of its channel's socket on the other, and its end too. Besides, the
 * stream carries the session protocol's messages.
 *
 * Flow control is per channel: a socket that isn't read (e.g. a busy
 * window) holds up its own channel only. When the stream itself is
 * full, no socket is read until it's drained.
 */

#include <stdbool.h>
#include <stdint.h>

#include "fdm.h"
#include "session-protocol.h"

struct session_mux;

struct session_mux_callbacks {
    /* A message of the session protocol; false: the stream is broken */
    bool (*message)(struct session_mux *mux, uint32_t type,
                    struct session_reader *payload, void *data);

    /* The other side announced a channel (CHANNEL): its socket is the next CREATE/ATTACH's */
    void (*announced)(struct session_mux *mux, uint32_t channel, void *data);

    /* A channel's socket is closed: by the other side ('theirs'), or here */
    void (*channel_closed)(struct session_mux *mux, uint32_t channel, bool theirs,
                           void *data);

    /* The stream is gone (an error, or its end); destroy the mux */
    void (*closed)(struct session_mux *mux, void *data);

    /* Optional: something came over the stream (before it's handled) */
    void (*received)(struct session_mux *mux, void *data);

    /* Optional: a channel's socket was read (e.g. a window's input), for the other side */
    void (*input)(struct session_mux *mux, uint32_t channel, void *data);
};

/* Over 'in_fd' and 'out_fd' (may be the same; both are taken) */
struct session_mux *session_mux_new(struct fdm *fdm, int in_fd, int out_fd,
                                    const struct session_mux_callbacks *callbacks,
                                    void *data);

/* Closes the stream, and the channels' sockets */
void session_mux_destroy(struct session_mux *mux);

/* Sends a message of the session protocol */
bool session_mux_send(struct session_mux *mux, struct session_msg *msg);

/* Announces 'channel' (CHANNEL), for the CREATE or ATTACH that's sent next */
bool session_mux_announce(struct session_mux *mux, uint32_t channel);

/* Relays the socket 'fd' (taken) as 'channel' */
bool session_mux_add(struct session_mux *mux, uint32_t channel, int fd);

/*
 * Stops relaying 'channel', and returns its socket (or -1): it stays
 * open. The other side's is closed.
 */
int session_mux_take(struct session_mux *mux, uint32_t channel);
