#import "keyboard.h"

#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>

#include <xkbcommon/xkbcommon.h>

#define LOG_MODULE "keyboard"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "wayland.h"

/* IOKit's device dependent modifier masks (NX_DEVICE*KEYMASK) */
#define DEVICE_LCTL   0x00000001
#define DEVICE_LSHIFT 0x00000002
#define DEVICE_RSHIFT 0x00000004
#define DEVICE_LCMD   0x00000008
#define DEVICE_RCMD   0x00000010
#define DEVICE_LALT   0x00000020
#define DEVICE_RALT   0x00000040
#define DEVICE_RCTL   0x00002000

/* Keys that don't produce text; their symbols don't depend on the layout */
static const struct {
    uint16_t keycode;
    const char *name;  /* XKB key name */
    const char *syms;  /* Symbols (levels), in XKB syntax */
} special_keys[] = {
    {kVK_Return, "RTRN", "Return"},
    {kVK_Tab, "TAB", "Tab, ISO_Left_Tab"},
    {kVK_Delete, "BKSP", "BackSpace"},
    {kVK_Escape, "ESC", "Escape"},
    {kVK_Command, "LWIN", "Super_L"},
    {kVK_RightCommand, "RWIN", "Super_R"},
    {kVK_Shift, "LFSH", "Shift_L"},
    {kVK_RightShift, "RTSH", "Shift_R"},
    {kVK_CapsLock, "CAPS", "Caps_Lock"},
    {kVK_Option, "LALT", "Alt_L"},
    {kVK_RightOption, "RALT", "Alt_R"},
    {kVK_Control, "LCTL", "Control_L"},
    {kVK_RightControl, "RCTL", "Control_R"},
    {kVK_F1, "FK01", "F1"},
    {kVK_F2, "FK02", "F2"},
    {kVK_F3, "FK03", "F3"},
    {kVK_F4, "FK04", "F4"},
    {kVK_F5, "FK05", "F5"},
    {kVK_F6, "FK06", "F6"},
    {kVK_F7, "FK07", "F7"},
    {kVK_F8, "FK08", "F8"},
    {kVK_F9, "FK09", "F9"},
    {kVK_F10, "FK10", "F10"},
    {kVK_F11, "FK11", "F11"},
    {kVK_F12, "FK12", "F12"},
    {kVK_F13, "FK13", "F13"},
    {kVK_F14, "FK14", "F14"},
    {kVK_F15, "FK15", "F15"},
    {kVK_F16, "FK16", "F16"},
    {kVK_F17, "FK17", "F17"},
    {kVK_F18, "FK18", "F18"},
    {kVK_F19, "FK19", "F19"},
    {kVK_F20, "FK20", "F20"},
    {kVK_Help, "INS", "Insert"},
    {kVK_Home, "HOME", "Home"},
    {kVK_End, "END", "End"},
    {kVK_PageUp, "PGUP", "Prior"},
    {kVK_PageDown, "PGDN", "Next"},
    {kVK_ForwardDelete, "DELE", "Delete"},
    {kVK_UpArrow, "UP", "Up"},
    {kVK_DownArrow, "DOWN", "Down"},
    {kVK_LeftArrow, "LEFT", "Left"},
    {kVK_RightArrow, "RGHT", "Right"},
    {kVK_ContextualMenu, "COMP", "Menu"},
    {kVK_ANSI_KeypadClear, "NMLK", "Clear"},
    {kVK_ANSI_KeypadEquals, "KPEQ", "KP_Equal"},
    {kVK_ANSI_KeypadDivide, "KPDV", "KP_Divide"},
    {kVK_ANSI_KeypadMultiply, "KPMU", "KP_Multiply"},
    {kVK_ANSI_KeypadMinus, "KPSU", "KP_Subtract"},
    {kVK_ANSI_KeypadPlus, "KPAD", "KP_Add"},
    {kVK_ANSI_KeypadEnter, "KPEN", "KP_Enter"},
    {kVK_ANSI_KeypadDecimal, "KPDL", "KP_Decimal"},
    {kVK_ANSI_Keypad0, "KP0", "KP_0"},
    {kVK_ANSI_Keypad1, "KP1", "KP_1"},
    {kVK_ANSI_Keypad2, "KP2", "KP_2"},
    {kVK_ANSI_Keypad3, "KP3", "KP_3"},
    {kVK_ANSI_Keypad4, "KP4", "KP_4"},
    {kVK_ANSI_Keypad5, "KP5", "KP_5"},
    {kVK_ANSI_Keypad6, "KP6", "KP_6"},
    {kVK_ANSI_Keypad7, "KP7", "KP_7"},
    {kVK_ANSI_Keypad8, "KP8", "KP_8"},
    {kVK_ANSI_Keypad9, "KP9", "KP_9"},
    {kVK_JIS_Eisu, "MUHE", "Eisu_toggle"},
    {kVK_JIS_Kana, "HENK", "Hiragana_Katakana"},
    {kVK_VolumeUp, "VOL+", "XF86AudioRaiseVolume"},
    {kVK_VolumeDown, "VOL-", "XF86AudioLowerVolume"},
    {kVK_Mute, "MUTE", "XF86AudioMute"},
};

/* Spacing accents produced by dead keys -> XKB dead keysyms */
static const struct {
    uint32_t cp;
    xkb_keysym_t sym;
} dead_keys[] = {
    {'`', XKB_KEY_dead_grave},
    {0x00b4, XKB_KEY_dead_acute},       /* ´ */
    {'\'', XKB_KEY_dead_acute},
    {'^', XKB_KEY_dead_circumflex},
    {0x02c6, XKB_KEY_dead_circumflex},  /* ˆ */
    {'~', XKB_KEY_dead_tilde},
    {0x02dc, XKB_KEY_dead_tilde},       /* ˜ */
    {0x00a8, XKB_KEY_dead_diaeresis},   /* ¨ */
    {'"', XKB_KEY_dead_diaeresis},
    {0x00b8, XKB_KEY_dead_cedilla},     /* ¸ */
    {0x02c7, XKB_KEY_dead_caron},       /* ˇ */
    {0x02d8, XKB_KEY_dead_breve},       /* ˘ */
    {0x02da, XKB_KEY_dead_abovering},   /* ˚ */
    {0x00b0, XKB_KEY_dead_abovering},   /* ° */
    {0x02dd, XKB_KEY_dead_doubleacute}, /* ˝ */
    {0x02db, XKB_KEY_dead_ogonek},      /* ˛ */
    {0x02d9, XKB_KEY_dead_abovedot},    /* ˙ */
    {0x00af, XKB_KEY_dead_macron},      /* ¯ */
};

static const UCKeyboardLayout *
current_layout(CFStringRef *name, TISInputSourceRef *source)
{
    *source = TISCopyCurrentKeyboardLayoutInputSource();
    if (*source == NULL)
        return NULL;

    CFDataRef data = TISGetInputSourceProperty(*source, kTISPropertyUnicodeKeyLayoutData);
    if (data == NULL) {
        /* Some input methods don't have their own layout */
        CFRelease(*source);
        *source = TISCopyCurrentASCIICapableKeyboardLayoutInputSource();
        if (*source == NULL)
            return NULL;
        data = TISGetInputSourceProperty(*source, kTISPropertyUnicodeKeyLayoutData);
    }

    *name = TISGetInputSourceProperty(*source, kTISPropertyLocalizedName);
    return data != NULL ? (const UCKeyboardLayout *)CFDataGetBytePtr(data) : NULL;
}

/*
 * Returns the symbol 'keycode' produces with the given (Carbon)
 * modifiers, or XKB_KEY_NoSymbol.
 */
static xkb_keysym_t
translate(const UCKeyboardLayout *layout, uint16_t keycode, UInt32 modifiers)
{
    UniChar chars[8];
    UniCharCount count = 0;
    UInt32 dead_key_state = 0;

    /* First without dead key processing, to get the accent itself */
    OSStatus err = UCKeyTranslate(
        layout, keycode, kUCKeyActionDown, (modifiers >> 8) & 0xff,
        LMGetKbdType(), kUCKeyTranslateNoDeadKeysMask, &dead_key_state,
        sizeof(chars) / sizeof(chars[0]), &count, chars);

    if (err != noErr || count == 0)
        return XKB_KEY_NoSymbol;

    uint32_t cp = chars[0];
    if (count == 2 && CFStringIsSurrogateHighCharacter(chars[0]) &&
        CFStringIsSurrogateLowCharacter(chars[1]))
    {
        cp = CFStringGetLongCharacterForSurrogatePair(chars[0], chars[1]);
    } else if (count > 1)
        return XKB_KEY_NoSymbol;  /* Multi-character output */

    if (cp < 0x20 || cp == 0x7f)
        return XKB_KEY_NoSymbol;

    /* Dead key? */
    dead_key_state = 0;
    count = 0;
    err = UCKeyTranslate(
        layout, keycode, kUCKeyActionDown, (modifiers >> 8) & 0xff,
        LMGetKbdType(), 0, &dead_key_state,
        sizeof(chars) / sizeof(chars[0]), &count, chars);

    if (err == noErr && count == 0 && dead_key_state != 0) {
        for (size_t i = 0; i < sizeof(dead_keys) / sizeof(dead_keys[0]); i++) {
            if (dead_keys[i].cp == cp)
                return dead_keys[i].sym;
        }
    }

    return xkb_utf32_to_keysym(cp);
}

static void
append(char **buf, size_t *len, size_t *size, const char *fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    int n = vsnprintf(NULL, 0, fmt, va);
    va_end(va);

    if (*len + n + 1 > *size) {
        while (*len + n + 1 > *size)
            *size = *size * 2;
        *buf = realloc(*buf, *size);
    }

    va_start(va, fmt);
    vsnprintf(*buf + *len, *size - *len, fmt, va);
    va_end(va);
    *len += n;
}

static const char *
special_key_name(uint16_t keycode)
{
    for (size_t i = 0; i < sizeof(special_keys) / sizeof(special_keys[0]); i++) {
        if (special_keys[i].keycode == keycode)
            return special_keys[i].name;
    }
    return NULL;
}

char *
keyboard_keymap_create(size_t *len)
{
    CFStringRef layout_name = NULL;
    TISInputSourceRef source = NULL;
    const UCKeyboardLayout *layout = current_layout(&layout_name, &source);

    size_t size = 32 * 1024;
    char *buf = malloc(size);
    *len = 0;

    char name[128] = "macOS";
    if (layout_name != NULL)
        CFStringGetCString(layout_name, name, sizeof(name), kCFStringEncodingUTF8);
    for (char *p = name; *p != '\0'; p++) {
        if (*p == '"' || *p == '\\')
            *p = ' ';
    }

    LOG_INFO("keyboard layout: %s", name);

    append(&buf, len, &size,
           "xkb_keymap {\n"
           "xkb_keycodes \"foot-macos\" {\n"
           "    minimum = 8;\n"
           "    maximum = 255;\n");

    for (uint16_t kc = 0; kc < 128; kc++) {
        const char *special = special_key_name(kc);
        if (special != NULL)
            append(&buf, len, &size, "    <%s> = %u;\n", special, kc + 8);
        else
            append(&buf, len, &size, "    <K%03u> = %u;\n", kc, kc + 8);
    }

    append(&buf, len, &size,
           "    indicator 1 = \"Caps Lock\";\n"
           "};\n"
           "xkb_types \"foot-macos\" {\n"
           "    virtual_modifiers Alt=Mod1,Meta=Mod1,Super=Mod4,LevelThree=Mod5,NumLock=Mod2;\n"
           "    type \"ONE_LEVEL\" {\n"
           "        modifiers = none;\n"
           "        level_name[Level1] = \"Any\";\n"
           "    };\n"
           "    type \"TWO_LEVEL\" {\n"
           "        modifiers = Shift;\n"
           "        map[Shift] = Level2;\n"
           "        level_name[Level1] = \"Base\";\n"
           "        level_name[Level2] = \"Shift\";\n"
           "    };\n"
           "    type \"ALPHABETIC\" {\n"
           "        modifiers = Shift+Lock;\n"
           "        map[Shift] = Level2;\n"
           "        map[Lock] = Level2;\n"
           "        level_name[Level1] = \"Base\";\n"
           "        level_name[Level2] = \"Caps\";\n"
           "    };\n"
           "    type \"FOUR_LEVEL\" {\n"
           "        modifiers = Shift+Mod5;\n"
           "        map[Shift] = Level2;\n"
           "        map[Mod5] = Level3;\n"
           "        map[Shift+Mod5] = Level4;\n"
           "        level_name[Level1] = \"Base\";\n"
           "        level_name[Level2] = \"Shift\";\n"
           "        level_name[Level3] = \"Option\";\n"
           "        level_name[Level4] = \"Shift Option\";\n"
           "    };\n"
           "    type \"FOUR_LEVEL_SEMIALPHABETIC\" {\n"
           "        modifiers = Shift+Lock+Mod5;\n"
           "        map[Shift] = Level2;\n"
           "        map[Lock] = Level2;\n"
           "        map[Mod5] = Level3;\n"
           "        map[Shift+Mod5] = Level4;\n"
           "        map[Lock+Mod5] = Level3;\n"
           "        map[Lock+Shift+Mod5] = Level4;\n"
           "        preserve[Lock+Mod5] = Lock;\n"
           "        preserve[Lock+Shift+Mod5] = Lock;\n"
           "        level_name[Level1] = \"Base\";\n"
           "        level_name[Level2] = \"Shift\";\n"
           "        level_name[Level3] = \"Option\";\n"
           "        level_name[Level4] = \"Shift Option\";\n"
           "    };\n"
           "};\n"
           "xkb_compat \"foot-macos\" {\n"
           "    virtual_modifiers Alt=Mod1,Meta=Mod1,Super=Mod4,LevelThree=Mod5,NumLock=Mod2;\n"
           "};\n"
           "xkb_symbols \"foot-macos\" {\n"
           "    name[Group1] = \"%s\";\n", name);

    for (uint16_t kc = 0; kc < 128; kc++) {
        bool is_special = false;
        for (size_t i = 0; i < sizeof(special_keys) / sizeof(special_keys[0]); i++) {
            if (special_keys[i].keycode != kc)
                continue;

            const bool two_level = strchr(special_keys[i].syms, ',') != NULL;
            append(&buf, len, &size,
                   "    key <%s> { type = \"%s\", [ %s ] };\n",
                   special_keys[i].name, two_level ? "TWO_LEVEL" : "ONE_LEVEL",
                   special_keys[i].syms);
            is_special = true;
            break;
        }

        if (is_special || layout == NULL)
            continue;

        const xkb_keysym_t syms[4] = {
            translate(layout, kc, 0),
            translate(layout, kc, shiftKey),
            translate(layout, kc, optionKey),
            translate(layout, kc, shiftKey | optionKey),
        };

        if (syms[0] == XKB_KEY_NoSymbol && syms[1] == XKB_KEY_NoSymbol)
            continue;

        const bool alphabetic =
            syms[0] != syms[1] && xkb_keysym_to_upper(syms[0]) == syms[1];
        const bool option_levels =
            (syms[2] != XKB_KEY_NoSymbol && syms[2] != syms[0]) ||
            (syms[3] != XKB_KEY_NoSymbol && syms[3] != syms[1]);

        char names[4][64];
        for (size_t i = 0; i < 4; i++) {
            if (syms[i] == XKB_KEY_NoSymbol ||
                xkb_keysym_get_name(syms[i], names[i], sizeof(names[i])) < 0)
            {
                strcpy(names[i], "NoSymbol");
            }
        }

        if (option_levels) {
            append(&buf, len, &size,
                   "    key <K%03u> { type = \"%s\", [ %s, %s, %s, %s ] };\n",
                   kc, alphabetic ? "FOUR_LEVEL_SEMIALPHABETIC" : "FOUR_LEVEL",
                   names[0], names[1], names[2], names[3]);
        } else {
            append(&buf, len, &size,
                   "    key <K%03u> { type = \"%s\", [ %s, %s ] };\n",
                   kc, alphabetic ? "ALPHABETIC" : "TWO_LEVEL",
                   names[0], names[1]);
        }
    }

    append(&buf, len, &size,
           "    modifier_map Shift { <LFSH>, <RTSH> };\n"
           "    modifier_map Lock { <CAPS> };\n"
           "    modifier_map Control { <LCTL>, <RCTL> };\n"
           "    modifier_map Mod1 { <LALT>, <RALT> };\n"
           "    modifier_map Mod4 { <LWIN>, <RWIN> };\n"
           "};\n"
           "};\n");

    if (source != NULL)
        CFRelease(source);

    return buf;
}

struct layout_observer {
    void (*cb)(void *data);
    void *data;
};

static void
layout_changed(CFNotificationCenterRef center, void *observer, CFStringRef name,
               const void *object, CFDictionaryRef user_info)
{
    const struct layout_observer *obs = observer;
    obs->cb(obs->data);
}

void
keyboard_observe_layout_changes(void (*cb)(void *data), void *data)
{
    struct layout_observer *obs = malloc(sizeof(*obs));
    *obs = (struct layout_observer){.cb = cb, .data = data};

    CFNotificationCenterAddObserver(
        CFNotificationCenterGetDistributedCenter(), obs, &layout_changed,
        kTISNotifySelectedKeyboardInputSourceChanged, NULL,
        CFNotificationSuspensionBehaviorDeliverImmediately);
}

bool
keyboard_option_is_alt(uint64_t ns_flags, enum option_as_alt option_as_alt)
{
    const bool left = (ns_flags & DEVICE_LALT) != 0;
    const bool right = (ns_flags & DEVICE_RALT) != 0;

    switch (option_as_alt) {
    case OPTION_AS_ALT_NONE:  return false;
    case OPTION_AS_ALT_LEFT:  return left;
    case OPTION_AS_ALT_RIGHT: return right;
    case OPTION_AS_ALT_BOTH:  return left || right;
    }
    return false;
}

void
keyboard_modifiers(const struct seat *seat, uint64_t ns_flags,
                   enum option_as_alt option_as_alt,
                   uint32_t *depressed, uint32_t *locked)
{
    *depressed = 0;
    *locked = 0;

    if (seat->kbd.xkb_keymap == NULL)
        return;

    const xkb_mod_index_t mod_level3 =
        xkb_keymap_mod_get_index(seat->kbd.xkb_keymap, XKB_MOD_NAME_MOD5);

#define set_mod(mask, idx) \
    do { if ((idx) != XKB_MOD_INVALID) (mask) |= 1u << (idx); } while (0)

    if (ns_flags & NSEventModifierFlagShift)
        set_mod(*depressed, seat->kbd.mod_shift);
    if (ns_flags & NSEventModifierFlagControl)
        set_mod(*depressed, seat->kbd.mod_ctrl);
    if (ns_flags & NSEventModifierFlagCommand)
        set_mod(*depressed, seat->kbd.mod_super);
    if (ns_flags & NSEventModifierFlagCapsLock)
        set_mod(*locked, seat->kbd.mod_caps);

    if (ns_flags & NSEventModifierFlagOption) {
        const bool left = (ns_flags & DEVICE_LALT) != 0;
        const bool right = (ns_flags & DEVICE_RALT) != 0;

        const bool left_is_alt =
            option_as_alt == OPTION_AS_ALT_LEFT || option_as_alt == OPTION_AS_ALT_BOTH;
        const bool right_is_alt =
            option_as_alt == OPTION_AS_ALT_RIGHT || option_as_alt == OPTION_AS_ALT_BOTH;

        /* Synthesized events may lack the device dependent bits */
        if (!left && !right) {
            if (option_as_alt == OPTION_AS_ALT_BOTH)
                set_mod(*depressed, seat->kbd.mod_alt);
            else
                set_mod(*depressed, mod_level3);
        } else {
            if ((left && left_is_alt) || (right && right_is_alt))
                set_mod(*depressed, seat->kbd.mod_alt);
            if ((left && !left_is_alt) || (right && !right_is_alt))
                set_mod(*depressed, mod_level3);
        }
    }

#undef set_mod
}

bool
keyboard_modifier_key_is_down(uint16_t keycode, uint64_t ns_flags)
{
    switch (keycode) {
    case kVK_Shift:        return ns_flags & DEVICE_LSHIFT;
    case kVK_RightShift:   return ns_flags & DEVICE_RSHIFT;
    case kVK_Control:      return ns_flags & DEVICE_LCTL;
    case kVK_RightControl: return ns_flags & DEVICE_RCTL;
    case kVK_Option:       return ns_flags & DEVICE_LALT;
    case kVK_RightOption:  return ns_flags & DEVICE_RALT;
    case kVK_Command:      return ns_flags & DEVICE_LCMD;
    case kVK_RightCommand: return ns_flags & DEVICE_RCMD;
    case kVK_CapsLock:     return ns_flags & NSEventModifierFlagCapsLock;
    }
    return false;
}
