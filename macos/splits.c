#include "splits.h"

#include <math.h>
#include <stdlib.h>

#include "debug.h"
#include "macros.h"
#include "util.h"
#include "xmalloc.h"

struct split *
split_new(void *pane)
{
    struct split *leaf = xcalloc(1, sizeof(*leaf));
    leaf->kind = SPLIT_LEAF;
    leaf->pane = pane;
    return leaf;
}

void
split_free(struct split *node)
{
    if (node == NULL)
        return;

    split_free(node->first);
    split_free(node->second);
    free(node);
}

/* Puts 'with' where 'node' is in the tree */
static void
replace(struct split **root, struct split *node, struct split *with)
{
    struct split *parent = node->parent;

    if (parent == NULL)
        *root = with;
    else if (parent->first == node)
        parent->first = with;
    else
        parent->second = with;

    if (with != NULL)
        with->parent = parent;
}

struct split *
split_insert(struct split **root, struct split *leaf, void *pane, enum split_dir dir)
{
    xassert(leaf->kind == SPLIT_LEAF);

    struct split *node = xcalloc(1, sizeof(*node));
    node->kind = dir == SPLIT_RIGHT || dir == SPLIT_LEFT
        ? SPLIT_HORIZONTAL : SPLIT_VERTICAL;
    node->ratio = .5;
    node->frame = leaf->frame;

    replace(root, leaf, node);

    struct split *new = split_new(pane);
    const bool after = dir == SPLIT_RIGHT || dir == SPLIT_DOWN;
    node->first = after ? leaf : new;
    node->second = after ? new : leaf;
    leaf->parent = node;
    new->parent = node;

    return new;
}

static struct split *
last(struct split *node)
{
    while (node->kind != SPLIT_LEAF)
        node = node->second;
    return node;
}

struct split *
split_first(struct split *node)
{
    if (node == NULL)
        return NULL;

    while (node->kind != SPLIT_LEAF)
        node = node->first;
    return node;
}

struct split *
split_remove(struct split **root, struct split *leaf)
{
    xassert(leaf->kind == SPLIT_LEAF);

    struct split *parent = leaf->parent;
    if (parent == NULL) {
        xassert(*root == leaf);
        *root = NULL;
        free(leaf);
        return NULL;
    }

    const bool was_first = parent->first == leaf;
    struct split *sibling = was_first ? parent->second : parent->first;

    replace(root, parent, sibling);
    free(parent);
    free(leaf);

    /* The sibling's pane on the side where the leaf was */
    return was_first ? split_first(sibling) : last(sibling);
}

void
split_min_size(const struct split *node, int *width, int *height)
{
    if (node->kind == SPLIT_LEAF) {
        *width = node->min_width;
        *height = node->min_height;
        return;
    }

    int w1, h1, w2, h2;
    split_min_size(node->first, &w1, &h1);
    split_min_size(node->second, &w2, &h2);

    if (node->kind == SPLIT_HORIZONTAL) {
        *width = w1 + SPLIT_DIVIDER + w2;
        *height = max(h1, h2);
    } else {
        *width = max(w1, w2);
        *height = h1 + SPLIT_DIVIDER + h2;
    }
}

/* first's minimum and second's, along the split's direction */
static void
min_lengths(const struct split *node, int *min1, int *min2)
{
    int w1, h1, w2, h2;
    split_min_size(node->first, &w1, &h1);
    split_min_size(node->second, &w2, &h2);

    const bool horizontal = node->kind == SPLIT_HORIZONTAL;
    *min1 = horizontal ? w1 : h1;
    *min2 = horizontal ? w2 : h2;
}

/* first's length, out of 'avail', keeping the minimums */
static int
clamp_first(int length, int avail, int min1, int min2)
{
    if (length > avail - min2)
        length = avail - min2;
    if (length < min1)
        length = min1;
    if (length > avail)
        length = avail;
    if (length < 0)
        length = 0;
    return length;
}

void
split_layout(struct split *node, struct split_rect area)
{
    node->frame = area;
    if (node->kind == SPLIT_LEAF)
        return;

    int min1, min2;
    min_lengths(node, &min1, &min2);

    const bool horizontal = node->kind == SPLIT_HORIZONTAL;
    const int length = horizontal ? area.width : area.height;
    const int avail = max(length - SPLIT_DIVIDER, 0);
    const int first = clamp_first((int)lround(avail * node->ratio), avail, min1, min2);
    const int second = avail - first;

    struct split_rect a = area;
    struct split_rect b = area;

    if (horizontal) {
        a.width = first;
        b.x = area.x + first + SPLIT_DIVIDER;
        b.width = second;
    } else {
        a.height = first;
        b.y = area.y + first + SPLIT_DIVIDER;
        b.height = second;
    }

    split_layout(node->first, a);
    split_layout(node->second, b);
}

/* The number of panes along 'kind', in 'node' */
static int
weight(const struct split *node, enum split_kind kind)
{
    if (node->kind != kind)
        return 1;
    return weight(node->first, kind) + weight(node->second, kind);
}

void
split_equalize(struct split *node)
{
    if (node->kind == SPLIT_LEAF)
        return;

    const int a = weight(node->first, node->kind);
    const int b = weight(node->second, node->kind);
    node->ratio = (double)a / (a + b);

    split_equalize(node->first);
    split_equalize(node->second);
}

static int
overlap(int a, int a_length, int b, int b_length)
{
    return min(a + a_length, b + b_length) - max(a, b);
}

struct split *
split_neighbor(const struct split *root, const struct split *leaf, enum split_dir dir)
{
    const struct split_rect *f = &leaf->frame;

    struct split *best = NULL;
    int best_distance = 0;
    int best_offset = 0;

    for (struct split *it = split_first((struct split *)root); it != NULL;
         it = split_following(it))
    {
        if (it == leaf)
            continue;

        const struct split_rect *r = &it->frame;
        int distance, shared, offset;

        switch (dir) {
        case SPLIT_RIGHT:
            distance = r->x - (f->x + f->width);
            shared = overlap(f->y, f->height, r->y, r->height);
            offset = abs(r->y - f->y);
            break;

        case SPLIT_LEFT:
            distance = f->x - (r->x + r->width);
            shared = overlap(f->y, f->height, r->y, r->height);
            offset = abs(r->y - f->y);
            break;

        case SPLIT_DOWN:
            distance = r->y - (f->y + f->height);
            shared = overlap(f->x, f->width, r->x, r->width);
            offset = abs(r->x - f->x);
            break;

        case SPLIT_UP:
        default:
            distance = f->y - (r->y + r->height);
            shared = overlap(f->x, f->width, r->x, r->width);
            offset = abs(r->x - f->x);
            break;
        }

        if (distance < 0 || shared <= 0)
            continue;

        /* The closest; of those, the one most in line with 'leaf' */
        if (best == NULL || distance < best_distance ||
            (distance == best_distance && offset < best_offset))
        {
            best = it;
            best_distance = distance;
            best_offset = offset;
        }
    }

    return best;
}

struct split *
split_following(const struct split *node)
{
    while (node->parent != NULL && node->parent->second == node)
        node = node->parent;

    return node->parent != NULL ? split_first(node->parent->second) : NULL;
}

static struct split *
preceding(const struct split *node)
{
    while (node->parent != NULL && node->parent->first == node)
        node = node->parent;

    return node->parent != NULL ? last(node->parent->first) : NULL;
}

struct split *
split_next(struct split *root, const struct split *leaf)
{
    struct split *next = split_following(leaf);
    return next != NULL ? next : split_first(root);
}

struct split *
split_prev(struct split *root, const struct split *leaf)
{
    struct split *prev = preceding(leaf);
    return prev != NULL ? prev : last(root);
}

size_t
split_count(const struct split *node)
{
    if (node == NULL)
        return 0;
    if (node->kind == SPLIT_LEAF)
        return 1;
    return split_count(node->first) + split_count(node->second);
}

bool
split_move_divider(struct split *node, int pos)
{
    if (node->kind == SPLIT_LEAF)
        return false;

    const bool horizontal = node->kind == SPLIT_HORIZONTAL;
    const int origin = horizontal ? node->frame.x : node->frame.y;
    const int length = horizontal ? node->frame.width : node->frame.height;
    const int avail = length - SPLIT_DIVIDER;
    if (avail <= 0)
        return false;

    int min1, min2;
    min_lengths(node, &min1, &min2);

    const int first = clamp_first(pos - origin, avail, min1, min2);
    const int current = horizontal ? node->first->frame.width : node->first->frame.height;
    if (first == current)
        return false;

    node->ratio = (double)first / avail;
    return true;
}

bool
split_resize(struct split *leaf, enum split_dir dir, int amount)
{
    const enum split_kind kind = dir == SPLIT_RIGHT || dir == SPLIT_LEFT
        ? SPLIT_HORIZONTAL : SPLIT_VERTICAL;

    struct split *node = leaf->parent;
    while (node != NULL && node->kind != kind)
        node = node->parent;
    if (node == NULL)
        return false;

    const struct split_rect divider = split_divider(node);
    const int pos = kind == SPLIT_HORIZONTAL ? divider.x : divider.y;
    const int delta = dir == SPLIT_RIGHT || dir == SPLIT_DOWN ? amount : -amount;
    return split_move_divider(node, pos + delta);
}

struct split_rect
split_divider(const struct split *node)
{
    xassert(node->kind != SPLIT_LEAF);

    const struct split_rect *f = &node->frame;
    const struct split_rect *first = &node->first->frame;

    if (node->kind == SPLIT_HORIZONTAL) {
        return (struct split_rect){
            first->x + first->width, f->y, SPLIT_DIVIDER, f->height};
    } else {
        return (struct split_rect){
            f->x, first->y + first->height, f->width, SPLIT_DIVIDER};
    }
}

/* Each split's children and divider fill it exactly */
static void
test_fills(const struct split *node)
{
    if (node->kind == SPLIT_LEAF) {
        xassert(node->frame.width >= 0 && node->frame.height >= 0);
        return;
    }

    const struct split_rect *f = &node->frame;
    const struct split_rect *a = &node->first->frame;
    const struct split_rect *b = &node->second->frame;

    if (node->kind == SPLIT_HORIZONTAL) {
        xassert(a->x == f->x && a->y == f->y && a->height == f->height);
        xassert(b->x == a->x + a->width + SPLIT_DIVIDER);
        xassert(b->y == f->y && b->height == f->height);
        xassert(a->width + SPLIT_DIVIDER + b->width == f->width);
    } else {
        xassert(a->x == f->x && a->y == f->y && a->width == f->width);
        xassert(b->y == a->y + a->height + SPLIT_DIVIDER);
        xassert(b->x == f->x && b->width == f->width);
        xassert(a->height + SPLIT_DIVIDER + b->height == f->height);
    }

    test_fills(node->first);
    test_fills(node->second);
}

static void
test_layout(struct split *root, int width, int height)
{
    split_layout(root, (struct split_rect){0, 0, width, height});
    test_fills(root);
}

UNITTEST
{
    /* Panes are identified by these */
    int a, b, c, d;

    struct split *root = split_new(&a);
    struct split *la = root;
    xassert(split_count(root) == 1);
    xassert(split_next(root, la) == la);
    xassert(split_prev(root, la) == la);

    /* A | (B / C), then (D | A) | (B / C) */
    struct split *lb = split_insert(&root, la, &b, SPLIT_RIGHT);
    struct split *lc = split_insert(&root, lb, &c, SPLIT_DOWN);
    struct split *ld = split_insert(&root, la, &d, SPLIT_LEFT);

    xassert(root->kind == SPLIT_HORIZONTAL);
    xassert(root->first->kind == SPLIT_HORIZONTAL);
    xassert(root->second->kind == SPLIT_VERTICAL);
    xassert(split_count(root) == 4);

    /* In order, wrapping around */
    xassert(split_first(root) == ld);
    xassert(split_following(ld) == la);
    xassert(split_following(la) == lb);
    xassert(split_following(lb) == lc);
    xassert(split_following(lc) == NULL);
    xassert(split_next(root, lc) == ld);
    xassert(split_prev(root, ld) == lc);
    xassert(split_prev(root, lb) == la);

    /* Halves, of the space without the dividers */
    test_layout(root, 201, 100);
    xassert(root->first->frame.width == 100);
    xassert(root->second->frame.x == 101 && root->second->frame.width == 100);
    xassert(ld->frame.width + 1 + la->frame.width == 100);
    xassert(lb->frame.height + 1 + lc->frame.height == 100);

    struct split_rect divider = split_divider(root);
    xassert(divider.x == 100 && divider.y == 0);
    xassert(divider.width == SPLIT_DIVIDER && divider.height == 100);

    /* Neighbors */
    xassert(split_neighbor(root, la, SPLIT_RIGHT) == lb);  /* In line with A's top */
    xassert(split_neighbor(root, lc, SPLIT_LEFT) == la);
    xassert(split_neighbor(root, lc, SPLIT_UP) == lb);
    xassert(split_neighbor(root, lb, SPLIT_DOWN) == lc);
    xassert(split_neighbor(root, ld, SPLIT_RIGHT) == la);
    xassert(split_neighbor(root, la, SPLIT_LEFT) == ld);
    xassert(split_neighbor(root, ld, SPLIT_LEFT) == NULL);
    xassert(split_neighbor(root, lb, SPLIT_UP) == NULL);
    xassert(split_neighbor(root, la, SPLIT_DOWN) == NULL);

    /* Odd sizes are still filled exactly (with room for the dividers) */
    for (int w = 2; w < 40; w++)
        test_layout(root, w, w / 2);

    /* Minimum sizes */
    for (struct split *it = split_first(root); it != NULL; it = split_following(it)) {
        it->min_width = 10;
        it->min_height = 5;
    }

    int min_width, min_height;
    split_min_size(root, &min_width, &min_height);
    xassert(min_width == 10 + 1 + 10 + 1 + 10);
    xassert(min_height == 5 + 1 + 5);

    test_layout(root, 201, 100);
    xassert(!split_move_divider(root, root->first->frame.width));
    xassert(split_move_divider(root, 0));
    test_layout(root, 201, 100);
    xassert(root->first->frame.width == 10 + 1 + 10);
    xassert(ld->frame.width == 10 && la->frame.width == 10);

    /* Too small for the minimums: the frames are still valid */
    test_layout(root, 5, 3);

    /* Removing: the neighbor on that side gets the focus */
    xassert(split_remove(&root, la) == ld);
    xassert(root->first == ld && ld->parent == root);
    xassert(split_remove(&root, lb) == lc);
    xassert(root->kind == SPLIT_HORIZONTAL && root->first == ld && root->second == lc);
    xassert(split_remove(&root, ld) == lc);
    xassert(root == lc && lc->parent == NULL);
    xassert(split_remove(&root, lc) == NULL);
    xassert(root == NULL);
}

UNITTEST
{
    int a, b, c;

    /* A | B | C: equal widths */
    struct split *root = split_new(&a);
    struct split *la = root;
    struct split *lb = split_insert(&root, la, &b, SPLIT_RIGHT);
    struct split *lc = split_insert(&root, lb, &c, SPLIT_RIGHT);

    split_equalize(root);
    test_layout(root, 302, 50);
    xassert(la->frame.width == 100);
    xassert(lb->frame.width == 100);
    xassert(lc->frame.width == 100);
    split_free(root);

    /* A | (B / C): halves, stacked panes count once */
    root = split_new(&a);
    lb = split_insert(&root, root, &b, SPLIT_RIGHT);
    split_insert(&root, lb, &c, SPLIT_DOWN);
    root->ratio = root->second->ratio = .2;

    split_equalize(root);
    xassert(root->ratio == .5);
    xassert(root->second->ratio == .5);
    split_free(root);
}

UNITTEST
{
    int a, b, c;

    /* A | B */
    struct split *root = split_new(&a);
    struct split *la = root;
    struct split *lb = split_insert(&root, la, &b, SPLIT_RIGHT);

    test_layout(root, 101, 50);
    xassert(la->frame.width == 50 && lb->frame.width == 50);

    /* The divider moves in the direction given, whichever side's pane it's from */
    xassert(split_resize(lb, SPLIT_LEFT, 10));
    test_layout(root, 101, 50);
    xassert(la->frame.width == 40 && lb->frame.width == 60);

    xassert(split_resize(la, SPLIT_RIGHT, 20));
    test_layout(root, 101, 50);
    xassert(la->frame.width == 60 && lb->frame.width == 40);

    /* Nothing stacked */
    xassert(!split_resize(la, SPLIT_UP, 10));

    /* Not beyond the minimum */
    la->min_width = lb->min_width = 10;
    xassert(split_resize(la, SPLIT_RIGHT, 1000));
    test_layout(root, 101, 50);
    xassert(la->frame.width == 90 && lb->frame.width == 10);
    xassert(!split_resize(la, SPLIT_RIGHT, 1));

    /* (A / C) | B: the closest split along the direction */
    struct split *lc = split_insert(&root, la, &c, SPLIT_DOWN);
    test_layout(root, 101, 51);
    xassert(split_resize(lc, SPLIT_UP, 5));
    test_layout(root, 101, 51);
    xassert(la->frame.height == 20 && lc->frame.height == 30);
    xassert(split_resize(lc, SPLIT_LEFT, 40));
    test_layout(root, 101, 51);
    xassert(la->frame.width == 50 && lc->frame.width == 50);

    split_free(root);
}
