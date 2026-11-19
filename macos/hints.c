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
#include "grid.h"
#include "terminal.h"
#include "uri.h"
#include "url-mode.h"
#include "util.h"
#include "xmalloc.h"

/*
 * (In brackets, "-" comes last.) The first group is the hint (url-mode.c); what's before it is only
 * there to not begin in the middle of something (e.g. a URL's path).
 * macOS' enhanced syntax: \b, and groups that aren't counted.
 */
#define PATH_CHARS "[:alnum:]_.+@%-"

/* A path's name: "\ " is a space the shell doesn't end it at */
#define PATH_NAME "(?:[" PATH_CHARS "]|\\\\.)+"

/* A file's line (and column), as compilers have it after the path: a.c:12:5 */
#define PATH_LINE "(?::[0-9]+(?::[0-9]+)?)?"

/* What may be a file's name, to open it (hints_show_open(), hints_open_at()) */
enum { HINT_FILE = HINT_COUNT, HINT_QUOTED, PATTERN_COUNT };

static const char *const patterns[PATTERN_COUNT] = {
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

    /* Any name as well (Makefile, a folder as ls lists it): only those of files there are count */
    [HINT_FILE] =
        "(?:^|[^~:/" PATH_CHARS "])("
        "(?:~|\\.{1,2})?/?" PATH_NAME "(?:/" PATH_NAME ")*/?" PATH_LINE
        ")",

    /* In quotes, a name is anything; Python has the line as in: File "a.py", line 12 */
    [HINT_QUOTED] = "((?:\"[^\"]+\"|'[^']+')(?::[0-9]+(?::[0-9]+)?|, line [0-9]+)?)",
};

static regex_t compiled[PATTERN_COUNT];
static bool is_compiled[PATTERN_COUNT];

static const regex_t *
pattern_of(int hint)
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

/* The labels of what's in the terminal's list; false if there's nothing */
static bool
show(struct terminal *term)
{
    if (tll_length(term->url.list) == 0)
        return false;

    urls_assign_key_combos(term->conf, &term->url.list);
    urls_render(term, &term->conf->url.launch);
    return true;
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
    return show(term);
}

/*
 * Whether there's such a file: in the shell's directory, if the path
 * isn't absolute (not known: no file).
 */
static bool
file_at(const struct terminal *term, const char *path, char full[static PATH_MAX])
{
    const char *home = getenv("HOME");

    if (path[0] == '~' && (path[1] == '/' || path[1] == '\0') && home != NULL)
        snprintf(full, PATH_MAX, "%s%s", home, path + 1);
    else if (path[0] == '/')
        snprintf(full, PATH_MAX, "%s", path);
    else if (term->cwd != NULL && term->cwd[0] == '/')
        snprintf(full, PATH_MAX, "%s/%s", term->cwd, path);
    else
        return false;

    return access(full, F_OK) == 0;
}

/*
 * Where in a file, as its URL has it for its fragment: "12", or "12:5"
 * with the column (kitty's have the line). Returns the length of what's
 * one at 's'; 0: nothing.
 */
#define DIGITS "0123456789"
#define DIGITS_MAX 9

static size_t
place_at(const char *s, int *line, int *column)
{
    *line = *column = 0;

    const size_t line_len = strspn(s, DIGITS);
    if (line_len < 1 || line_len > DIGITS_MAX)
        return 0;
    *line = atoi(s);

    const char *col = s + line_len + 1;
    const size_t col_len = col[-1] == ':' ? strspn(col, DIGITS) : 0;
    if (col_len < 1 || col_len > DIGITS_MAX)
        return line_len;
    *column = atoi(col);
    return line_len + 1 + col_len;
}

/*
 * The path in a hint's text (free() it): what's in the quotes, or
 * without the "\" before a character. 'rest' is what follows it: where
 * in the file (":12:5", ", line 12"), or nothing.
 */
static char *
path_in(const char *text, const char **rest)
{
    if (text[0] == '"' || text[0] == '\'') {
        const char *end = strchr(text + 1, text[0]);
        xassert(end != NULL);
        *rest = end + 1;
        return xstrndup(text + 1, end - (text + 1));
    }

    char *path = xmalloc(strlen(text) + 1);
    char *out = path;
    for (; *text != '\0' && *text != ':'; text++) {
        if (text[0] == '\\' && text[1] != '\0')
            text++;
        *out++ = *text;
    }
    *out = '\0';
    *rest = text;
    return path;
}

/*
 * The file's URL, if there's such a file. What looks like a path often
 * isn't one ("and/or", a date, any word). With the place in the file
 * 'text' has after the path, for the URL's fragment.
 */
static char *
file_url(const struct terminal *term, const char *text)
{
    const char *place;
    char *path = path_in(text, &place);
    place += strcspn(place, DIGITS);

    char full[PATH_MAX];
    bool found = file_at(term, path, full);

    /* git's diffs: a/macos/view.m, b/macos/view.m */
    if (!found && (path[0] == 'a' || path[0] == 'b') && path[1] == '/')
        found = file_at(term, path + 2, full);

    /* A sentence's end: "see macos/view.m." */
    const size_t len = strlen(path);
    if (!found && len >= 2 && path[len - 1] == '.' && path[len - 2] != '.' &&
        path[len - 2] != '/')
    {
        path[len - 1] = '\0';
        found = file_at(term, path, full);
    }

    free(path);
    if (!found)
        return NULL;

    /* As a URL's path is read (uri.c): "%" begins an encoded character */
    int line, column;
    const int place_len = place_at(place, &line, &column);
    const size_t size = strlen("file://") + 3 * strlen(full) + 1 + place_len + 1;
    char *url = xmalloc(size);
    char *out = stpcpy(url, "file://");
    for (const char *p = full; *p != '\0'; p++) {
        if (*p == '%' || *p == '?' || *p == '#')
            out += snprintf(out, url + size - out, "%%%02X", (unsigned char)*p);
        else
            *out++ = *p;
    }
    *out = '\0';

    if (place_len > 0)
        snprintf(out, url + size - out, "#%.*s", place_len, place);
    return url;
}

bool
hints_open_line(struct terminal *term, const char *url)
{
    /* (A [regex:name]'s matches are for its own launcher) */
    if (term->url.launch != &term->conf->url.launch)
        return false;

    /* Not uri.c's fragment: all that's after a file's URL's host is its path there */
    const char *place = strrchr(url, '#');
    int line, column;
    if (place == NULL || place[1] == '\0' || place[1 + place_at(place + 1, &line, &column)] != '\0')
        return false;

    char *scheme, *host, *path;
    if (!uri_parse(url, place - url, &scheme, NULL, NULL, &host, NULL, &path, NULL, NULL))
        return false;

    /* (A hyperlink may be one to a file with "#12" in its name) */
    const bool opened =
        streq(scheme, "file") && hostname_is_localhost(host) && access(path, F_OK) == 0 &&
        wayl_open_in_editor(term->wl, path, line, column);

    free(scheme);
    free(host);
    free(path);
    return opened;
}

/* Where a hint is on the screen, as one number */
static long
position(const struct terminal *term, struct coord at)
{
    return (long)at.row * term->cols + at.col;
}

/* Whether a hint has cells of 'range' (NULL: any hint has) */
static bool
is_in(const struct terminal *term, const struct url *url, const struct range *range)
{
    return range == NULL || (position(term, url->range.start) <= position(term, range->end) &&
                             position(term, url->range.end) >= position(term, range->start));
}

/*
 * A name with spaces is several of HINT_FILE's: whether 'next' may go
 * on what 'name' begins, after one space (only a name's end has the
 * line)
 */
static bool
goes_on(const struct terminal *term, const struct url *name, const struct url *next)
{
    const long space = position(term, name->range.end) + 1;
    if (position(term, next->range.start) != space + 1 || strchr(name->url, ':') != NULL)
        return false;

    const struct row *row = grid_row_in_view(term->grid, space / term->cols - term->grid->view);
    const char32_t wc = row->cells[space % term->cols].wc;
    return wc == U' ' || wc == 0;
}

/* The words of a name with spaces that isn't in quotes, at most: each is looked for */
#define WORDS_MAX 4

/*
 * Adds those of 'names' (a pattern's matches, in the screen's order;
 * taken) there are files of, as their URLs. 'join': with spaces between
 * them, some may be one file's name; the longest there is is it.
 */
static void
add_files(const struct terminal *term, const struct range *at, url_list_t *names, bool join,
          url_list_t *found)
{
    const size_t count = tll_length(*names);
    struct url *all = xcalloc(count + 1, sizeof(all[0]));

    size_t n = 0;
    tll_foreach(*names, it) {
        all[n++] = it->item;
        tll_remove(*names, it);
    }

    for (size_t i = 0; i < count; i++) {
        size_t words = 1;
        while (join && words < WORDS_MAX && i + words < count &&
               goes_on(term, &all[i + words - 1], &all[i + words]))
        {
            words++;
        }

        for (; words >= 1; words--) {
            struct url file = all[i];
            file.range.end = all[i + words - 1].range.end;

            /* Not what's (in) a URL already, e.g. a hyperlink on a file's name */
            bool taken = !is_in(term, &file, at);
            tll_foreach(*found, it) {
                if (taken)
                    break;
                taken = is_in(term, &file, &it->item.range);
            }
            if (taken)
                continue;

            char *text = xstrdup(all[i].url);
            for (size_t w = 1; w < words; w++) {
                char *longer = xasprintf("%s %s", text, all[i + w].url);
                free(text);
                text = longer;
            }

            file.url = file_url(term, text);
            free(text);

            if (file.url != NULL) {
                tll_push_back(*found, file);
                i += words - 1;
                break;
            }
        }
    }

    for (size_t i = 0; i < count; i++)
        free(all[i].url);
    free(all);
}

/*
 * What can be opened: as foot's show-urls-launch, the URLs and the
 * hyperlinks (OSC 8), and the paths of files: first what's in quotes
 * (one name, whatever is in it), then the names in what's left. With
 * 'at', only what's there (and only there, it's looked for a file).
 */
static void
collect_open(const struct terminal *term, const struct range *at, url_list_t *found)
{
    /* (All of them first: urls_collect() removes what overlaps in a list) */
    static const int kinds[] = {HINT_QUOTED, HINT_FILE};
    url_list_t names[ALEN(kinds)] = {0};

    for (size_t i = 0; i < ALEN(kinds); i++) {
        const regex_t *preg = pattern_of(kinds[i]);
        if (preg != NULL)
            urls_collect(term, URL_ACTION_LAUNCH, preg, false, &names[i]);
    }

    urls_collect(term, URL_ACTION_LAUNCH, &term->conf->url.preg, true, found);

    for (size_t i = 0; i < ALEN(kinds); i++)
        add_files(term, at, &names[i], kinds[i] == HINT_FILE, found);

    tll_foreach(*found, it) {
        if (!is_in(term, &it->item, at)) {
            free(it->item.url);
            tll_remove(*found, it);
        }
    }
}

bool
hints_show_open(struct terminal *term)
{
    if (urls_mode_is_active(term))
        return false;

    /* (Not into the terminal's list: urls_collect() expects that it has none yet) */
    url_list_t found = tll_init();
    collect_open(term, NULL, &found);

    tll_foreach(found, it) {
        tll_push_back(term->url.list, it->item);
        tll_remove(found, it);
    }
    return show(term);
}

bool
hints_open_at(struct seat *seat, struct terminal *term, int row, int col, bool open,
              uint32_t serial)
{
    if (urls_mode_is_active(term) || row < 0 || col < 0)
        return false;

    const struct coord cell = {.col = col, .row = term->grid->view + row};
    const struct range at = {.start = cell, .end = cell};
    url_list_t found = tll_init();
    collect_open(term, &at, &found);

    const bool any = tll_length(found) > 0;
    tll_foreach(found, it) {
        /* OSC-8 URLs can be anything: not an option for the launcher */
        if (open && it->item.url[0] != '-') {
            urls_activate(seat, term, &it->item, serial);
            open = false;
        }

        free(it->item.url);
        tll_remove(found, it);
    }
    return any;
}

/* For the tests: all of 'text's hints, each followed by "|" */
static UNUSED char *
hints_in(int hint, const char *text)
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
expect(int hint, const char *text, const char *hints)
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

    expect(HINT_FILE, "Makefile  macos/  ../a.c:12:5: in ~/Work (see doc/foot.ini.)",
           "Makefile|macos/|../a.c:12:5|in|~/Work|see|doc/foot.ini.|");
    expect(HINT_FILE, "File \"/tmp/a.py\", line 12, in main; b.c:7: 12:30",
           "File|/tmp/a.py|line|12|in|main|b.c:7|12:30|");
    expect(HINT_FILE, "ls My\\ File.txt:3 a\\ b/c", "ls|My\\ File.txt:3|a\\ b/c|");
    expect(HINT_QUOTED, "File \"/tmp/a b.py\", line 12, in 'x (1)':3:4 \"z\"",
           "\"/tmp/a b.py\", line 12|'x (1)':3:4|\"z\"|");

    const char *rest;
    char *path = path_in("'a b':3", &rest);
    xassert(streq(path, "a b") && streq(rest, ":3"));
    free(path);
    path = path_in("a\\ b/c.d:12:5", &rest);
    xassert(streq(path, "a b/c.d") && streq(rest, ":12:5"));
    free(path);

    int line, column;
    xassert(place_at("12:5:", &line, &column) == 4 && line == 12 && column == 5);
    xassert(place_at("12, in", &line, &column) == 2 && line == 12 && column == 0);
    xassert(place_at("x", &line, &column) == 0 && line == 0);
    expect(HINT_FILE, "--- b/macos/view.m https://example.com/a/b.html", "---|b/macos/view.m|https|");

    expect(HINT_HASH, "commit 1710e40c8a (HEAD -> main) 46c3b37a: 123456 deadbeefg",
           "1710e40c8a|46c3b37a|");
    expect(HINT_IP, "inet 192.168.1.20 netmask 0xffffff00, 1.1.1.1:53, v1.2.3", "192.168.1.20|1.1.1.1|");
    expect(HINT_LINE, "   two words  ", "two words|");
    expect(HINT_LINE, "x", "x|");
    expect(HINT_WORD, " a  bc ", "a|bc|");
}
