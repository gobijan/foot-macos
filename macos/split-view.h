#pragma once

#import <AppKit/AppKit.h>

#include "splits.h"
#include "wayland.h"

@class FootView;

/*
 * A terminal window's content view: its panes (FootViews, a terminal
 * each), split as in macos/splits.c, with dividers between them.
 *
 * Every terminal window has one, with at least one pane. The window's
 * delegate (window.m) belongs to it too.
 *
 * The focused pane is the window's first responder; it has the
 * keyboard focus while the window is key. The other panes are dimmed.
 */
@interface FootSplitView : NSView

- (instancetype)initWithPane:(struct wl_window *)win;

/* The NSWindow's delegate (NSWindow doesn't retain it) */
@property (nonatomic, strong) id<NSWindowDelegate> windowDelegate;

@property (nonatomic, readonly) struct wl_window *focused;

/* Shell > Send Input to All Splits: what's typed in one pane, all get */
@property (nonatomic) BOOL sendsInputToAll;
@property (nonatomic, readonly) size_t count;

/* The panes, in order (their 'win' is NULL once destroyed) */
@property (nonatomic, readonly) NSArray<FootView *> *panes;

/* Adds 'win' (whose view isn't in a window yet) next to 'to', in 'dir' */
- (void)insertPane:(struct wl_window *)win next:(struct wl_window *)to
         direction:(enum split_dir)dir;

/* Whether 'win' is large enough to be split in 'dir' */
- (BOOL)canSplit:(struct wl_window *)win direction:(enum split_dir)dir;

/* Lays out the panes again (e.g. a new one, once configured) */
- (void)relayout;

/* Removes 'win'; the pane next to it gets the focus */
- (void)removePane:(struct wl_window *)win;

/* Makes 'win' the focused pane (and shows it, if another one is zoomed) */
- (void)focusPane:(struct wl_window *)win;

/* FootView: it became the window's first responder */
- (void)paneBecameFirstResponder:(struct wl_window *)win;

/* The pane's minimum size (points) changed */
- (void)setMinimumSize:(NSSize)size ofPane:(struct wl_window *)win;

/* The pane's colors changed: dimming and dividers follow */
- (void)paneColorsChanged:(struct wl_window *)win;

/* The window's occlusion state; zoomed out panes are always occluded */
- (void)setWindowOccluded:(BOOL)occluded;

/* Whether the mouse (window coordinates) is over a divider, or a pane's grab handle */
- (BOOL)isDividerOrHandleAt:(NSPoint)point;

/* While dividers are dragged, the terminals resize as while the window is */
@property (nonatomic, readonly) BOOL dragging;

/* The pane a close confirmation (sheet) is shown for, if just one */
@property (nonatomic) struct wl_window *sheetPane;

/* The tree (macos/layout.m saves it) */
@property (nonatomic, readonly) struct split *root;

/* Restoring a layout: the split that 'win' (just inserted) is the second of */
- (void)setRatio:(double)ratio ofSplitAbove:(struct wl_window *)win;

@end
