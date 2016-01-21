/*
 * Copyright (c) 2012-2015 Etnaviv Project
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Wladimir J. van der Laan <laanwj@gmail.com>
 */

#include "etnaviv_resource.h"

#include "hw/common.xml.h"

#include "etnaviv_context.h"
#include "etnaviv_screen.h"
#include "etnaviv_debug.h"
#include "etnaviv_translate.h"

#include "util/u_memory.h"
#include "util/u_inlines.h"
#include "util/u_transfer.h" /* u_default_resource_get_handle */

/* A tile is 4x4 pixels, having 'screen->specs.bits_per_tile' of tile status.
 * So, in a buffer of N pixels, there are N / (4 * 4) tiles.
 * We need N * screen->specs.bits_per_tile / (4 * 4) bits of tile status, or
 * N * screen->specs.bits_per_tile / (4 * 4 * 8) bytes.
 */
bool etna_screen_resource_alloc_ts(struct pipe_screen *pscreen, struct etna_resource *rsc)
{
    struct etna_screen *screen = etna_screen(pscreen);
    size_t rt_ts_size, ts_layer_stride, pixels;
    assert(!rsc->ts_bo);
    /* TS only for level 0 -- XXX is this formula correct? */
    pixels = rsc->levels[0].layer_stride / util_format_get_blocksize(rsc->base.format);
    ts_layer_stride = align(pixels*screen->specs.bits_per_tile/0x80, 0x100);
    rt_ts_size = ts_layer_stride * rsc->base.array_size;
    if (rt_ts_size == 0)
        return true;

    DBG_F(ETNA_DBG_RESOURCE_MSGS, "%p: Allocating tile status of size %i", rsc, rt_ts_size);
    struct etna_bo *rt_ts = 0;
    if (unlikely((rt_ts = etna_bo_new(screen->dev, rt_ts_size, DRM_ETNA_GEM_CACHE_WC)) == NULL))
    {
        BUG("Problem allocating tile status for resource");
        return false;
    }
    rsc->ts_bo = rt_ts;
    rsc->levels[0].ts_offset = 0;
    rsc->levels[0].ts_layer_stride = ts_layer_stride;
    rsc->levels[0].ts_size = rt_ts_size;
    /* It is important to initialize the TS, as random pattern
     * can result in crashes. Do this on the CPU as this only happens once
     * per surface anyway and it's a small area, so it may not be worth
     * queuing this to the GPU.
     */
    void *ts_map = etna_bo_map(rt_ts);
    memset(ts_map, screen->specs.ts_clear_value, rt_ts_size);
    return true;
}

static boolean etna_screen_can_create_resource(struct pipe_screen *pscreen,
                              const struct pipe_resource *templat)
{
    struct etna_screen *screen = etna_screen(pscreen);
    if (!translate_samples_to_xyscale(templat->nr_samples, NULL, NULL, NULL))
        return false;
    if (templat->bind & (PIPE_BIND_RENDER_TARGET | PIPE_BIND_DEPTH_STENCIL | PIPE_BIND_SAMPLER_VIEW))
    {
        uint max_size = (templat->bind & (PIPE_BIND_RENDER_TARGET | PIPE_BIND_DEPTH_STENCIL)) ?
                            screen->specs.max_rendertarget_size :
                            screen->specs.max_texture_size;
        if (templat->width0 > max_size || templat->height0 > max_size)
            return false;
    }
    return true;
}

/* Allocate 2D texture or render target resource */
struct pipe_resource *etna_resource_alloc(struct pipe_screen *pscreen,
                         unsigned layout, const struct pipe_resource *templat)
{
    struct etna_screen *screen = etna_screen(pscreen);

    /* Determine scaling for antialiasing, allow override using debug flag */
    int nr_samples = templat->nr_samples;
    if ((templat->bind & (PIPE_BIND_RENDER_TARGET | PIPE_BIND_DEPTH_STENCIL)) &&
       !(templat->bind & PIPE_BIND_SAMPLER_VIEW))
    {
        if (DBG_ENABLED(ETNA_DBG_MSAA_2X))
            nr_samples = 2;
        if (DBG_ENABLED(ETNA_DBG_MSAA_4X))
            nr_samples = 4;
    }
    int msaa_xscale = 1, msaa_yscale = 1;
    if (!translate_samples_to_xyscale(nr_samples, &msaa_xscale, &msaa_yscale, NULL))
    {
        /* Number of samples not supported */
        return NULL;
    }

    /* Determine needed padding (alignment of height/width) */
    unsigned paddingX = 0, paddingY = 0;
    unsigned halign = TEXTURE_HALIGN_FOUR;
    etna_layout_multiple(layout,
                         screen->specs.pixel_pipes,
            (templat->bind & PIPE_BIND_SAMPLER_VIEW) && !VIV_FEATURE(screen, chipMinorFeatures1, TEXTURE_HALIGN),
            &paddingX, &paddingY, &halign);
    assert(paddingX && paddingY);

    if (templat->bind != PIPE_BUFFER)
    {
        unsigned min_paddingY = 4 * screen->specs.pixel_pipes;
        if (paddingY < min_paddingY) paddingY = min_paddingY;
    }

    /* compute mipmap level sizes and offsets */
    struct etna_resource *rsc = CALLOC_STRUCT(etna_resource);

    if (!rsc)
      return NULL;

    int max_mip_level = templat->last_level;
    if (unlikely(max_mip_level >= ETNA_NUM_LOD)) /* max LOD supported by hw */
        max_mip_level = ETNA_NUM_LOD - 1;

    unsigned ix = 0;
    unsigned x = templat->width0, y = templat->height0;
    unsigned offset = 0;
    while(true)
    {
        struct etna_resource_level *mip = &rsc->levels[ix];
        mip->width = x;
        mip->height = y;
        mip->padded_width = align(x * msaa_xscale, paddingX);
        mip->padded_height = align(y * msaa_yscale, paddingY);
        mip->stride = util_format_get_stride(templat->format, mip->padded_width);
        mip->offset = offset;
        mip->layer_stride = mip->stride * util_format_get_nblocksy(templat->format, mip->padded_height);
        mip->size = templat->array_size * mip->layer_stride;
        offset += align(mip->size, ETNA_PE_ALIGNMENT); /* align mipmaps to 64 bytes to be able to render to them */
        if (ix == max_mip_level || (x == 1 && y == 1))
            break; // stop at last level
        x = u_minify(x, 1);
        y = u_minify(y, 1);
        ix += 1;
    }

    /* allocate bo */
    DBG_F(ETNA_DBG_RESOURCE_MSGS, "%p: Allocate surface of %ix%i (padded to %ix%i), %i layers, of format %s, size %08x flags %08x",
            rsc,
            templat->width0, templat->height0, rsc->levels[0].padded_width, rsc->levels[0].padded_height, templat->array_size, util_format_name(templat->format),
            offset, templat->bind);

    struct etna_bo *bo = etna_bo_new(screen->dev, offset, DRM_ETNA_GEM_CACHE_WC);
    if (unlikely(bo == NULL))
    {
        BUG("Problem allocating video memory for resource");
        return NULL;
    }

    rsc->base = *templat;
    rsc->base.last_level = ix; /* real last mipmap level */
    rsc->base.screen = pscreen;
    rsc->base.nr_samples = nr_samples;
    rsc->layout = layout;
    rsc->halign = halign;
    rsc->bo = bo;
    rsc->ts_bo = 0; /* TS is only created when first bound to surface */
    pipe_reference_init(&rsc->base.reference, 1);
    list_inithead(&rsc->list);

    if (DBG_ENABLED(ETNA_DBG_ZERO))
    {
        void *map = etna_bo_map(bo);
        memset(map, 0, offset);
    }

    return &rsc->base;
}

static struct pipe_resource *etna_resource_create(struct pipe_screen *pscreen,
                                         const struct pipe_resource *templat)
{
    struct etna_screen *screen = etna_screen(pscreen);

    /* Figure out what tiling to use -- for now, assume that textures cannot be supertiled, and cannot be linear.
     * There is a feature flag SUPERTILED_TEXTURE (not supported on any known hw) that may allow this, as well
     * as LINEAR_TEXTURE_SUPPORT (supported on gc880 and gc2000 at least), but not sure how it works.
     * Buffers always have LINEAR layout.
     */
    unsigned layout = ETNA_LAYOUT_LINEAR;
    if (templat->target != PIPE_BUFFER)
    {
        bool want_multitiled = screen->specs.pixel_pipes > 1;
        bool want_supertiled = screen->specs.can_supertile && !DBG_ENABLED(ETNA_DBG_NO_SUPERTILE);

        /* Keep single byte blocksized resources as tiled, since we
         * are unable to use the RS blit to de-tile them. However,
         * if they're used as a render target or depth/stencil, they
         * must be multi-tiled for GPUs with multiple pixel pipes.
         * Ignore depth/stencil here, but it is an error for a render
         * target.
         */
        if (util_format_get_blocksize(templat->format) == 1 &&
            !(templat->bind & PIPE_BIND_DEPTH_STENCIL))
        {
            assert(!(templat->bind & PIPE_BIND_RENDER_TARGET && want_multitiled));
            want_multitiled = want_supertiled = false;
        }

        layout = ETNA_LAYOUT_BIT_TILE;
        if (want_multitiled)
            layout |= ETNA_LAYOUT_BIT_MULTI;
        if (want_supertiled)
            layout |= ETNA_LAYOUT_BIT_SUPER;
    }

    return etna_resource_alloc(pscreen, layout, templat);
}

static void etna_resource_destroy(struct pipe_screen *pscreen,
                        struct pipe_resource *prsc)
{
    struct etna_resource *rsc = etna_resource(prsc);

    if (rsc->bo)
      etna_bo_del(rsc->bo);

    if (rsc->ts_bo)
      etna_bo_del(rsc->ts_bo);

    list_delinit(&rsc->list);

    pipe_resource_reference(&rsc->texture, NULL);

    FREE(rsc);
}

static struct pipe_resource *
etna_resource_from_handle(struct pipe_screen *pscreen,
                const struct pipe_resource *tmpl,
                struct winsys_handle *handle)
{
    struct etna_screen *screen = etna_screen(pscreen);
    struct etna_resource *rsc = CALLOC_STRUCT(etna_resource);
    struct etna_resource_level *level = &rsc->levels[0];
    struct pipe_resource *prsc = &rsc->base;

    DBG("target=%d, format=%s, %ux%ux%u, array_size=%u, last_level=%u, "
        "nr_samples=%u, usage=%u, bind=%x, flags=%x",
        tmpl->target, util_format_name(tmpl->format),
        tmpl->width0, tmpl->height0, tmpl->depth0,
        tmpl->array_size, tmpl->last_level, tmpl->nr_samples,
        tmpl->usage, tmpl->bind, tmpl->flags);

    if (!rsc)
        return NULL;

    *prsc = *tmpl;

    pipe_reference_init(&prsc->reference, 1);
    list_inithead(&rsc->list);
    prsc->screen = pscreen;

    rsc->bo = etna_screen_bo_from_handle(pscreen, handle, &level->stride);
    if (!rsc->bo)
        goto fail;

    level->width = tmpl->width0;
    level->height = tmpl->height0;

    /* We will be using the RS to copy with this resource, so we must
     * ensure that it is appropriately aligned for the RS requirements. */
    unsigned paddingX = ETNA_RS_WIDTH_MASK + 1;
    unsigned paddingY = (ETNA_RS_HEIGHT_MASK + 1) * screen->specs.pixel_pipes;

    level->padded_width = align(level->width, paddingX);
    level->padded_height = align(level->height, paddingY);

    /* The DDX must give us a BO which conforms to our padding size.
     * The stride of the BO must be greater or equal to our padded
     * stride. The size of the BO must accomodate the padded height. */
    if (level->stride < util_format_get_stride(tmpl->format, level->padded_width)) {
        BUG("BO stride is too small for RS engine width padding");
        goto fail;
    }
    if (etna_bo_size(rsc->bo) < level->stride * level->padded_height) {
        BUG("BO size is too small for RS engine height padding");
        goto fail;
    }

    return prsc;

fail:
    etna_resource_destroy(pscreen, prsc);
    return NULL;
}

void etna_resource_screen_init(struct pipe_screen *pscreen)
{
    pscreen->can_create_resource = etna_screen_can_create_resource;
    pscreen->resource_create = etna_resource_create;
    pscreen->resource_from_handle = etna_resource_from_handle;
    pscreen->resource_get_handle = u_default_resource_get_handle;
    pscreen->resource_destroy = etna_resource_destroy;
}
