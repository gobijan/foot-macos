#pragma once

#include <stdbool.h>
#include <stddef.h>

/*
 * Parser for fontconfig style font names, the format foot's
 * configuration (and fcft's API) uses:
 *
 *   family[,family...][-size][:key=value[:key=value...]]
 *
 * '\' escapes the next character. Elements without a value
 * (e.g. ":bold") are fontconfig constants.
 */

struct font_pattern_element {
    char *key;    /* Unescaped */
    char *value;  /* Unescaped; NULL for constants */
    char *raw;    /* The element, exactly as written (still escaped) */
};

struct font_pattern {
    char *family;      /* First family, unescaped; NULL if none */
    char *family_raw;  /* All families, exactly as written */
    char *size_raw;    /* The "-size" part (without the dash), if any */

    size_t count;
    struct font_pattern_element *elems;
};

bool font_pattern_parse(const char *str, struct font_pattern *pat);
void font_pattern_free(struct font_pattern *pat);

/* Returns the (last) value for 'key', or NULL */
const char *font_pattern_get(const struct font_pattern *pat, const char *key);

/* Returns true if the constant 'name' is present (e.g. "bold") */
bool font_pattern_has_constant(const struct font_pattern *pat, const char *name);
