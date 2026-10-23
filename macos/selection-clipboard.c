/*
 * The clipboard (NSPasteboard), the primary selection (kept
 * internally; there's none on macOS) and drag-and-drop, for
 * selection.c. Replaces upstream's Wayland data device code, which is
 * left in selection.c, but not compiled on macOS.
 */

#include "selection.h"

#include <string.h>

#include <sys/epoll.h>

#define LOG_MODULE "selection"
#define LOG_ENABLE_DBG 0
#include "log.h"

#include "macros.h"
#include "terminal.h"
#include "uri.h"
#include "util.h"
#include "xmalloc.h"

#if defined(_DEBUG) && LOG_ENABLE_DBG
static const char *const mime_type_map[] = {
    [DATA_OFFER_MIME_UNSET] = NULL,
    [DATA_OFFER_MIME_TEXT_PLAIN] = "text/plain",
    [DATA_OFFER_MIME_TEXT_UTF8] = "text/plain;charset=utf-8",
    [DATA_OFFER_MIME_URI_LIST] = "text/uri-list",
    [DATA_OFFER_MIME_TEXT_TEXT] = "TEXT",
    [DATA_OFFER_MIME_TEXT_STRING] = "STRING",
    [DATA_OFFER_MIME_TEXT_UTF8_STRING] = "UTF8_STRING",
};
#endif

bool
selection_clipboard_has_data(const struct seat *seat)
{
    return wayl_clipboard_has_data(seat->wayl);
}

bool
selection_primary_has_data(const struct seat *seat)
{
    return seat->primary.text != NULL;
}

void
selection_clipboard_unset(struct seat *seat)
{
    struct wl_clipboard *clipboard = &seat->clipboard;

    if (clipboard->text == NULL)
        return;

    /* Only clears the pasteboard if it still holds our text */
    wayl_clipboard_clear(seat->wayl, clipboard->change_count);

    clipboard->serial = 0;
    free(clipboard->text);
    clipboard->text = NULL;
}

void
selection_primary_unset(struct seat *seat)
{
    struct wl_primary *primary = &seat->primary;

    primary->serial = 0;
    free(primary->text);
    primary->text = NULL;
}

bool
text_to_clipboard(struct seat *seat, struct terminal *term, char *text, uint32_t serial)
{
    if (text == NULL || text[0] == '\0')
        return false;

    struct wl_clipboard *clipboard = &seat->clipboard;

    long change_count = wayl_clipboard_set(seat->wayl, text);
    if (change_count < 0) {
        LOG_ERR("failed to put text on the pasteboard");
        return false;
    }

    free(clipboard->text);
    clipboard->text = text;
    clipboard->change_count = change_count;
    clipboard->serial = serial;
    return true;
}

void
selection_to_clipboard(struct seat *seat, struct terminal *term, uint32_t serial)
{
    if (term->selection.coords.start.row < 0 || term->selection.coords.end.row < 0)
        return;

    /* Get selection as a string */
    char *text = selection_to_text(term);
    if (!text_to_clipboard(seat, term, text, serial))
        free(text);
}

struct clipboard_receive {
    bool bracketed;
    bool no_strip;
    bool quote_paths;

    void (*decoder)(struct clipboard_receive *ctx, char *data, size_t size);
    void (*finish)(struct clipboard_receive *ctx);

    /* URI state */
    bool add_space;
    struct {
        char *data;
        size_t sz;
        size_t idx;
    } buf;

    /* Callback data */
    void (*cb)(char *data, size_t size, void *user);
    void (*done)(void *user);
    void *user;
};

static void
receive_decoder_plain(struct clipboard_receive *ctx, char *data, size_t size)
{
    ctx->cb(data, size, ctx->user);
}

static void
receive_finish_plain(struct clipboard_receive *ctx)
{
}

static bool
decode_one_uri(struct clipboard_receive *ctx, char *uri, size_t len)
{
    LOG_DBG("URI: \"%.*s\"", (int)len, uri);

    if (len == 0)
        return false;

    char *scheme, *host, *path;
    if (!uri_parse(uri, len, &scheme, NULL, NULL, &host, NULL, &path, NULL, NULL)) {
        LOG_ERR("drag-and-drop: invalid URI: %.*s", (int)len, uri);
        return false;
    }

    if (ctx->add_space)
        ctx->cb(" ", 1, ctx->user);
    ctx->add_space = true;

    if (streq(scheme, "file") && hostname_is_localhost(host)) {
        if (ctx->quote_paths)
            ctx->cb("'", 1, ctx->user);

        char *path_remaining = path;
        for (char *next_quote = strchr(path_remaining, '\'');
             next_quote != NULL;
             path_remaining = next_quote + 1,
                 next_quote = strchr(path_remaining, '\''))
        {
            ctx->cb(path_remaining, next_quote - path_remaining, ctx->user);
            ctx->cb("\\'", 2, ctx->user);
        }
        ctx->cb(path_remaining, strlen(path_remaining), ctx->user);

        if (ctx->quote_paths)
            ctx->cb("'", 1, ctx->user);
    } else
        ctx->cb(uri, len, ctx->user);

    free(scheme);
    free(host);
    free(path);
    return true;
}

static void
receive_decoder_uri(struct clipboard_receive *ctx, char *data, size_t size)
{
    while (ctx->buf.idx + size > ctx->buf.sz) {
        size_t new_sz = ctx->buf.sz == 0 ? size : 2 * ctx->buf.sz;
        ctx->buf.data = xrealloc(ctx->buf.data, new_sz);
        ctx->buf.sz = new_sz;
    }

    memcpy(&ctx->buf.data[ctx->buf.idx], data, size);
    ctx->buf.idx += size;

    char *start = ctx->buf.data;
    char *end = NULL;

    while (true) {
        for (end = start; end < &ctx->buf.data[ctx->buf.idx]; end++) {
            if (*end == '\r' || *end == '\n')
                break;
        }

        if (end >= &ctx->buf.data[ctx->buf.idx])
            break;

        decode_one_uri(ctx, start, end - start);
        start = end + 1;
    }

    const size_t ofs = start - ctx->buf.data;
    const size_t left = ctx->buf.idx - ofs;

    memmove(&ctx->buf.data[0], &ctx->buf.data[ofs], left);
    ctx->buf.idx = left;
}

static void
receive_finish_uri(struct clipboard_receive *ctx)
{
    LOG_DBG("finish: %.*s", (int)ctx->buf.idx, ctx->buf.data);
    decode_one_uri(ctx, ctx->buf.data, ctx->buf.idx);
}

/*
 * Calls the decoder (and thus the user callback) with 'text', while
 * at the same time replacing:
 *   - \r\n -> \r  (non-bracketed paste)
 *   - \n -> \r    (non-bracketed paste)
 *   - C0 -> <nothing>  (strip non-formatting C0 characters)
 *   - \e -> <nothing>  (i.e. strip ESC)
 *
 * Note: modifies 'text'
 */
static void
receive_data(struct clipboard_receive *ctx, char *text, size_t count)
{
    const bool no_strip = ctx->no_strip;
    const bool bracketed = ctx->bracketed;

    char *p = text;
    size_t left = count;

#define skip_one()                              \
    do {                                        \
        ctx->decoder(ctx, p, i);                \
        xassert(i + 1 <= left);                 \
        p += i + 1;                             \
        left -= i + 1;                          \
    } while (0)

again:
    for (size_t i = 0; i < left; i++) {
        switch (p[i]) {
        default:
            break;

        case '\n':
            if (!no_strip && !bracketed) {
                p[i] = '\r';
            }
            break;

        case '\r':
            /* Convert \r\n -> \r */
            if (!no_strip && !bracketed && i + 1 < left && p[i + 1] == '\n') {
                i++;
                skip_one();
                goto again;
            }
            break;

        /* C0 non-formatting control characters (\b \t \n \r excluded) */
        case '\x01': case '\x02': case '\x03': case '\x04': case '\x05':
        case '\x06': case '\x07': case '\x0e': case '\x0f': case '\x10':
        case '\x11': case '\x12': case '\x13': case '\x14': case '\x15':
        case '\x16': case '\x17': case '\x18': case '\x19': case '\x1a':
        case '\x1b': case '\x1c': case '\x1d': case '\x1e': case '\x1f':
            if (!no_strip) {
                skip_one();
                goto again;
            }
            break;

        /*
         * In addition to stripping non-formatting C0 controls,
         * XTerm has an option, "disallowedPasteControls", that
         * defines C0 controls that will be replaced with spaces
         * when pasted.
         *
         * It's default value is BS,DEL,ENQ,EOT,NUL
         *
         * Instead of replacing them with spaces, we allow them in
         * bracketed paste mode, and strip them completely in
         * non-bracketed mode.
         *
         * Note some of the (default) XTerm controls are already
         * handled above.
         */
        case '\b': case '\x7f': case '\x00':
            if (!no_strip && !bracketed) {
                skip_one();
                goto again;
            }
            break;
        }
    }

    ctx->decoder(ctx, p, left);

#undef skip_one
}

/* Takes ownership of 'text' */
static void
receive_text(struct terminal *term, bool no_strip,
             char *text, size_t len, enum data_offer_mime_type mime_type,
             void (*cb)(char *data, size_t size, void *user),
             void (*done)(void *user), void *user)
{
    struct clipboard_receive ctx = {
        .bracketed = term->bracketed_paste,
        .no_strip = no_strip,
        .quote_paths = term->grid == &term->normal,
        .decoder = (mime_type == DATA_OFFER_MIME_URI_LIST
                    ? &receive_decoder_uri
                    : &receive_decoder_plain),
        .finish = (mime_type == DATA_OFFER_MIME_URI_LIST
                   ? &receive_finish_uri
                   : &receive_finish_plain),
        .cb = cb,
        .done = done,
        .user = user,
    };

    receive_data(&ctx, text, len);
    ctx.finish(&ctx);

    free(ctx.buf.data);
    free(text);
    done(user);
}

void
text_from_clipboard(struct seat *seat, struct terminal *term,
                    bool no_strip,
                    void (*cb)(char *data, size_t size, void *user),
                    void (*done)(void *user), void *user)
{
    enum data_offer_mime_type mime_type;
    size_t len;
    char *text = wayl_clipboard_get(seat->wayl, &mime_type, &len);

    if (text == NULL) {
        done(user);
        return;
    }

    LOG_DBG("receive from clipboard: mime-type=%s", mime_type_map[mime_type]);
    receive_text(term, no_strip, text, len, mime_type, cb, done, user);
}

static void
receive_offer(char *data, size_t size, void *user)
{
    struct terminal *term = user;
    xassert(term->is_sending_paste_data);
    term_paste_data_to_slave(term, data, size);
}

static void
receive_offer_done(void *user)
{
    struct terminal *term = user;

    if (term->bracketed_paste)
        term_paste_data_to_slave(term, "\033[201~", 6);

    term->is_sending_paste_data = false;

    /* Make sure we send any queued up non-paste data */
    if (tll_length(term->ptmx_buffers) > 0)
        fdm_event_add(term->fdm, term->ptmx, EPOLLOUT);
}

void
selection_from_clipboard(struct seat *seat, struct terminal *term, uint32_t serial)
{
    if (term->is_sending_paste_data) {
        /* We're already pasting... */
        return;
    }

    if (!wayl_clipboard_has_data(seat->wayl))
        return;

    term->is_sending_paste_data = true;

    if (term->bracketed_paste)
        term_paste_data_to_slave(term, "\033[200~", 6);

    text_from_clipboard(
        seat, term, false, &receive_offer, &receive_offer_done, term);
}

bool
text_to_primary(struct seat *seat, struct terminal *term, char *text, uint32_t serial)
{
    if (text == NULL || text[0] == '\0')
        return false;

    struct wl_primary *primary = &seat->primary;

    free(primary->text);
    primary->text = text;
    primary->serial = serial;
    return true;
}

void
selection_to_primary(struct seat *seat, struct terminal *term, uint32_t serial)
{
    /* Get selection as a string */
    char *text = selection_to_text(term);
    if (!text_to_primary(seat, term, text, serial))
        free(text);
}

void
text_from_primary(
    struct seat *seat, struct terminal *term, bool no_strip,
    void (*cb)(char *data, size_t size, void *user),
    void (*done)(void *user), void *user)
{
    const struct wl_primary *primary = &seat->primary;
    if (primary->text == NULL) {
        done(user);
        return;
    }

    receive_text(
        term, no_strip, xstrdup(primary->text), strlen(primary->text),
        DATA_OFFER_MIME_TEXT_UTF8, cb, done, user);
}

void
selection_from_primary(struct seat *seat, struct terminal *term)
{
    if (term->is_sending_paste_data) {
        /* We're already pasting... */
        return;
    }

    if (seat->primary.text == NULL)
        return;

    term->is_sending_paste_data = true;
    if (term->bracketed_paste)
        term_paste_data_to_slave(term, "\033[200~", 6);

    text_from_primary(
        seat, term, false, &receive_offer, &receive_offer_done, term);
}

void
selection_drop(struct seat *seat, struct terminal *term,
               char *data, size_t len, enum data_offer_mime_type mime_type)
{
    if (term->is_sending_paste_data) {
        /* We're already pasting... */
        free(data);
        return;
    }

    LOG_DBG("DnD drop: mime-type=%s", mime_type_map[mime_type]);

    term->is_sending_paste_data = true;

    if (term->bracketed_paste)
        term_paste_data_to_slave(term, "\033[200~", 6);

    receive_text(
        term, false, data, len, mime_type,
        &receive_offer, &receive_offer_done, term);
}
