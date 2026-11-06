#pragma once

/*
 * A window's splits (macos/split-view.m): a binary tree, whose leaves
 * are panes, a terminal each. Geometry is in points, with the origin
 * at the top left, and neighbors are separated by a divider.
 */

#include <stdbool.h>
#include <stddef.h>

/* The divider's width (points) */
#define SPLIT_DIVIDER 1

/* Where a new pane goes, relative to the one that's split; also directions */
enum split_dir {
    SPLIT_RIGHT,
    SPLIT_DOWN,
    SPLIT_LEFT,
    SPLIT_UP,
};

enum split_kind {
    SPLIT_LEAF,
    SPLIT_HORIZONTAL,  /* Side by side: first is left */
    SPLIT_VERTICAL,    /* Stacked: first is on top */
};

struct split_rect {
    int x;
    int y;
    int width;
    int height;
};

struct split {
    enum split_kind kind;
    struct split *parent;

    /* Leaves */
    void *pane;
    int min_width;
    int min_height;

    /* Splits */
    struct split *first;
    struct split *second;
    double ratio;  /* first's share, of the space without the divider */

    /* As of the last split_layout() */
    struct split_rect frame;
};

/* A tree of one pane */
struct split *split_new(void *pane);
void split_free(struct split *root);

/* Splits 'leaf' in two: 'pane' goes in 'dir', getting half. Returns its leaf */
struct split *split_insert(
    struct split **root, struct split *leaf, void *pane, enum split_dir dir);

/*
 * Removes (and frees) 'leaf'; its sibling takes the parent's place.
 * Returns the leaf that's next to where it was (for the focus), or
 * NULL if it was the last one.
 */
struct split *split_remove(struct split **root, struct split *leaf);

/* Sets the frames of all nodes, within 'area' */
void split_layout(struct split *root, struct split_rect area);

/* The smallest size the tree fits in, keeping each pane's minimum */
void split_min_size(const struct split *node, int *width, int *height);

/* Gives all panes the same size, along each split's direction */
void split_equalize(struct split *root);

/* The leaf next to 'leaf' in 'dir' (as of the last layout), or NULL */
struct split *split_neighbor(
    const struct split *root, const struct split *leaf, enum split_dir dir);

/* Leaves in order (left to right, top to bottom); NULL at the end */
struct split *split_first(struct split *root);
struct split *split_following(const struct split *leaf);

/* As above, but wrapping around */
struct split *split_next(struct split *root, const struct split *leaf);
struct split *split_prev(struct split *root, const struct split *leaf);

size_t split_count(const struct split *root);

/*
 * Moves the divider of the closest split around 'leaf' that's along
 * 'dir', by 'amount' points in 'dir', keeping the panes' minimum
 * sizes. Returns whether anything moved (lay out again then).
 */
bool split_resize(struct split *leaf, enum split_dir dir, int amount);

/*
 * Moves 'node's divider to 'pos' (along its direction, in the
 * coordinates of the last layout), keeping the minimum sizes. Returns
 * whether it moved.
 */
bool split_move_divider(struct split *node, int pos);

/* The divider of split 'node', as of the last layout */
struct split_rect split_divider(const struct split *node);
