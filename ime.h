#pragma once

#include <stdint.h>

struct seat;
struct terminal;

/* Input method events, from the platform layer (NSTextInputClient) */
void ime_enter(struct seat *seat, struct terminal *term);
void ime_leave(struct seat *seat);
void ime_preedit(struct seat *seat, const char *text,
                 int32_t cursor_begin, int32_t cursor_end);
void ime_commit(struct seat *seat, const char *text);
void ime_done(struct seat *seat);

void ime_enable(struct seat *seat);
void ime_disable(struct seat *seat);
void ime_update_cursor_rect(struct seat *seat);

void ime_reset_pending(struct seat *seat);
void ime_reset_preedit(struct seat *seat);
