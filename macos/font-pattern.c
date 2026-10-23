#include "font-pattern.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Returns a pointer to the first unescaped char in 'stops', or the terminating NUL */
static const char *
find_unescaped(const char *s, const char *stops)
{
    for (; *s != '\0'; s++) {
        if (*s == '\\') {
            if (s[1] == '\0')
                break;
            s++;
            continue;
        }

        if (strchr(stops, *s) != NULL)
            return s;
    }

    return s;
}

static char *
unescape(const char *s, size_t len)
{
    char *ret = malloc(len + 1);
    if (ret == NULL)
        return NULL;

    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\\' && i + 1 < len)
            i++;
        ret[j++] = s[i];
    }

    ret[j] = '\0';
    return ret;
}

static char *
copy(const char *s, size_t len)
{
    char *ret = malloc(len + 1);
    if (ret == NULL)
        return NULL;
    memcpy(ret, s, len);
    ret[len] = '\0';
    return ret;
}

bool
font_pattern_parse(const char *str, struct font_pattern *pat)
{
    *pat = (struct font_pattern){0};

    if (str == NULL)
        return false;

    /* Families, terminated by an unescaped '-' or ':' */
    const char *families_end = find_unescaped(str, "-:");

    if (families_end > str) {
        pat->family_raw = copy(str, families_end - str);

        const char *first_end = find_unescaped(str, ",-:");
        pat->family = unescape(str, first_end - str);

        if (pat->family_raw == NULL || pat->family == NULL)
            goto err;
    }

    const char *p = families_end;

    /* Optional size, "-12" */
    if (*p == '-') {
        p++;
        const char *size_end = find_unescaped(p, ":");
        pat->size_raw = copy(p, size_end - p);
        if (pat->size_raw == NULL)
            goto err;
        p = size_end;
    }

    /* key=value elements */
    while (*p == ':') {
        p++;

        const char *elem_end = find_unescaped(p, ":");
        if (elem_end == p)
            continue;

        const char *eq = find_unescaped(p, "=:");

        struct font_pattern_element elem = {
            .raw = copy(p, elem_end - p),
        };

        if (*eq == '=') {
            elem.key = unescape(p, eq - p);
            elem.value = unescape(eq + 1, elem_end - (eq + 1));
        } else
            elem.key = unescape(p, elem_end - p);

        struct font_pattern_element *new_elems = realloc(
            pat->elems, (pat->count + 1) * sizeof(pat->elems[0]));

        if (elem.raw == NULL || elem.key == NULL ||
            (*eq == '=' && elem.value == NULL) || new_elems == NULL)
        {
            free(elem.raw);
            free(elem.key);
            free(elem.value);
            if (new_elems != NULL)
                pat->elems = new_elems;
            goto err;
        }

        pat->elems = new_elems;
        pat->elems[pat->count++] = elem;
        p = elem_end;
    }

    return true;

err:
    font_pattern_free(pat);
    return false;
}

void
font_pattern_free(struct font_pattern *pat)
{
    free(pat->family);
    free(pat->family_raw);
    free(pat->size_raw);

    for (size_t i = 0; i < pat->count; i++) {
        free(pat->elems[i].key);
        free(pat->elems[i].value);
        free(pat->elems[i].raw);
    }

    free(pat->elems);
    *pat = (struct font_pattern){0};
}

const char *
font_pattern_get(const struct font_pattern *pat, const char *key)
{
    for (size_t i = pat->count; i > 0; i--) {
        const struct font_pattern_element *e = &pat->elems[i - 1];
        if (e->value != NULL && strcasecmp(e->key, key) == 0)
            return e->value;
    }

    if (strcasecmp(key, "size") == 0)
        return pat->size_raw;

    return NULL;
}

bool
font_pattern_has_constant(const struct font_pattern *pat, const char *name)
{
    for (size_t i = 0; i < pat->count; i++) {
        const struct font_pattern_element *e = &pat->elems[i];
        if (e->value == NULL && strcasecmp(e->key, name) == 0)
            return true;
    }
    return false;
}
