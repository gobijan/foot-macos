#import "app.h"

#include <glob.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <tllist.h>

#define LOG_MODULE "connections"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "render.h"
#include "session.h"
#include "session-protocol.h"
#include "terminal.h"
#include "xmalloc.h"

/*
 * Shell > Connections: the other machines, for sessions there (over
 * SSH; macos/session.h). Those added there (or that windows were opened
 * on, with Other…), ~/.ssh/config's, and those footd has sessions on;
 * with how footd's connections to them are, as they change
 * (sessions_watch_remotes(), while it's open). A name given there is
 * shown instead of the address: in the menus, and the windows' titles;
 * with a badge, a dot in the machine's color (given there, or else
 * picked by its address).
 */

/* Those in the list: {Address, Name, Color (optional)}, in the user defaults */
static NSString *const servers_key = @"Servers";

/* Before, the machines used last (up to 8): the list, now */
static NSString *const recent_hosts_key = @"RecentHosts";

/*
 * The badges' colors. Emoji: the titles have them, and they're in color
 * wherever titles are (tabs, the Window menu, Mission Control…).
 */
static const struct {
    const char *key;    /* Color, in the list */
    const char *name;
    const char *emoji;
} badge_colors[] = {
    {"red", "Red", "🔴"},  /* Never picked: e.g. for production machines */
    {"orange", "Orange", "🟠"},
    {"yellow", "Yellow", "🟡"},
    {"green", "Green", "🟢"},
    {"blue", "Blue", "🔵"},
    {"purple", "Purple", "🟣"},
    {"brown", "Brown", "🟤"},
};

#define BADGE_COLORS (sizeof(badge_colors) / sizeof(badge_colors[0]))

@interface FootServer : NSObject
@property (nonatomic, copy) NSString *address;  /* As for ssh: [user@]host, or a Host of ~/.ssh/config */
@property (nonatomic, copy) NSString *name;     /* nil: none */
@property (nonatomic, copy) NSString *color;    /* Its key; nil: picked by the address */
@property (nonatomic) BOOL saved;               /* In the list */
@property (nonatomic) BOOL ssh_config;          /* A Host of ~/.ssh/config */
@property (nonatomic, readonly) NSString *title;  /* Its name, or else its address */
@end

@implementation FootServer
- (NSString *)title
{
    return self.name != nil ? self.name : self.address;
}
@end

/* What footd says about a machine (REMOTES) */
@interface FootServerStatus : NSObject
@property (nonatomic) enum session_remote_state state;
@property (nonatomic) uint32_t sessions;
@property (nonatomic, copy) NSString *detail;   /* nil: none */
@end

@implementation FootServerStatus
@end

/*
 * ~/.ssh/config's hosts (and its includes'): those that aren't patterns
 */
static void
ssh_config_hosts(const char *path, NSMutableOrderedSet<NSString *> *hosts, int depth)
{
    FILE *f = depth < 8 ? fopen(path, "r") : NULL;
    if (f == NULL)
        return;

    const char *home = getenv("HOME");
    char *line = NULL;
    size_t size = 0;

    while (getline(&line, &size, f) > 0) {
        char *p = line;
        char *key = strsep(&p, " \t=");
        while (key != NULL && key[0] == '\0')
            key = strsep(&p, " \t=");
        if (key == NULL || p == NULL)
            continue;

        const bool host = strcasecmp(key, "Host") == 0;
        if (!host && strcasecmp(key, "Include") != 0)
            continue;

        for (char *word; (word = strsep(&p, " \t\r\n")) != NULL;) {
            if (word[0] == '#')
                break;
            if (word[0] == '\0')
                continue;

            if (host) {
                /* Patterns aren't machines */
                NSString *name = strpbrk(word, "*?!\"") == NULL
                    ? [NSString stringWithUTF8String:word] : nil;
                if (name != nil)
                    [hosts addObject:name];
                continue;
            }

            /* Relative to ~/.ssh, or the home's */
            char *pattern = word[0] == '~' && home != NULL
                ? xasprintf("%s%s", home, word + 1)
                : word[0] != '/' && home != NULL
                ? xasprintf("%s/.ssh/%s", home, word) : xstrdup(word);

            glob_t g;
            if (glob(pattern, 0, NULL, &g) == 0) {
                for (size_t i = 0; i < g.gl_pathc; i++)
                    ssh_config_hosts(g.gl_pathv[i], hosts, depth + 1);
                globfree(&g);
            }
            free(pattern);
        }
    }

    free(line);
    fclose(f);
}

static NSArray<NSString *> *
ssh_hosts(void)
{
    NSMutableOrderedSet<NSString *> *hosts = [NSMutableOrderedSet orderedSet];
    const char *home = getenv("HOME");
    if (home != NULL) {
        char *path = xasprintf("%s/.ssh/config", home);
        ssh_config_hosts(path, hosts, 0);
        free(path);
    }
    return hosts.array;
}

/*
 * The list
 */

static NSArray<NSDictionary *> *
saved_servers(void)
{
    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;

    if ([defaults objectForKey:servers_key] == nil) {
        NSMutableArray *servers = [NSMutableArray array];
        for (id host in [defaults arrayForKey:recent_hosts_key]) {
            if ([host isKindOfClass:[NSString class]])
                [servers addObject:@{@"Address": host}];
        }
        [defaults setObject:servers forKey:servers_key];
        [defaults removeObjectForKey:recent_hosts_key];
    }

    NSMutableArray *servers = [NSMutableArray array];
    for (id server in [defaults arrayForKey:servers_key]) {
        if ([server isKindOfClass:[NSDictionary class]] &&
            [server[@"Address"] isKindOfClass:[NSString class]])
        {
            [servers addObject:server];
        }
    }
    return servers;
}

/* The list's entries, by address (while the list doesn't change) */
static NSDictionary<NSString *, NSDictionary *> *saved_by_address;

static void
save_servers(NSArray<NSDictionary *> *servers)
{
    [NSUserDefaults.standardUserDefaults setObject:servers forKey:servers_key];
    saved_by_address = nil;

    /* The windows' titles have the names, and colors */
    tll_foreach(app_wayland()->terms, it) {
        if (session_link_host(it->item->session) != NULL)
            render_refresh_title(it->item);
    }
}

static NSDictionary *
saved_server(NSString *address)
{
    if (address == nil)
        return nil;

    if (saved_by_address == nil) {
        NSMutableDictionary *all = [NSMutableDictionary dictionary];
        for (NSDictionary *server in saved_servers())
            all[server[@"Address"]] = server;
        saved_by_address = all;
    }
    return saved_by_address[address];
}

/* A string of the list's entry (nil: none) */
static NSString *
saved_string(NSString *address, NSString *key)
{
    NSString *value = saved_server(address)[key];
    return [value isKindOfClass:[NSString class]] && value.length > 0 ? value : nil;
}

NSString *
connections_name(NSString *address)
{
    return saved_string(address, @"Name");
}

NSString *
connections_title(NSString *address)
{
    NSString *name = connections_name(address);
    return name != nil ? name : address;
}

/* Picked by the address: always the same one (FNV-1a), not red */
static size_t
picked_color(NSString *address)
{
    uint32_t hash = 2166136261u;
    for (const char *p = address.UTF8String; p != NULL && *p != '\0'; p++)
        hash = (hash ^ (uint8_t)*p) * 16777619u;
    return 1 + hash % (BADGE_COLORS - 1);
}

/* 'key''s (nil, or unknown: picked by the address) */
static size_t
badge_color(NSString *address, NSString *key)
{
    for (size_t i = 0; key != nil && i < BADGE_COLORS; i++) {
        if (strcmp(key.UTF8String, badge_colors[i].key) == 0)
            return i;
    }
    return picked_color(address);
}

NSString *
connections_badge(NSString *address)
{
    if (address == nil)
        return nil;

    /* Its name, or else its host (without the user) */
    NSString *what = connections_name(address);
    if (what == nil) {
        const NSRange at = [address rangeOfString:@"@" options:NSBackwardsSearch];
        what = at.location != NSNotFound ? [address substringFromIndex:NSMaxRange(at)] : address;
    }

    const size_t color = badge_color(address, saved_string(address, @"Color"));
    return [NSString stringWithFormat:@"%@ %@", @(badge_colors[color].emoji), what];
}

/* The list, and the address's entry in it (or NSNotFound) */
static NSMutableArray<NSDictionary *> *
servers_with(NSString *address, NSUInteger *index)
{
    NSMutableArray *servers = [saved_servers() mutableCopy];
    *index = [servers indexOfObjectPassingTest:^BOOL(NSDictionary *server, NSUInteger i, BOOL *stop) {
        return [server[@"Address"] isEqualToString:address];
    }];
    return servers;
}

void
connections_remember(NSString *address)
{
    NSUInteger index;
    NSMutableArray *servers = servers_with(address, &index);
    if (index != NSNotFound || [ssh_hosts() containsObject:address])
        return;

    [servers addObject:@{@"Address": address}];
    save_servers(servers);
}

/* 'old' (nil: none) is now 'address', called 'name', in 'color' (nil: none, picked) */
static void
save_server(NSString *old, NSString *address, NSString *name, NSString *color)
{
    NSUInteger index;
    NSMutableArray *servers = servers_with(old != nil ? old : address, &index);

    /* (Another one had that address: it's this one now) */
    NSIndexSet *others = [servers indexesOfObjectsPassingTest:^BOOL(NSDictionary *server, NSUInteger i, BOOL *stop) {
        return i != index && [server[@"Address"] isEqualToString:address];
    }];

    NSMutableDictionary *server = [NSMutableDictionary dictionaryWithObject:address forKey:@"Address"];
    if (name != nil)
        server[@"Name"] = name;
    if (color != nil)
        server[@"Color"] = color;
    if (index != NSNotFound)
        servers[index] = server;
    else
        [servers addObject:server];
    [servers removeObjectsAtIndexes:others];
    save_servers(servers);
}

static void
forget_server(NSString *address)
{
    NSUInteger index;
    NSMutableArray *servers = servers_with(address, &index);
    if (index == NSNotFound)
        return;

    [servers removeObjectAtIndex:index];
    save_servers(servers);
}

/* All of them, by title: the list's, ~/.ssh/config's, and those footd has sessions on */
static NSArray<FootServer *> *
all_servers(NSDictionary<NSString *, FootServerStatus *> *status)
{
    NSMutableDictionary<NSString *, FootServer *> *by_address = [NSMutableDictionary dictionary];
    FootServer *(^server_at)(NSString *) = ^FootServer *(NSString *address) {
        FootServer *server = by_address[address];
        if (server == nil) {
            server = [[FootServer alloc] init];
            server.address = address;
            by_address[address] = server;
        }
        return server;
    };

    for (NSDictionary *saved in saved_servers()) {
        FootServer *server = server_at(saved[@"Address"]);
        server.saved = YES;
        server.name = connections_name(server.address);
        server.color = saved_string(server.address, @"Color");
    }
    for (NSString *host in ssh_hosts())
        server_at(host).ssh_config = YES;
    [status enumerateKeysAndObjectsUsingBlock:^(NSString *host, FootServerStatus *s, BOOL *stop) {
        if (s.sessions > 0 || s.state != SESSION_REMOTE_IDLE)
            server_at(host);
    }];

    return [by_address.allValues sortedArrayUsingComparator:^NSComparisonResult(FootServer *a, FootServer *b) {
        return [a.title localizedStandardCompare:b.title];
    }];
}

NSArray<NSString *> *
connections_addresses(void)
{
    return [all_servers(nil) valueForKey:@"address"];
}

/* As for ssh, and not an option: [user@]host, or a Host of ~/.ssh/config (nil: it isn't) */
NSString *
connections_address(NSString *text)
{
    NSString *address = [text stringByTrimmingCharactersInSet:
        NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if (address.length == 0 || [address hasPrefix:@"-"] ||
        [address rangeOfCharacterFromSet:NSCharacterSet.whitespaceAndNewlineCharacterSet].location != NSNotFound)
    {
        return nil;
    }
    return address;
}

/*
 * The window
 */

@interface FootServerCell : NSTableCellView
@property (nonatomic) NSTextField *badge;       /* Its color (as in the titles) */
@property (nonatomic) NSTextField *title;
@property (nonatomic) NSTextField *subtitle;
@property (nonatomic) NSImageView *status_icon;
@property (nonatomic) NSTextField *status;
@end

static NSTextField *
label(CGFloat size, NSColor *color)
{
    NSTextField *label = [NSTextField labelWithString:@""];
    label.font = [NSFont systemFontOfSize:size];
    label.textColor = color;
    label.lineBreakMode = NSLineBreakByTruncatingTail;
    label.translatesAutoresizingMaskIntoConstraints = NO;
    return label;
}

@implementation FootServerCell

- (instancetype)init
{
    self = [super initWithFrame:NSZeroRect];
    if (self == nil)
        return nil;

    _badge = label(NSFont.systemFontSize, NSColor.labelColor);
    _title = label(NSFont.systemFontSize, NSColor.labelColor);
    _subtitle = label(NSFont.smallSystemFontSize, NSColor.secondaryLabelColor);
    _status = label(NSFont.smallSystemFontSize, NSColor.secondaryLabelColor);
    _status.alignment = NSTextAlignmentRight;
    _status_icon = [[NSImageView alloc] init];
    _status_icon.translatesAutoresizingMaskIntoConstraints = NO;
    _status_icon.symbolConfiguration =
        [NSImageSymbolConfiguration configurationWithPointSize:NSFont.smallSystemFontSize
                                                        weight:NSFontWeightMedium];

    /* The status gives way first */
    [_badge setContentCompressionResistancePriority:NSLayoutPriorityRequired
                                     forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_title setContentCompressionResistancePriority:NSLayoutPriorityDefaultHigh
                                     forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_status setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                      forOrientation:NSLayoutConstraintOrientationHorizontal];

    NSStackView *text = [NSStackView stackViewWithViews:@[_title, _subtitle]];
    text.orientation = NSUserInterfaceLayoutOrientationVertical;
    text.alignment = NSLayoutAttributeLeading;
    text.spacing = 1;
    text.translatesAutoresizingMaskIntoConstraints = NO;

    for (NSView *view in @[_badge, text, _status_icon, _status])
        [self addSubview:view];
    self.textField = _title;

    [NSLayoutConstraint activateConstraints:@[
        [_badge.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:8],
        [_badge.firstBaselineAnchor constraintEqualToAnchor:_title.firstBaselineAnchor],
        [text.leadingAnchor constraintEqualToAnchor:_badge.trailingAnchor constant:6],
        [text.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
        [_status_icon.leadingAnchor constraintGreaterThanOrEqualToAnchor:text.trailingAnchor constant:12],
        [_status_icon.centerYAnchor constraintEqualToAnchor:_status.centerYAnchor],
        [_status.leadingAnchor constraintEqualToAnchor:_status_icon.trailingAnchor constant:4],
        [_status.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-8],
        [_status.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
    ]];
    return self;
}

@end

/* What the status says, and its symbol (nil: none) in 'color' */
static NSString *
status_text(FootServerStatus *s, NSString **symbol, NSColor **color)
{
    *symbol = nil;
    *color = NSColor.secondaryLabelColor;
    if (s == nil)
        return @"";

    /* (ssh says what it is) */
    NSString *detail = [s.detail hasPrefix:@"ssh: "] ? [s.detail substringFromIndex:5] : s.detail;

    NSString *sessions = s.sessions == 1 ? @"1 session"
        : [NSString stringWithFormat:@"%u sessions", s.sessions];

    switch (s.state) {
    case SESSION_REMOTE_IDLE:
        if (s.sessions == 0)
            return @"";
        *symbol = @"pause.circle";
        return [NSString stringWithFormat:@"Not connected · %@", sessions];

    case SESSION_REMOTE_CONNECTING:
        *symbol = @"arrow.triangle.2.circlepath";
        return @"Connecting…";

    case SESSION_REMOTE_CONNECTED:
        *symbol = @"checkmark.circle.fill";
        *color = NSColor.systemGreenColor;
        return s.sessions > 0 ? [NSString stringWithFormat:@"Connected · %@", sessions] : @"Connected";

    case SESSION_REMOTE_RETRYING:
        *symbol = @"exclamationmark.triangle.fill";
        *color = NSColor.systemOrangeColor;
        return detail != nil
            ? [NSString stringWithFormat:@"Can't connect, trying again: %@", detail]
            : @"Connecting again…";

    case SESSION_REMOTE_FAILED:
        *symbol = @"xmark.octagon.fill";
        *color = NSColor.systemRedColor;
        return detail != nil
            ? [NSString stringWithFormat:@"Can't connect: %@", detail] : @"Can't connect";

    case SESSION_REMOTE_ASKING:
        *symbol = @"questionmark.circle.fill";
        *color = NSColor.systemBlueColor;
        return @"footd isn't installed: its window asks";

    case SESSION_REMOTE_INSTALLING:
        *symbol = @"arrow.down.circle";
        return @"Installing footd…";

    case SESSION_REMOTE_REMOVING:
        *symbol = @"trash.circle";
        return @"Removing footd…";
    }
    return @"";
}

/* Its name, address and color, in an alert (adding one: 'server' nil) */
@interface FootServerEditor : NSObject
@property (nonatomic) NSAlert *alert;
@property (nonatomic) NSTextField *name;
@property (nonatomic) NSTextField *address;
@property (nonatomic) NSPopUpButton *color;   /* Its items' represented objects: the keys */
@property (nonatomic) NSButton *ok;
@end

@implementation FootServerEditor

- (instancetype)initWithServer:(FootServer *)server
{
    self = [super init];
    if (self == nil)
        return nil;

    NSAlert *alert = [[NSAlert alloc] init];
    alert.messageText = server == nil ? @"Add a Server" : @"Edit Server";
    alert.informativeText = @"The address is as for ssh: user@host, or a host in "
        "~/.ssh/config. foot connects with your SSH key, and installs its footd there "
        "if it's missing (asking first). The color marks its windows and tabs.";
    _ok = [alert addButtonWithTitle:server == nil ? @"Add" : @"Save"];
    [alert addButtonWithTitle:@"Cancel"];
    _alert = alert;

    _name = [NSTextField textFieldWithString:server.name != nil ? server.name : @""];
    _name.placeholderString = @"Optional";
    _address = [NSTextField textFieldWithString:server != nil ? server.address : @""];
    _address.placeholderString = @"user@host";

    _color = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [_color addItemWithTitle:@""];  /* Automatic (updateAutomatic) */
    [_color.menu addItem:[NSMenuItem separatorItem]];
    for (size_t i = 0; i < BADGE_COLORS; i++) {
        [_color addItemWithTitle:[NSString stringWithFormat:@"%@ %@", @(badge_colors[i].emoji),
                                  @(badge_colors[i].name)]];
        _color.lastItem.representedObject = @(badge_colors[i].key);
        if (server.color != nil && strcmp(server.color.UTF8String, badge_colors[i].key) == 0)
            [_color selectItem:_color.lastItem];
    }
    [self updateAutomatic];

    NSGridView *grid = [NSGridView gridViewWithViews:@[
        @[[NSTextField labelWithString:@"Name:"], _name],
        @[[NSTextField labelWithString:@"Address:"], _address],
        @[[NSTextField labelWithString:@"Color:"], _color],
    ]];
    [grid columnAtIndex:0].xPlacement = NSGridCellPlacementTrailing;
    grid.rowAlignment = NSGridRowAlignmentFirstBaseline;
    grid.rowSpacing = 8;
    [_name.widthAnchor constraintEqualToConstant:200].active = YES;
    [_address.widthAnchor constraintEqualToConstant:200].active = YES;
    grid.frame = (NSRect){.size = grid.fittingSize};
    alert.accessoryView = grid;
    alert.window.initialFirstResponder = server == nil ? _address : _name;
    return self;
}

/* Automatic: the color picked by the address (as typed so far) */
- (void)updateAutomatic
{
    NSString *address = connections_address(self.address.stringValue);
    NSMenuItem *automatic = [self.color itemAtIndex:0];
    automatic.title = [NSString stringWithFormat:@"%@ Automatic",
                       @(badge_colors[picked_color(address != nil ? address : @"")].emoji)];
    [self.color synchronizeTitleAndSelectedItem];
}

- (NSString *)selectedColor
{
    return self.color.selectedItem.representedObject;
}

@end

/* The buttons below the list: +, −, and the actions' menu */
#define BUTTON_WIDTH 28

@interface FootConnections : NSObject <NSTableViewDataSource, NSTableViewDelegate,
                                       NSWindowDelegate, NSMenuDelegate>
@property (nonatomic) NSPanel *panel;
@property (nonatomic) NSTableView *table;
@property (nonatomic) NSTextField *empty;
@property (nonatomic) NSSegmentedControl *buttons;   /* +, −, and the actions' menu */
@property (nonatomic) NSMenu *actions;
@property (nonatomic) NSButton *tab_button;
@property (nonatomic) NSButton *window_button;

@property (nonatomic) NSArray<FootServer *> *servers;
@property (nonatomic) NSDictionary<NSString *, FootServerStatus *> *status;
@end

static void
remotes_changed(const struct session_remote *remotes, size_t count, void *data);

@implementation FootConnections

- (NSMenuItem *)item:(NSString *)title action:(SEL)action
{
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:title action:action keyEquivalent:@""];
    item.target = self;
    return item;
}

- (void)createPanel
{
    NSPanel *panel = [[NSPanel alloc]
        initWithContentRect:NSMakeRect(0, 0, 500, 340)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:YES];
    panel.title = @"Connections";
    panel.releasedWhenClosed = NO;
    panel.hidesOnDeactivate = NO;
    panel.contentMinSize = NSMakeSize(400, 240);
    panel.delegate = self;
    [panel center];
    [panel setFrameAutosaveName:@"Connections"];
    self.panel = panel;

    NSTableView *table = [[NSTableView alloc] init];
    NSTableColumn *column = [[NSTableColumn alloc] initWithIdentifier:@"server"];
    column.resizingMask = NSTableColumnAutoresizingMask;
    [table addTableColumn:column];
    table.headerView = nil;
    table.style = NSTableViewStyleFullWidth;
    table.rowHeight = 40;
    table.columnAutoresizingStyle = NSTableViewUniformColumnAutoresizingStyle;
    table.dataSource = self;
    table.delegate = self;
    table.target = self;
    table.doubleAction = @selector(newWindow:);
    table.menu = [[NSMenu alloc] init];
    table.menu.delegate = self;
    self.table = table;

    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.documentView = table;
    scroll.hasVerticalScroller = YES;
    scroll.borderType = NSBezelBorder;
    scroll.translatesAutoresizingMaskIntoConstraints = NO;

    NSTextField *empty = [NSTextField wrappingLabelWithString:
        @"No servers yet: add one with +.\n\n"
        "foot connects with SSH, with your SSH key, and installs its footd there (one "
        "file, no sudo). Your sessions there keep running when the connection is lost, "
        "or foot quits."];
    empty.alignment = NSTextAlignmentCenter;
    empty.textColor = NSColor.secondaryLabelColor;
    empty.translatesAutoresizingMaskIntoConstraints = NO;
    self.empty = empty;

    NSSegmentedControl *buttons = [NSSegmentedControl
        segmentedControlWithImages:@[[NSImage imageNamed:NSImageNameAddTemplate],
                                     [NSImage imageNamed:NSImageNameRemoveTemplate],
                                     [NSImage imageNamed:NSImageNameActionTemplate]]
                      trackingMode:NSSegmentSwitchTrackingMomentary
                            target:self
                            action:@selector(buttonPressed:)];
    buttons.segmentStyle = NSSegmentStyleSmallSquare;
    for (NSInteger i = 0; i < 3; i++)
        [buttons setWidth:BUTTON_WIDTH forSegment:i];
    [buttons setToolTip:@"Add a server" forSegment:0];
    [buttons setToolTip:@"Remove the server from the list" forSegment:1];
    [buttons setToolTip:@"More" forSegment:2];
    buttons.translatesAutoresizingMaskIntoConstraints = NO;
    self.buttons = buttons;

    self.actions = [[NSMenu alloc] init];
    [self.actions addItem:[self item:@"Edit…" action:@selector(editServer:)]];
    [self.actions addItem:[self item:@"Remove footd from Server…" action:@selector(removeFootd:)]];

    NSButton *tab_button = [NSButton buttonWithTitle:@"New Tab" target:self action:@selector(newTab:)];
    tab_button.translatesAutoresizingMaskIntoConstraints = NO;
    self.tab_button = tab_button;

    NSButton *window_button = [NSButton buttonWithTitle:@"New Window" target:self
                                              action:@selector(newWindow:)];
    window_button.keyEquivalent = @"\r";
    window_button.translatesAutoresizingMaskIntoConstraints = NO;
    self.window_button = window_button;

    NSView *content = panel.contentView;
    for (NSView *view in @[scroll, empty, buttons, tab_button, window_button])
        [content addSubview:view];

    [NSLayoutConstraint activateConstraints:@[
        [scroll.topAnchor constraintEqualToAnchor:content.topAnchor constant:20],
        [scroll.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:20],
        [scroll.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-20],

        [empty.centerXAnchor constraintEqualToAnchor:scroll.centerXAnchor],
        [empty.centerYAnchor constraintEqualToAnchor:scroll.centerYAnchor],
        [empty.widthAnchor constraintEqualToAnchor:scroll.widthAnchor multiplier:0.8],

        [buttons.topAnchor constraintEqualToAnchor:scroll.bottomAnchor constant:-1],
        [buttons.leadingAnchor constraintEqualToAnchor:scroll.leadingAnchor],

        [window_button.topAnchor constraintEqualToAnchor:buttons.bottomAnchor constant:16],
        [window_button.trailingAnchor constraintEqualToAnchor:scroll.trailingAnchor],
        [window_button.bottomAnchor constraintEqualToAnchor:content.bottomAnchor constant:-20],
        [tab_button.trailingAnchor constraintEqualToAnchor:window_button.leadingAnchor constant:-12],
        [tab_button.centerYAnchor constraintEqualToAnchor:window_button.centerYAnchor],
    ]];
}

- (void)show
{
    if (self.panel == nil)
        [self createPanel];

    [self reload];
    [self.panel makeKeyAndOrderFront:nil];
    [self.panel makeFirstResponder:self.table];
    sessions_watch_remotes(app_wayland()->fdm, &remotes_changed, (__bridge void *)self);
}

- (void)windowWillClose:(NSNotification *)notification
{
    sessions_watch_remotes(NULL, NULL, NULL);
    self.status = nil;
}

/* ~/.ssh/config may have changed meanwhile */
- (void)windowDidBecomeKey:(NSNotification *)notification
{
    [self reload];
}

- (FootServer *)selected
{
    const NSInteger row = self.table.selectedRow;
    return row >= 0 && row < (NSInteger)self.servers.count ? self.servers[row] : nil;
}

/* The context menu's (the one right-clicked), else the one selected */
- (FootServer *)serverFor:(id)sender
{
    if ([sender isKindOfClass:[NSMenuItem class]] &&
        [[sender representedObject] isKindOfClass:[FootServer class]])
    {
        return [sender representedObject];
    }
    return self.selected;
}

- (void)reload
{
    [self reloadSelecting:self.selected.address];
}

- (void)reloadSelecting:(NSString *)address
{
    self.servers = all_servers(self.status);
    [self.table reloadData];

    const NSUInteger row = [self.servers indexOfObjectPassingTest:^BOOL(FootServer *s, NSUInteger i, BOOL *stop) {
        return [s.address isEqualToString:address];
    }];
    if (row != NSNotFound) {
        [self.table selectRowIndexes:[NSIndexSet indexSetWithIndex:row] byExtendingSelection:NO];
        [self.table scrollRowToVisible:row];
    } else if (self.servers.count > 0 && self.table.selectedRow < 0)
        [self.table selectRowIndexes:[NSIndexSet indexSetWithIndex:0] byExtendingSelection:NO];

    self.empty.hidden = self.servers.count > 0;
    [self selectionChanged];
}

- (void)selectionChanged
{
    FootServer *server = self.selected;
    self.window_button.enabled = self.tab_button.enabled = server != nil;
    [self.buttons setEnabled:server.saved forSegment:1];
    [self.buttons setEnabled:server != nil forSegment:2];
}

- (void)tableViewSelectionDidChange:(NSNotification *)notification
{
    [self selectionChanged];
}

- (NSInteger)numberOfRowsInTableView:(NSTableView *)table
{
    return self.servers.count;
}

- (NSView *)tableView:(NSTableView *)table viewForTableColumn:(NSTableColumn *)column row:(NSInteger)row
{
    FootServerCell *cell = [table makeViewWithIdentifier:@"server" owner:self];
    if (cell == nil) {
        cell = [[FootServerCell alloc] init];
        cell.identifier = @"server";
    }

    FootServer *server = self.servers[row];
    FootServerStatus *status = self.status[server.address];

    cell.title.stringValue = server.title;

    NSMutableArray *about = [NSMutableArray array];
    if (server.name != nil)
        [about addObject:server.address];
    if (server.ssh_config && !server.saved)
        [about addObject:@"~/.ssh/config"];
    cell.subtitle.stringValue = [about componentsJoinedByString:@" · "];
    cell.subtitle.hidden = about.count == 0;

    cell.badge.stringValue = @(badge_colors[badge_color(server.address, server.color)].emoji);

    NSString *symbol;
    NSColor *color;
    NSString *text = status_text(status, &symbol, &color);
    cell.status.stringValue = text;
    cell.status.toolTip = text.length > 0 ? text : nil;
    cell.status_icon.image = symbol != nil
        ? [NSImage imageWithSystemSymbolName:symbol accessibilityDescription:nil] : nil;
    cell.status_icon.contentTintColor = color;
    return cell;
}

/*
 * Actions
 */

- (void)newWindow:(id)sender
{
    /* (Double-clicked: not on an empty row) */
    if (sender == self.table && self.table.clickedRow < 0)
        return;

    FootServer *server = [self serverFor:sender];
    if (server != nil)
        app_open_on(server.address, false);
}

- (void)newTab:(id)sender
{
    FootServer *server = [self serverFor:sender];
    if (server != nil)
        app_open_on(server.address, true);
}

/* Esc */
- (void)cancelOperation:(id)sender
{
    [self.panel performClose:sender];
}

- (void)buttonPressed:(NSSegmentedControl *)sender
{
    switch (sender.selectedSegment) {
    case 0:
        [self edit:nil];
        break;
    case 1:
        [self removeFromList:sender];
        break;
    case 2:
        /* Below the button */
        [self.actions popUpMenuPositioningItem:nil
                                    atLocation:NSMakePoint(2 * BUTTON_WIDTH,
                                                           sender.isFlipped ? NSMaxY(sender.bounds) + 4 : -4)
                                        inView:sender];
        break;
    }
}

- (void)addServer:(id)sender
{
    [self edit:nil];
}

- (void)editServer:(id)sender
{
    FootServer *server = [self serverFor:sender];
    if (server != nil)
        [self edit:server];
}

- (void)removeFromList:(id)sender
{
    FootServer *server = [self serverFor:sender];
    if (server == nil || !server.saved)
        return;

    forget_server(server.address);
    [self reloadSelecting:nil];
}

- (void)removeFootd:(id)sender
{
    FootServer *server = [self serverFor:sender];
    if (server != nil)
        app_remove_footd(server.address);
}

/* Adding one ('server' nil), or changing its name, address or color */
- (void)edit:(FootServer *)server
{
    FootServerEditor *editor = [[FootServerEditor alloc] initWithServer:server];
    NSAlert *alert = editor.alert;
    NSTextField *name = editor.name;
    NSTextField *address = editor.address;
    NSButton *ok = editor.ok;

    /* Only with an address */
    ok.enabled = connections_address(address.stringValue) != nil;
    id observer = [NSNotificationCenter.defaultCenter
        addObserverForName:NSControlTextDidChangeNotification
                    object:address
                     queue:nil
                usingBlock:^(NSNotification *notification) {
        ok.enabled = connections_address(address.stringValue) != nil;
        [editor updateAutomatic];
    }];

    [alert beginSheetModalForWindow:self.panel completionHandler:^(NSModalResponse response) {
        [NSNotificationCenter.defaultCenter removeObserver:observer];

        NSString *new_address = connections_address(address.stringValue);
        if (response != NSAlertFirstButtonReturn || new_address == nil)
            return;

        NSString *new_name = [name.stringValue stringByTrimmingCharactersInSet:
            NSCharacterSet.whitespaceAndNewlineCharacterSet];
        save_server(server.saved ? server.address : nil, new_address,
                    new_name.length > 0 ? new_name : nil, editor.selectedColor);
        [self reloadSelecting:new_address];
    }];
}

/* The context menu: the server right-clicked */
- (void)menuNeedsUpdate:(NSMenu *)menu
{
    [menu removeAllItems];

    const NSInteger row = self.table.clickedRow;
    if (row < 0 || row >= (NSInteger)self.servers.count) {
        [menu addItem:[self item:@"Add Server…" action:@selector(addServer:)]];
        return;
    }

    FootServer *server = self.servers[row];
    NSArray<NSMenuItem *> *items = @[
        [self item:@"New Window" action:@selector(newWindow:)],
        [self item:@"New Tab" action:@selector(newTab:)],
        [NSMenuItem separatorItem],
        [self item:@"Edit…" action:@selector(editServer:)],
        [self item:[NSString stringWithFormat:@"Remove footd from %@…", server.title]
            action:@selector(removeFootd:)],
    ];
    if (server.saved) {
        items = [items arrayByAddingObjectsFromArray:@[
            [NSMenuItem separatorItem],
            [self item:@"Remove from List" action:@selector(removeFromList:)],
        ]];
    }
    for (NSMenuItem *item in items) {
        item.representedObject = server;
        [menu addItem:item];
    }
}

/* footd's connections changed (or footd is gone: 'remotes' NULL) */
- (void)setRemotes:(const struct session_remote *)remotes count:(size_t)count
{
    NSMutableDictionary *status = [NSMutableDictionary dictionary];
    for (size_t i = 0; i < count; i++) {
        NSString *host = [NSString stringWithUTF8String:remotes[i].host];
        if (host == nil)
            continue;

        FootServerStatus *s = [[FootServerStatus alloc] init];
        s.state = remotes[i].state;
        s.sessions = remotes[i].sessions;
        s.detail = remotes[i].detail != NULL ? [NSString stringWithUTF8String:remotes[i].detail] : nil;
        status[host] = s;
    }
    self.status = status;
    [self reload];
}

@end

static FootConnections *connections;

static void
remotes_changed(const struct session_remote *remotes, size_t count, void *data)
{
    [connections setRemotes:remotes count:count];
}

void
connections_show(void)
{
    if (connections == nil)
        connections = [[FootConnections alloc] init];
    [connections show];
}
