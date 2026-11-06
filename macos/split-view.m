#import "split-view.h"

#import "app.h"
#import "session-layout.h"
#import "view.h"

#include <math.h>

#define LOG_MODULE "splits"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "debug.h"
#include "terminal.h"

/* Moving a divider with the keyboard (points) */
#define RESIZE_STEP 10

/* The divider can be grabbed this far (points) on each side of it */
#define DIVIDER_GRAB 3

/* How much of the terminal shows through in panes without focus (as Ghostty) */
#define UNFOCUSED_OPACITY .7

static FootView *
view_of(const struct wl_window *win)
{
    return (__bridge FootView *)win->view;
}

static CGColorRef
create_color(uint32_t rgb, double alpha)
{
    return CGColorCreateSRGB(
        ((rgb >> 16) & 0xff) / 255., ((rgb >> 8) & 0xff) / 255., (rgb & 0xff) / 255., alpha);
}

/* 'a', with 't' of 'b' */
static uint32_t
mix(uint32_t a, uint32_t b, double t)
{
    uint32_t rgb = 0;
    for (int shift = 0; shift <= 16; shift += 8) {
        const double ca = (a >> shift) & 0xff;
        const double cb = (b >> shift) & 0xff;
        rgb |= (uint32_t)lround(ca + (cb - ca) * t) << shift;
    }
    return rgb;
}

static NSRect
rect_of(struct split_rect r)
{
    return NSMakeRect(r.x, r.y, r.width, r.height);
}

/*
 * Dividers
 */

@interface FootDividerView : NSView
@property (nonatomic, weak) FootSplitView *splits;
@property (nonatomic) struct split *node;  /* NULL once removed */
- (void)setColor:(CGColorRef)color;
@end

@interface FootSplitView ()
- (void)dividerDragBegan;
- (void)dividerDragEnded;
- (void)moveDivider:(struct split *)node to:(int)pos;
@end

@implementation FootDividerView {
    CALayer *_line;
    NSTrackingArea *_trackingArea;
    int _grab;  /* Where the mouse is on the divider, while dragging */
}

- (instancetype)initWithFrame:(NSRect)frame
{
    self = [super initWithFrame:frame];
    if (self == nil)
        return nil;

    /* Layer hosting, like FootView; nothing animates */
    CALayer *layer = [CALayer layer];
    NSNull *null = [NSNull null];
    layer.actions = @{@"bounds": null, @"position": null, @"frame": null,
                      @"hidden": null, @"sublayers": null};
    self.layer = layer;
    self.wantsLayer = YES;

    _line = view_create_layer();
    _line.hidden = NO;
    [layer addSublayer:_line];
    return self;
}

- (BOOL)isFlipped
{
    return YES;
}

- (BOOL)mouseDownCanMoveWindow
{
    return NO;
}

- (BOOL)horizontal
{
    return _node == NULL || _node->kind == SPLIT_HORIZONTAL;
}

/* The line in the middle, the rest is for grabbing it */
- (void)updateLine
{
    const NSSize size = self.bounds.size;
    _line.frame = [self horizontal]
        ? CGRectMake(DIVIDER_GRAB, 0, SPLIT_DIVIDER, size.height)
        : CGRectMake(0, DIVIDER_GRAB, size.width, SPLIT_DIVIDER);
}

- (void)setNode:(struct split *)node
{
    _node = node;
    [self updateLine];
}

- (void)setFrameSize:(NSSize)size
{
    [super setFrameSize:size];
    [self updateLine];
}

- (void)setColor:(CGColorRef)color
{
    if (!CGColorEqualToColor(_line.backgroundColor, color))
        _line.backgroundColor = color;
}

- (NSCursor *)cursor
{
    if (@available(macOS 15, *)) {
        return [self horizontal] ? [NSCursor columnResizeCursor] : [NSCursor rowResizeCursor];
    }
    return [self horizontal] ? [NSCursor resizeLeftRightCursor] : [NSCursor resizeUpDownCursor];
}

- (void)updateTrackingAreas
{
    if (_trackingArea != nil)
        [self removeTrackingArea:_trackingArea];

    _trackingArea = [[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:(NSTrackingMouseEnteredAndExited |
                      NSTrackingMouseMoved |
                      NSTrackingCursorUpdate |
                      NSTrackingActiveInActiveApp |
                      NSTrackingInVisibleRect)
               owner:self
            userInfo:nil];

    [self addTrackingArea:_trackingArea];
    [super updateTrackingAreas];
}

- (void)cursorUpdate:(NSEvent *)event   { [[self cursor] set]; }
- (void)mouseEntered:(NSEvent *)event   { [[self cursor] set]; }
- (void)mouseMoved:(NSEvent *)event     { [[self cursor] set]; }

/* Along the split's direction, in the split view's coordinates */
- (int)positionOf:(NSEvent *)event
{
    const NSPoint p = [self.superview convertPoint:event.locationInWindow fromView:nil];
    return (int)floor([self horizontal] ? p.x : p.y);
}

- (void)mouseDown:(NSEvent *)event
{
    if (_node == NULL)
        return;

    const struct split_rect divider = split_divider(_node);
    _grab = [self positionOf:event] - ([self horizontal] ? divider.x : divider.y);
    [_splits dividerDragBegan];
}

- (void)mouseDragged:(NSEvent *)event
{
    [[self cursor] set];
    if (_node != NULL)
        [_splits moveDivider:_node to:[self positionOf:event] - _grab];
}

- (void)mouseUp:(NSEvent *)event
{
    [_splits dividerDragEnded];
}

@end

/*
 * The split view
 */

@implementation FootSplitView {
    struct split *_root;
    struct split *_zoomed;
    NSMutableArray<FootDividerView *> *_dividers;
    BOOL _windowOccluded;
}

- (instancetype)initWithPane:(struct wl_window *)win
{
    FootView *view = view_of(win);

    self = [super initWithFrame:view.frame];
    if (self == nil)
        return nil;

    self.wantsLayer = YES;
    self.autoresizesSubviews = NO;

    _root = split_new(win);
    _dividers = [NSMutableArray array];
    _focused = win;

    win->splits = (__bridge void *)self;
    [self addSubview:view];
    return self;
}

- (void)dealloc
{
    split_free(_root);
}

- (BOOL)isFlipped
{
    return YES;
}

- (size_t)count
{
    return split_count(_root);
}

- (struct split *)root
{
    return _root;
}

- (void)setRatio:(double)ratio ofSplitAbove:(struct wl_window *)win
{
    struct split *leaf = [self leafOf:win];
    if (leaf == NULL || leaf->parent == NULL || !(ratio > 0. && ratio < 1.))
        return;

    leaf->parent->ratio = ratio;
    [self relayout];
}

- (struct split *)leafOf:(const struct wl_window *)win
{
    for (struct split *it = split_first(_root); it != NULL; it = split_following(it)) {
        if (it->pane == win)
            return it;
    }
    return NULL;
}

- (NSArray<FootView *> *)panes
{
    NSMutableArray<FootView *> *panes = [NSMutableArray array];
    for (struct split *it = split_first(_root); it != NULL; it = split_following(it))
        [panes addObject:view_of(it->pane)];
    return panes;
}

/*
 * Layout
 */

- (void)setFrameSize:(NSSize)size
{
    [super setFrameSize:size];
    [self relayout];
}

- (void)relayout
{
    if (_root == NULL)
        return;

    const NSSize size = self.bounds.size;
    split_layout(_root, (struct split_rect){0, 0, (int)size.width, (int)size.height});

    for (struct split *it = split_first(_root); it != NULL; it = split_following(it)) {
        FootView *view = view_of(it->pane);
        const BOOL hidden = _zoomed != NULL && it != _zoomed;

        if (view.hidden != hidden) {
            view.hidden = hidden;
            [view setOccluded:_windowOccluded || hidden];
        }

        /* Zoomed out panes keep their size (their programs aren't bothered) */
        if (hidden)
            continue;

        NSRect frame = _zoomed != NULL ? self.bounds : rect_of(it->frame);

        /* Up to the window's edges, also where it's not whole points */
        if (it->frame.x + it->frame.width >= (int)size.width)
            frame.size.width = size.width - NSMinX(frame);
        if (it->frame.y + it->frame.height >= (int)size.height)
            frame.size.height = size.height - NSMinY(frame);

        if (!NSEqualRects(view.frame, frame))
            view.frame = frame;
    }

    [self updateDividers];
    [self updateDecorations];

    /* Sessions: the panes, and their sizes, are kept */
    session_layout_changed();
}

- (void)collectSplits:(struct split *)node into:(NSMutableArray<NSValue *> *)splits
{
    if (node->kind == SPLIT_LEAF)
        return;

    [splits addObject:[NSValue valueWithPointer:node]];
    [self collectSplits:node->first into:splits];
    [self collectSplits:node->second into:splits];
}

- (void)updateDividers
{
    NSMutableArray<NSValue *> *splits = [NSMutableArray array];
    if (_root != NULL)
        [self collectSplits:_root into:splits];

    while (_dividers.count > splits.count) {
        FootDividerView *divider = _dividers.lastObject;
        divider.node = NULL;
        [divider removeFromSuperview];
        [_dividers removeLastObject];
    }

    while (_dividers.count < splits.count) {
        FootDividerView *divider = [[FootDividerView alloc] initWithFrame:NSZeroRect];
        divider.splits = self;
        [self addSubview:divider positioned:NSWindowAbove relativeTo:nil];
        [_dividers addObject:divider];
    }

    for (NSUInteger i = 0; i < splits.count; i++) {
        FootDividerView *divider = _dividers[i];
        struct split *node = splits[i].pointerValue;
        const struct split_rect r = split_divider(node);

        divider.node = node;
        divider.hidden = _zoomed != NULL;

        const NSRect frame = node->kind == SPLIT_HORIZONTAL
            ? NSMakeRect(r.x - DIVIDER_GRAB, r.y, r.width + 2 * DIVIDER_GRAB, r.height)
            : NSMakeRect(r.x, r.y - DIVIDER_GRAB, r.width, r.height + 2 * DIVIDER_GRAB);

        if (!NSEqualRects(divider.frame, frame))
            divider.frame = frame;
    }
}

/* Panes without the focus are dimmed; the dividers have the focused pane's colors */
- (void)updateDecorations
{
    const BOOL many = self.count > 1;

    for (struct split *it = split_first(_root); it != NULL; it = split_following(it)) {
        const struct wl_window *win = it->pane;
        const BOOL dimmed = many && win != _focused && _zoomed == NULL;

        CGColorRef color = dimmed
            ? create_color(win->term->colors.bg, 1. - UNFOCUSED_OPACITY) : NULL;
        [view_of(win) setDimColor:color];
        CGColorRelease(color);
    }

    const struct wl_window *colors = _focused != NULL
        ? _focused : (_root != NULL ? split_first(_root)->pane : NULL);
    if (colors == NULL || _dividers.count == 0)
        return;

    const struct terminal *term = colors->term;
    CGColorRef color = create_color(mix(term->colors.bg, term->colors.fg, .2), 1.);
    for (FootDividerView *divider in _dividers)
        [divider setColor:color];
    CGColorRelease(color);
}

- (void)paneColorsChanged:(struct wl_window *)win
{
    if ([self leafOf:win] != NULL)
        [self updateDecorations];
}

/* Panes in splits get the size they're given, as tiled windows do */
- (void)updateTiling
{
    const BOOL many = self.count > 1;
    for (struct split *it = split_first(_root); it != NULL; it = split_following(it)) {
        struct wl_window *win = it->pane;
        win->is_tiled = win->quake || many;
    }
}

- (void)updateMinimumSize
{
    if (_root == NULL || self.window == nil)
        return;

    int width, height;
    split_min_size(_root, &width, &height);
    self.window.contentMinSize = NSMakeSize(width, height);
}

- (void)setMinimumSize:(NSSize)size ofPane:(struct wl_window *)win
{
    struct split *leaf = [self leafOf:win];
    if (leaf == NULL)
        return;

    leaf->min_width = (int)ceil(size.width);
    leaf->min_height = (int)ceil(size.height);
    [self updateMinimumSize];
}

- (void)setWindowOccluded:(BOOL)occluded
{
    _windowOccluded = occluded;
    for (FootView *view in self.panes)
        [view setOccluded:occluded || view.hidden];
}

- (void)setZoomed:(struct split *)leaf
{
    if (_zoomed == leaf)
        return;

    _zoomed = leaf;
    [self relayout];
}

- (BOOL)isDividerAt:(NSPoint)point
{
    for (FootDividerView *divider in _dividers) {
        if (!divider.hidden &&
            NSPointInRect([divider convertPoint:point fromView:nil], divider.bounds))
        {
            return YES;
        }
    }
    return NO;
}

/*
 * Panes
 */

- (void)insertPane:(struct wl_window *)win next:(struct wl_window *)to
         direction:(enum split_dir)dir
{
    struct split *leaf = [self leafOf:to];
    xassert(leaf != NULL);

    _zoomed = NULL;

    struct split *new = split_insert(&_root, leaf, win, dir);
    new->min_width = leaf->min_width;
    new->min_height = leaf->min_height;

    win->splits = (__bridge void *)self;

    /* Below the dividers. Laid out once configured (see window_configure()) */
    [self addSubview:view_of(win) positioned:NSWindowBelow relativeTo:nil];
    [self updateTiling];
}

- (BOOL)canSplit:(struct wl_window *)win direction:(enum split_dir)dir
{
    const struct split *leaf = [self leafOf:win];
    if (leaf == NULL)
        return NO;

    if (dir == SPLIT_RIGHT || dir == SPLIT_LEFT)
        return leaf->frame.width >= 2 * leaf->min_width + SPLIT_DIVIDER;
    else
        return leaf->frame.height >= 2 * leaf->min_height + SPLIT_DIVIDER;
}

- (void)removePane:(struct wl_window *)win
{
    struct split *leaf = [self leafOf:win];
    if (leaf == NULL)
        return;

    if (_zoomed == leaf)
        _zoomed = NULL;
    if (_sheetPane == win)
        _sheetPane = NULL;

    const BOOL was_focused = _focused == win;
    if (was_focused)
        _focused = NULL;

    struct split *next = split_remove(&_root, leaf);
    [view_of(win) removeFromSuperview];
    win->splits = NULL;

    if (_root == NULL) {
        [self updateDividers];
        return;
    }

    /*
     * The terminal is being destroyed (in an FDM handler): lay out,
     * and render, the remaining ones once back in the run loop
     */
    __weak FootSplitView *weak_self = self;
    __weak FootView *weak_next = was_focused ? view_of(next->pane) : nil;
    CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{
        FootSplitView *splits = weak_self;
        if (splits == nil)
            return;

        [splits updateTiling];
        [splits updateMinimumSize];
        [splits relayout];

        FootView *view = weak_next;
        if (view != nil && view.win != NULL && splits.focused == NULL &&
            view.win->splits == (__bridge void *)splits)
        {
            [splits focusPane:view.win];
        }
        app_run_hooks();
    });
    CFRunLoopWakeUp(CFRunLoopGetMain());
}

/*
 * Focus
 */

- (void)focusPane:(struct wl_window *)win
{
    struct split *leaf = [self leafOf:win];
    if (leaf == NULL)
        return;

    if (_zoomed != NULL && _zoomed != leaf)
        [self setZoomed:NULL];

    FootView *view = view_of(win);
    if (self.window != nil && self.window.firstResponder != view)
        [self.window makeFirstResponder:view];
    else
        [self paneBecameFirstResponder:win];
}

- (void)paneBecameFirstResponder:(struct wl_window *)win
{
    struct wl_window *old = _focused;
    if (old == win || [self leafOf:win] == NULL)
        return;

    _focused = win;

    if (self.window.isKeyWindow) {
        if (old != NULL)
            window_focus_out(old);
        window_focus_in(win);
    }

    if (old != NULL)
        [view_of(old) resetInputMethod];

    self.window.title = win->title != NULL
        ? [NSString stringWithUTF8String:win->title] : @"foot";

    [self updateDecorations];
    session_layout_changed();
    app_run_hooks();
}

/*
 * Dragging dividers: the terminals resize as while the window is
 * resized (e.g. resize-delay-ms)
 */

- (void)dividerDragBegan
{
    _dragging = YES;
    for (FootView *view in self.panes) {
        if (view.win != NULL)
            view.win->is_resizing = true;
    }
}

- (void)dividerDragEnded
{
    if (!_dragging)
        return;

    _dragging = NO;
    for (FootView *view in self.panes) {
        if (view.win != NULL)
            window_live_resize_ended(view.win);
    }
    app_run_hooks();
}

- (void)moveDivider:(struct split *)node to:(int)pos
{
    if (split_move_divider(node, pos))
        [self relayout];
}

/*
 * Menu actions (the focused pane is the first responder; this is
 * next in the responder chain)
 */

- (struct split *)focusedLeaf
{
    return _focused != NULL ? [self leafOf:_focused] : NULL;
}

- (void)split:(enum split_dir)dir
{
    struct wl_window *win = _focused;
    if (win == NULL || win->quake)
        return;

    if (![self canSplit:win direction:dir]) {
        NSBeep();
        return;
    }

    app_new_split(win, dir);
}

- (void)splitRight:(id)sender  { [self split:SPLIT_RIGHT]; }
- (void)splitDown:(id)sender   { [self split:SPLIT_DOWN]; }
- (void)splitLeft:(id)sender   { [self split:SPLIT_LEFT]; }
- (void)splitUp:(id)sender     { [self split:SPLIT_UP]; }

- (void)selectSplit:(struct split *)leaf
{
    if (leaf != NULL)
        [self focusPane:leaf->pane];
}

- (void)selectNextSplit:(id)sender
{
    struct split *leaf = [self focusedLeaf];
    if (leaf != NULL)
        [self selectSplit:split_next(_root, leaf)];
}

- (void)selectPreviousSplit:(id)sender
{
    struct split *leaf = [self focusedLeaf];
    if (leaf != NULL)
        [self selectSplit:split_prev(_root, leaf)];
}

- (void)selectSplitInDirection:(enum split_dir)dir
{
    struct split *leaf = [self focusedLeaf];
    if (leaf == NULL)
        return;

    /* The neighbors as they are without the zoom */
    [self setZoomed:NULL];

    struct split *neighbor = split_neighbor(_root, leaf, dir);
    if (neighbor != NULL)
        [self selectSplit:neighbor];
    else
        NSBeep();
}

- (void)selectSplitAbove:(id)sender  { [self selectSplitInDirection:SPLIT_UP]; }
- (void)selectSplitBelow:(id)sender  { [self selectSplitInDirection:SPLIT_DOWN]; }
- (void)selectSplitLeft:(id)sender   { [self selectSplitInDirection:SPLIT_LEFT]; }
- (void)selectSplitRight:(id)sender  { [self selectSplitInDirection:SPLIT_RIGHT]; }

- (void)toggleSplitZoom:(id)sender
{
    struct split *leaf = [self focusedLeaf];
    if (leaf != NULL && self.count > 1)
        [self setZoomed:_zoomed != NULL ? NULL : leaf];
}

- (void)equalizeSplits:(id)sender
{
    if (_root == NULL)
        return;

    [self setZoomed:NULL];
    split_equalize(_root);
    [self relayout];
}

- (void)moveDividerInDirection:(enum split_dir)dir
{
    struct split *leaf = [self focusedLeaf];
    if (leaf == NULL)
        return;

    [self setZoomed:NULL];
    if (split_resize(leaf, dir, RESIZE_STEP))
        [self relayout];
    else
        NSBeep();
}

- (void)moveDividerUp:(id)sender     { [self moveDividerInDirection:SPLIT_UP]; }
- (void)moveDividerDown:(id)sender   { [self moveDividerInDirection:SPLIT_DOWN]; }
- (void)moveDividerLeft:(id)sender   { [self moveDividerInDirection:SPLIT_LEFT]; }
- (void)moveDividerRight:(id)sender  { [self moveDividerInDirection:SPLIT_RIGHT]; }

/* Close (Cmd+W): the focused pane, if there are others; else the window (or tab) */
- (void)performClose:(id)sender
{
    if (self.count > 1 && _focused != NULL)
        window_close(_focused);
    else
        [self.window performClose:sender];
    app_run_hooks();
}

/* Close Window (Cmd+Shift+W): all tabs, and all their panes */
- (void)performCloseWindow:(id)sender
{
    NSWindow *window = self.window;
    NSArray<NSWindow *> *windows = window.tabbedWindows != nil
        ? window.tabbedWindows : @[window];
    window_close_all(window, windows);
    app_run_hooks();
}

- (BOOL)validateMenuItem:(NSMenuItem *)item
{
    const SEL action = item.action;
    const BOOL many = self.count > 1;
    const BOOL quake = _focused != NULL && _focused->quake;

    if (action == @selector(performClose:)) {
        if (many) {
            item.title = @"Close Split";
            return YES;
        }
        return [self.window validateMenuItem:item];
    }

    if (action == @selector(performCloseWindow:))
        return !quake;

    if (action == @selector(splitRight:) || action == @selector(splitDown:) ||
        action == @selector(splitLeft:) || action == @selector(splitUp:))
    {
        return _focused != NULL && !quake;
    }

    if (action == @selector(toggleSplitZoom:)) {
        item.state = _zoomed != NULL ? NSControlStateValueOn : NSControlStateValueOff;
        return many;
    }

    if (action == @selector(selectNextSplit:) || action == @selector(selectPreviousSplit:) ||
        action == @selector(selectSplitAbove:) || action == @selector(selectSplitBelow:) ||
        action == @selector(selectSplitLeft:) || action == @selector(selectSplitRight:) ||
        action == @selector(equalizeSplits:) ||
        action == @selector(moveDividerUp:) || action == @selector(moveDividerDown:) ||
        action == @selector(moveDividerLeft:) || action == @selector(moveDividerRight:))
    {
        return many;
    }

    return YES;
}

@end
