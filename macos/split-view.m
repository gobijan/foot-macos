#import "split-view.h"

#import "app.h"
#import "session-layout.h"
#import "view.h"

#import <IOSurface/IOSurface.h>

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

/*
 * The grab handles at the top of the panes (as Ghostty's): their size,
 * and the part of the pane (from the top) where the mouse shows them
 */
#define HANDLE_WIDTH 80
#define HANDLE_HEIGHT 12
#define HANDLE_REVEAL .2

/* The handle's dots: their diameter, the space between them, their opacity */
#define HANDLE_DOT 3
#define HANDLE_DOT_GAP 2.5
#define HANDLE_OPACITY .35
#define HANDLE_OPACITY_HOVER .85

/* The pane being dragged, shown at this scale (as Ghostty) */
#define DRAG_IMAGE_SCALE .2

/* What a pane's handle puts on the pasteboard (the pane is the drag's source) */
static NSPasteboardType const pane_pasteboard_type = @"io.github.gobijan.foot.pane";

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
 * Grab handles: one at the top of each pane (with several). Dragging one
 * moves its pane next to another one, on the side closest to the mouse
 * (highlighted meanwhile). Its dots show while the mouse is in the top of
 * the pane.
 */

@interface FootGrabHandle : NSView <NSDraggingSource>
@property (nonatomic, weak) FootSplitView *splits;
@property (nonatomic) struct wl_window *pane;  /* NULL once removed */
@property (nonatomic) BOOL revealed;
- (void)setColor:(CGColorRef)color;
@end

@interface FootSplitView ()
- (void)handleDragEnded;
@end

/* The terminal's current frame, with the root layer's background around it */
static NSImage *
pane_image(FootView *view)
{
    const NSSize size = view.bounds.size;
    if (size.width < 1 || size.height < 1)
        return nil;

    NSImage *image = [[NSImage alloc] initWithSize:size];
    NSBitmapImageRep *rep = [[NSBitmapImageRep alloc]
        initWithBitmapDataPlanes:NULL
                      pixelsWide:(NSInteger)size.width
                      pixelsHigh:(NSInteger)size.height
                   bitsPerSample:8
                 samplesPerPixel:4
                        hasAlpha:YES
                        isPlanar:NO
                  colorSpaceName:NSDeviceRGBColorSpace
                     bytesPerRow:0
                    bitsPerPixel:0];
    if (rep == nil)
        return nil;

    NSGraphicsContext *gc = [NSGraphicsContext graphicsContextWithBitmapImageRep:rep];
    CGContextRef cg = gc.CGContext;

    CGColorRef bg = view.rootLayer.backgroundColor;
    if (bg != NULL) {
        CGContextSetFillColorWithColor(cg, bg);
        CGContextFillRect(cg, CGRectMake(0, 0, size.width, size.height));
    }

    /* A copy: the renderer writes to the surface while it's dragged */
    id contents = view.gridLayer.contents;
    if (contents != nil && CFGetTypeID((__bridge CFTypeRef)contents) == IOSurfaceGetTypeID()) {
        IOSurfaceRef surface = (__bridge IOSurfaceRef)contents;
        IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);

        const size_t width = IOSurfaceGetWidth(surface);
        const size_t height = IOSurfaceGetHeight(surface);
        const size_t stride = IOSurfaceGetBytesPerRow(surface);
        CFDataRef data = CFDataCreate(NULL, IOSurfaceGetBaseAddress(surface), stride * height);

        IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);

        CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
        CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGImageRef grid = CGImageCreate(
            width, height, 8, 32, stride, srgb,
            kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little,
            provider, NULL, false, kCGRenderingIntentDefault);

        /* At the top left; the bitmap's origin is at the bottom */
        const CGRect frame = view.gridLayer.frame;
        if (grid != NULL) {
            CGContextDrawImage(cg, CGRectMake(
                frame.origin.x, size.height - frame.origin.y - frame.size.height,
                frame.size.width, frame.size.height), grid);
        }

        CGImageRelease(grid);
        CGColorSpaceRelease(srgb);
        CGDataProviderRelease(provider);
        CFRelease(data);
    }

    [gc flushGraphics];
    [image addRepresentation:rep];
    return image;
}

@implementation FootGrabHandle {
    CALayer *_dots;
    NSTrackingArea *_trackingArea;
    BOOL _hovering;
    BOOL _dragging;
}

- (instancetype)initWithFrame:(NSRect)frame
{
    self = [super initWithFrame:frame];
    if (self == nil)
        return nil;

    /* Layer hosting; only the dots' opacity animates (a fade) */
    CALayer *layer = [CALayer layer];
    NSNull *null = [NSNull null];
    layer.actions = @{@"bounds": null, @"position": null, @"frame": null,
                      @"hidden": null, @"sublayers": null};
    self.layer = layer;
    self.wantsLayer = YES;

    _dots = [CALayer layer];
    _dots.actions = @{@"bounds": null, @"position": null, @"frame": null,
                      @"sublayers": null};
    _dots.opacity = 0.;
    for (int i = 0; i < 3; i++) {
        CALayer *dot = view_create_layer();
        dot.hidden = NO;
        dot.cornerRadius = HANDLE_DOT / 2.;
        dot.frame = CGRectMake(i * (HANDLE_DOT + HANDLE_DOT_GAP), 0, HANDLE_DOT, HANDLE_DOT);
        [_dots addSublayer:dot];
    }
    [layer addSublayer:_dots];
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

/* A pane of a window in the background can be dragged too */
- (BOOL)acceptsFirstMouse:(NSEvent *)event
{
    return YES;
}

/* The dots in the middle */
- (void)layoutDots
{
    const NSSize size = self.bounds.size;
    const CGFloat width = 3 * HANDLE_DOT + 2 * HANDLE_DOT_GAP;
    _dots.frame = CGRectMake(
        round((size.width - width) / 2), round((size.height - HANDLE_DOT) / 2),
        width, HANDLE_DOT);
}

- (void)setFrameSize:(NSSize)size
{
    [super setFrameSize:size];
    [self layoutDots];
}

- (void)setColor:(CGColorRef)color
{
    for (CALayer *dot in _dots.sublayers) {
        if (!CGColorEqualToColor(dot.backgroundColor, color))
            dot.backgroundColor = color;
    }
}

- (void)updateOpacity
{
    const float opacity = _hovering || _dragging
        ? HANDLE_OPACITY_HOVER : (_revealed ? HANDLE_OPACITY : 0.);

    if (_dots.opacity != opacity) {
        [CATransaction begin];
        [CATransaction setAnimationDuration:.15];
        _dots.opacity = opacity;
        [CATransaction commit];
    }
}

- (void)setRevealed:(BOOL)revealed
{
    _revealed = revealed;
    [self updateOpacity];
}

- (void)setHidden:(BOOL)hidden
{
    [super setHidden:hidden];
    if (hidden) {
        _hovering = NO;
        [self updateOpacity];
    }
}

- (void)updateTrackingAreas
{
    if (_trackingArea != nil)
        [self removeTrackingArea:_trackingArea];

    _trackingArea = [[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:(NSTrackingMouseEnteredAndExited |
                      NSTrackingCursorUpdate |
                      NSTrackingActiveInActiveApp |
                      NSTrackingInVisibleRect)
               owner:self
            userInfo:nil];

    [self addTrackingArea:_trackingArea];
    [super updateTrackingAreas];
}

- (void)cursorUpdate:(NSEvent *)event
{
    [(_dragging ? [NSCursor closedHandCursor] : [NSCursor openHandCursor]) set];
}

- (void)mouseEntered:(NSEvent *)event
{
    _hovering = YES;
    [self updateOpacity];
    [self cursorUpdate:event];
}

- (void)mouseExited:(NSEvent *)event
{
    _hovering = NO;
    [self updateOpacity];
}

/* Not the terminal's (nor the window's: it doesn't move) */
- (void)mouseDown:(NSEvent *)event
{
}

- (void)mouseDragged:(NSEvent *)event
{
    if (_dragging || _pane == NULL)
        return;

    NSPasteboardItem *item = [[NSPasteboardItem alloc] init];
    [item setString:@"" forType:pane_pasteboard_type];
    NSDraggingItem *dragging = [[NSDraggingItem alloc] initWithPasteboardWriter:item];

    /* The pane, small, with the mouse in its middle (as tabs) */
    FootView *view = (__bridge FootView *)_pane->view;
    NSImage *image = pane_image(view);
    const NSSize size = NSMakeSize(
        round(NSWidth(view.bounds) * DRAG_IMAGE_SCALE),
        round(NSHeight(view.bounds) * DRAG_IMAGE_SCALE));
    const NSPoint mouse = [self convertPoint:event.locationInWindow fromView:nil];
    [dragging setDraggingFrame:NSMakeRect(
        mouse.x - size.width / 2, mouse.y - size.height / 2, size.width, size.height)
                      contents:image];

    _dragging = YES;
    [self updateOpacity];

    NSDraggingSession *session =
        [self beginDraggingSessionWithItems:@[dragging] event:event source:self];
    session.animatesToStartingPositionsOnCancelOrFail = YES;
}

- (NSDragOperation)draggingSession:(NSDraggingSession *)session
    sourceOperationMaskForDraggingContext:(NSDraggingContext)context
{
    return context == NSDraggingContextWithinApplication
        ? NSDragOperationMove : NSDragOperationNone;
}

- (void)draggingSession:(NSDraggingSession *)session movedToPoint:(NSPoint)point
{
    [[NSCursor closedHandCursor] set];
}

- (void)draggingSession:(NSDraggingSession *)session
           endedAtPoint:(NSPoint)point
              operation:(NSDragOperation)operation
{
    _dragging = NO;

    /* Where it ended (the pane may have moved away) */
    const NSPoint p = [self convertPoint:[self.window convertPointFromScreen:point] fromView:nil];
    _hovering = !self.hidden && NSPointInRect(p, self.bounds);
    [self updateOpacity];

    [_splits handleDragEnded];
}

@end

/* Where a pane dropped at 'p' on 'r' goes: the side of 'r' closest to it */
static enum split_dir
drop_side(NSPoint p, struct split_rect r)
{
    const double left = (p.x - r.x) / r.width;
    const double top = (p.y - r.y) / r.height;
    const double closest = MIN(MIN(left, 1. - left), MIN(top, 1. - top));

    if (closest == left)
        return SPLIT_LEFT;
    if (closest == 1. - left)
        return SPLIT_RIGHT;
    if (closest == top)
        return SPLIT_UP;
    return SPLIT_DOWN;
}

/*
 * The split view
 */

@implementation FootSplitView {
    struct split *_root;
    struct split *_zoomed;
    NSMutableArray<FootDividerView *> *_dividers;
    NSMutableArray<FootGrabHandle *> *_handles;
    NSView *_dropZone;
    NSTrackingArea *_trackingArea;
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
    _handles = [NSMutableArray array];
    _focused = win;

    win->splits = (__bridge void *)self;
    [self addSubview:view];

    /* Panes dragged by their handles */
    [self registerForDraggedTypes:@[pane_pasteboard_type]];
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
    [self updateHandles];
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

/* A handle at the top of each pane, while there are several (and none is zoomed) */
- (void)updateHandles
{
    const NSUInteger count = _root != NULL && self.count > 1 ? self.count : 0;

    while (_handles.count > count) {
        FootGrabHandle *handle = _handles.lastObject;
        handle.pane = NULL;
        [handle removeFromSuperview];
        [_handles removeLastObject];
    }

    while (_handles.count < count) {
        FootGrabHandle *handle = [[FootGrabHandle alloc] initWithFrame:NSZeroRect];
        handle.splits = self;
        [self addSubview:handle positioned:NSWindowAbove relativeTo:nil];
        [_handles addObject:handle];
    }

    NSUInteger i = 0;
    for (struct split *it = split_first(_root); it != NULL && i < count; it = split_following(it)) {
        FootGrabHandle *handle = _handles[i++];
        handle.pane = it->pane;
        handle.hidden = _zoomed != NULL;

        const struct split_rect r = it->frame;
        const int width = MIN(HANDLE_WIDTH, r.width);
        const NSRect frame = NSMakeRect(r.x + (r.width - width) / 2, r.y, width, HANDLE_HEIGHT);

        if (!NSEqualRects(handle.frame, frame))
            handle.frame = frame;
    }

    /* Where the mouse is now */
    if (self.window != nil) {
        const NSPoint p = [self convertPoint:self.window.mouseLocationOutsideOfEventStream fromView:nil];
        [self revealHandlesAt:p];
    }
}

/* The handle's dots show while the mouse is in the top of its pane */
- (void)revealHandlesAt:(NSPoint)point
{
    const BOOL inside = NSPointInRect(point, self.bounds);

    for (FootGrabHandle *handle in _handles) {
        BOOL reveal = NO;
        const struct split *leaf = handle.pane != NULL ? [self leafOf:handle.pane] : NULL;

        if (inside && leaf != NULL && !handle.hidden) {
            const struct split_rect r = leaf->frame;
            const double height = MIN(r.height, MAX(HANDLE_HEIGHT, r.height * HANDLE_REVEAL));
            reveal = NSPointInRect(point, NSMakeRect(r.x, r.y, r.width, height));
        }

        if (handle.revealed != reveal)
            handle.revealed = reveal;
    }
}

- (void)updateTrackingAreas
{
    if (_trackingArea != nil)
        [self removeTrackingArea:_trackingArea];

    _trackingArea = [[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:(NSTrackingMouseEnteredAndExited |
                      NSTrackingMouseMoved |
                      NSTrackingActiveInActiveApp |
                      NSTrackingInVisibleRect)
               owner:self
            userInfo:nil];

    [self addTrackingArea:_trackingArea];
    [super updateTrackingAreas];
}

- (void)mouseEntered:(NSEvent *)event
{
    [self revealHandlesAt:[self convertPoint:event.locationInWindow fromView:nil]];
}

- (void)mouseMoved:(NSEvent *)event
{
    [self revealHandlesAt:[self convertPoint:event.locationInWindow fromView:nil]];
}

- (void)mouseExited:(NSEvent *)event
{
    [self revealHandlesAt:NSMakePoint(-1, -1)];
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

    /* The handles have their pane's foreground color */
    for (FootGrabHandle *handle in _handles) {
        if (handle.pane == NULL || [self leafOf:handle.pane] == NULL)
            continue;

        CGColorRef fg = create_color(handle.pane->term->colors.fg, 1.);
        [handle setColor:fg];
        CGColorRelease(fg);
    }
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

- (BOOL)isDividerOrHandleAt:(NSPoint)point
{
    for (NSArray<NSView *> *views in @[_dividers, _handles]) {
        for (NSView *view in views) {
            if (!view.hidden &&
                NSPointInRect([view convertPoint:point fromView:nil], view.bounds))
            {
                return YES;
            }
        }
    }
    return NO;
}

/*
 * Dropping a pane (dragged by its handle): next to the one it's over,
 * on the side closest to the mouse (as Ghostty)
 */

- (struct split *)leafAt:(NSPoint)point
{
    for (struct split *it = split_first(_root); it != NULL; it = split_following(it)) {
        if (NSPointInRect(point, rect_of(it->frame)))
            return it;
    }
    return NULL;
}

/* The pane dragged by a handle of this view, and where it'd go; NULL: nowhere */
- (struct split *)dropOf:(id<NSDraggingInfo>)info
                    into:(struct split **)target
                    side:(enum split_dir *)side
{
    id source = info.draggingSource;
    if (![source isKindOfClass:[FootGrabHandle class]] ||
        ((FootGrabHandle *)source).splits != self || _zoomed != NULL)
    {
        return NULL;
    }

    struct wl_window *pane = ((FootGrabHandle *)source).pane;
    struct split *leaf = pane != NULL ? [self leafOf:pane] : NULL;
    if (leaf == NULL)
        return NULL;

    const NSPoint p = [self convertPoint:info.draggingLocation fromView:nil];
    struct split *to = [self leafAt:p];
    if (to == NULL || to == leaf)
        return NULL;

    /* Room for both */
    const enum split_dir dir = drop_side(p, to->frame);
    const bool fits = dir == SPLIT_LEFT || dir == SPLIT_RIGHT
        ? to->frame.width >= to->min_width + SPLIT_DIVIDER + leaf->min_width
        : to->frame.height >= to->min_height + SPLIT_DIVIDER + leaf->min_height;
    if (!fits)
        return NULL;

    *target = to;
    *side = dir;
    return leaf;
}

/* The half of 'leaf' on 'side' is highlighted; NULL: nothing is */
- (void)showDropZone:(struct split *)leaf side:(enum split_dir)side
{
    if (leaf == NULL) {
        _dropZone.hidden = YES;
        return;
    }

    if (_dropZone == nil) {
        _dropZone = [[NSView alloc] initWithFrame:NSZeroRect];
        CALayer *layer = view_create_layer();
        layer.hidden = NO;
        _dropZone.layer = layer;
        _dropZone.wantsLayer = YES;
    }

    /* Above everything (handles are added later) */
    if (_dropZone.superview != self || self.subviews.lastObject != _dropZone)
        [self addSubview:_dropZone positioned:NSWindowAbove relativeTo:nil];

    _dropZone.layer.backgroundColor =
        [NSColor.controlAccentColor colorWithAlphaComponent:.3].CGColor;

    NSRect frame = rect_of(leaf->frame);
    switch (side) {
    case SPLIT_LEFT:
        frame.size.width = floor(frame.size.width / 2);
        break;
    case SPLIT_RIGHT:
        frame.origin.x += floor(frame.size.width / 2);
        frame.size.width -= floor(frame.size.width / 2);
        break;
    case SPLIT_UP:
        frame.size.height = floor(frame.size.height / 2);
        break;
    case SPLIT_DOWN:
        frame.origin.y += floor(frame.size.height / 2);
        frame.size.height -= floor(frame.size.height / 2);
        break;
    }

    _dropZone.frame = frame;
    _dropZone.hidden = NO;
}

- (NSDragOperation)dropUpdated:(id<NSDraggingInfo>)info
{
    struct split *to;
    enum split_dir side;
    if ([self dropOf:info into:&to side:&side] == NULL) {
        [self showDropZone:NULL side:SPLIT_RIGHT];
        return NSDragOperationNone;
    }

    [self showDropZone:to side:side];
    return NSDragOperationMove;
}

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)info
{
    return [self dropUpdated:info];
}

- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)info
{
    return [self dropUpdated:info];
}

- (void)draggingExited:(id<NSDraggingInfo>)info
{
    [self showDropZone:NULL side:SPLIT_RIGHT];
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)info
{
    [self showDropZone:NULL side:SPLIT_RIGHT];

    struct split *to;
    enum split_dir side;
    struct split *leaf = [self dropOf:info into:&to side:&side];
    if (leaf == NULL)
        return NO;

    struct wl_window *pane = leaf->pane;
    split_move(&_root, leaf, to, side);
    [self relayout];
    [self focusPane:pane];
    app_run_hooks();
    return YES;
}

- (void)handleDragEnded
{
    [self showDropZone:NULL side:SPLIT_RIGHT];
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

    /* Not its handle (the terminal is being destroyed) */
    [self updateHandles];

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
    tabs_window_colors_changed(win);
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
