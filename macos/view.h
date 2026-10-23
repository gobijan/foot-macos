#pragma once

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

#include "wayland.h"

/*
 * A terminal window's content view.
 *
 * Layer hosting: the root layer is flipped (y grows downwards). The
 * grid layer (the window's main surface) sits at the origin; sub
 * surfaces are sibling layers stacked above it.
 *
 * The view also translates keyboard, input method, mouse and
 * drag-and-drop events to foot's input handling, and drives frame
 * callbacks with a display link.
 */
@interface FootView : NSView <NSTextInputClient>

- (instancetype)initWithFrame:(NSRect)frame window:(struct wl_window *)win;

@property (nonatomic, readonly) CALayer *rootLayer;
@property (nonatomic, readonly) CALayer *gridLayer;

/* Cleared when the window is destroyed */
@property (nonatomic) struct wl_window *win;

/* Calls the window's frame handler at the next display refresh */
- (void)requestFrame;
- (void)setOccluded:(BOOL)occluded;

/* Discards any input method composition in progress */
- (void)resetInputMethod;

- (void)invalidate;

@end

/* A layer that never animates */
CALayer *view_create_layer(void);
