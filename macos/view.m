#import "view.h"

#import <Carbon/Carbon.h>

#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>

#define LOG_MODULE "view"
#define LOG_ENABLE_DBG 0
#include "log.h"
#import "app.h"
#include "ime.h"
#include "input.h"
#include "render.h"
#include "selection.h"
#include "shm.h"
#include "terminal.h"
#include "url-mode.h"

static NSDictionary<NSString *, id<CAAction>> *
null_actions(void)
{
    static NSDictionary *actions = nil;
    if (actions == nil) {
        NSNull *null = [NSNull null];
        actions = @{
            @"contents": null, @"contentsScale": null, @"bounds": null,
            @"position": null, @"hidden": null, @"backgroundColor": null,
            @"opacity": null, @"sublayers": null, @"onOrderIn": null,
            @"onOrderOut": null, @"frame": null,
        };
    }
    return actions;
}

CALayer *
view_create_layer(void)
{
    CALayer *layer = [CALayer layer];
    layer.actions = null_actions();
    layer.anchorPoint = CGPointZero;
    layer.position = CGPointZero;
    layer.contentsGravity = kCAGravityTopLeft;
    layer.magnificationFilter = kCAFilterNearest;
    layer.minificationFilter = kCAFilterNearest;
    layer.edgeAntialiasingMask = 0;
    layer.hidden = YES;
    return layer;
}

static int
button_for_event(NSEvent *event)
{
    switch (event.buttonNumber) {
    case 0:  return BTN_LEFT;
    case 1:  return BTN_RIGHT;
    case 2:  return BTN_MIDDLE;
    case 3:  return BTN_SIDE;
    case 4:  return BTN_EXTRA;
    default: return BTN_TASK;
    }
}

/* Byte offset of UTF-16 index 'idx' in 'str' when encoded as UTF-8 */
static int32_t
utf8_offset(NSString *str, NSUInteger idx)
{
    idx = MIN(idx, str.length);
    return (int32_t)[[str substringToIndex:idx] lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
}

@implementation FootView {
    CADisplayLink *_displayLink;
    BOOL _occluded;
    NSTrackingArea *_trackingArea;
    unsigned _buttonsDown;

    /* Input method */
    NSMutableAttributedString *_markedText;
    BOOL _inKeyDown;
    NSString *_keyDownText;   /* Text inserted during -keyDown:, without composition */
    BOOL _keyDownComposed;    /* The input method consumed the key */
    NSMutableSet<NSNumber *> *_consumedKeys;  /* Whose release must be ignored */

    /* Mouse wheels may report fractional steps */
    double _wheelSteps[2];
}

- (instancetype)initWithFrame:(NSRect)frame window:(struct wl_window *)win
{
    self = [super initWithFrame:frame];
    if (self == nil)
        return nil;

    _win = win;

    /* Layer hosting: set the layer *before* wantsLayer */
    _rootLayer = [CALayer layer];
    _rootLayer.actions = null_actions();
    _rootLayer.geometryFlipped = YES;
    _rootLayer.masksToBounds = YES;
    self.layer = _rootLayer;
    self.wantsLayer = YES;

    _gridLayer = view_create_layer();
    [_rootLayer addSublayer:_gridLayer];

    _markedText = [[NSMutableAttributedString alloc] init];
    _consumedKeys = [NSMutableSet set];

    [self registerForDraggedTypes:@[NSPasteboardTypeFileURL, NSPasteboardTypeString]];

    _displayLink = [self displayLinkWithTarget:self selector:@selector(frameTick:)];
    _displayLink.paused = YES;
    [_displayLink addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];

    return self;
}

- (void)invalidate
{
    [_displayLink invalidate];
    _displayLink = nil;
    _win = NULL;
}

- (struct terminal *)term
{
    return _win != NULL ? _win->term : NULL;
}

- (BOOL)isFlipped
{
    return YES;
}

- (BOOL)acceptsFirstResponder
{
    return YES;
}

- (BOOL)mouseDownCanMoveWindow
{
    return NO;
}

/*
 * Frames
 */

- (void)frameTick:(CADisplayLink *)link
{
    link.paused = YES;

    struct wl_window *win = _win;
    if (win == NULL || !win->frame_callback)
        return;

    struct terminal *term = win->term;

    /* Buffers the window server is done with can be re-used */
    shm_chain_poll_releases(term->render.chains.grid);

    render_frame_callback(term);
}

- (void)requestFrame
{
    if (!_occluded)
        _displayLink.paused = NO;
}

- (void)setOccluded:(BOOL)occluded
{
    _occluded = occluded;

    if (occluded)
        _displayLink.paused = YES;
    else if (_win != NULL && _win->frame_callback)
        _displayLink.paused = NO;
}

/*
 * Geometry
 */

- (void)setFrameSize:(NSSize)size
{
    [super setFrameSize:size];
    if (_win != NULL)
        window_resized(_win);
}

- (void)viewDidChangeBackingProperties
{
    [super viewDidChangeBackingProperties];
    if (_win != NULL)
        window_backing_changed(_win);
}

/*
 * Keyboard
 */

- (void)updateModifiers:(NSEventModifierFlags)flags
{
    struct seat *seat = app_seat();
    uint32_t depressed, locked;
    keyboard_modifiers(seat, flags, app_option_as_alt(), &depressed, &locked);
    input_modifiers(seat, depressed, 0, locked, 0);
}

/* Does the keymap produce 'text' for 'key', with the current modifiers? */
static BOOL
keymap_produces(const struct seat *seat, uint32_t key, NSString *text)
{
    if (seat->kbd.xkb_state == NULL)
        return NO;

    char buf[64];
    int count = xkb_state_key_get_utf8(seat->kbd.xkb_state, key, buf, sizeof(buf));
    return count > 0 && (size_t)count < sizeof(buf) && strcmp(buf, text.UTF8String) == 0;
}

- (void)keyDown:(NSEvent *)event
{
    struct terminal *term = self.term;
    struct seat *seat = app_seat();
    if (term == NULL || seat == NULL || seat->kbd_focus != term)
        return;

    const NSEventModifierFlags flags = event.modifierFlags;
    const uint32_t key = keyboard_xkb_keycode(event.keyCode);

    [self updateModifiers:flags];

    /*
     * Let the input method see the key first, unless it obviously
     * isn't text (Control/Command combos, and Option acting as Alt).
     * Dead keys and input methods (e.g. Japanese) result in marked
     * text, and eventually in inserted text.
     */
    const BOOL had_marked_text = _markedText.length > 0;
    const BOOL ime_eligible =
        term->ime_enabled &&
        !(flags & (NSEventModifierFlagControl | NSEventModifierFlagCommand)) &&
        !((flags & NSEventModifierFlagOption) &&
          keyboard_option_is_alt(flags, app_option_as_alt()));

    _keyDownText = nil;
    _keyDownComposed = NO;

    if (had_marked_text || ime_eligible) {
        _inKeyDown = YES;
        [self interpretKeyEvents:@[event]];
        _inKeyDown = NO;
    }

    if (_markedText.length > 0 || had_marked_text || _keyDownComposed) {
        /* Composing, or a composition was just committed */
        [_consumedKeys addObject:@(event.keyCode)];
        return;
    }

    if (_keyDownText != nil && !keymap_produces(seat, key, _keyDownText)) {
        /* E.g. an input method inserting text directly */
        ime_preedit(seat, NULL, 0, 0);
        ime_commit(seat, _keyDownText.UTF8String);
        ime_done(seat);
        [_consumedKeys addObject:@(event.keyCode)];
        return;
    }

    input_key(seat, app_next_serial(), key, KEY_STATE_PRESSED, event.isARepeat);
}

- (void)keyUp:(NSEvent *)event
{
    struct terminal *term = self.term;
    struct seat *seat = app_seat();
    if (term == NULL || seat == NULL || seat->kbd_focus != term)
        return;

    [self updateModifiers:event.modifierFlags];

    NSNumber *keycode = @(event.keyCode);
    if ([_consumedKeys containsObject:keycode]) {
        [_consumedKeys removeObject:keycode];
        return;
    }

    input_key(seat, app_next_serial(), keyboard_xkb_keycode(event.keyCode),
              KEY_STATE_RELEASED, false);
}

- (void)flagsChanged:(NSEvent *)event
{
    struct terminal *term = self.term;
    struct seat *seat = app_seat();
    if (term == NULL || seat == NULL || seat->kbd_focus != term)
        return;

    const uint16_t keycode = event.keyCode;
    const uint32_t key = keyboard_xkb_keycode(keycode);

    /* Like Wayland: the key event first, then the modifier state */
    if (keycode == kVK_CapsLock) {
        input_key(seat, app_next_serial(), key, KEY_STATE_PRESSED, false);
        input_key(seat, app_next_serial(), key, KEY_STATE_RELEASED, false);
    } else {
        const bool down = keyboard_modifier_key_is_down(keycode, event.modifierFlags);
        input_key(seat, app_next_serial(), key,
                  down ? KEY_STATE_PRESSED : KEY_STATE_RELEASED, false);
    }

    [self updateModifiers:event.modifierFlags];
}

/* Cmd+1-8 select that tab, Cmd+9 the last one, as in web browsers */
- (BOOL)selectTab:(NSEvent *)event
{
    NSString *chars = event.charactersIgnoringModifiers;
    NSArray<NSWindow *> *tabs = self.window.tabbedWindows;
    if (tabs == nil || chars.length != 1)
        return NO;

    const unichar c = [chars characterAtIndex:0];
    if (c < '1' || c > '9')
        return NO;

    const NSUInteger index = c == '9' ? tabs.count - 1 : c - '1';
    if (index < tabs.count)
        self.window.tabGroup.selectedWindow = tabs[index];
    return YES;
}

- (BOOL)performKeyEquivalent:(NSEvent *)event
{
    /*
     * AppKit claims some Control combinations (e.g. Control+Tab) for
     * itself. Command combinations go to the menu first.
     */
    if (self.window.firstResponder == self && event.type == NSEventTypeKeyDown) {
        const NSEventModifierFlags flags =
            event.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask;

        if ((flags & NSEventModifierFlagControl) && !(flags & NSEventModifierFlagCommand)) {
            [self keyDown:event];
            return YES;
        }

        if (flags == NSEventModifierFlagCommand && [self selectTab:event])
            return YES;
    }

    return [super performKeyEquivalent:event];
}

/*
 * NSTextInputClient
 */

- (void)insertText:(id)string replacementRange:(NSRange)replacementRange
{
    /* Copy: an attributed string's -string is its (mutable) backing store */
    NSString *text = [string isKindOfClass:[NSAttributedString class]]
        ? [[string string] copy] : [string copy];

    struct seat *seat = app_seat();
    const BOOL had_marked_text = _markedText.length > 0;
    [_markedText deleteCharactersInRange:NSMakeRange(0, _markedText.length)];

    if (_inKeyDown && !had_marked_text) {
        /* A plain key; encoded by -keyDown: */
        _keyDownText = text;
        return;
    }

    if (_inKeyDown)
        _keyDownComposed = YES;

    if (seat == NULL)
        return;

    ime_preedit(seat, NULL, 0, 0);
    if (text.length > 0)
        ime_commit(seat, text.UTF8String);
    ime_done(seat);
}

- (void)setMarkedText:(id)string selectedRange:(NSRange)selected
     replacementRange:(NSRange)replacementRange
{
    NSAttributedString *astr = [string isKindOfClass:[NSAttributedString class]]
        ? string : [[NSAttributedString alloc] initWithString:string];

    [_markedText setAttributedString:astr];

    if (_inKeyDown)
        _keyDownComposed = YES;

    struct seat *seat = app_seat();
    if (seat == NULL)
        return;

    NSString *text = astr.string;
    if (text.length == 0) {
        ime_preedit(seat, NULL, 0, 0);
    } else {
        ime_preedit(seat, text.UTF8String,
                    utf8_offset(text, selected.location),
                    utf8_offset(text, NSMaxRange(selected)));
    }
    ime_done(seat);
}

- (void)unmarkText
{
    [_markedText deleteCharactersInRange:NSMakeRange(0, _markedText.length)];

    struct seat *seat = app_seat();
    if (seat != NULL) {
        ime_preedit(seat, NULL, 0, 0);
        ime_done(seat);
    }
}

- (void)resetInputMethod
{
    [_markedText deleteCharactersInRange:NSMakeRange(0, _markedText.length)];
    [self.inputContext discardMarkedText];
}

- (BOOL)hasMarkedText
{
    return _markedText.length > 0;
}

- (NSRange)markedRange
{
    return _markedText.length > 0
        ? NSMakeRange(0, _markedText.length)
        : NSMakeRange(NSNotFound, 0);
}

- (NSRange)selectedRange
{
    return NSMakeRange(NSNotFound, 0);
}

- (NSArray<NSAttributedStringKey> *)validAttributesForMarkedText
{
    return @[];
}

- (NSAttributedString *)attributedSubstringForProposedRange:(NSRange)range
                                                actualRange:(NSRangePointer)actual
{
    return nil;
}

- (NSUInteger)characterIndexForPoint:(NSPoint)point
{
    return NSNotFound;
}

- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(NSRangePointer)actual
{
    struct terminal *term = self.term;
    struct seat *seat = app_seat();
    NSRect rect = NSZeroRect;

#if defined(FOOT_IME_ENABLED) && FOOT_IME_ENABLED
    if (term != NULL && seat != NULL) {
        const struct rect *c = &seat->ime.cursor_rect.pending;
        rect = NSMakeRect(c->x / term->scale, c->y / term->scale,
                          c->width / term->scale, c->height / term->scale);
    }
#endif

    return [self.window convertRectToScreen:[self convertRect:rect toView:nil]];
}

- (void)doCommandBySelector:(SEL)selector
{
    /* Keys are encoded by foot, in -keyDown: */
}

/*
 * Mouse
 */

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

- (void)cursorUpdate:(NSEvent *)event
{
    [window_cursor() set];
}

- (NSPoint)locationOf:(NSEvent *)event
{
    return [self convertPoint:event.locationInWindow fromView:nil];
}

/* Makes sure we have pointer focus; returns the seat, or NULL */
- (struct seat *)pointerFocus:(NSEvent *)event
{
    struct terminal *term = self.term;
    struct seat *seat = app_seat();
    if (term == NULL || seat == NULL)
        return NULL;

    if (seat->mouse_focus != term) {
        if (seat->mouse_focus != NULL)
            input_pointer_leave(seat);

        const NSPoint p = [self locationOf:event];
        input_pointer_enter(seat, term, app_next_serial(), p.x, p.y);
    }

    return seat;
}

- (void)mouseEntered:(NSEvent *)event
{
    [self pointerFocus:event];
}

- (void)mouseExited:(NSEvent *)event
{
    struct seat *seat = app_seat();
    if (seat == NULL || seat->mouse_focus != self.term)
        return;

    /* Keep focus while dragging, for e.g. selection auto-scrolling */
    if (_buttonsDown == 0)
        input_pointer_leave(seat);
}

- (void)motion:(NSEvent *)event
{
    struct seat *seat = [self pointerFocus:event];
    if (seat == NULL)
        return;

    const NSPoint p = [self locationOf:event];
    input_pointer_motion(seat, p.x, p.y);
}

- (void)mouseMoved:(NSEvent *)event         { [self motion:event]; }
- (void)mouseDragged:(NSEvent *)event       { [self motion:event]; }
- (void)rightMouseDragged:(NSEvent *)event  { [self motion:event]; }
- (void)otherMouseDragged:(NSEvent *)event  { [self motion:event]; }

- (void)button:(NSEvent *)event pressed:(BOOL)pressed
{
    struct seat *seat = [self pointerFocus:event];
    if (seat == NULL)
        return;

    const NSPoint p = [self locationOf:event];
    input_pointer_motion(seat, p.x, p.y);

    const int button = button_for_event(event);
    const unsigned bit = 1u << MIN(event.buttonNumber, 31);

    if (pressed)
        _buttonsDown |= bit;
    else
        _buttonsDown &= ~bit;

    input_pointer_button(
        seat, app_next_serial(), button,
        pressed ? BUTTON_STATE_PRESSED : BUTTON_STATE_RELEASED);

    /* Released outside the window, at the end of a drag */
    if (!pressed && _buttonsDown == 0 && !NSPointInRect(p, self.bounds))
        input_pointer_leave(seat);
}

- (void)mouseDown:(NSEvent *)event        { [self button:event pressed:YES]; }
- (void)mouseUp:(NSEvent *)event          { [self button:event pressed:NO]; }
- (void)rightMouseDown:(NSEvent *)event   { [self button:event pressed:YES]; }
- (void)rightMouseUp:(NSEvent *)event     { [self button:event pressed:NO]; }
- (void)otherMouseDown:(NSEvent *)event   { [self button:event pressed:YES]; }
- (void)otherMouseUp:(NSEvent *)event     { [self button:event pressed:NO]; }

- (void)scrollWheel:(NSEvent *)event
{
    struct seat *seat = [self pointerFocus:event];
    if (seat == NULL)
        return;

    /* Positive values scroll down/right; macOS deltas are the opposite */
    const double dx = -event.scrollingDeltaX;
    const double dy = -event.scrollingDeltaY;

    if (event.hasPreciseScrollingDeltas) {
        /* Trackpads, Magic Mouse: pixels */
        if (dy != 0.)
            input_pointer_scroll(seat, SCROLL_AXIS_VERTICAL, dy);
        if (dx != 0.)
            input_pointer_scroll(seat, SCROLL_AXIS_HORIZONTAL, dx);

        if (event.phase & (NSEventPhaseEnded | NSEventPhaseCancelled) ||
            event.momentumPhase & (NSEventPhaseEnded | NSEventPhaseCancelled))
        {
            input_pointer_scroll_stop(seat, SCROLL_AXIS_VERTICAL);
            input_pointer_scroll_stop(seat, SCROLL_AXIS_HORIZONTAL);
        }
    } else {
        /* Mouse wheels: (possibly accelerated, fractional) steps */
        const double deltas[2] = {
            [SCROLL_AXIS_VERTICAL] = dy,
            [SCROLL_AXIS_HORIZONTAL] = dx,
        };

        for (int axis = 0; axis < 2; axis++) {
            _wheelSteps[axis] += deltas[axis];
            const int steps = (int)_wheelSteps[axis];
            if (steps != 0) {
                input_pointer_scroll_discrete(seat, axis, steps);
                _wheelSteps[axis] -= steps;
            }
        }
    }
}

/*
 * Drag and drop
 */

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)info
{
    return self.term != NULL ? NSDragOperationCopy : NSDragOperationNone;
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)info
{
    struct terminal *term = self.term;
    struct seat *seat = app_seat();
    if (term == NULL || seat == NULL)
        return NO;

    NSPasteboard *pb = info.draggingPasteboard;
    NSArray<NSURL *> *urls = [pb readObjectsForClasses:@[[NSURL class]]
                                               options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}];

    if (urls.count > 0) {
        NSMutableString *list = [NSMutableString string];
        for (NSURL *url in urls)
            [list appendFormat:@"%@\r\n", url.absoluteString];

        const char *data = list.UTF8String;
        selection_drop(seat, term, strdup(data), strlen(data), DATA_OFFER_MIME_URI_LIST);
        return YES;
    }

    NSString *text = [pb stringForType:NSPasteboardTypeString];
    if (text != nil) {
        const char *data = text.UTF8String;
        selection_drop(seat, term, strdup(data), strlen(data), DATA_OFFER_MIME_TEXT_UTF8);
        return YES;
    }

    return NO;
}

/*
 * Menu actions
 */

- (void)action:(enum bind_action_normal)action
{
    struct terminal *term = self.term;
    struct seat *seat = app_seat();
    if (term != NULL && seat != NULL)
        input_execute_action(seat, term, action);
}

- (void)copy:(id)sender                    { [self action:BIND_ACTION_CLIPBOARD_COPY]; }
- (void)paste:(id)sender                   { [self action:BIND_ACTION_CLIPBOARD_PASTE]; }
- (void)pasteSelection:(id)sender          { [self action:BIND_ACTION_PRIMARY_PASTE]; }
- (void)performFindPanelAction:(id)sender  { [self action:BIND_ACTION_SEARCH_START]; }
- (void)increaseFontSize:(id)sender        { [self action:BIND_ACTION_FONT_SIZE_UP]; }
- (void)decreaseFontSize:(id)sender        { [self action:BIND_ACTION_FONT_SIZE_DOWN]; }
- (void)resetFontSize:(id)sender           { [self action:BIND_ACTION_FONT_SIZE_RESET]; }

- (void)showURLs:(id)sender
{
    struct terminal *term = self.term;
    if (term != NULL && !urls_mode_is_active(term))
        [self action:BIND_ACTION_SHOW_URLS_LAUNCH];
}

- (BOOL)validateMenuItem:(NSMenuItem *)item
{
    struct terminal *term = self.term;
    if (term == NULL)
        return NO;

    if (item.action == @selector(copy:))
        return term->selection.coords.end.row >= 0;
    if (item.action == @selector(paste:))
        return selection_clipboard_has_data(app_seat());
    if (item.action == @selector(pasteSelection:))
        return selection_primary_has_data(app_seat());
    if (item.action == @selector(showURLs:))
        return !urls_mode_is_active(term);

    return YES;
}

@end
