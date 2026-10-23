#include <stdlib.h>
#include <string.h>

#define LOG_MODULE "cursor-shape"
#define LOG_ENABLE_DBG 0
#include "log.h"

#include "cursor-shape.h"
#include "debug.h"
#include "util.h"

const char *const *
cursor_shape_to_string(enum cursor_shape shape)
{
    static const char *const table[][CURSOR_SHAPE_COUNT]= {
        [CURSOR_SHAPE_NONE] = {NULL},
        [CURSOR_SHAPE_HIDDEN] = {"hidden", NULL},
        [CURSOR_SHAPE_LEFT_PTR] = {"default", "left_ptr", NULL},
        [CURSOR_SHAPE_TEXT] = {"text", "xterm", NULL},
        [CURSOR_SHAPE_TOP_LEFT_CORNER] = {"nw-resize", "top_left_corner", NULL},
        [CURSOR_SHAPE_TOP_RIGHT_CORNER] = {"ne-resize", "top_right_corner", NULL},
        [CURSOR_SHAPE_BOTTOM_LEFT_CORNER] = {"sw-resize", "bottom_left_corner", NULL},
        [CURSOR_SHAPE_BOTTOM_RIGHT_CORNER] = {"se-resize", "bottom_right_corner", NULL},
        [CURSOR_SHAPE_LEFT_SIDE] = {"w-resize", "left_side", NULL},
        [CURSOR_SHAPE_RIGHT_SIDE] = {"e-resize", "right_side", NULL},
        [CURSOR_SHAPE_TOP_SIDE] = {"n-resize", "top_side", NULL},
        [CURSOR_SHAPE_BOTTOM_SIDE] = {"s-resize", "bottom_side", NULL},

    };

    xassert(shape <= ALEN(table));
    return table[shape];
}
