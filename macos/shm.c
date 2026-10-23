#include "shm.h"

#include <string.h>
#include <errno.h>

#include <CoreGraphics/CoreGraphics.h>
#include <IOSurface/IOSurfaceRef.h>

#include <pixman.h>

#include <tllist.h>

#define LOG_MODULE "shm"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "debug.h"
#include "macros.h"
#include "util.h"
#include "xmalloc.h"

/*
 * Buffers are IOSurfaces: shared memory the window server composites
 * directly from, when set as a CALayer's contents. We render into
 * them on the CPU, with pixman.
 *
 * A buffer is locked (CPU access) from the moment it is handed out,
 * until it is attached to a layer. Unlocking bumps the surface's
 * seed, which is how Core Animation detects the new contents.
 */

#define FORCED_DOUBLE_BUFFERING 0

struct buffer_chain;
struct buffer_private {
    struct buffer public;
    struct buffer_chain *chain;

    size_t ref_count;
    bool busy;                /* Not available for re-use */
    bool locked;              /* Locked for CPU access */
    bool attached;            /* Currently some layer's contents */
    bool committed;           /* Has been attached since handed out */

    void (*release_cb)(struct buffer *buf, void *data);
    void *cb_data;
};

struct buffer_chain {
    tll(struct buffer_private *) bufs;
    size_t pix_instances;

    void (*release_cb)(struct buffer *buf, void *data);
    void *cb_data;
};

/* Unreferenced buffers, waiting for the window server to release them */
static tll(struct buffer_private *) deferred;

static CGColorSpaceRef srgb = NULL;

void
shm_set_max_pool_size(off_t max_pool_size)
{
}

void
shm_set_min_stride_alignment(size_t min_stride_alignment)
{
}

static bool
buffer_lock(struct buffer_private *buf)
{
    if (buf->locked)
        return true;

    kern_return_t r = IOSurfaceLock(buf->public.iosurface, 0, NULL);
    if (r != kIOReturnSuccess) {
        LOG_ERR("failed to lock IOSurface: 0x%08x", r);
        return false;
    }

    /* Memory backed surfaces never move */
    xassert(IOSurfaceGetBaseAddress(buf->public.iosurface) == buf->public.data);

    buf->locked = true;
    return true;
}

static void
buffer_unlock(struct buffer_private *buf)
{
    if (!buf->locked)
        return;

    IOSurfaceUnlock(buf->public.iosurface, 0, NULL);
    buf->locked = false;
}

static void
buffer_destroy(struct buffer_private *buf)
{
    struct buffer *pub = &buf->public;

    if (pub->pix != NULL) {
        for (size_t i = 0; i < pub->pix_instances; i++)
            if (pub->pix[i] != NULL)
                pixman_image_unref(pub->pix[i]);
    }

    if (pub->iosurface != NULL) {
        buffer_unlock(buf);
        CFRelease(pub->iosurface);
    }

    if (pub->dirty != NULL) {
        for (size_t i = 0; i < pub->pix_instances; i++)
            pixman_region32_fini(&pub->dirty[i]);
    }

    free(pub->pix);
    free(pub->dirty);
    free(buf);
}

static bool
buffer_unref_no_remove_from_chain(struct buffer_private *buf)
{
    xassert(buf->ref_count > 0);
    buf->ref_count--;

    if (buf->ref_count > 0)
        return false;

    if (buf->busy)
        tll_push_back(deferred, buf);
    else
        buffer_destroy(buf);
    return true;
}

void
shm_fini(void)
{
    LOG_DBG("deferred buffers: %zu", tll_length(deferred));

    tll_foreach(deferred, it) {
        buffer_destroy(it->item);
        tll_remove(deferred, it);
    }

    if (srgb != NULL) {
        CGColorSpaceRelease(srgb);
        srgb = NULL;
    }
}

/* Returns true if the window server is done with a busy buffer */
static bool
buffer_is_released(const struct buffer_private *buf)
{
    return buf->busy &&
        buf->committed &&
        !buf->attached &&
        !IOSurfaceIsInUse(buf->public.iosurface);
}

static void
poll_deferred(void)
{
    tll_foreach(deferred, it) {
        struct buffer_private *buf = it->item;

        if (buf->busy && !buffer_is_released(buf))
            continue;

        tll_remove(deferred, it);
        buffer_destroy(buf);
    }
}

static void
poll_releases(struct buffer_chain *chain, bool run_callbacks)
{
    poll_deferred();

    tll_foreach(chain->bufs, it) {
        struct buffer_private *buf = it->item;

        if (!buffer_is_released(buf))
            continue;

        buf->busy = false;
        buf->committed = false;

        if (run_callbacks && buf->release_cb != NULL) {
            /* The callback may start rendering into it (pre-applied damage) */
            buffer_lock(buf);
            buf->release_cb(&buf->public, buf->cb_data);
        }
    }
}

void
shm_chain_poll_releases(struct buffer_chain *chain)
{
    poll_releases(chain, true);
}

void
shm_buffer_attached(struct buffer *_buf)
{
    struct buffer_private *buf = (struct buffer_private *)_buf;

    /* Publish our writes; bumps the surface seed */
    buffer_unlock(buf);

    buf->attached = true;
    buf->committed = true;
}

void
shm_buffer_detached(struct buffer *_buf)
{
    struct buffer_private *buf = (struct buffer_private *)_buf;
    buf->attached = false;
}

static IOSurfaceRef
create_iosurface(int width, int height)
{
    const size_t bytes_per_row = IOSurfaceAlignProperty(
        kIOSurfaceBytesPerRow, (size_t)width * 4);

    const void *keys[] = {
        kIOSurfaceWidth,
        kIOSurfaceHeight,
        kIOSurfaceBytesPerElement,
        kIOSurfaceBytesPerRow,
        kIOSurfacePixelFormat,
    };

    const int bytes_per_element = 4;
    const int bytes_per_row_int = (int)bytes_per_row;
    const uint32_t pixel_format = 'BGRA';  /* == PIXMAN_a8r8g8b8 on little endian */

    const void *values[] = {
        CFNumberCreate(NULL, kCFNumberIntType, &width),
        CFNumberCreate(NULL, kCFNumberIntType, &height),
        CFNumberCreate(NULL, kCFNumberIntType, &bytes_per_element),
        CFNumberCreate(NULL, kCFNumberIntType, &bytes_per_row_int),
        CFNumberCreate(NULL, kCFNumberSInt32Type, &pixel_format),
    };

    CFDictionaryRef props = CFDictionaryCreate(
        NULL, keys, values, ALEN(keys),
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

    for (size_t i = 0; i < ALEN(values); i++)
        CFRelease(values[i]);

    IOSurfaceRef surface = IOSurfaceCreate(props);
    CFRelease(props);

    if (surface == NULL)
        return NULL;

    /* We render sRGB colors; let the window server color match them */
    if (srgb == NULL)
        srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);

    CFPropertyListRef cs_props = CGColorSpaceCopyPropertyList(srgb);
    if (cs_props != NULL) {
        IOSurfaceSetValue(surface, kIOSurfaceColorSpace, cs_props);
        CFRelease(cs_props);
    }

    return surface;
}

static struct buffer_private *
buffer_create(struct buffer_chain *chain, int width, int height)
{
    IOSurfaceRef surface = create_iosurface(width, height);
    if (surface == NULL) {
        LOG_ERR("failed to create a %dx%d IOSurface", width, height);
        return NULL;
    }

    struct buffer_private *buf = xmalloc(sizeof(*buf));
    *buf = (struct buffer_private){
        .public = {
            .width = width,
            .height = height,
            .stride = (int)IOSurfaceGetBytesPerRow(surface),
            .iosurface = surface,
            .pix_instances = chain->pix_instances,
            .age = 1234,  /* Force a full repaint */
        },
        .chain = chain,
        .busy = true,
        .release_cb = chain->release_cb,
        .cb_data = chain->cb_data,
    };

    if (IOSurfaceLock(surface, 0, NULL) != kIOReturnSuccess) {
        LOG_ERR("failed to lock new IOSurface");
        CFRelease(surface);
        free(buf);
        return NULL;
    }

    buf->locked = true;
    buf->public.data = IOSurfaceGetBaseAddress(surface);

    /* One pixman image for each worker thread */
    buf->public.pix = xcalloc(chain->pix_instances, sizeof(buf->public.pix[0]));
    for (size_t i = 0; i < chain->pix_instances; i++) {
        buf->public.pix[i] = pixman_image_create_bits_no_clear(
            PIXMAN_a8r8g8b8, width, height,
            (uint32_t *)buf->public.data, buf->public.stride);

        if (buf->public.pix[i] == NULL) {
            LOG_ERR("failed to create pixman image");
            buffer_destroy(buf);
            return NULL;
        }
    }

    buf->public.dirty = xmalloc(
        chain->pix_instances * sizeof(buf->public.dirty[0]));
    for (size_t i = 0; i < chain->pix_instances; i++)
        pixman_region32_init(&buf->public.dirty[i]);

    return buf;
}

void
shm_did_not_use_buf(struct buffer *_buf)
{
    struct buffer_private *buf = (struct buffer_private *)_buf;
    buf->busy = false;
    buf->committed = false;
}

void
shm_get_many(struct buffer_chain *chain, size_t count,
             int widths[static count], int heights[static count],
             struct buffer *bufs[static count])
{
    /* Buffers are destroyed as soon as the window server releases them */
    for (size_t i = 0; i < count; i++) {
        bufs[i] = NULL;

        if (widths[i] <= 0 || heights[i] <= 0)
            continue;

        struct buffer_private *buf = buffer_create(chain, widths[i], heights[i]);
        if (buf == NULL)
            abort();

        buf->ref_count = 0;
        tll_push_front(deferred, buf);
        bufs[i] = &buf->public;
    }
}

struct buffer *
shm_get_buffer(struct buffer_chain *chain, int width, int height)
{
    LOG_DBG(
        "chain=%p: looking for a reusable %dx%d buffer "
        "among %zu potential buffers",
        (void *)chain, width, height, tll_length(chain->bufs));

    /*
     * Like Wayland, release callbacks are only run from the event
     * loop (shm_chain_poll_releases()), never while rendering.
     */
    poll_releases(chain, false);

    struct buffer_private *cached = NULL;
    tll_foreach(chain->bufs, it) {
        struct buffer_private *buf = it->item;

        if (buf->public.width != width || buf->public.height != height) {
            LOG_DBG("purging mismatching buffer %p", (void *)buf);
            if (buffer_unref_no_remove_from_chain(buf))
                tll_remove(chain->bufs, it);
            continue;
        }

        if (buf->busy)
            buf->public.age++;
        else
#if FORCED_DOUBLE_BUFFERING
            if (buf->public.age == 0)
                buf->public.age++;
            else
#endif
            {
                if (cached == NULL) {
                    cached = buf;
                } else {
                    /* We have multiple buffers eligible for
                     * reuse. Pick the "youngest" one, and mark the
                     * other one for purging */
                    if (buf->public.age < cached->public.age) {
                        shm_unref(&cached->public);
                        cached = buf;
                    } else {
                        if (buffer_unref_no_remove_from_chain(buf))
                            tll_remove(chain->bufs, it);
                    }
                }
            }
    }

    if (cached != NULL) {
        LOG_DBG("reusing buffer %p from cache", (void *)cached);
        if (!buffer_lock(cached))
            abort();

        cached->busy = true;
        for (size_t i = 0; i < cached->public.pix_instances; i++)
            pixman_region32_clear(&cached->public.dirty[i]);
        xassert(cached->public.pix_instances == chain->pix_instances);
        return &cached->public;
    }

    struct buffer_private *buf = buffer_create(chain, width, height);
    if (buf == NULL)
        abort();

    buf->ref_count = 1;
    tll_push_front(chain->bufs, buf);
    return &buf->public;
}

bool
shm_can_scroll(const struct buffer *buf)
{
    /* Scrolling is done with memmove() */
    return false;
}

bool
shm_scroll(struct buffer *buf, int rows,
           int top_margin, int top_keep_rows,
           int bottom_margin, int bottom_keep_rows)
{
    return false;
}

void
shm_purge(struct buffer_chain *chain)
{
    LOG_DBG("chain: %p: purging all buffers", (void *)chain);

    tll_foreach(chain->bufs, it) {
        if (buffer_unref_no_remove_from_chain(it->item))
            tll_remove(chain->bufs, it);
    }
}

void
shm_addref(struct buffer *_buf)
{
    struct buffer_private *buf = (struct buffer_private *)_buf;
    buf->ref_count++;
}

void
shm_unref(struct buffer *_buf)
{
    if (_buf == NULL)
        return;

    struct buffer_private *buf = (struct buffer_private *)_buf;
    struct buffer_chain *chain = buf->chain;

    tll_foreach(chain->bufs, it) {
        if (it->item != buf)
            continue;

        if (buffer_unref_no_remove_from_chain(buf))
            tll_remove(chain->bufs, it);
        break;
    }
}

struct buffer_chain *
shm_chain_new(struct wayland *wayl, bool scrollable, size_t pix_instances,
              enum shm_bit_depth desired_bit_depth,
              void (*release_cb)(struct buffer *buf, void *data), void *cb_data)
{
    static bool have_logged = false;
    if (!have_logged) {
        have_logged = true;
        LOG_INFO("using 8-bit BGRA IOSurfaces");
    }

    struct buffer_chain *chain = xmalloc(sizeof(*chain));
    *chain = (struct buffer_chain){
        .bufs = tll_init(),
        .pix_instances = pix_instances,
        .release_cb = release_cb,
        .cb_data = cb_data,
    };
    return chain;
}

void
shm_chain_free(struct buffer_chain *chain)
{
    if (chain == NULL)
        return;

    shm_purge(chain);

    if (tll_length(chain->bufs) > 0) {
        BUG("chain=%p: there are buffers remaining; "
            "is there a missing call to shm_unref()?", (void *)chain);
    }

    free(chain);
}

enum shm_bit_depth
shm_chain_bit_depth(const struct buffer_chain *chain)
{
    return SHM_BITS_8;
}
