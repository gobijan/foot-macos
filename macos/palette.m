#import "app.h"

#include <ctype.h>

/*
 * View > Command Palette (Cmd+Shift+P, as Ghostty's and Sublime Text's):
 * what the menus have, found by typing. That's the other machines too
 * (Shell > New Window on, New Tab on), the color themes (View > Theme),
 * the settings (foot > Settings) and the windows (Window): nothing is
 * listed here, so what the menus get, the palette has. What has a
 * check mark in the menus has one here. The settings come last: the
 * list starts with the commands.
 *
 * A command is its menu item, with the target it has while the
 * terminal's window is still key (the palette's panel is, while it's
 * open).
 */

#define WIDTH 560
#define HEADER 51       /* The field, and the line below it */
#define MARGIN 6        /* Around the list */
#define MAX_ROWS 12

@implementation FootCommand
@end

/* Borderless windows don't become key */
@interface FootPalettePanel : NSPanel
@end

@implementation FootPalettePanel
- (BOOL)canBecomeKeyWindow
{
    return YES;
}
@end

/* As the menus show it, e.g. ⇧⌘P */
static NSString *
shortcut(NSMenuItem *item)
{
    NSString *key = item.keyEquivalent;
    if (key.length == 0)
        return @"";

    NSEventModifierFlags mods = item.keyEquivalentModifierMask;
    const unichar c = [key characterAtIndex:0];
    if (c < 0x80 && isupper(c))
        mods |= NSEventModifierFlagShift;

    NSString *name;
    switch (c) {
    case '\r': name = @"↩"; break;
    case '\t': name = @"⇥"; break;
    case ' ': name = @"Space"; break;
    case 0x1b: name = @"⎋"; break;
    case '\b': case 0x7f: name = @"⌫"; break;
    case NSUpArrowFunctionKey: name = @"↑"; break;
    case NSDownArrowFunctionKey: name = @"↓"; break;
    case NSLeftArrowFunctionKey: name = @"←"; break;
    case NSRightArrowFunctionKey: name = @"→"; break;
    default:
        name = c >= NSF1FunctionKey && c <= NSF35FunctionKey
            ? [NSString stringWithFormat:@"F%d", c - NSF1FunctionKey + 1]
            : key.uppercaseString;
        break;
    }

    return [NSString stringWithFormat:@"%@%@%@%@%@",
            mods & NSEventModifierFlagControl ? @"⌃" : @"",
            mods & NSEventModifierFlagOption ? @"⌥" : @"",
            mods & NSEventModifierFlagShift ? @"⇧" : @"",
            mods & NSEventModifierFlagCommand ? @"⌘" : @"", name];
}

/*
 * The menu's items that can be chosen now, and its submenus'. Those of
 * a submenu have its title first (e.g. "Theme: Nord"); 'prefix' is the
 * outermost one's (nil: 'menu' is one of the menu bar's). With 'path',
 * the submenus' in between as well (MENU_TAG_PATH); those go to 'last'.
 */
/*
 * As NSApp's targetForAction:to:from: finds it, in 'first's responder
 * chain instead of the key window's: and whether it can be chosen there
 */
static id
target_from(NSResponder *first, NSMenuItem *item)
{
    id target = item.target;

    if (target == nil) {
        for (NSResponder *r = first; r != nil && target == nil; r = r.nextResponder) {
            if ([r respondsToSelector:item.action])
                target = r;
        }

        NSWindow *window = [first isKindOfClass:NSView.class] ? ((NSView *)first).window : nil;
        const id candidates[] = {window.delegate, NSApp, NSApp.delegate};
        for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
            if (target == nil && [candidates[i] respondsToSelector:item.action])
                target = candidates[i];
        }
    }

    if (target == nil || ![target respondsToSelector:item.action])
        return nil;

    if ([target respondsToSelector:@selector(validateMenuItem:)])
        return [target validateMenuItem:item] ? target : nil;
    if ([target respondsToSelector:@selector(validateUserInterfaceItem:)])
        return [target validateUserInterfaceItem:item] ? target : nil;
    return target;
}

/* 'first': the terminal's view (foot @, control.m); nil: the key window's */
static void
collect(NSMenu *menu, NSString *prefix, BOOL path, NSResponder *first,
        NSMutableArray<FootCommand *> *commands, NSMutableArray<FootCommand *> *last)
{
    /* Some are filled when they're opened */
    id<NSMenuDelegate> delegate = menu.delegate;
    if ([delegate respondsToSelector:@selector(menuNeedsUpdate:)])
        [delegate menuNeedsUpdate:menu];
    [menu update];

    for (NSMenuItem *item in menu.itemArray) {
        if (item.separatorItem || item.hidden || item.alternate)
            continue;

        if (item.submenu != nil) {
            if (item.submenu != NSApp.servicesMenu) {
                NSString *title = prefix == nil ? item.title
                    : path ? [NSString stringWithFormat:@"%@: %@", prefix, item.title] : prefix;
                const BOOL tagged = item.tag == MENU_TAG_PATH;
                collect(item.submenu, title, path || tagged, first, tagged ? last : commands, last);
            }
            continue;
        }

        if (item.action == NULL || item.action == @selector(showCommandPalette:))
            continue;

        /* (The menu enables its items for the key window) */
        id target;
        if (first != nil)
            target = target_from(first, item);
        else
            target = item.enabled ? [NSApp targetForAction:item.action to:item.target from:item] : nil;
        if (target == nil)
            continue;

        FootCommand *command = [[FootCommand alloc] init];
        command.title = prefix != nil && ![item.title hasPrefix:prefix]
            ? [NSString stringWithFormat:@"%@: %@", prefix, item.title] : item.title;
        command.shortcut = shortcut(item);
        command.checked = item.state == NSControlStateValueOn;
        command.item = item;
        command.target = target;
        [commands addObject:command];
    }
}

static NSTableCellView *
cell_view(NSTableView *table, NSString *identifier, NSTextAlignment alignment)
{
    NSTableCellView *cell = [table makeViewWithIdentifier:identifier owner:nil];
    if (cell != nil)
        return cell;

    cell = [[NSTableCellView alloc] init];
    cell.identifier = identifier;

    NSTextField *label = [NSTextField labelWithString:@""];
    label.alignment = alignment;
    label.lineBreakMode = NSLineBreakByTruncatingTail;
    label.translatesAutoresizingMaskIntoConstraints = NO;
    [cell addSubview:label];
    cell.textField = label;

    [NSLayoutConstraint activateConstraints:@[
        [label.leadingAnchor constraintEqualToAnchor:cell.leadingAnchor],
        [label.trailingAnchor constraintEqualToAnchor:cell.trailingAnchor],
        [label.centerYAnchor constraintEqualToAnchor:cell.centerYAnchor],
    ]];
    return cell;
}

NSArray<FootCommand *> *
palette_commands(NSResponder *first)
{
    NSMutableArray<FootCommand *> *commands = [NSMutableArray array];
    NSMutableArray<FootCommand *> *last = [NSMutableArray array];

    for (NSMenuItem *item in NSApp.mainMenu.itemArray) {
        if (item.submenu != nil)
            collect(item.submenu, nil, NO, first, commands, last);
    }

    /* The settings: many, and below what one does more often */
    [commands addObjectsFromArray:last];
    return commands;
}

void
palette_run(FootCommand *command)
{
    /* The item's menu may be filled anew meanwhile: the item itself is the sender */
    [NSApp sendAction:command.item.action to:command.target from:command.item];
}

@interface FootPalette : NSObject <NSTableViewDataSource, NSTableViewDelegate,
                                   NSTextFieldDelegate, NSWindowDelegate>
@property (nonatomic) NSPanel *panel;
@property (nonatomic) NSTextField *field;
@property (nonatomic) NSBox *line;
@property (nonatomic) NSTableView *table;
@property (nonatomic) BOOL open;
@property (nonatomic) NSRect frame;                     /* With all its rows */
@property (nonatomic, weak) NSWindow *window;           /* The one it was opened in */
@property (nonatomic) NSArray<FootCommand *> *commands;
@property (nonatomic) NSArray<FootCommand *> *shown;    /* Those matching what's typed */
@end

@implementation FootPalette

- (void)createPanel
{
    NSPanel *panel = [[FootPalettePanel alloc]
        initWithContentRect:NSMakeRect(0, 0, WIDTH, HEADER)
                  styleMask:NSWindowStyleMaskBorderless
                    backing:NSBackingStoreBuffered
                      defer:YES];
    panel.releasedWhenClosed = NO;
    panel.hidesOnDeactivate = NO;
    panel.opaque = NO;
    panel.backgroundColor = NSColor.clearColor;
    panel.hasShadow = YES;
    panel.collectionBehavior = NSWindowCollectionBehaviorFullScreenAuxiliary |
        NSWindowCollectionBehaviorTransient;
    panel.delegate = self;
    self.panel = panel;

    NSVisualEffectView *content = [[NSVisualEffectView alloc] init];
    content.material = NSVisualEffectMaterialMenu;
    content.state = NSVisualEffectStateActive;
    content.wantsLayer = YES;
    content.layer.cornerRadius = 12;
    content.layer.masksToBounds = YES;
    panel.contentView = content;

    NSTextField *field = [[NSTextField alloc] init];
    field.placeholderString = @"Command, setting, machine or theme";
    field.font = [NSFont systemFontOfSize:18];
    field.bordered = NO;
    field.drawsBackground = NO;
    field.focusRingType = NSFocusRingTypeNone;
    field.delegate = self;
    field.translatesAutoresizingMaskIntoConstraints = NO;
    self.field = field;

    NSBox *line = [[NSBox alloc] init];
    line.boxType = NSBoxSeparator;
    line.translatesAutoresizingMaskIntoConstraints = NO;
    self.line = line;

    NSTableView *table = [[NSTableView alloc] init];
    NSTableColumn *title = [[NSTableColumn alloc] initWithIdentifier:@"title"];
    title.resizingMask = NSTableColumnAutoresizingMask;
    [table addTableColumn:title];
    NSTableColumn *keys = [[NSTableColumn alloc] initWithIdentifier:@"shortcut"];
    keys.width = keys.minWidth = keys.maxWidth = 80;
    keys.resizingMask = NSTableColumnNoResizing;
    [table addTableColumn:keys];
    table.headerView = nil;
    table.style = NSTableViewStyleInset;
    table.rowHeight = 28;
    table.backgroundColor = NSColor.clearColor;
    table.columnAutoresizingStyle = NSTableViewFirstColumnOnlyAutoresizingStyle;
    table.allowsEmptySelection = NO;
    table.refusesFirstResponder = YES;  /* The field keeps the keyboard */
    table.dataSource = self;
    table.delegate = self;
    table.target = self;
    table.action = @selector(clicked:);
    self.table = table;

    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.documentView = table;
    scroll.hasVerticalScroller = YES;
    scroll.drawsBackground = NO;
    scroll.translatesAutoresizingMaskIntoConstraints = NO;

    for (NSView *view in @[field, line, scroll])
        [content addSubview:view];

    [NSLayoutConstraint activateConstraints:@[
        [field.topAnchor constraintEqualToAnchor:content.topAnchor constant:12],
        [field.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:18],
        [field.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-18],
        [field.heightAnchor constraintEqualToConstant:26],

        [line.topAnchor constraintEqualToAnchor:field.bottomAnchor constant:12],
        [line.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
        [line.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [line.heightAnchor constraintEqualToConstant:1],

        [scroll.topAnchor constraintEqualToAnchor:line.bottomAnchor constant:MARGIN],
        [scroll.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
        [scroll.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [scroll.bottomAnchor constraintEqualToAnchor:content.bottomAnchor constant:-MARGIN],
    ]];
}

/* As high as its rows (up to MAX_ROWS); its top stays where it is */
- (void)layout
{
    const NSInteger rows = MIN(self.shown.count, (NSUInteger)MAX_ROWS);

    /* The list has some room above its first row: as much below the last one */
    const CGFloat list = rows > 0
        ? NSMaxY([self.table rectOfRow:rows - 1]) + NSMinY([self.table rectOfRow:0]) : 0;
    const CGFloat height = HEADER + 2 * MARGIN + list;

    self.line.hidden = rows == 0;

    NSRect frame = self.frame;
    frame.origin.y = NSMaxY(frame) - height;
    frame.size.height = height;
    [self.panel setFrame:frame display:YES];
    [self.panel invalidateShadow];
}

/*
 * Those with all the words typed, in the menus' order; first those
 * starting with what's typed.
 */
- (void)filter
{
    const NSStringCompareOptions options = NSCaseInsensitiveSearch | NSDiacriticInsensitiveSearch;
    NSCharacterSet *space = NSCharacterSet.whitespaceCharacterSet;
    NSString *typed = [self.field.stringValue stringByTrimmingCharactersInSet:space];
    NSArray<NSString *> *words = [typed componentsSeparatedByCharactersInSet:space];

    NSMutableArray<FootCommand *> *first = [NSMutableArray array];
    NSMutableArray<FootCommand *> *others = [NSMutableArray array];

    for (FootCommand *command in self.commands) {
        BOOL matches = YES;
        for (NSString *word in words) {
            if (word.length > 0 &&
                [command.title rangeOfString:word options:options].location == NSNotFound)
            {
                matches = NO;
                break;
            }
        }
        if (!matches)
            continue;

        const BOOL starts = typed.length > 0 &&
            [command.title rangeOfString:typed options:options | NSAnchoredSearch].location == 0;
        [(starts ? first : others) addObject:command];
    }

    [first addObjectsFromArray:others];
    self.shown = first;

    [self.table reloadData];
    if (first.count > 0) {
        [self.table selectRowIndexes:[NSIndexSet indexSetWithIndex:0] byExtendingSelection:NO];
        [self.table scrollRowToVisible:0];
    }
    [self layout];
}

- (void)show
{
    if (self.panel == nil)
        [self createPanel];

    /* While the terminal's window is key: the commands are those it has */
    NSWindow *window = NSApp.keyWindow != nil ? NSApp.keyWindow : NSApp.mainWindow;
    self.commands = palette_commands(nil);
    self.window = window;

    /* Over the window's upper part; without one, the screen's */
    NSRect frame;
    if (window != nil) {
        const NSRect w = window.frame;
        frame.size.width = MAX(MIN(WIDTH, NSWidth(w) - 40), 280);
        frame.origin.x = round(NSMidX(w) - NSWidth(frame) / 2);
        frame.origin.y = NSMaxY(w) - MIN(80, NSHeight(w) / 4);
    } else {
        const NSRect s = NSScreen.mainScreen.visibleFrame;
        frame.size.width = WIDTH;
        frame.origin.x = round(NSMidX(s) - WIDTH / 2);
        frame.origin.y = NSMaxY(s) - round(NSHeight(s) / 5);
    }
    frame.size.height = 0;
    self.frame = frame;

    /* Above the quake terminal too */
    self.panel.level = window != nil ? MAX(window.level, NSNormalWindowLevel) : NSNormalWindowLevel;

    self.open = YES;
    self.field.stringValue = @"";
    [self filter];
    [self.panel makeKeyAndOrderFront:nil];
    [self.panel makeFirstResponder:self.field];
}

- (void)close
{
    if (!self.open)
        return;

    self.open = NO;
    self.commands = self.shown = nil;
    [self.table reloadData];

    const BOOL was_key = self.panel.keyWindow;
    [self.panel orderOut:nil];
    if (was_key)
        [self.window makeKeyWindow];
    self.window = nil;
}

- (void)run:(FootCommand *)command
{
    [self close];
    palette_run(command);
}

- (void)runSelected
{
    const NSInteger row = self.table.selectedRow;
    if (row >= 0 && (NSUInteger)row < self.shown.count)
        [self run:self.shown[row]];
}

- (void)clicked:(id)sender
{
    const NSInteger row = self.table.clickedRow;
    if (row >= 0 && (NSUInteger)row < self.shown.count)
        [self run:self.shown[row]];
}

- (void)select:(NSInteger)row
{
    const NSInteger count = self.shown.count;
    if (count == 0)
        return;

    row = MAX(0, MIN(row, count - 1));
    [self.table selectRowIndexes:[NSIndexSet indexSetWithIndex:row] byExtendingSelection:NO];
    [self.table scrollRowToVisible:row];
}

- (void)controlTextDidChange:(NSNotification *)notification
{
    [self filter];
}

/* The field has the keyboard: the keys that are the list's */
- (BOOL)control:(NSControl *)control textView:(NSTextView *)view doCommandBySelector:(SEL)selector
{
    if (selector == @selector(moveUp:))
        [self select:self.table.selectedRow - 1];
    else if (selector == @selector(moveDown:))
        [self select:self.table.selectedRow + 1];
    else if (selector == @selector(scrollPageUp:) || selector == @selector(pageUp:))
        [self select:self.table.selectedRow - MAX_ROWS];
    else if (selector == @selector(scrollPageDown:) || selector == @selector(pageDown:))
        [self select:self.table.selectedRow + MAX_ROWS];
    else if (selector == @selector(insertNewline:))
        [self runSelected];
    else if (selector == @selector(cancelOperation:))
        [self close];
    else
        return NO;
    return YES;
}

/* E.g. a click in another window */
- (void)windowDidResignKey:(NSNotification *)notification
{
    [self close];
}

- (NSInteger)numberOfRowsInTableView:(NSTableView *)table
{
    return self.shown.count;
}

- (NSView *)tableView:(NSTableView *)table viewForTableColumn:(NSTableColumn *)column row:(NSInteger)row
{
    FootCommand *command = self.shown[row];

    if ([column.identifier isEqualToString:@"title"]) {
        NSTableCellView *cell = cell_view(table, @"title", NSTextAlignmentNatural);
        cell.textField.stringValue = command.title;
        return cell;
    }

    NSTableCellView *cell = cell_view(table, @"shortcut", NSTextAlignmentRight);
    cell.textField.stringValue = command.shortcut.length == 0 && command.checked
        ? @"✓" : command.shortcut;
    cell.textField.textColor = NSColor.secondaryLabelColor;
    return cell;
}

@end

static FootPalette *palette;

void
palette_toggle(void)
{
    if (palette == nil)
        palette = [[FootPalette alloc] init];

    if (palette.open)
        [palette close];
    else
        [palette show];
}

bool
palette_is_open(void)
{
    return palette.open;
}
