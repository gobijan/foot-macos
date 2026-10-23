#import "app.h"

#include <stdlib.h>

#include <tllist.h>

#define LOG_MODULE "themes"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "config.h"
#include "terminal.h"
#include "windows.h"

/*
 * View > Theme: foot's color themes (themes/, bundled in foot.app).
 *
 * Selecting a theme rewrites the theme include= in foot.ini, and
 * leaves everything else in the file as it is. Then, all windows take
 * the colors of the re-loaded configuration (new windows load it
 * anyway).
 *
 * Theme includes are the include= of files in a directory named
 * 'themes' (like /usr/share/foot/themes/). A new theme goes after the
 * last of them. Without macos-follow-appearance, there is one theme:
 * the new one replaces all others. With it, there's a dark and a light
 * one: themes are removed once later ones cover all of their colors
 * (e.g. a dark and a light theme replace one with both).
 */

@interface FootTheme : NSObject
@property (nonatomic, copy) NSString *name;
@property (nonatomic) BOOL dark;   /* Has [colors-dark] */
@property (nonatomic) BOOL light;  /* Has [colors-light] */
@end

@implementation FootTheme
@end

/* foot.ini, line by line, and what the menu needs to know about it */
@interface FootThemeSettings : NSObject
@property (nonatomic, copy) NSString *path;  /* Symlinks resolved */
@property (nonatomic) NSMutableArray<NSString *> *lines;

/* In [main] */
@property (nonatomic) NSMutableIndexSet *theme_lines;
@property (nonatomic) NSMutableDictionary<NSNumber *, NSString *> *theme_paths;  /* By line */
@property (nonatomic) NSMutableSet<NSString *> *selected;  /* Theme names */
@property (nonatomic) NSInteger last_include;              /* Line, or -1 */
@property (nonatomic) NSInteger initial_theme_line;        /* Line, or -1 */
@property (nonatomic) NSInteger follow_line;               /* Line, or -1 */
@property (nonatomic) BOOL follow;
@end

@implementation FootThemeSettings
@end

@interface FootThemesMenu : NSObject <NSMenuDelegate>
@end

/* The bundled themes; or, in the build directory, the bundle's */
static NSString *
themes_dir(void)
{
    NSBundle *bundle = [NSBundle mainBundle];
    NSString *exe_dir = bundle.executablePath.stringByDeletingLastPathComponent;

    for (NSString *dir in @[[bundle.resourcePath stringByAppendingPathComponent:@"themes"],
                            [exe_dir stringByAppendingPathComponent:@"foot.app/Contents/Resources/themes"]])
    {
        BOOL is_dir = NO;
        if ([[NSFileManager defaultManager] fileExistsAtPath:dir isDirectory:&is_dir] && is_dir)
            return dir;
    }

    return nil;
}

static NSString *
trim(NSString *s)
{
    return [s stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
}

/* Which appearances a theme is for; NO if it can't be read */
static BOOL
theme_sections(NSString *path, BOOL *dark, BOOL *light)
{
    NSString *text = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
    if (text == nil)
        return NO;

    *dark = *light = NO;
    for (NSString *line in [text componentsSeparatedByString:@"\n"]) {
        NSString *l = trim(line);
        if ([l isEqualToString:@"[colors-dark]"])
            *dark = YES;
        else if ([l isEqualToString:@"[colors-light]"])
            *light = YES;
    }
    return YES;
}

static NSArray<FootTheme *> *
bundled_themes(void)
{
    static NSArray<FootTheme *> *themes = nil;
    if (themes != nil)
        return themes;

    NSString *dir = themes_dir();
    NSArray<NSString *> *names = dir != nil
        ? [[NSFileManager defaultManager] contentsOfDirectoryAtPath:dir error:nil]
        : nil;

    NSMutableArray<FootTheme *> *list = [NSMutableArray array];
    for (NSString *name in [names sortedArrayUsingSelector:@selector(localizedStandardCompare:)]) {
        BOOL dark, light;
        if ([name hasPrefix:@"."] ||
            !theme_sections([dir stringByAppendingPathComponent:name], &dark, &light) ||
            (!dark && !light))
        {
            continue;
        }

        FootTheme *theme = [[FootTheme alloc] init];
        theme.name = name;
        theme.dark = dark;
        theme.light = light;
        [list addObject:theme];
    }

    themes = list;
    return themes;
}

/* The include= path of a theme include, or nil */
static NSString *
theme_include(NSString *key, NSString *value)
{
    if (![key isEqualToString:@"include"])
        return nil;

    NSString *path = value.stringByExpandingTildeInPath;
    return [path.stringByDeletingLastPathComponent.lastPathComponent isEqualToString:@"themes"]
        ? path : nil;
}

static BOOL
str_to_bool(NSString *value)
{
    for (NSString *yes in @[@"on", @"true", @"yes", @"1"]) {
        if ([value caseInsensitiveCompare:yes] == NSOrderedSame)
            return YES;
    }
    return NO;
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

/* 'create': create foot.ini (from the documented defaults) if it doesn't exist */
static FootThemeSettings *
load_settings(BOOL create)
{
    char *conf_path = windows_settings_path();
    if (conf_path == NULL)
        return nil;

    if (create && !windows_create_settings(conf_path)) {
        free(conf_path);
        return nil;
    }

    FootThemeSettings *s = [[FootThemeSettings alloc] init];
    s.path = @(conf_path).stringByResolvingSymlinksInPath;
    s.theme_lines = [NSMutableIndexSet indexSet];
    s.theme_paths = [NSMutableDictionary dictionary];
    s.selected = [NSMutableSet set];
    s.last_include = s.initial_theme_line = s.follow_line = -1;
    free(conf_path);

    NSError *error = nil;
    NSString *text = [NSString stringWithContentsOfFile:s.path encoding:NSUTF8StringEncoding error:&error];

    if (text == nil) {
        if (!create && ![[NSFileManager defaultManager] fileExistsAtPath:s.path]) {
            s.lines = [NSMutableArray array];
            return s;
        }

        if (create)
            show_error([NSString stringWithFormat:@"Couldn't read %@", s.path], error);
        return nil;
    }

    s.lines = [[text componentsSeparatedByString:@"\n"] mutableCopy];

    /* As parse_config_file() */
    BOOL in_main = YES;
    for (NSUInteger i = 0; i < s.lines.count; i++) {
        NSString *line = trim(s.lines[i]);
        if (line.length == 0 || [line hasPrefix:@"#"])
            continue;

        if ([line hasPrefix:@"["]) {
            in_main = [line isEqualToString:@"[main]"];
            continue;
        }

        if (!in_main)
            continue;

        /* A trailing comment is separated by a blank */
        NSRange comment = [line rangeOfString:@"[ \t]#" options:NSRegularExpressionSearch];
        if (comment.location != NSNotFound)
            line = [line substringToIndex:comment.location];

        NSRange eq = [line rangeOfString:@"="];
        if (eq.location == NSNotFound)
            continue;

        NSString *key = trim([line substringToIndex:eq.location]);
        NSString *value = trim([line substringFromIndex:NSMaxRange(eq)]);

        if ([key isEqualToString:@"include"])
            s.last_include = i;

        NSString *theme = theme_include(key, value);
        if (theme != nil) {
            [s.theme_lines addIndex:i];
            s.theme_paths[@(i)] = theme;
            [s.selected addObject:theme.lastPathComponent];
        } else if ([key isEqualToString:@"initial-color-theme"])
            s.initial_theme_line = i;
        else if ([key isEqualToString:@"macos-follow-appearance"]) {
            s.follow_line = i;
            s.follow = str_to_bool(value);
        }
    }

    return s;
}

/* The top of the file, after the mode line (if any) */
static NSUInteger
top_line(FootThemeSettings *s)
{
    return s.lines.count > 0 && [s.lines[0] hasPrefix:@"# -*-"] ? 1 : 0;
}

static BOOL
save_settings(FootThemeSettings *s)
{
    NSError *error = nil;
    if (![[s.lines componentsJoinedByString:@"\n"]
            writeToFile:s.path atomically:YES encoding:NSUTF8StringEncoding error:&error])
    {
        show_error([NSString stringWithFormat:@"Couldn't save %@", s.path], error);
        return NO;
    }

    LOG_INFO("%s: updated the color theme", s.path.UTF8String);
    return YES;
}

/* All windows take the colors of the (re-loaded) configuration */
static void
apply_settings(void)
{
    struct config conf;
    if (!windows_load_config(&conf)) {
        config_free(&conf);
        return;
    }

    tll_foreach(app_wayland()->terms, it) {
        struct terminal *term = it->item;

        /* Not const objects (see app_apply_color_theme()) */
        struct config *term_conf = (struct config *)term->conf;
        term_conf->colors_dark = conf.colors_dark;
        term_conf->colors_light = conf.colors_light;
        term_conf->initial_color_theme = conf.initial_color_theme;
        term_conf->macos.follow_appearance = conf.macos.follow_appearance;

        app_apply_color_theme(term, true);
    }

    config_free(&conf);
    app_run_hooks();
}

@implementation FootThemesMenu

- (void)selectTheme:(NSMenuItem *)item
{
    FootTheme *theme = item.representedObject;
    FootThemeSettings *s = load_settings(YES);
    if (s == nil)
        return;

    /* With one theme (not following the appearance), it decides
     * whether the dark or the light colors are used */
    if (!s.follow && theme.dark != theme.light && s.initial_theme_line >= 0) {
        s.lines[s.initial_theme_line] = theme.dark
            ? @"initial-color-theme=dark" : @"initial-color-theme=light";
    }

    /* The themes covered by later ones (the new one included) */
    NSMutableIndexSet *remove = [NSMutableIndexSet indexSet];
    __block BOOL covered_dark = theme.dark, covered_light = theme.light;

    [s.theme_lines enumerateIndexesWithOptions:NSEnumerationReverse
                                    usingBlock:^(NSUInteger i, BOOL *stop) {
        BOOL dark = NO, light = NO;
        theme_sections(s.theme_paths[@(i)], &dark, &light);  /* Unreadable: removed */

        if (!s.follow || ((!dark || covered_dark) && (!light || covered_light)))
            [remove addIndex:i];

        covered_dark |= dark;
        covered_light |= light;
    }];

    NSUInteger at = s.theme_lines.count > 0 ? s.theme_lines.lastIndex + 1
                  : s.last_include >= 0 ? (NSUInteger)s.last_include + 1
                  : top_line(s);

    NSString *path = [themes_dir() stringByAppendingPathComponent:theme.name];
    [s.lines insertObject:[@"include=" stringByAppendingString:path] atIndex:at];

    /* All before the inserted line */
    [s.lines removeObjectsAtIndexes:remove];

    if (save_settings(s))
        apply_settings();
}

- (void)defaultColors:(NSMenuItem *)item
{
    FootThemeSettings *s = load_settings(YES);
    if (s == nil || s.theme_lines.count == 0)
        return;

    [s.lines removeObjectsAtIndexes:s.theme_lines];

    if (save_settings(s))
        apply_settings();
}

- (void)followAppearance:(NSMenuItem *)item
{
    FootThemeSettings *s = load_settings(YES);
    if (s == nil)
        return;

    NSString *line = s.follow
        ? @"macos-follow-appearance=no" : @"macos-follow-appearance=yes";

    if (s.follow_line >= 0)
        s.lines[s.follow_line] = line;
    else
        [s.lines insertObject:line atIndex:top_line(s)];

    if (save_settings(s))
        apply_settings();
}

static NSMenuItem *
submenu_item(NSString *title, NSMenu *submenu)
{
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:title action:nil keyEquivalent:@""];
    item.submenu = submenu;

    for (NSMenuItem *it in submenu.itemArray) {
        if (it.state == NSControlStateValueOn)
            item.state = NSControlStateValueOn;
    }
    return item;
}

/* Re-built whenever it's opened, from foot.ini */
- (void)menuNeedsUpdate:(NSMenu *)menu
{
    [menu removeAllItems];

    NSArray<FootTheme *> *themes = bundled_themes();
    if (themes.count == 0) {
        [menu addItemWithTitle:@"No Themes Found" action:nil keyEquivalent:@""];
        return;
    }

    FootThemeSettings *s = load_settings(NO);

    NSMenu *dark = [[NSMenu alloc] initWithTitle:@"Dark"];
    NSMenu *light = [[NSMenu alloc] initWithTitle:@"Light"];
    NSMenu *both = [[NSMenu alloc] initWithTitle:@"Dark and Light"];

    for (FootTheme *theme in themes) {
        NSMenuItem *item = [[NSMenuItem alloc]
            initWithTitle:theme.name action:@selector(selectTheme:) keyEquivalent:@""];
        item.target = self;
        item.representedObject = theme;
        item.state = [s.selected containsObject:theme.name]
            ? NSControlStateValueOn : NSControlStateValueOff;

        [(theme.dark && theme.light ? both : theme.dark ? dark : light) addItem:item];
    }

    [menu addItem:submenu_item(@"Dark", dark)];
    [menu addItem:submenu_item(@"Light", light)];
    [menu addItem:submenu_item(@"Dark and Light", both)];
    [menu addItem:[NSMenuItem separatorItem]];

    NSMenuItem *follow = [[NSMenuItem alloc]
        initWithTitle:@"Follow System Appearance" action:@selector(followAppearance:) keyEquivalent:@""];
    follow.target = self;
    follow.state = s.follow ? NSControlStateValueOn : NSControlStateValueOff;
    [menu addItem:follow];

    NSMenuItem *none = [[NSMenuItem alloc]
        initWithTitle:@"Default Colors" action:@selector(defaultColors:) keyEquivalent:@""];
    none.target = self;
    none.state = s != nil && s.theme_lines.count == 0
        ? NSControlStateValueOn : NSControlStateValueOff;
    [menu addItem:none];
}

/* There are none; don't re-build the menu on every key press */
- (BOOL)menuHasKeyEquivalent:(NSMenu *)menu forEvent:(NSEvent *)event
                      target:(id *)target action:(SEL *)action
{
    return NO;
}

@end

NSMenuItem *
themes_menu_item(void)
{
    /* NSMenu's delegate is a weak reference */
    static FootThemesMenu *delegate = nil;
    delegate = [[FootThemesMenu alloc] init];

    NSMenu *menu = [[NSMenu alloc] initWithTitle:@"Theme"];
    menu.delegate = delegate;

    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:@"Theme" action:nil keyEquivalent:@""];
    item.submenu = menu;
    return item;
}
