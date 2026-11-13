#import "app.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>

#define LOG_MODULE "settings"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "config.h"
#include "font-pattern.h"
#include "terminal.h"
#include "util.h"
#include "windows.h"

/*
 * foot > Settings: the options one changes now and then (the font, its
 * size, the cursor, the padding...), in a menu, and so in the command
 * palette ("Settings: Cursor: Beam") and for foot @ run.
 *
 * foot.ini stays what's true: the menu shows what the file says
 * (loaded when the menu is opened, the files it includes too), and
 * choosing an item writes that option to the file, leaving the rest of
 * it as it is (FootIni). Then the configuration is reloaded
 * (macos/reload.m), as foot > Reload Configuration does: the windows
 * that are open take it too. What only new windows get says so.
 */

static NSString *
trim(NSString *s)
{
    return [s stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
}

static void
show_error(NSString *message, NSError *error)
{
    LOG_ERR("%s: %s", message.UTF8String, error.localizedDescription.UTF8String);

    NSAlert *alert = [[NSAlert alloc] init];
    alert.messageText = message;
    alert.informativeText = error != nil ? error.localizedDescription : @"";
    [alert runModal];
}

@implementation FootIni

+ (instancetype)load:(BOOL)create
{
    char *conf_path = windows_settings_path();
    if (conf_path == NULL)
        return nil;

    if (create && !windows_create_settings(conf_path)) {
        free(conf_path);
        return nil;
    }

    FootIni *ini = [[FootIni alloc] init];
    ini.path = @(conf_path).stringByResolvingSymlinksInPath;
    free(conf_path);

    NSError *error = nil;
    NSString *text = [NSString stringWithContentsOfFile:ini.path encoding:NSUTF8StringEncoding error:&error];

    if (text == nil) {
        if (!create && ![[NSFileManager defaultManager] fileExistsAtPath:ini.path]) {
            ini.lines = [NSMutableArray array];
            return ini;
        }

        if (create)
            show_error([NSString stringWithFormat:@"Couldn't read %@", ini.path], error);
        return nil;
    }

    ini.lines = [[text componentsSeparatedByString:@"\n"] mutableCopy];
    return ini;
}

- (NSUInteger)top
{
    return self.lines.count > 0 && [self.lines[0] hasPrefix:@"# -*-"] ? 1 : 0;
}

/*
 * Sections and assignments as parse_config_file() reads them. The last
 * assignment is the one that counts, so that's the one replaced (its
 * indentation and its comment stay). Without one, the new line goes
 * below the option's documented default ("# key=value", as in the
 * foot.ini foot creates), or else to the end of the section; a section
 * that isn't there is added. Always below the section's include= lines:
 * the file's own options count, not those of the files it includes.
 */
- (void)set:(NSString *)key in:(NSString *)section to:(NSString *)value
{
    NSString *assignment = [NSString stringWithFormat:@"%@=%@", key, value];
    NSString *documented_prefix = [NSString stringWithFormat:@"%@=", key];

    BOOL inside = [section isEqualToString:@"main"];  /* The file starts there */
    NSInteger assigned = -1, documented = -1, include = -1;

    /* Of the section's last part: after its last assignment, or else its last line */
    NSInteger after_assignment = -1;
    NSInteger after_content = inside ? (NSInteger)[self top] : -1;

    for (NSUInteger i = [self top]; i < self.lines.count; i++) {
        NSString *line = trim(self.lines[i]);
        if (line.length == 0)
            continue;

        if ([line hasPrefix:@"["]) {
            const NSRange close = [line rangeOfString:@"]"];
            NSString *name = close.location != NSNotFound
                ? [line substringWithRange:NSMakeRange(1, close.location - 1)] : @"";

            inside = [name isEqualToString:section];
            if (inside) {
                after_assignment = -1;
                after_content = i + 1;
            }
            continue;
        }

        if (!inside)
            continue;

        if ([line hasPrefix:@"#"]) {
            NSString *comment = trim([line substringFromIndex:1]);
            if ([[comment stringByReplacingOccurrencesOfString:@" " withString:@""]
                    hasPrefix:documented_prefix])
            {
                documented = i;
            }
            after_content = i + 1;
            continue;
        }

        const NSRange eq = [line rangeOfString:@"="];
        if (eq.location == NSNotFound)
            continue;

        NSString *name = trim([line substringToIndex:eq.location]);
        if ([name isEqualToString:key])
            assigned = i;
        else if ([name isEqualToString:@"include"])
            include = i;
        after_assignment = after_content = i + 1;
    }

    if (assigned >= 0) {
        NSString *old = self.lines[assigned];
        [self.lines removeObjectAtIndex:assigned];

        /* Below an include= after it (what's included there would count instead) */
        const NSUInteger at = assigned < include ? after_assignment - 1 : assigned;

        NSUInteger indent = 0;
        while (indent < old.length && isblank([old characterAtIndex:indent]))
            indent++;

        /* A comment is separated by a blank */
        const NSRange comment = [old rangeOfString:@"[ \t]+#" options:NSRegularExpressionSearch
                                             range:NSMakeRange(indent, old.length - indent)];

        NSString *new = [NSString stringWithFormat:@"%@%@%@",
            [old substringToIndex:indent], assignment,
            comment.location != NSNotFound ? [old substringFromIndex:comment.location] : @""];
        [self.lines insertObject:new atIndex:at];
        return;
    }

    /* Not above an include= (what's included would count instead) */
    if (documented > include) {
        [self.lines insertObject:assignment atIndex:documented + 1];
        return;
    }

    const NSInteger end = after_assignment >= 0 ? after_assignment : after_content;
    if (end >= 0) {
        [self.lines insertObject:assignment atIndex:MIN((NSUInteger)end, self.lines.count)];
        return;
    }

    NSUInteger at = self.lines.count;
    while (at > 0 && trim(self.lines[at - 1]).length == 0)
        at--;

    NSArray<NSString *> *added = @[@"", [NSString stringWithFormat:@"[%@]", section], assignment];
    [self.lines insertObjects:added
                    atIndexes:[NSIndexSet indexSetWithIndexesInRange:NSMakeRange(at, added.count)]];
}

- (BOOL)save
{
    /* The last line ends too */
    if (self.lines.count > 0 && self.lines.lastObject.length > 0)
        [self.lines addObject:@""];

    NSError *error = nil;
    if (![[self.lines componentsJoinedByString:@"\n"]
            writeToFile:self.path atomically:YES encoding:NSUTF8StringEncoding error:&error])
    {
        show_error([NSString stringWithFormat:@"Couldn't save %@", self.path], error);
        return NO;
    }

    return YES;
}

@end

/* An assignment: what an item writes (it may be several) */
static NSArray<NSString *> *
assign(NSString *section, NSString *key, NSString *value)
{
    return @[section, key, value];
}

static void
apply(NSArray<NSArray<NSString *> *> *assignments)
{
    FootIni *ini = [FootIni load:YES];
    if (ini == nil || assignments.count == 0)
        return;

    for (NSArray<NSString *> *a in assignments)
        [ini set:a[1] in:a[0] to:a[2]];

    if (![ini save])
        return;

    LOG_INFO("%s: %s=%s", ini.path.UTF8String, assignments[0][1].UTF8String,
             assignments[0][2].UTF8String);
    app_reload_config();
}

/* Without the zeros a number doesn't need: 13, 13.5 */
static NSString *
number(double value)
{
    NSString *s = [NSString stringWithFormat:@"%.2f", value];
    while ([s hasSuffix:@"0"])
        s = [s substringToIndex:s.length - 1];
    return [s hasSuffix:@"."] ? [s substringToIndex:s.length - 1] : s;
}

static NSString *
with_size(NSString *pattern, float pt_size, int px_size)
{
    return px_size > 0
        ? [NSString stringWithFormat:@"%@:pixelsize=%d", pattern, px_size]
        : [NSString stringWithFormat:@"%@:size=%@", pattern, number(pt_size)];
}

static NSString *const font_keys[] = {@"font", @"font-bold", @"font-italic", @"font-bold-italic"};

/*
 * font=, and the styles' options that are set, as the configuration
 * has them, in other sizes: 'sizes' (a terminal's: a list for each
 * option), or else 'pt_size' for all. 'family' (may be nil) replaces
 * the first font of font=; what follows it (fonts for the glyphs it
 * doesn't have) stays.
 */
static NSArray<NSArray<NSString *> *> *
fonts(const struct config *conf, struct config_font *const *sizes, float pt_size, NSString *family)
{
    NSMutableArray<NSArray<NSString *> *> *assignments = [NSMutableArray array];

    for (size_t i = 0; i < ALEN(conf->fonts); i++) {
        const struct config_font_list *list = &conf->fonts[i];
        NSMutableArray<NSString *> *patterns = [NSMutableArray array];

        for (size_t j = 0; j < list->count; j++) {
            NSString *pattern = @(list->arr[j].pattern);
            if (i == 0 && j == 0 && family != nil)
                pattern = family;

            const struct config_font *size = sizes != NULL ? &sizes[i][j] : &list->arr[j];
            const bool given = sizes != NULL || pt_size <= 0.;

            [patterns addObject:with_size(pattern, given ? size->pt_size : pt_size,
                                          given ? size->px_size : 0)];
        }

        if (patterns.count > 0 && (family == nil || i == 0)) {
            [assignments addObject:
                assign(@"main", font_keys[i], [patterns componentsJoinedByString:@", "])];
        }
    }

    return assignments;
}

/* As a font pattern has it ('\' escapes what ends a family there) */
static NSString *
escaped_family(NSString *family)
{
    NSMutableString *escaped = [NSMutableString string];
    for (NSUInteger i = 0; i < family.length; i++) {
        const unichar c = [family characterAtIndex:i];
        if (c == '\\' || c == '-' || c == ':' || c == ',')
            [escaped appendString:@"\\"];
        [escaped appendFormat:@"%C", c];
    }
    return escaped;
}

/* The fonts with characters of one width, by family */
static NSArray<NSString *> *
fixed_pitch_families(void)
{
    static NSArray<NSString *> *families = nil;
    static dispatch_once_t once;

    /* Fonts were installed, or removed */
    dispatch_once(&once, ^{
        [[NSNotificationCenter defaultCenter]
            addObserverForName:NSFontSetChangedNotification object:nil queue:nil
                    usingBlock:^(NSNotification *n) { families = nil; }];
    });

    if (families != nil)
        return families;

    NSFontManager *manager = NSFontManager.sharedFontManager;
    NSMutableArray<NSString *> *list = [NSMutableArray array];

    for (NSString *family in manager.availableFontFamilies) {
        if ([family hasPrefix:@"."])
            continue;

        /* Name, style, weight, traits */
        for (NSArray *member in [manager availableMembersOfFontFamily:family]) {
            if (member.count > 3 && ([member[3] unsignedIntegerValue] & NSFixedPitchFontMask)) {
                [list addObject:family];
                break;
            }
        }
    }

    families = [list sortedArrayUsingSelector:@selector(localizedStandardCompare:)];
    return families;
}

/* The first family of font= ("monospace": the default, SF Mono) */
static NSString *
font_family(const struct config *conf)
{
    struct font_pattern pattern;
    if (conf->fonts[0].count == 0 || !font_pattern_parse(conf->fonts[0].arr[0].pattern, &pattern))
        return @"";

    NSString *family = pattern.family != NULL ? [NSString stringWithUTF8String:pattern.family] : nil;
    font_pattern_free(&pattern);
    return family != nil ? family : @"";
}

static BOOL
same(NSString *a, NSString *b)
{
    return [a caseInsensitiveCompare:b] == NSOrderedSame;
}

@interface FootSettingsMenu : NSObject <NSMenuDelegate>
@end

@implementation FootSettingsMenu

- (void)choose:(NSMenuItem *)item
{
    apply(item.representedObject);
}

- (NSMenuItem *)item:(NSString *)title on:(BOOL)on set:(NSArray<NSArray<NSString *> *> *)assignments
{
    NSMenuItem *item = [[NSMenuItem alloc]
        initWithTitle:title action:@selector(choose:) keyEquivalent:@""];
    item.target = self;
    item.representedObject = assignments;
    item.state = on ? NSControlStateValueOn : NSControlStateValueOff;
    return item;
}

/* Choosing it turns it off if it's on, and on if it's off */
- (NSMenuItem *)toggle:(NSString *)title on:(BOOL)on section:(NSString *)section key:(NSString *)key
{
    return [self item:title on:on set:@[assign(section, key, on ? @"no" : @"yes")]];
}

static NSMenu *
submenu(NSMenu *menu, NSString *title)
{
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:title action:nil keyEquivalent:@""];
    item.submenu = [[NSMenu alloc] initWithTitle:title];
    item.tag = MENU_TAG_PATH;
    [menu addItem:item];
    return item.submenu;
}

/* One of them: 'choices' are titles and values, 'current' the configuration's */
- (void)choices:(NSArray<NSArray<NSString *> *> *)choices current:(NSString *)current
        section:(NSString *)section key:(NSString *)key menu:(NSMenu *)menu
{
    for (NSArray<NSString *> *choice in choices) {
        [menu addItem:[self item:choice[0] on:[choice[1] isEqualToString:current]
                             set:@[assign(section, key, choice[1])]]];
    }
}

- (void)fontsOf:(const struct config *)conf menu:(NSMenu *)menu
{
    NSString *current = font_family(conf);

    [menu addItem:[self item:@"Default (SF Mono)" on:same(current, @"monospace")
                         set:fonts(conf, NULL, 0., @"monospace")]];
    [menu addItem:[NSMenuItem separatorItem]];

    /* The file's, if it isn't one of those below (e.g. it isn't installed) */
    NSArray<NSString *> *families = fixed_pitch_families();
    if (current.length > 0 && !same(current, @"monospace") &&
        [families indexOfObjectPassingTest:^BOOL(NSString *family, NSUInteger i, BOOL *stop) {
            return same(family, current);
        }] == NSNotFound)
    {
        families = [@[current] arrayByAddingObjectsFromArray:families];
    }

    for (NSString *family in families) {
        [menu addItem:[self item:family on:same(current, family)
                             set:fonts(conf, NULL, 0., escaped_family(family))]];
    }
}

- (void)fontSizesOf:(const struct config *)conf menu:(NSMenu *)menu
{
    static const float sizes[] = {9, 10, 11, 12, 13, 14, 15, 16, 18, 20, 24, 28};

    const struct config_font *font = conf->fonts[0].count > 0 ? &conf->fonts[0].arr[0] : NULL;

    for (size_t i = 0; i < ALEN(sizes); i++) {
        const BOOL on = font != NULL && font->px_size <= 0 && fabsf(font->pt_size - sizes[i]) < .005;
        [menu addItem:[self item:[NSString stringWithFormat:@"%@ pt", number(sizes[i])]
                              on:on set:fonts(conf, NULL, sizes[i], nil)]];
    }

    /* The size Cmd-+ and Cmd-- made it (the terminal's view: macos/view.m) */
    [menu addItem:[NSMenuItem separatorItem]];
    [menu addItem:[[NSMenuItem alloc] initWithTitle:@"This Window's"
                                             action:@selector(makeFontSizeDefault:)
                                      keyEquivalent:@""]];
}

- (void)build:(NSMenu *)menu from:(const struct config *)conf
{
    NSMenu *sub;

    [self fontsOf:conf menu:submenu(menu, @"Font")];
    [self fontSizesOf:conf menu:submenu(menu, @"Font Size")];

    /* Its strength; off is an option of its own */
    sub = submenu(menu, @"Font Smoothing");
    [sub addItem:[self item:@"Off" on:!conf->macos.font_smoothing
                        set:@[assign(@"main", @"macos-font-smoothing", @"no")]]];
    for (int percent = 25; percent <= 100; percent += 25) {
        const BOOL on = conf->macos.font_smoothing &&
            lroundf(conf->macos.font_smoothing_strength * 100) == percent;
        [sub addItem:[self item:[NSString stringWithFormat:@"%d%%", percent] on:on set:@[
            assign(@"main", @"macos-font-smoothing", @"yes"),
            assign(@"main", @"macos-font-smoothing-strength", number(percent / 100.)),
        ]]];
    }

    [menu addItem:[NSMenuItem separatorItem]];

    /* The background's, of the dark colors and of the light ones */
    sub = submenu(menu, @"Opacity");
    const long dark = lround(conf->colors_dark.alpha * 100. / 65535);
    const long light = lround(conf->colors_light.alpha * 100. / 65535);
    for (long percent = 100; percent >= 50; percent -= percent > 80 ? 5 : 10) {
        NSString *alpha = number(percent / 100.);
        [sub addItem:[self item:[NSString stringWithFormat:@"%ld%%", percent]
                             on:dark == percent && light == percent
                            set:@[assign(@"colors-dark", @"alpha", alpha),
                                  assign(@"colors-light", @"alpha", alpha)]]];
    }

    const BOOL blur = conf->colors_dark.blur && conf->colors_light.blur;
    [sub addItem:[NSMenuItem separatorItem]];
    [sub addItem:[self item:@"Blur What's Behind" on:blur
                        set:@[assign(@"colors-dark", @"blur", blur ? @"no" : @"yes"),
                              assign(@"colors-light", @"blur", blur ? @"no" : @"yes")]]];

    /* Around the text, in points; when it's centered stays as it is */
    sub = submenu(menu, @"Padding");
    static const unsigned pads[] = {0, 2, 4, 8, 12, 16, 24, 32};
    static const char *const center[] = {
        [CENTER_INVALID] = "",
        [CENTER_NEVER] = "",
        [CENTER_FULLSCREEN] = " center-when-fullscreen",
        [CENTER_MAXIMIZED_AND_FULLSCREEN] = " center-when-maximized-and-fullscreen",
        [CENTER_ALWAYS] = " center",
    };
    for (size_t i = 0; i < ALEN(pads); i++) {
        const BOOL on = conf->pad_left == pads[i] && conf->pad_right == pads[i] &&
            conf->pad_top == pads[i] && conf->pad_bottom == pads[i];
        NSString *title = pads[i] == 0 ? @"None" : [NSString stringWithFormat:@"%u pt", pads[i]];
        NSString *pad = [NSString stringWithFormat:@"%ux%u%s", pads[i], pads[i],
                         center[conf->center_when]];
        [sub addItem:[self item:title on:on set:@[assign(@"main", @"pad", pad)]]];
    }

    sub = submenu(menu, @"Cursor");
    static NSString *const styles[] = {
        [CURSOR_BLOCK] = @"block", [CURSOR_UNDERLINE] = @"underline",
        [CURSOR_BEAM] = @"beam", [CURSOR_HOLLOW] = @"hollow",
    };
    [self choices:@[@[@"Block", @"block"], @[@"Beam", @"beam"], @[@"Underline", @"underline"],
                    @[@"Hollow", @"hollow"]]
          current:styles[conf->cursor.style] section:@"cursor" key:@"style" menu:sub];
    [sub addItem:[NSMenuItem separatorItem]];
    [sub addItem:[self toggle:@"Blink" on:conf->cursor.blink.enabled section:@"cursor" key:@"blink"]];

    [menu addItem:[NSMenuItem separatorItem]];

    sub = submenu(menu, @"Scrollback (New Windows)");
    [self choices:@[@[@"1,000 Lines", @"1000"], @[@"10,000 Lines", @"10000"],
                    @[@"50,000 Lines", @"50000"], @[@"100,000 Lines", @"100000"],
                    @[@"1,000,000 Lines", @"1000000"]]
          current:[NSString stringWithFormat:@"%u", conf->scrollback.lines]
          section:@"scrollback" key:@"lines" menu:sub];

    sub = submenu(menu, @"Bell");
    [sub addItem:[self toggle:@"Sound" on:conf->bell.system_bell section:@"bell" key:@"system"]];
    [sub addItem:[self toggle:@"Flash" on:conf->bell.flash section:@"bell" key:@"visual"]];
    [sub addItem:[self toggle:@"Notification" on:conf->bell.notify section:@"bell" key:@"notify"]];
    [sub addItem:[self toggle:@"Bounce in the Dock" on:conf->bell.urgent section:@"bell" key:@"urgent"]];

    sub = submenu(menu, @"Notify When a Command Finishes");
    const float notify = conf->macos.notify_on_command_finish;
    [self choices:@[@[@"Never", @"never"], @[@"After 5 Seconds", @"5"], @[@"After 10 Seconds", @"10"],
                    @[@"After 30 Seconds", @"30"], @[@"After a Minute", @"60"]]
          current:notify < 0. ? @"never" : number(notify)
          section:@"main" key:@"macos-notify-on-command-finish" menu:sub];

    [menu addItem:[NSMenuItem separatorItem]];

    sub = submenu(menu, @"Option as Alt");
    static NSString *const option[] = {@"auto", @"none", @"left", @"right", @"both"};
    const int option_as_alt = conf->macos.option_as_alt;
    [self choices:@[@[@"By Keyboard Layout", @"auto"], @[@"Left Option Key", @"left"],
                    @[@"Right Option Key", @"right"], @[@"Both Option Keys", @"both"],
                    @[@"Neither", @"none"]]
          current:option_as_alt >= 0 && (size_t)option_as_alt < ALEN(option) ? option[option_as_alt] : @""
          section:@"main" key:@"macos-option-as-alt" menu:sub];

    [menu addItem:[self toggle:@"Hide the Mouse Pointer While Typing" on:conf->mouse.hide_when_typing
                       section:@"mouse" key:@"hide-when-typing"]];
    [menu addItem:[self toggle:@"Ask Before Closing a Running Program" on:conf->macos.confirm_close
                       section:@"main" key:@"macos-confirm-close"]];

    /* (macos/tabs.m) */
    if (@available(macOS 26, *)) {
        const BOOL tabs = conf->macos.titlebar_style == MACOS_TITLEBAR_TABS;
        [menu addItem:[self item:@"Tabs in the Titlebar (New Windows)" on:tabs
                             set:@[assign(@"main", @"macos-titlebar-style", tabs ? @"native" : @"tabs")]]];
    }

    [menu addItem:[NSMenuItem separatorItem]];

    [menu addItem:[self toggle:@"Shell Integration (New Windows)" on:conf->macos.shell_integration
                       section:@"main" key:@"macos-shell-integration"]];
    [menu addItem:[self toggle:@"Persistent Sessions (New Windows)" on:conf->macos.sessions
                       section:@"main" key:@"macos-sessions"]];
    [menu addItem:[self toggle:@"Remote Control (After a Restart)" on:conf->macos.remote_control
                       section:@"main" key:@"macos-remote-control"]];

    [menu addItem:[NSMenuItem separatorItem]];

    sub = submenu(menu, @"Quake Terminal");
    static NSString *const positions[] = {@"top", @"bottom", @"left", @"right", @"center"};
    const int position = conf->macos.quake.position;
    [self choices:@[@[@"Top", @"top"], @[@"Bottom", @"bottom"], @[@"Left", @"left"],
                    @[@"Right", @"right"], @[@"Center", @"center"]]
          current:position >= 0 && (size_t)position < ALEN(positions) ? positions[position] : @""
          section:@"main" key:@"macos-quake-position" menu:sub];
    [sub addItem:[NSMenuItem separatorItem]];
    [sub addItem:[self toggle:@"Hide When It Loses the Focus" on:conf->macos.quake.auto_hide
                      section:@"main" key:@"macos-quake-auto-hide"]];
}

/* Re-built whenever it's opened, from foot.ini */
- (void)menuNeedsUpdate:(NSMenu *)menu
{
    [menu removeAllItems];

    struct config conf;
    if (windows_load_config(&conf))
        [self build:menu from:&conf];
    else
        [menu addItemWithTitle:@"foot.ini Couldn't Be Loaded" action:nil keyEquivalent:@""];

    config_free(&conf);
}

/* There are none; don't re-build the menu on every key press */
- (BOOL)menuHasKeyEquivalent:(NSMenu *)menu forEvent:(NSEvent *)event
                      target:(id *)target action:(SEL *)action
{
    return NO;
}

@end

NSMenuItem *
settings_menu_item(void)
{
    /* NSMenu's delegate is a weak reference */
    static FootSettingsMenu *delegate = nil;
    delegate = [[FootSettingsMenu alloc] init];

    NSMenu *menu = [[NSMenu alloc] initWithTitle:@"Settings"];
    menu.delegate = delegate;

    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:@"Settings" action:nil keyEquivalent:@""];
    item.submenu = menu;
    item.tag = MENU_TAG_PATH;
    return item;
}

void
settings_set_font_size(const struct terminal *term)
{
    struct config conf;
    if (windows_load_config(&conf)) {
        /* As many fonts as the file has: its own configuration is the file's */
        bool same_fonts = true;
        for (size_t i = 0; i < ALEN(conf.fonts); i++)
            same_fonts = same_fonts && conf.fonts[i].count == term->conf->fonts[i].count;

        if (same_fonts)
            apply(fonts(&conf, term->font_sizes, 0., nil));
        else if (term->conf->fonts[0].count > 0 && term->font_sizes[0][0].px_size <= 0)
            apply(fonts(&conf, NULL, term->font_sizes[0][0].pt_size, nil));
    }

    config_free(&conf);
}
