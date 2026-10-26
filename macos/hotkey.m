#import "app.h"

#import <Carbon/Carbon.h>

#include <xkbcommon/xkbcommon.h>

#define LOG_MODULE "hotkey"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "config.h"

/*
 * The quake terminal's global hotkey (macos-quake-hotkey): a Carbon
 * hot key, which, unlike an event tap, needs no permission. It's a key
 * of the current keyboard layout (a virtual keycode), so it's
 * registered again when the layout changes.
 */

static struct {
    uint32_t sym;
    unsigned mods;
    void (*pressed)(void);

    EventHotKeyRef ref;
    EventHandlerRef handler;
} hotkey;

/* E.g. Mod1+grave, for the log */
static void
describe(char *buf, size_t size)
{
    char name[64];
    if (xkb_keysym_get_name(hotkey.sym, name, sizeof(name)) < 0)
        strlcpy(name, "?", sizeof(name));

    snprintf(buf, size, "%s%s%s%s%s",
             hotkey.mods & MACOS_HOTKEY_SHIFT ? "Shift+" : "",
             hotkey.mods & MACOS_HOTKEY_CONTROL ? "Control+" : "",
             hotkey.mods & MACOS_HOTKEY_OPTION ? "Mod1+" : "",
             hotkey.mods & MACOS_HOTKEY_COMMAND ? "Mod4+" : "",
             name);
}

/*
 * The key that types 'sym' (without modifiers), in the current layout.
 * If none types ` (e.g. with a German layout), the key below Esc, left
 * of 1: the one it is on a US keyboard, a quake terminal's usual key.
 */
static bool
keycode_for(uint32_t sym, uint16_t *keycode)
{
    const struct seat *seat = app_seat();
    if (seat == NULL || seat->kbd.xkb_keymap == NULL)
        return false;

    const xkb_keysym_t lower = xkb_keysym_to_lower(sym);

    for (uint16_t kc = 0; kc < 128; kc++) {
        const xkb_keysym_t *syms;
        const int count = xkb_keymap_key_get_syms_by_level(
            seat->kbd.xkb_keymap, keyboard_xkb_keycode(kc), 0, 0, &syms);

        if (count > 0 && (syms[0] == sym || syms[0] == lower)) {
            *keycode = kc;
            return true;
        }
    }

    if (sym == XKB_KEY_grave) {
        /* On ISO keyboards, macOS calls it the section key */
        *keycode = KBGetLayoutType(LMGetKbdType()) == kKeyboardISO
            ? kVK_ISO_Section : kVK_ANSI_Grave;
        return true;
    }

    return false;
}

static OSStatus
hotkey_pressed(EventHandlerCallRef next, EventRef event, void *data)
{
    if (hotkey.pressed != NULL)
        hotkey.pressed();
    return noErr;
}

static void
register_hotkey(void)
{
    if (hotkey.ref != NULL) {
        UnregisterEventHotKey(hotkey.ref);
        hotkey.ref = NULL;
    }

    if (hotkey.sym == XKB_KEY_NoSymbol)
        return;

    char name[128];
    describe(name, sizeof(name));

    uint16_t keycode;
    if (!keycode_for(hotkey.sym, &keycode)) {
        LOG_WARN("macos-quake-hotkey=%s: no key of the keyboard layout types it", name);
        return;
    }

    if (hotkey.handler == NULL) {
        const EventTypeSpec type = {kEventClassKeyboard, kEventHotKeyPressed};
        InstallApplicationEventHandler(&hotkey_pressed, 1, &type, NULL, &hotkey.handler);
    }

    const UInt32 mods =
        (hotkey.mods & MACOS_HOTKEY_SHIFT ? shiftKey : 0) |
        (hotkey.mods & MACOS_HOTKEY_CONTROL ? controlKey : 0) |
        (hotkey.mods & MACOS_HOTKEY_OPTION ? optionKey : 0) |
        (hotkey.mods & MACOS_HOTKEY_COMMAND ? cmdKey : 0);

    const EventHotKeyID id = {.signature = 'foot', .id = 1};
    const OSStatus status = RegisterEventHotKey(
        keycode, mods, id, GetApplicationEventTarget(), 0, &hotkey.ref);

    if (status != noErr) {
        LOG_WARN("macos-quake-hotkey=%s: can't be registered "
                 "(is another application using it?): %d", name, (int)status);
        hotkey.ref = NULL;
        return;
    }

    LOG_INFO("the quake terminal's hotkey: %s", name);
}

void
hotkey_register(uint32_t sym, unsigned mods, void (*pressed)(void))
{
    hotkey.sym = sym;
    hotkey.mods = mods;
    hotkey.pressed = pressed;
    register_hotkey();
}

void
hotkey_layout_changed(void)
{
    if (hotkey.pressed != NULL)
        register_hotkey();
}

void
hotkey_set_key_equivalent(NSMenuItem *item, uint32_t sym, unsigned mods)
{
    unichar c = 0;

    if (sym >= XKB_KEY_F1 && sym <= XKB_KEY_F20)
        c = NSF1FunctionKey + (sym - XKB_KEY_F1);
    else {
        switch (sym) {
        case XKB_KEY_Return:    c = '\r'; break;
        case XKB_KEY_Tab:       c = '\t'; break;
        case XKB_KEY_Escape:    c = 0x1b; break;
        case XKB_KEY_BackSpace: c = NSBackspaceCharacter; break;
        case XKB_KEY_Left:      c = NSLeftArrowFunctionKey; break;
        case XKB_KEY_Right:     c = NSRightArrowFunctionKey; break;
        case XKB_KEY_Up:        c = NSUpArrowFunctionKey; break;
        case XKB_KEY_Down:      c = NSDownArrowFunctionKey; break;

        default: {
            const uint32_t utf32 = xkb_keysym_to_utf32(xkb_keysym_to_lower(sym));
            if (utf32 >= 0x20 && utf32 != 0x7f && utf32 <= 0xffff)
                c = utf32;
            break;
        }
        }
    }

    if (sym == XKB_KEY_NoSymbol || c == 0) {
        item.keyEquivalent = @"";
        item.keyEquivalentModifierMask = 0;
        return;
    }

    item.keyEquivalent = [NSString stringWithCharacters:&c length:1];
    item.keyEquivalentModifierMask =
        (mods & MACOS_HOTKEY_SHIFT ? NSEventModifierFlagShift : 0) |
        (mods & MACOS_HOTKEY_CONTROL ? NSEventModifierFlagControl : 0) |
        (mods & MACOS_HOTKEY_OPTION ? NSEventModifierFlagOption : 0) |
        (mods & MACOS_HOTKEY_COMMAND ? NSEventModifierFlagCommand : 0);
}
