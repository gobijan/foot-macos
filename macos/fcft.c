/*
 * foot's font API (fcft), implemented natively on CoreText.
 *
 * Fonts are resolved and shaped by CoreText, and glyphs rasterized
 * with Core Graphics into pixman images: 8-bit alpha masks for
 * regular glyphs, pre-multiplied ARGB for color glyphs (emoji).
 *
 * Glyphs and graphemes are cached per font. The caches are shared by
 * foot's render worker threads, and protected by a rwlock.
 *
 * Glyphs are looked up for every rendered cell, by all render worker
 * threads at once, and a contended rwlock is expensive on macOS. Each
 * thread therefore has a small, lock-free cache of its own in front
 * of the shared one (as for pre-composed characters). Glyphs live as
 * long as their font, and font IDs are never re-used: an entry is
 * valid if its font ID matches the font's.
 */

#include <fcft/fcft.h>

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wchar.h>

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>

#if defined(FOOT_GRAPHEME_CLUSTERING)
 #include <utf8proc.h>
#endif

#define LOG_MODULE "fcft"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "font-pattern.h"

#define ALEN(v) (sizeof(v) / sizeof((v)[0]))

/* Used when the pattern has neither 'size' nor 'pixelsize' */
#define DEFAULT_PT_SIZE 13.
#define DEFAULT_DPI 72.

/* fontconfig weights and slants */
#define FC_WEIGHT_REGULAR 80
#define FC_WEIGHT_BOLD 200
#define FC_SLANT_ROMAN 0
#define FC_SLANT_ITALIC 100

struct face {
    CTFontRef font;
    bool is_color;      /* Font has color glyphs (e.g. emoji) */
    bool synth_bold;    /* Bold requested, but font has no bold face */
};

struct glyph_entry {
    uint32_t cp;
    struct fcft_glyph *glyph;
};

struct grapheme_entry {
    uint32_t hash;
    size_t len;
    uint32_t *cps;
    struct fcft_grapheme *grapheme;
    struct grapheme_entry *next;
};

struct font_priv {
    struct fcft_font public;  /* Must be first */
    atomic_int ref_count;

    struct face *faces;       /* [0] is the primary font, the rest custom fallbacks */
    size_t face_count;

    double pixel_size;
    bool want_bold;
    bool want_italic;
    bool antialias;
    bool smoothing;           /* Font smoothing (thickening) */
    double smoothing_strength;

    enum fcft_emoji_presentation emoji_presentation;
    pixman_format_code_t color_format;

    pthread_rwlock_t lock;
    uint64_t id;              /* Unique; keys the per-thread caches */

    struct {
        struct glyph_entry *table;
        size_t size;   /* Power of two */
        size_t count;
    } glyphs;

    struct {
        struct grapheme_entry **table;
        size_t size;   /* Power of two */
    } graphemes;
};

static CGColorSpaceRef srgb = NULL;

bool
fcft_init(enum fcft_log_colorize colorize, bool do_syslog,
          enum fcft_log_class log_level)
{
    /* foot's own logging is used */
    if (srgb == NULL)
        srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    return srgb != NULL;
}

void
fcft_fini(void)
{
    if (srgb != NULL) {
        CGColorSpaceRelease(srgb);
        srgb = NULL;
    }
}

enum fcft_capabilities
fcft_capabilities(void)
{
    return FCFT_CAPABILITY_GRAPHEME_SHAPING | FCFT_CAPABILITY_TEXT_RUN_SHAPING;
}

struct fcft_font_options *
fcft_font_options_create(void)
{
    struct fcft_font_options *options = calloc(1, sizeof(*options));
    if (options == NULL)
        return NULL;

    options->emoji_presentation = FCFT_EMOJI_PRESENTATION_DEFAULT;
    options->color_glyphs.format = PIXMAN_a8r8g8b8;
    options->scaling_filter = FCFT_SCALING_FILTER_CUBIC;
    options->smoothing.enabled = true;
    options->smoothing.strength = 1.;
    return options;
}

void
fcft_font_options_destroy(struct fcft_font_options *options)
{
    free(options);
}

bool
fcft_set_scaling_filter(enum fcft_scaling_filter filter)
{
    /* CoreText scales outlines (and color bitmaps) itself */
    return true;
}

void
fcft_set_emoji_presentation(struct fcft_font *_font,
                            enum fcft_emoji_presentation presentation)
{
    struct font_priv *font = (struct font_priv *)_font;
    font->emoji_presentation = presentation;
}

/*
 * Strings
 */

static size_t
utf16_encode(uint32_t cp, UniChar out[static 2])
{
    if (cp >= 0x10000) {
        cp -= 0x10000;
        out[0] = 0xd800 + (cp >> 10);
        out[1] = 0xdc00 + (cp & 0x3ff);
        return 2;
    }

    out[0] = cp;
    return 1;
}

static CFStringRef
cfstring_from_utf32(size_t len, const uint32_t text[static len])
{
    UniChar utf16[len * 2 + 1];
    size_t count = 0;

    for (size_t i = 0; i < len; i++)
        count += utf16_encode(text[i], &utf16[count]);

    return CFStringCreateWithCharacters(NULL, utf16, count);
}

static CFStringRef
cfstring_from_utf8(const char *s)
{
    return CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8);
}

static char *
utf8_from_cfstring(CFStringRef s)
{
    if (s == NULL)
        return NULL;

    CFIndex len = CFStringGetMaximumSizeForEncoding(
        CFStringGetLength(s), kCFStringEncodingUTF8) + 1;
    char *ret = malloc(len);
    if (ret == NULL)
        return NULL;

    if (!CFStringGetCString(s, ret, len, kCFStringEncodingUTF8)) {
        free(ret);
        return NULL;
    }
    return ret;
}

/* Maps a UTF-16 string index (from CoreText) back to a UTF-32 index */
static size_t
utf32_index(size_t len, const uint32_t text[static len], CFIndex utf16_idx)
{
    CFIndex units = 0;
    for (size_t i = 0; i < len; i++) {
        if (units >= utf16_idx)
            return i;
        units += text[i] >= 0x10000 ? 2 : 1;
    }
    return len;
}

/*
 * Font resolution
 */

static const char *
pattern_str(const struct font_pattern *pat, const struct font_pattern *attrs,
            const char *key)
{
    const char *v = font_pattern_get(pat, key);
    return v != NULL ? v : font_pattern_get(attrs, key);
}

static int
pattern_int(const struct font_pattern *pat, const struct font_pattern *attrs,
            const char *key, int default_value)
{
    const char *v = pattern_str(pat, attrs, key);
    if (v == NULL)
        return default_value;

    char *end;
    long value = strtol(v, &end, 10);
    if (*end == '\0')
        return value;

    /* Named fontconfig constants */
    static const struct { const char *name; int value; } constants[] = {
        {"thin", 0}, {"extralight", 40}, {"ultralight", 40},
        {"light", 50}, {"demilight", 55}, {"semilight", 55},
        {"book", 75}, {"regular", 80}, {"normal", 80}, {"medium", 100},
        {"demibold", 180}, {"semibold", 180}, {"bold", 200},
        {"extrabold", 205}, {"ultrabold", 205}, {"black", 210},
        {"heavy", 210}, {"extrablack", 215}, {"ultrablack", 215},
        {"roman", 0}, {"italic", 100}, {"oblique", 110},
        {"true", 1}, {"yes", 1}, {"false", 0}, {"no", 0},
    };

    for (size_t i = 0; i < ALEN(constants); i++) {
        if (strcasecmp(v, constants[i].name) == 0)
            return constants[i].value;
    }

    return default_value;
}

static double
pattern_double(const struct font_pattern *pat, const struct font_pattern *attrs,
               const char *key, double default_value)
{
    const char *v = pattern_str(pat, attrs, key);
    return v != NULL ? strtod(v, NULL) : default_value;
}

static bool
pattern_constant(const struct font_pattern *pat, const struct font_pattern *attrs,
                 const char *name)
{
    return font_pattern_has_constant(pat, name) ||
        font_pattern_has_constant(attrs, name);
}

/* Maps fontconfig's generic family names to macOS fonts */
static CFStringRef
family_for_name(const char *family)
{
    if (family == NULL ||
        strcasecmp(family, "monospace") == 0 ||
        strcasecmp(family, "mono") == 0)
    {
        /* The system's monospaced font, if installed */
        CTFontRef sf_mono = CTFontCreateWithName(CFSTR("SF Mono"), 12., NULL);
        CFStringRef name = CTFontCopyFamilyName(sf_mono);
        CFRelease(sf_mono);

        if (name != NULL &&
            CFStringCompare(name, CFSTR("SF Mono"), 0) == kCFCompareEqualTo)
        {
            return name;
        }

        if (name != NULL)
            CFRelease(name);
        return CFRetain(CFSTR("Menlo"));
    }

    if (strcasecmp(family, "sans-serif") == 0 || strcasecmp(family, "sans") == 0)
        return CFRetain(CFSTR("Helvetica Neue"));
    if (strcasecmp(family, "serif") == 0)
        return CFRetain(CFSTR("Times New Roman"));
    if (strcasecmp(family, "emoji") == 0)
        return CFRetain(CFSTR("Apple Color Emoji"));

    return cfstring_from_utf8(family);
}

/* fontconfig weight -> CoreText weight trait (-1.0 .. 1.0) */
static double
ct_weight(int fc_weight)
{
    static const struct { int fc; double ct; } map[] = {
        {0, -0.8}, {40, -0.6}, {50, -0.4}, {55, -0.3}, {75, -0.1},
        {80, 0.0}, {100, 0.23}, {180, 0.3}, {200, 0.4}, {205, 0.56},
        {210, 0.62}, {215, 0.7},
    };

    if (fc_weight <= map[0].fc)
        return map[0].ct;

    for (size_t i = 1; i < ALEN(map); i++) {
        if (fc_weight <= map[i].fc) {
            const double t = (double)(fc_weight - map[i - 1].fc) /
                (map[i].fc - map[i - 1].fc);
            return map[i - 1].ct + t * (map[i].ct - map[i - 1].ct);
        }
    }
    return map[ALEN(map) - 1].ct;
}

/* "ss01,zero,-calt,cv01=2" -> CoreText OpenType feature settings */
static CFArrayRef
parse_features(const char *features)
{
    if (features == NULL || features[0] == '\0')
        return NULL;

    CFMutableArrayRef settings = CFArrayCreateMutable(
        NULL, 0, &kCFTypeArrayCallBacks);

    char *copy = strdup(features);
    for (char *save = NULL, *f = strtok_r(copy, ", ", &save);
         f != NULL;
         f = strtok_r(NULL, ", ", &save))
    {
        int value = 1;
        if (f[0] == '-') {
            value = 0;
            f++;
        } else if (f[0] == '+')
            f++;

        char *eq = strchr(f, '=');
        if (eq != NULL) {
            *eq = '\0';
            value = atoi(eq + 1);
        }

        if (strlen(f) != 4) {
            LOG_WARN("%s: invalid OpenType feature tag", f);
            continue;
        }

        CFStringRef tag = cfstring_from_utf8(f);
        CFNumberRef num = CFNumberCreate(NULL, kCFNumberIntType, &value);

        const void *keys[] = {kCTFontOpenTypeFeatureTag, kCTFontOpenTypeFeatureValue};
        const void *values[] = {tag, num};
        CFDictionaryRef setting = CFDictionaryCreate(
            NULL, keys, values, 2,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

        CFArrayAppendValue(settings, setting);
        CFRelease(setting);
        CFRelease(num);
        CFRelease(tag);
    }
    free(copy);

    if (CFArrayGetCount(settings) == 0) {
        CFRelease(settings);
        return NULL;
    }
    return settings;
}

static bool
cfstr_equal(CFStringRef a, CFStringRef b)
{
    return a != NULL && b != NULL &&
        CFStringCompare(a, b, kCFCompareCaseInsensitive) == kCFCompareEqualTo;
}

/* Does 'font' match the requested name (family, PostScript or full name)? */
static bool
font_matches(CTFontRef font, CFStringRef name)
{
    CFStringRef family = CTFontCopyFamilyName(font);
    CFStringRef ps = CTFontCopyPostScriptName(font);
    CFStringRef full = CTFontCopyFullName(font);

    bool ret = cfstr_equal(family, name) || cfstr_equal(ps, name) ||
        cfstr_equal(full, name);

    if (family != NULL) CFRelease(family);
    if (ps != NULL) CFRelease(ps);
    if (full != NULL) CFRelease(full);
    return ret;
}

static CTFontRef
copy_with_features(CTFontRef font, CFArrayRef features)
{
    if (features == NULL)
        return font;

    CFDictionaryRef attrs = CFDictionaryCreate(
        NULL, (const void *[]){kCTFontFeatureSettingsAttribute},
        (const void *[]){features}, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CTFontDescriptorRef desc = CTFontDescriptorCreateWithAttributes(attrs);
    CTFontRef ret = CTFontCreateCopyWithAttributes(font, 0., NULL, desc);
    CFRelease(desc);
    CFRelease(attrs);

    if (ret == NULL)
        return font;

    CFRelease(font);
    return ret;
}

static CTFontRef
create_font(CFStringRef name, const char *style, double pixel_size,
            int weight, int slant, CFArrayRef features,
            bool *synth_bold, bool *found)
{
    CFMutableDictionaryRef attrs = CFDictionaryCreateMutable(
        NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

    CFDictionarySetValue(attrs, kCTFontFamilyNameAttribute, name);

    if (style != NULL) {
        CFStringRef s = cfstring_from_utf8(style);
        CFDictionarySetValue(attrs, kCTFontStyleNameAttribute, s);
        CFRelease(s);
    }

    CTFontDescriptorRef desc = CTFontDescriptorCreateWithAttributes(attrs);
    CFRelease(attrs);

    CTFontRef font = CTFontCreateWithFontDescriptor(desc, pixel_size, NULL);
    CFRelease(desc);

    if (!font_matches(font, name)) {
        /* Not a family name; try as a PostScript, or full name */
        CTFontRef by_name = CTFontCreateWithName(name, pixel_size, NULL);
        if (font_matches(by_name, name)) {
            CFRelease(font);
            font = by_name;
        } else
            CFRelease(by_name);
    }

    *found = font_matches(font, name);
    *synth_bold = false;

    /* Bold/italic: prefer real faces, fall back to synthesized ones */
    CTFontSymbolicTraits want = 0;
    if (weight >= FC_WEIGHT_BOLD)
        want |= kCTFontBoldTrait;
    if (slant >= FC_SLANT_ITALIC)
        want |= kCTFontItalicTrait;

    if (style == NULL && want != 0) {
        CTFontRef styled = CTFontCreateCopyWithSymbolicTraits(
            font, pixel_size, NULL, want, want);

        if (styled == NULL && (want & kCTFontItalicTrait)) {
            styled = CTFontCreateCopyWithSymbolicTraits(
                font, pixel_size, NULL, kCTFontItalicTrait, kCTFontItalicTrait);
        }

        if (styled != NULL) {
            CFRelease(font);
            font = styled;
        }

        const CTFontSymbolicTraits have = CTFontGetSymbolicTraits(font);

        if ((want & kCTFontBoldTrait) && !(have & kCTFontBoldTrait))
            *synth_bold = true;

        if ((want & kCTFontItalicTrait) && !(have & kCTFontItalicTrait)) {
            const CGAffineTransform skew = {1, 0, tan(12. * M_PI / 180.), 1, 0, 0};
            CTFontDescriptorRef fdesc = CTFontCopyFontDescriptor(font);
            CTFontRef sheared = CTFontCreateWithFontDescriptor(fdesc, pixel_size, &skew);
            CFRelease(fdesc);
            CFRelease(font);
            font = sheared;
        }
    }

    else if (style == NULL && weight != FC_WEIGHT_REGULAR) {
        /* Other weights (light, medium etc) */
        CFNumberRef w = CFNumberCreate(
            NULL, kCFNumberDoubleType, &(double){ct_weight(weight)});
        CFDictionaryRef traits = CFDictionaryCreate(
            NULL, (const void *[]){kCTFontWeightTrait}, (const void *[]){w}, 1,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFStringRef family = CTFontCopyFamilyName(font);
        CFDictionaryRef wattrs = CFDictionaryCreate(
            NULL,
            (const void *[]){kCTFontFamilyNameAttribute, kCTFontTraitsAttribute},
            (const void *[]){family, traits}, 2,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

        CTFontDescriptorRef wdesc = CTFontDescriptorCreateWithAttributes(wattrs);
        CFSetRef mandatory = CFSetCreate(
            NULL, (const void *[]){kCTFontFamilyNameAttribute}, 1, &kCFTypeSetCallBacks);
        CTFontDescriptorRef match = CTFontDescriptorCreateMatchingFontDescriptor(
            wdesc, mandatory);

        if (match != NULL) {
            CTFontRef weighted = CTFontCreateWithFontDescriptor(match, pixel_size, NULL);
            if (weighted != NULL) {
                CFRelease(font);
                font = weighted;
            }
            CFRelease(match);
        }

        CFRelease(mandatory);
        CFRelease(wdesc);
        CFRelease(wattrs);
        CFRelease(family);
        CFRelease(traits);
        CFRelease(w);
    }

    return copy_with_features(font, features);
}

static bool
font_is_color(CTFontRef font)
{
    return (CTFontGetSymbolicTraits(font) & kCTFontColorGlyphsTrait) != 0;
}

/* Reads strikeout metrics from the OS/2 table, in pixels */
static bool
strikeout_metrics(CTFontRef font, double *position, double *thickness)
{
    CFDataRef os2 = CTFontCopyTable(font, kCTFontTableOS2, kCTFontTableOptionNoOptions);
    if (os2 == NULL)
        return false;

    bool ret = false;
    const UInt8 *data = CFDataGetBytePtr(os2);

    if (CFDataGetLength(os2) >= 30) {
        const int16_t size = (int16_t)(data[26] << 8 | data[27]);
        const int16_t pos = (int16_t)(data[28] << 8 | data[29]);
        const double scale = CTFontGetSize(font) / CTFontGetUnitsPerEm(font);

        if (pos != 0 && size != 0) {
            *position = pos * scale;
            *thickness = size * scale;
            ret = true;
        }
    }

    CFRelease(os2);
    return ret;
}

static void
calculate_metrics(struct font_priv *font)
{
    CTFontRef ct = font->faces[0].font;
    struct fcft_font *m = &font->public;

    const double ascent = CTFontGetAscent(ct);
    const double descent = CTFontGetDescent(ct);
    const double leading = CTFontGetLeading(ct);

    m->ascent = ceil(ascent);
    m->descent = ceil(descent);
    m->height = ceil(ascent + descent + leading);

    /* Widest advance; for a monospaced font, the advance of any glyph */
    CGGlyph glyph;
    UniChar M = 'M';
    CGSize advance = {0};
    if (CTFontGetGlyphsForCharacters(ct, &M, &glyph, 1))
        CTFontGetAdvancesForGlyphs(ct, kCTFontOrientationHorizontal, &glyph, &advance, 1);

    const CGRect bbox = CTFontGetBoundingBox(ct);
    m->max_advance.x = (CTFontGetSymbolicTraits(ct) & kCTFontMonoSpaceTrait)
        ? lround(advance.width)
        : ceil(bbox.size.width);
    m->max_advance.y = m->height;

    double underline_position = CTFontGetUnderlinePosition(ct);
    double underline_thickness = CTFontGetUnderlineThickness(ct);

    if (underline_position == 0.) {
        underline_thickness = descent / 5.;
        underline_position = -2 * underline_thickness;
    }

    /* Position is the top of the line, relative to the baseline */
    m->underline.position = trunc(underline_position + underline_thickness / 2.);
    m->underline.thickness = round(fmax(1., underline_thickness));

    double strikeout_position, strikeout_thickness;
    if (!strikeout_metrics(ct, &strikeout_position, &strikeout_thickness)) {
        strikeout_thickness = underline_thickness;
        strikeout_position = 3. * ascent / 8. - underline_thickness / 2.;
    }

    m->strikeout.position = trunc(strikeout_position + strikeout_thickness / 2.);
    m->strikeout.thickness = round(fmax(1., strikeout_thickness));

    m->antialias = font->antialias;
    m->subpixel = FCFT_SUBPIXEL_NONE;  /* macOS has no LCD anti-aliasing */
}

struct fcft_font *
fcft_from_name2(size_t count, const char *names[static count],
                const char *attributes, const struct fcft_font_options *options)
{
    if (count == 0)
        return NULL;

    /* Attributes apply to all names. Names' own values take precedence */
    struct font_pattern attrs;
    {
        size_t len = attributes != NULL ? strlen(attributes) : 0;
        char with_colon[len + 2];
        with_colon[0] = ':';
        memcpy(&with_colon[1], attributes != NULL ? attributes : "", len + 1);

        if (!font_pattern_parse(with_colon, &attrs)) {
            LOG_ERR("%s: failed to parse font attributes", attributes);
            return NULL;
        }
    }

    struct font_priv *font = calloc(1, sizeof(*font));
    if (font == NULL) {
        font_pattern_free(&attrs);
        return NULL;
    }

    atomic_init(&font->ref_count, 1);
    font->faces = calloc(count, sizeof(font->faces[0]));
    font->emoji_presentation = options != NULL
        ? options->emoji_presentation : FCFT_EMOJI_PRESENTATION_DEFAULT;
    font->color_format = PIXMAN_a8r8g8b8;
    font->smoothing = options != NULL ? options->smoothing.enabled : true;
    font->smoothing_strength = options != NULL ? options->smoothing.strength : 1.;
    pthread_rwlock_init(&font->lock, NULL);

    static atomic_uint_fast64_t next_id = 1;
    font->id = atomic_fetch_add(&next_id, 1);

    for (size_t i = 0; i < count; i++) {
        struct font_pattern pat;
        if (!font_pattern_parse(names[i], &pat)) {
            LOG_ERR("%s: failed to parse font name", names[i]);
            continue;
        }

        const double dpi = pattern_double(&pat, &attrs, "dpi", DEFAULT_DPI);
        double pixel_size = pattern_double(&pat, &attrs, "pixelsize", -1.);
        if (pixel_size <= 0.) {
            const double pt_size = pattern_double(&pat, &attrs, "size", DEFAULT_PT_SIZE);
            pixel_size = pt_size * dpi / 72.;
        }

        int weight = pattern_int(&pat, &attrs, "weight", FC_WEIGHT_REGULAR);
        int slant = pattern_int(&pat, &attrs, "slant", FC_SLANT_ROMAN);

        if (pattern_constant(&pat, &attrs, "bold"))
            weight = FC_WEIGHT_BOLD;
        if (pattern_constant(&pat, &attrs, "italic"))
            slant = FC_SLANT_ITALIC;

        const char *style = pattern_str(&pat, &attrs, "style");
        CFArrayRef features = parse_features(pattern_str(&pat, &attrs, "fontfeatures"));
        CFStringRef family = family_for_name(pat.family);

        bool found = false;
        struct face *face = &font->faces[font->face_count];
        face->font = create_font(
            family, style, pixel_size, weight, slant, features,
            &face->synth_bold, &found);

        if (!found) {
            LOG_WARN("%s: font not found%s", names[i],
                     i == 0 ? ", using the system's monospace font" : "");

            CFRelease(face->font);
            face->font = NULL;

            if (i == 0) {
                CFRelease(family);
                family = family_for_name("monospace");
                face->font = create_font(
                    family, NULL, pixel_size, weight, slant, features,
                    &face->synth_bold, &found);
            }
        }

        if (face->font != NULL) {
            face->is_color = font_is_color(face->font);

            if (font->face_count == 0) {
                font->pixel_size = pixel_size;
                font->want_bold = weight >= FC_WEIGHT_BOLD;
                font->want_italic = slant >= FC_SLANT_ITALIC;
                font->antialias = pattern_int(&pat, &attrs, "antialias", 1) != 0;

                CFStringRef name = CTFontCopyFullName(face->font);
                font->public.name = utf8_from_cfstring(name);
                if (name != NULL)
                    CFRelease(name);

                LOG_INFO("%s: size=%.2fpx%s",
                         font->public.name != NULL ? font->public.name : names[i],
                         pixel_size, face->synth_bold ? " (synthesized bold)" : "");
            }

            font->face_count++;
        }

        if (features != NULL)
            CFRelease(features);
        CFRelease(family);
        font_pattern_free(&pat);
    }

    font_pattern_free(&attrs);

    if (font->face_count == 0) {
        fcft_destroy(&font->public);
        return NULL;
    }

    calculate_metrics(font);
    return &font->public;
}

struct fcft_font *
fcft_from_name(size_t count, const char *names[static count],
               const char *attributes)
{
    return fcft_from_name2(count, names, attributes, NULL);
}

struct fcft_font *
fcft_clone(const struct fcft_font *_font)
{
    if (_font == NULL)
        return NULL;

    struct font_priv *font = (struct font_priv *)_font;
    atomic_fetch_add(&font->ref_count, 1);
    return &font->public;
}

static void
glyph_destroy(struct fcft_glyph *glyph)
{
    if (glyph == NULL)
        return;

    if (glyph->pix != NULL)
        pixman_image_unref(glyph->pix);
    free((char *)glyph->font_name);
    free(glyph);
}

static void
grapheme_destroy(struct fcft_grapheme *grapheme)
{
    if (grapheme == NULL)
        return;

    for (size_t i = 0; i < grapheme->count; i++)
        glyph_destroy((struct fcft_glyph *)grapheme->glyphs[i]);
    free(grapheme->glyphs);
    free(grapheme);
}

void
fcft_destroy(struct fcft_font *_font)
{
    if (_font == NULL)
        return;

    struct font_priv *font = (struct font_priv *)_font;
    if (atomic_fetch_sub(&font->ref_count, 1) > 1)
        return;

    for (size_t i = 0; i < font->glyphs.size; i++)
        glyph_destroy(font->glyphs.table[i].glyph);
    free(font->glyphs.table);

    for (size_t i = 0; i < font->graphemes.size; i++) {
        for (struct grapheme_entry *e = font->graphemes.table[i], *next;
             e != NULL; e = next)
        {
            next = e->next;
            grapheme_destroy(e->grapheme);
            free(e->cps);
            free(e);
        }
    }
    free(font->graphemes.table);

    for (size_t i = 0; i < font->face_count; i++)
        CFRelease(font->faces[i].font);
    free(font->faces);

    pthread_rwlock_destroy(&font->lock);
    free((char *)font->public.name);
    free(font);
}

/*
 * Rasterization
 */

static void
free_pixel_data(pixman_image_t *image, void *data)
{
    free(data);
}

/*
 * Allocates a pixel buffer, and a bitmap context covering 'bounds'
 * (in glyph space, y up) plus some padding. The context's origin is
 * at the pen position, on the baseline.
 */
static CGContextRef
create_glyph_context(const struct font_priv *font, CGRect bounds, bool color,
                     double extra_pad, struct fcft_glyph *glyph, void **data)
{
    const double pad = 1. + extra_pad;

    int x0, y0, x1, y1;
    if (CGRectIsNull(bounds) || CGRectIsEmpty(bounds)) {
        x0 = y0 = 0;
        x1 = y1 = 1;
    } else {
        x0 = floor(bounds.origin.x - pad);
        y0 = floor(bounds.origin.y - pad);
        x1 = ceil(bounds.origin.x + bounds.size.width + pad);
        y1 = ceil(bounds.origin.y + bounds.size.height + pad);
    }

    const int width = x1 - x0;
    const int height = y1 - y0;
    const int bpp = color ? 4 : 1;
    const int stride = ((width * bpp) + 3) & ~3;  /* pixman: 32-bit aligned rows */

    *data = calloc(1, (size_t)stride * height);
    if (*data == NULL)
        return NULL;

    CGContextRef ctx = color
        ? CGBitmapContextCreate(
            *data, width, height, 8, stride, srgb,
            kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little)
        : CGBitmapContextCreate(
            *data, width, height, 8, stride, NULL, kCGImageAlphaOnly);

    if (ctx == NULL) {
        free(*data);
        *data = NULL;
        return NULL;
    }

    CGContextSetAllowsAntialiasing(ctx, font->antialias);
    CGContextSetShouldAntialias(ctx, font->antialias);
    /*
     * Font smoothing dilates the glyphs, like macOS' own text rendering
     * (and kitty's, and Ghostty's 'font-thicken'). Without it, light
     * text on dark backgrounds looks thin and faint on non-Retina
     * displays. In an alpha-only context, the fill color doesn't color
     * anything, but its brightness sets how much glyphs are dilated.
     */
    CGContextSetAllowsFontSmoothing(ctx, font->smoothing);
    CGContextSetShouldSmoothFonts(ctx, font->smoothing);
    CGContextSetAllowsFontSubpixelPositioning(ctx, false);
    CGContextSetShouldSubpixelPositionFonts(ctx, false);
    CGContextSetAllowsFontSubpixelQuantization(ctx, false);
    CGContextSetShouldSubpixelQuantizeFonts(ctx, false);

    if (!color)
        CGContextSetGrayFillColor(ctx, font->smoothing ? font->smoothing_strength : 1., 1.);

    CGContextTranslateCTM(ctx, -x0, -y0);

    glyph->x = x0;
    glyph->y = y1;
    glyph->width = width;
    glyph->height = height;
    glyph->is_color_glyph = color;
    return ctx;
}

static bool
finish_glyph(const struct font_priv *font, CGContextRef ctx, void *data,
             struct fcft_glyph *glyph)
{
    const size_t stride = CGBitmapContextGetBytesPerRow(ctx);
    CGContextRelease(ctx);

    glyph->pix = pixman_image_create_bits_no_clear(
        glyph->is_color_glyph ? font->color_format : PIXMAN_a8,
        glyph->width, glyph->height, data, stride);

    if (glyph->pix == NULL) {
        free(data);
        return false;
    }

    pixman_image_set_destroy_function(glyph->pix, &free_pixel_data, data);
    return true;
}

/* Rasterizes a single glyph */
static struct fcft_glyph *
rasterize_glyph(const struct font_priv *font, const struct face *face,
                CTFontRef ct, CGGlyph glyph_id)
{
    struct fcft_glyph *glyph = calloc(1, sizeof(*glyph));
    if (glyph == NULL)
        return NULL;

    CGRect bounds;
    CTFontGetBoundingRectsForGlyphs(ct, kCTFontOrientationHorizontal, &glyph_id, &bounds, 1);

    CGSize advance;
    CTFontGetAdvancesForGlyphs(ct, kCTFontOrientationHorizontal, &glyph_id, &advance, 1);

    const bool color = font_is_color(ct);
    const bool synth_bold = face->synth_bold && !color;
    const double stroke = synth_bold ? fmax(1., font->pixel_size / 24.) : 0.;

    void *data;
    CGContextRef ctx = create_glyph_context(font, bounds, color, stroke, glyph, &data);
    if (ctx == NULL) {
        free(glyph);
        return NULL;
    }

    if (synth_bold) {
        CGContextSetTextDrawingMode(ctx, kCGTextFillStroke);
        CGContextSetLineWidth(ctx, stroke);
        CGContextSetGrayStrokeColor(ctx, font->smoothing ? font->smoothing_strength : 1., 1.);
    }

    CTFontDrawGlyphs(ct, &glyph_id, &CGPointZero, 1, ctx);

    if (!finish_glyph(font, ctx, data, glyph)) {
        free(glyph);
        return NULL;
    }

    glyph->advance.x = lround(advance.width);
    glyph->advance.y = 0;

    CFStringRef name = CTFontCopyFullName(ct);
    glyph->font_name = utf8_from_cfstring(name);
    if (name != NULL)
        CFRelease(name);

    return glyph;
}

static int
codepoint_width(uint32_t cp)
{
#if defined(FOOT_GRAPHEME_CLUSTERING)
    return utf8proc_charwidth(cp);
#else
    int w = wcwidth(cp);
    return w < 0 ? 1 : w;
#endif
}

/*
 * Icon fonts (e.g. Nerd Fonts) put their glyphs in Unicode's private
 * use areas. They aren't part of the system's font cascade, so we look
 * for installed ones ourselves. Preferred: "Symbols Nerd Font Mono",
 * then other monospaced Nerd Fonts, then the rest.
 */
static pthread_once_t symbol_families_once = PTHREAD_ONCE_INIT;
static CFArrayRef symbol_families = NULL;

static bool
cfstr_contains(CFStringRef str, CFStringRef needle)
{
    return CFStringFind(str, needle, kCFCompareCaseInsensitive).location != kCFNotFound;
}

static void
find_symbol_families(void)
{
    CFArrayRef all = CTFontManagerCopyAvailableFontFamilyNames();
    CFMutableArrayRef symbols = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    CFMutableArrayRef mono = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    CFMutableArrayRef other = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);

    for (CFIndex i = 0; all != NULL && i < CFArrayGetCount(all); i++) {
        CFStringRef family = CFArrayGetValueAtIndex(all, i);

        if (!cfstr_contains(family, CFSTR("Nerd Font")))
            continue;

        if (cfstr_contains(family, CFSTR("Symbols")))
            CFArrayAppendValue(symbols, family);
        else if (CFStringHasSuffix(family, CFSTR("Mono")))
            CFArrayAppendValue(mono, family);
        else if (!cfstr_contains(family, CFSTR("Propo")))
            CFArrayAppendValue(other, family);
    }

    CFArrayAppendArray(symbols, mono, CFRangeMake(0, CFArrayGetCount(mono)));
    CFArrayAppendArray(symbols, other, CFRangeMake(0, CFArrayGetCount(other)));
    symbol_families = symbols;

    CFRelease(mono);
    CFRelease(other);
    if (all != NULL)
        CFRelease(all);
}

static bool
is_private_use(uint32_t cp)
{
    return (cp >= 0xe000 && cp <= 0xf8ff) ||
        (cp >= 0xf0000 && cp <= 0xffffd) ||
        (cp >= 0x100000 && cp <= 0x10fffd);
}

/* Returns a retained font from an installed icon font, or NULL */
static CTFontRef
symbol_font_for(const struct font_priv *font, const UniChar utf16[static 2],
                CFIndex count, CGGlyph *glyph)
{
    pthread_once(&symbol_families_once, &find_symbol_families);

    for (CFIndex i = 0; i < CFArrayGetCount(symbol_families); i++) {
        CTFontRef ct = CTFontCreateWithName(
            CFArrayGetValueAtIndex(symbol_families, i), font->pixel_size, NULL);

        CGGlyph glyphs[2] = {0};
        if (CTFontGetGlyphsForCharacters(ct, utf16, glyphs, count) && glyphs[0] != 0) {
            *glyph = glyphs[0];
            return ct;
        }
        CFRelease(ct);
    }

    return NULL;
}

/*
 * Finds a font with a glyph for 'cp': the primary font, then the
 * custom fallbacks, and finally the system's font cascade.
 *
 * Returns a retained CTFont.
 */
static CTFontRef
font_for_codepoint(const struct font_priv *font, uint32_t cp,
                   const struct face **face, CGGlyph *glyph)
{
    UniChar utf16[2];
    const CFIndex count = utf16_encode(cp, utf16);

    const bool prefer_emoji =
        font->emoji_presentation == FCFT_EMOJI_PRESENTATION_EMOJI;

    for (size_t i = 0; i < font->face_count; i++) {
        const struct face *f = &font->faces[i];

        if (prefer_emoji && !f->is_color)
            continue;

        CGGlyph glyphs[2] = {0};
        if (CTFontGetGlyphsForCharacters(f->font, utf16, glyphs, count) &&
            glyphs[0] != 0)
        {
            *face = f;
            *glyph = glyphs[0];
            return CFRetain(f->font);
        }
    }

    *face = &font->faces[0];

    if (is_private_use(cp)) {
        CTFontRef symbol = symbol_font_for(font, utf16, count, glyph);
        if (symbol != NULL)
            return symbol;
    }

    /* The system's font cascade */
    CFStringRef str = CFStringCreateWithCharacters(NULL, utf16, count);
    CTFontRef fallback = CTFontCreateForString(
        font->faces[0].font, str, CFRangeMake(0, count));
    CFRelease(str);

    *face = &font->faces[0];

    if (fallback == NULL) {
        *glyph = 0;
        return CFRetain(font->faces[0].font);
    }

    /* Apply the requested style to the fallback font */
    if (!font_is_color(fallback) && (font->want_bold || font->want_italic)) {
        const CTFontSymbolicTraits want =
            (font->want_bold ? kCTFontBoldTrait : 0) |
            (font->want_italic ? kCTFontItalicTrait : 0);
        CTFontRef styled = CTFontCreateCopyWithSymbolicTraits(
            fallback, 0., NULL, want, want);
        if (styled != NULL) {
            CFRelease(fallback);
            fallback = styled;
        }
    }

    CGGlyph glyphs[2] = {0};
    if (!CTFontGetGlyphsForCharacters(fallback, utf16, glyphs, count) ||
        glyphs[0] == 0)
    {
        CFRelease(fallback);

        /* Last resort: icon fonts */
        CTFontRef symbol = !is_private_use(cp)
            ? symbol_font_for(font, utf16, count, glyph) : NULL;
        if (symbol != NULL)
            return symbol;

        /* Render the primary font's .notdef */
        *glyph = 0;
        return CFRetain(font->faces[0].font);
    }

    *glyph = glyphs[0];
    return fallback;
}

static struct fcft_glyph *
glyph_cache_lookup(const struct font_priv *font, uint32_t cp)
{
    if (font->glyphs.size == 0)
        return NULL;

    const size_t mask = font->glyphs.size - 1;
    for (size_t i = (cp * 2654435761u) & mask; ; i = (i + 1) & mask) {
        const struct glyph_entry *e = &font->glyphs.table[i];
        if (e->glyph == NULL)
            return NULL;
        if (e->cp == cp)
            return e->glyph;
    }
}

/* Must be called with the write lock held */
static void
glyph_cache_insert(struct font_priv *font, uint32_t cp, struct fcft_glyph *glyph)
{
    if ((font->glyphs.count + 1) * 4 >= font->glyphs.size * 3) {
        const size_t new_size = font->glyphs.size == 0 ? 256 : font->glyphs.size * 2;
        struct glyph_entry *new_table = calloc(new_size, sizeof(new_table[0]));

        for (size_t i = 0; i < font->glyphs.size; i++) {
            const struct glyph_entry *e = &font->glyphs.table[i];
            if (e->glyph == NULL)
                continue;

            for (size_t j = (e->cp * 2654435761u) & (new_size - 1); ;
                 j = (j + 1) & (new_size - 1))
            {
                if (new_table[j].glyph == NULL) {
                    new_table[j] = *e;
                    break;
                }
            }
        }

        free(font->glyphs.table);
        font->glyphs.table = new_table;
        font->glyphs.size = new_size;
    }

    const size_t mask = font->glyphs.size - 1;
    for (size_t i = (cp * 2654435761u) & mask; ; i = (i + 1) & mask) {
        struct glyph_entry *e = &font->glyphs.table[i];
        if (e->glyph == NULL) {
            e->cp = cp;
            e->glyph = glyph;
            font->glyphs.count++;
            return;
        }
    }
}

/* Per-thread caches (see the top of this file); 'key' is at most 42 bits */
static size_t
thread_cache_slot(uint64_t font_id, uint64_t key, unsigned bits)
{
    return ((key ^ (font_id << 42)) * 0x9e3779b97f4a7c15ull) >> (64 - bits);
}

#define THREAD_GLYPHS_BITS 10

struct thread_glyph {
    uint64_t font_id;
    uint32_t cp;
    struct fcft_glyph *glyph;
};

static _Thread_local struct thread_glyph thread_glyphs[1 << THREAD_GLYPHS_BITS];

const struct fcft_glyph *
fcft_rasterize_char_utf32(struct fcft_font *_font, uint32_t cp,
                          enum fcft_subpixel subpixel)
{
    struct font_priv *font = (struct font_priv *)_font;

    struct thread_glyph *cached =
        &thread_glyphs[thread_cache_slot(font->id, cp, THREAD_GLYPHS_BITS)];
    if (cached->font_id == font->id && cached->cp == cp)
        return cached->glyph;

    pthread_rwlock_rdlock(&font->lock);
    struct fcft_glyph *glyph = glyph_cache_lookup(font, cp);
    pthread_rwlock_unlock(&font->lock);

    if (glyph != NULL) {
        *cached = (struct thread_glyph){.font_id = font->id, .cp = cp, .glyph = glyph};
        return glyph;
    }

    const struct face *face;
    CGGlyph glyph_id;
    CTFontRef ct = font_for_codepoint(font, cp, &face, &glyph_id);

    glyph = rasterize_glyph(font, face, ct, glyph_id);
    CFRelease(ct);

    if (glyph == NULL)
        return NULL;

    glyph->cp = cp;
    glyph->cols = codepoint_width(cp);

    pthread_rwlock_wrlock(&font->lock);
    {
        /* Another thread may have beaten us to it */
        struct fcft_glyph *existing = glyph_cache_lookup(font, cp);
        if (existing != NULL) {
            glyph_destroy(glyph);
            glyph = existing;
        } else
            glyph_cache_insert(font, cp, glyph);
    }
    pthread_rwlock_unlock(&font->lock);

    *cached = (struct thread_glyph){.font_id = font->id, .cp = cp, .glyph = glyph};
    return glyph;
}

/*
 * Graphemes and text runs are shaped by CoreText, which also takes
 * care of font fallback for each part of the string.
 */

static CTLineRef
create_line(const struct font_priv *font, size_t len, const uint32_t text[static len])
{
    CFStringRef str = cfstring_from_utf32(len, text);
    if (str == NULL)
        return NULL;

    CFDictionaryRef attrs = CFDictionaryCreate(
        NULL, (const void *[]){kCTFontAttributeName},
        (const void *[]){font->faces[0].font}, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

    CFAttributedStringRef astr = CFAttributedStringCreate(NULL, str, attrs);
    CTLineRef line = CTLineCreateWithAttributedString(astr);

    CFRelease(astr);
    CFRelease(attrs);
    CFRelease(str);
    return line;
}

static CTFontRef
run_font(const struct font_priv *font, CTRunRef run)
{
    CTFontRef ct = CFDictionaryGetValue(CTRunGetAttributes(run), kCTFontAttributeName);
    return ct != NULL ? ct : font->faces[0].font;
}

static bool
line_has_color_glyphs(const struct font_priv *font, CTLineRef line)
{
    CFArrayRef runs = CTLineGetGlyphRuns(line);
    for (CFIndex i = 0; i < CFArrayGetCount(runs); i++) {
        if (font_is_color(run_font(font, CFArrayGetValueAtIndex(runs, i))))
            return true;
    }
    return false;
}

static uint32_t
grapheme_hash(size_t len, const uint32_t cps[static len])
{
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        hash ^= cps[i];
        hash *= 16777619u;
    }
    return hash;
}

static struct fcft_grapheme *
grapheme_cache_lookup(const struct font_priv *font, uint32_t hash,
                      size_t len, const uint32_t cps[static len])
{
    if (font->graphemes.size == 0)
        return NULL;

    for (const struct grapheme_entry *e =
             font->graphemes.table[hash & (font->graphemes.size - 1)];
         e != NULL; e = e->next)
    {
        if (e->hash == hash && e->len == len &&
            memcmp(e->cps, cps, len * sizeof(cps[0])) == 0)
        {
            return e->grapheme;
        }
    }

    return NULL;
}

/* Column width of a grapheme cluster */
static int
grapheme_width(size_t len, const uint32_t cps[static len])
{
    bool emoji_presentation = false;
    int regional_indicators = 0;
    int width = 0;

    for (size_t i = 0; i < len; i++) {
        const uint32_t cp = cps[i];

        if (cp == 0xfe0f || cp == 0x200d || cp == 0x20e3 ||
            (cp >= 0x1f3fb && cp <= 0x1f3ff))
        {
            emoji_presentation = true;
        } else if (cp == 0xfe0e)
            return 1;
        else if (cp >= 0x1f1e6 && cp <= 0x1f1ff)
            regional_indicators++;

        width += codepoint_width(cp);
    }

    if (emoji_presentation || regional_indicators >= 2)
        return 2;

    return width > 0 ? width : 1;
}

const struct fcft_grapheme *
fcft_rasterize_grapheme_utf32(struct fcft_font *_font,
                              size_t len, const uint32_t cluster[static len],
                              enum fcft_subpixel subpixel)
{
    struct font_priv *font = (struct font_priv *)_font;
    const uint32_t hash = grapheme_hash(len, cluster);

    pthread_rwlock_rdlock(&font->lock);
    struct fcft_grapheme *grapheme = grapheme_cache_lookup(font, hash, len, cluster);
    pthread_rwlock_unlock(&font->lock);

    if (grapheme != NULL)
        return grapheme;

    CTLineRef line = create_line(font, len, cluster);
    if (line == NULL)
        return NULL;

    /* The whole cluster is rendered as a single glyph */
    const bool color = line_has_color_glyphs(font, line);
    const CGRect bounds = CTLineGetBoundsWithOptions(line, kCTLineBoundsUseGlyphPathBounds);
    const double advance = CTLineGetTypographicBounds(line, NULL, NULL, NULL);

    struct fcft_glyph *glyph = calloc(1, sizeof(*glyph));
    void *data = NULL;
    CGContextRef ctx = glyph != NULL
        ? create_glyph_context(font, bounds, color, 0., glyph, &data)
        : NULL;

    if (ctx == NULL) {
        free(glyph);
        CFRelease(line);
        return NULL;
    }

    CGContextSetTextPosition(ctx, 0., 0.);
    CTLineDraw(line, ctx);
    CFRelease(line);

    if (!finish_glyph(font, ctx, data, glyph)) {
        free(glyph);
        return NULL;
    }

    glyph->cp = cluster[0];
    glyph->cols = grapheme_width(len, cluster);
    glyph->advance.x = lround(advance);

    grapheme = malloc(sizeof(*grapheme));
    grapheme->cols = glyph->cols;
    grapheme->count = 1;
    grapheme->glyphs = malloc(sizeof(grapheme->glyphs[0]));
    grapheme->glyphs[0] = glyph;

    struct grapheme_entry *entry = malloc(sizeof(*entry));
    *entry = (struct grapheme_entry){
        .hash = hash,
        .len = len,
        .cps = malloc(len * sizeof(cluster[0])),
        .grapheme = grapheme,
    };
    memcpy(entry->cps, cluster, len * sizeof(cluster[0]));

    pthread_rwlock_wrlock(&font->lock);
    {
        /* Another thread may have beaten us to it */
        struct fcft_grapheme *existing = grapheme_cache_lookup(font, hash, len, cluster);

        if (existing != NULL) {
            grapheme_destroy(grapheme);
            free(entry->cps);
            free(entry);
            grapheme = existing;
        } else {
            if (font->graphemes.size == 0) {
                font->graphemes.size = 256;
                font->graphemes.table = calloc(
                    font->graphemes.size, sizeof(font->graphemes.table[0]));
            }

            struct grapheme_entry **head =
                &font->graphemes.table[hash & (font->graphemes.size - 1)];
            entry->next = *head;
            *head = entry;
        }
    }
    pthread_rwlock_unlock(&font->lock);

    return grapheme;
}

struct fcft_text_run *
fcft_rasterize_text_run_utf32(struct fcft_font *_font, size_t len,
                              const uint32_t text[static len],
                              enum fcft_subpixel subpixel)
{
    struct font_priv *font = (struct font_priv *)_font;

    CTLineRef line = create_line(font, len, text);
    if (line == NULL)
        return NULL;

    const CFIndex total = CTLineGetGlyphCount(line);
    const double line_width = CTLineGetTypographicBounds(line, NULL, NULL, NULL);

    struct fcft_text_run *run = calloc(1, sizeof(*run));
    run->glyphs = calloc(total > 0 ? total : 1, sizeof(run->glyphs[0]));
    run->cluster = calloc(total > 0 ? total : 1, sizeof(run->cluster[0]));

    /* Pen position of each glyph, to derive the shaped advances */
    double positions[total > 0 ? total : 1];

    CFArrayRef ct_runs = CTLineGetGlyphRuns(line);
    for (CFIndex r = 0; r < CFArrayGetCount(ct_runs); r++) {
        CTRunRef ct_run = CFArrayGetValueAtIndex(ct_runs, r);
        CTFontRef ct = run_font(font, ct_run);

        const CFIndex count = CTRunGetGlyphCount(ct_run);
        CGGlyph glyph_ids[count];
        CGPoint pos[count];
        CFIndex indices[count];

        CTRunGetGlyphs(ct_run, CFRangeMake(0, 0), glyph_ids);
        CTRunGetPositions(ct_run, CFRangeMake(0, 0), pos);
        CTRunGetStringIndices(ct_run, CFRangeMake(0, 0), indices);

        for (CFIndex i = 0; i < count; i++) {
            struct fcft_glyph *glyph = rasterize_glyph(
                font, &font->faces[0], ct, glyph_ids[i]);
            if (glyph == NULL)
                continue;

            const size_t idx = utf32_index(len, text, indices[i]);
            glyph->cp = idx < len ? text[idx] : 0;
            glyph->cols = idx < len ? codepoint_width(text[idx]) : 1;

            /* Always NULL in text-runs */
            free((char *)glyph->font_name);
            glyph->font_name = NULL;

            positions[run->count] = pos[i].x;
            run->glyphs[run->count] = glyph;
            run->cluster[run->count] = idx;
            run->count++;
        }
    }

    for (size_t i = 0; i < run->count; i++) {
        const double next = i + 1 < run->count ? positions[i + 1] : line_width;
        struct fcft_glyph *glyph = (struct fcft_glyph *)run->glyphs[i];
        glyph->advance.x = lround(next - positions[i]);
    }

    CFRelease(line);
    return run;
}

void
fcft_text_run_destroy(struct fcft_text_run *run)
{
    if (run == NULL)
        return;

    for (size_t i = 0; i < run->count; i++)
        glyph_destroy((struct fcft_glyph *)run->glyphs[i]);

    free(run->glyphs);
    free(run->cluster);
    free(run);
}

bool
fcft_kerning(struct fcft_font *font, uint32_t left, uint32_t right,
             long *restrict x, long *restrict y)
{
    /* Kerning is applied by CoreText, when shaping text runs */
    if (x != NULL)
        *x = 0;
    if (y != NULL)
        *y = 0;
    return false;
}

static bool
font_has_codepoint(CTFontRef font, uint32_t cp)
{
    UniChar utf16[2];
    const CFIndex count = utf16_encode(cp, utf16);

    CGGlyph glyphs[2] = {0};
    return CTFontGetGlyphsForCharacters(font, utf16, glyphs, count) && glyphs[0] != 0;
}

#define THREAD_PRECOMPOSED_BITS 8

struct thread_precomposed {
    uint64_t font_id;
    uint32_t base;
    uint32_t comb;
    uint32_t composed;
    bool base_is_from_primary;
    bool comb_is_from_primary;
    bool composed_is_from_primary;
};

static _Thread_local struct thread_precomposed
    thread_precomposed[1 << THREAD_PRECOMPOSED_BITS];

static void
precompose(const struct font_priv *font, struct thread_precomposed *out)
{
    CTFontRef primary = font->faces[0].font;
    const uint32_t base = out->base;
    const uint32_t comb = out->comb;

    out->base_is_from_primary = font_has_codepoint(primary, base);
    out->comb_is_from_primary = font_has_codepoint(primary, comb);

    /* Canonical composition (NFC) of the pair */
    const uint32_t pair[] = {base, comb};
    CFStringRef str = cfstring_from_utf32(2, pair);
    CFMutableStringRef nfc = CFStringCreateMutableCopy(NULL, 0, str);
    CFRelease(str);
    CFStringNormalize(nfc, kCFStringNormalizationFormC);

    uint32_t composed = (uint32_t)-1;
    const CFIndex units = CFStringGetLength(nfc);

    if (units == 1)
        composed = CFStringGetCharacterAtIndex(nfc, 0);
    else if (units == 2) {
        const UniChar hi = CFStringGetCharacterAtIndex(nfc, 0);
        const UniChar lo = CFStringGetCharacterAtIndex(nfc, 1);
        if (CFStringIsSurrogateHighCharacter(hi) && CFStringIsSurrogateLowCharacter(lo))
            composed = CFStringGetLongCharacterForSurrogatePair(hi, lo);
    }
    CFRelease(nfc);

    out->composed = composed;
    out->composed_is_from_primary =
        composed != (uint32_t)-1 && font_has_codepoint(primary, composed);
}

uint32_t
fcft_precompose(const struct fcft_font *_font, uint32_t base, uint32_t comb,
                bool *base_is_from_primary,
                bool *comb_is_from_primary,
                bool *composed_is_from_primary)
{
    const struct font_priv *font = (const struct font_priv *)_font;

    /* Normalizing is expensive; called for every combining character */
    struct thread_precomposed *cached = &thread_precomposed[thread_cache_slot(
        font->id, (uint64_t)base << 21 | comb, THREAD_PRECOMPOSED_BITS)];

    if (cached->font_id != font->id || cached->base != base || cached->comb != comb) {
        *cached = (struct thread_precomposed){
            .font_id = font->id, .base = base, .comb = comb};
        precompose(font, cached);
    }

    if (base_is_from_primary != NULL)
        *base_is_from_primary = cached->base_is_from_primary;
    if (comb_is_from_primary != NULL)
        *comb_is_from_primary = cached->comb_is_from_primary;
    if (composed_is_from_primary != NULL)
        *composed_is_from_primary = cached->composed_is_from_primary;

    return cached->composed;
}
