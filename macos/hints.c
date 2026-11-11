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
    const enum hint which = hint == HINT_OPEN_PATH ? HINT_PATH : hint;

    if (!is_compiled[which]) {
        const int r = regcomp(&compiled[which], patterns[which], REG_EXTENDED | REG_ENHANCED);
        if (r != 0) {
            char err[128];
            regerror(r, &compiled[which], err, sizeof(err));
            LOG_ERR("%s: %s", patterns[which], err);
            return NULL;
        }
        is_compiled[which] = true;
    }
    return &compiled[which];
}

/* "open" takes neither "~", nor a name that begins with "-" */
static char *open_argv[] = {
    "/bin/sh", "-c",
    "p=$1; case $p in '~') p=$HOME;; '~/'*) p=$HOME/${p#??};; /*) ;; *) p=./$p;; esac; "
    "exec /usr/bin/open \"$p\"",
    "sh", "${match}", NULL,
};
static const struct config_spawn_template open_launch = {.argv = {.args = open_argv}};

/* Whether there's such a file: in the shell's directory (OSC 7), if not absolute */
static bool
exists(const struct terminal *term, const char *path)
{
    char full[PATH_MAX];
    const char *home = getenv("HOME");

    if (path[0] == '~' && (path[1] == '/' || path[1] == '\0') && home != NULL)
        snprintf(full, sizeof(full), "%s%s", home, path + 1);
    else if (path[0] == '/')
        snprintf(full, sizeof(full), "%s", path);
    else if (term->cwd != NULL)
        snprintf(full, sizeof(full), "%s/%s", term->cwd, path);
    else
        return true;    /* Can't tell */

    return access(full, F_OK) == 0;
}

bool
hints_show(struct terminal *term, enum hint hint)
{
    if (urls_mode_is_active(term))
        return false;

    const regex_t *preg = pattern_of(hint);
    if (preg == NULL)
        return false;

    const bool open = hint == HINT_OPEN_PATH;
    urls_collect(term, open ? URL_ACTION_LAUNCH : URL_ACTION_COPY, preg, false,
                 &term->url.list);

    /* What looks like a path often isn't one ("and/or", a date) */
    if (open) {
        tll_foreach(term->url.list, it) {
            if (!exists(term, it->item.url)) {
                free(it->item.url);
                tll_remove(term->url.list, it);
            }
        }
    }

    if (tll_length(term->url.list) == 0)
        return false;

    urls_assign_key_combos(term->conf, &term->url.list);
    urls_render(term, open ? &open_launch : &term->conf->url.launch);
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
