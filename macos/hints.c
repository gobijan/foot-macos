#include "hints.h"

#include <limits.h>
#include <regex.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LOG_MODULE "hints"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "debug.h"
#include "terminal.h"
#include "url-mode.h"
#include "util.h"
#include "xmalloc.h"

/*
 * (In brackets, "-" comes last.) The first group is the hint (url-mode.c); what's before it is only
 * there to not begin in the middle of something (e.g. a URL's path).
 * macOS' enhanced syntax: \b, and groups that aren't counted.
 */
#define PATH_CHARS "[:alnum:]_.+@%-"

static const char *const patterns[HINT_COUNT] = {
    /* With a slash (~/a, ./a/b, /a, a/b.c), or a name with an extension (a.c) */
    [HINT_PATH] =
        "(?:^|[^~:/" PATH_CHARS "])("
        "(?:~|\\.{1,2})?/?[" PATH_CHARS "]+(?:/[" PATH_CHARS "]+)+/?|"
        "(?:~|\\.{1,2})?/?[" PATH_CHARS "]+/|"
        "~?/[" PATH_CHARS "]+|"
        "[[:alnum:]_+@%-][" PATH_CHARS "]*\\.[[:alpha:]][[:alnum:]]{0,6}"
        ")",

    /* git's, abbreviated or not */
    [HINT_HASH] = "\\b([0-9a-f]{7,40})\\b",

    [HINT_IP] = "\\b((?:[0-9]{1,3}\\.){3}[0-9]{1,3})\\b",

    /* Without the white space around it */
    [HINT_LINE] = "^[[:space:]]*([^[:space:]].*[^[:space:]]|[^[:space:]])",

    [HINT_WORD] = "([^[:space:]]+)",
};

static regex_t compiled[HINT_COUNT];
static bool is_compiled[HINT_COUNT];

static const regex_t *
pattern_of(enum hint hint)
{
    if (!is_compiled[hint]) {
        const int r = regcomp(&compiled[hint], patterns[hint], REG_EXTENDED | REG_ENHANCED);
        if (r != 0) {
            char err[128];
            regerror(r, &compiled[hint], err, sizeof(err));
            LOG_ERR("%s: %s", patterns[hint], err);
            return NULL;
        }
        is_compiled[hint] = true;
    }
    return &compiled[hint];
}

bool
hints_show(struct terminal *term, enum hint hint)
{
    if (urls_mode_is_active(term))
        return false;

    const regex_t *preg = pattern_of(hint);
    if (preg == NULL)
        return false;

    urls_collect(term, URL_ACTION_COPY, preg, false, &term->url.list);
    if (tll_length(term->url.list) == 0)
        return false;

    urls_assign_key_combos(term->conf, &term->url.list);
    urls_render(term, &term->conf->url.launch);
    return true;
}

/*
 * The file's URL, if there's such a file: in the shell's directory, if
 * the path isn't absolute (not known: no file). What looks like a path
 * often isn't one ("and/or", a date).
 */
static char *
file_url(const struct terminal *term, const char *path)
{
    char full[PATH_MAX];
    const char *home = getenv("HOME");

    if (path[0] == '~' && (path[1] == '/' || path[1] == '\0') && home != NULL)
        snprintf(full, sizeof(full), "%s%s", home, path + 1);
    else if (path[0] == '/')
        snprintf(full, sizeof(full), "%s", path);
    else if (term->cwd != NULL && term->cwd[0] == '/')
        snprintf(full, sizeof(full), "%s/%s", term->cwd, path);
    else
        return NULL;

    if (access(full, F_OK) < 0)
        return NULL;

    /* As a URL's path is read (uri.c): "%" begins an encoded character */
    char *url = xmalloc(strlen("file://") + 3 * strlen(full) + 1);
    char *out = stpcpy(url, "file://");
    for (const char *p = full; *p != '\0'; p++) {
        if (*p == '%' || *p == '?' || *p == '#')
            out += sprintf(out, "%%%02X", (unsigned char)*p);
        else
            *out++ = *p;
    }
    *out = '\0';
    return url;
}

/* Where a hint is on the screen, as one number */
static long
position(const struct terminal *term, struct coord at)
{
    return (long)at.row * term->cols + at.col;
}

bool
hints_show_open(struct terminal *term)
{
    if (urls_mode_is_active(term))
        return false;

    /* (First: urls_collect() expects that the terminal has none yet) */
    const regex_t *preg = pattern_of(HINT_PATH);
    url_list_t paths = tll_init();
    if (preg != NULL)
        urls_collect(term, URL_ACTION_LAUNCH, preg, false, &paths);

    /* As foot's show-urls-launch: the URLs, and the hyperlinks (OSC 8) */
    urls_collect(term, URL_ACTION_LAUNCH, &term->conf->url.preg, true, &term->url.list);

    tll_foreach(paths, it) {
        struct url path = it->item;
        tll_remove(paths, it);

        /* Not what's (in) a URL already, e.g. a hyperlink on a file's name */
        bool taken = false;
        tll_foreach(term->url.list, url) {
            if (position(term, path.range.start) <= position(term, url->item.range.end) &&
                position(term, path.range.end) >= position(term, url->item.range.start))
            {
                taken = true;
                break;
            }
        }

        char *url = taken ? NULL : file_url(term, path.url);
        free(path.url);
        if (url == NULL)
            continue;

        path.url = url;
        tll_push_back(term->url.list, path);
    }

    if (tll_length(term->url.list) == 0)
        return false;

    urls_assign_key_combos(term->conf, &term->url.list);
    urls_render(term, &term->conf->url.launch);
    return true;
}

/* For the tests: all of 'text's hints, each followed by "|" */
static UNUSED char *
hints_in(enum hint hint, const char *text)
{
    const regex_t *preg = pattern_of(hint);
    xassert(preg != NULL);

    char *all = xstrdup("");
    while (true) {
        /* As url-mode.c searches: on, after the whole match */
        regmatch_t matches[2];
        if (regexec(preg, text, 2, matches, 0) == REG_NOMATCH)
            break;

        char *more = xasprintf("%s%.*s|", all, (int)(matches[1].rm_eo - matches[1].rm_so),
                               &text[matches[1].rm_so]);
        free(all);
        all = more;
        text += matches[0].rm_eo;
    }
    return all;
}

static UNUSED void
expect(enum hint hint, const char *text, const char *hints)
{
    char *got = hints_in(hint, text);
    if (!streq(got, hints)) {
        /* (Before main(): no log yet) */
        fprintf(stderr, "hints in \"%s\": \"%s\", expected \"%s\"\n", text, got, hints);
        abort();
    }
    free(got);
}

UNITTEST
{
    expect(HINT_PATH, "modified:   macos/view.m", "macos/view.m|");
    expect(HINT_PATH, "../macos/hints.c:12:5: error: in 'main.c'", "../macos/hints.c|main.c|");
    expect(HINT_PATH, "cd ~/Work/foot && ls /tmp ./a.out ~/", "~/Work/foot|/tmp|./a.out|");
    expect(HINT_PATH, "-rw-r--r--  1 me  staff  12 Nov 11 Makefile README.md", "README.md|");
    expect(HINT_PATH, "see https://example.com/a/b.html (v1.28.0)", "");
    expect(HINT_PATH, "(src/a b.c) \"dir/\"", "src/a|b.c|dir/|");

    expect(HINT_HASH, "commit 1710e40c8a (HEAD -> main) 46c3b37a: 123456 deadbeefg",
           "1710e40c8a|46c3b37a|");
    expect(HINT_IP, "inet 192.168.1.20 netmask 0xffffff00, 1.1.1.1:53, v1.2.3", "192.168.1.20|1.1.1.1|");
    expect(HINT_LINE, "   two words  ", "two words|");
    expect(HINT_LINE, "x", "x|");
    expect(HINT_WORD, " a  bc ", "a|bc|");
}
