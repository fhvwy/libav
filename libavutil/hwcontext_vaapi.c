/*
 * This file is part of Libav.
 *
 * Libav is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Libav is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with Libav; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "config.h"

#if HAVE_VAAPI_X11
#   include <va/va_x11.h>
#endif
#if HAVE_VAAPI_DRM
#   include <va/va_drm.h>
#endif

#include <fcntl.h>
#if HAVE_UNISTD_H
#   include <unistd.h>
#endif


#include "avassert.h"
#include "buffer.h"
#include "common.h"
#include "hwcontext.h"
#include "hwcontext_internal.h"
#include "hwcontext_vaapi.h"
#include "mem.h"
#include "pixdesc.h"
#include "pixfmt.h"

typedef struct VAAPIDevicePriv {
#if HAVE_VAAPI_X11
    Display *x11_display;
#endif

    int drm_fd;
} VAAPIDevicePriv;

typedef struct VAAPISurfaceFormat {
    enum AVPixelFormat pix_fmt;
    VAImageFormat image_format;
} VAAPISurfaceFormat;

typedef struct VAAPIDeviceContext {
    // Surface formats which can be used with this device.
    VAAPISurfaceFormat *formats;
    int              nb_formats;
} VAAPIDeviceContext;

typedef struct VAAPIFramesContext {
    // Surface attributes set at create time.
    VASurfaceAttrib *attributes;
    int           nb_attributes;
    // RT format of the underlying surface (Intel driver ignores this anyway).
    unsigned int rt_format;
    // Whether vaDeriveImage works.
    int derive_works;
} VAAPIFramesContext;

typedef struct VAAPIMapping {
    // Handle to the derived or copied image which is mapped.
    VAImage image;
    // The mapping flags actually used.
    int flags;
} VAAPIMapping;

#define MAP(va, rt, av) { \
        VA_FOURCC_ ## va, \
        VA_RT_FORMAT_ ## rt, \
        AV_PIX_FMT_ ## av \
    }
// The map fourcc <-> pix_fmt isn't bijective because of the annoying U/V
// plane swap cases.  The frame handling below tries to hide these.
static const struct {
    unsigned int fourcc;
    unsigned int rt_format;
    enum AVPixelFormat pix_fmt;
} vaapi_format_map[] = {
    MAP(NV12, YUV420,  NV12),
    MAP(YV12, YUV420,  YUV420P), // With U/V planes swapped.
    MAP(IYUV, YUV420,  YUV420P),
  //MAP(I420, YUV420,  YUV420P), // Not in libva but used by Intel driver.
#ifdef VA_FOURCC_YV16
    MAP(YV16, YUV422,  YUV422P), // With U/V planes swapped.
#endif
    MAP(422H, YUV422,  YUV422P),
    MAP(UYVY, YUV422,  UYVY422),
    MAP(YUY2, YUV422,  YUYV422),
    MAP(Y800, YUV400,  GRAY8),
#ifdef VA_FOURCC_P010
    MAP(P010, YUV420_10BPP, P010),
#endif
    MAP(BGRA, RGB32,   BGRA),
  //MAP(BGRX, RGB32,   BGR0),
    MAP(RGBA, RGB32,   RGBA),
  //MAP(RGBX, RGB32,   RGB0),
#ifdef VA_FOURCC_ABGR
    MAP(ABGR, RGB32,   ABGR),
  //MAP(XBGR, RGB32,   0BGR),
#endif
    MAP(ARGB, RGB32,   ARGB),
  //MAP(XRGB, RGB32,   0RGB),
};
#undef MAP

static enum AVPixelFormat vaapi_pix_fmt_from_fourcc(unsigned int fourcc)
{
    int i;
    for (i = 0; i < FF_ARRAY_ELEMS(vaapi_format_map); i++)
        if (vaapi_format_map[i].fourcc == fourcc)
            return vaapi_format_map[i].pix_fmt;
    return AV_PIX_FMT_NONE;
}

unsigned int ff_vaapi_fourcc_from_pix_fmt(enum AVPixelFormat pix_fmt)
{
    int i;
    for (i = 0; i < FF_ARRAY_ELEMS(vaapi_format_map); i++)
        if (vaapi_format_map[i].pix_fmt == pix_fmt)
            return vaapi_format_map[i].fourcc;
    return 0;
}

static int vaapi_get_image_format(AVHWDeviceContext *hwdev,
                                  enum AVPixelFormat pix_fmt,
                                  VAImageFormat **image_format)
{
    VAAPIDeviceContext *ctx = hwdev->internal->priv;
    int i;

    for (i = 0; i < ctx->nb_formats; i++) {
        if (ctx->formats[i].pix_fmt == pix_fmt) {
            if (image_format)
                *image_format = &ctx->formats[i].image_format;
            return 0;
        }
    }
    return AVERROR(EINVAL);
}

static int vaapi_frames_get_constraints(AVHWDeviceContext *hwdev,
                                        const void *hwconfig,
                                        AVHWFramesConstraints *constraints)
{
    AVVAAPIDeviceContext *hwctx = hwdev->hwctx;
    const AVVAAPIHWConfig *config = hwconfig;
    VAAPIDeviceContext *ctx = hwdev->internal->priv;
    VASurfaceAttrib *attr_list = NULL;
    VAStatus vas;
    enum AVPixelFormat pix_fmt;
    unsigned int fourcc;
    int err, i, j, attr_count, pix_fmt_count;

    if (config &&
        !(hwctx->driver_quirks & AV_VAAPI_DRIVER_QUIRK_SURFACE_ATTRIBUTES)) {
        attr_count = 0;
        vas = vaQuerySurfaceAttributes(hwctx->display, config->config_id,
                                       0, &attr_count);
        if (vas != VA_STATUS_SUCCESS) {
            av_log(hwdev, AV_LOG_ERROR, "Failed to query surface attributes: "
                   "%d (%s).\n", vas, vaErrorStr(vas));
            err = AVERROR(ENOSYS);
            goto fail;
        }

        attr_list = av_malloc(attr_count * sizeof(*attr_list));
        if (!attr_list) {
            err = AVERROR(ENOMEM);
            goto fail;
        }

        vas = vaQuerySurfaceAttributes(hwctx->display, config->config_id,
                                       attr_list, &attr_count);
        if (vas != VA_STATUS_SUCCESS) {
            av_log(hwdev, AV_LOG_ERROR, "Failed to query surface attributes: "
                   "%d (%s).\n", vas, vaErrorStr(vas));
            err = AVERROR(ENOSYS);
            goto fail;
        }

        pix_fmt_count = 0;
        for (i = 0; i < attr_count; i++) {
            switch (attr_list[i].type) {
            case VASurfaceAttribPixelFormat:
                fourcc = attr_list[i].value.value.i;
                pix_fmt = vaapi_pix_fmt_from_fourcc(fourcc);
                if (pix_fmt != AV_PIX_FMT_NONE) {
                    ++pix_fmt_count;
                } else {
                    // Something unsupported - ignore.
                }
                break;
            case VASurfaceAttribMinWidth:
                constraints->min_width  = attr_list[i].value.value.i;
                break;
            case VASurfaceAttribMinHeight:
                constraints->min_height = attr_list[i].value.value.i;
                break;
            case VASurfaceAttribMaxWidth:
                constraints->max_width  = attr_list[i].value.value.i;
                break;
            case VASurfaceAttribMaxHeight:
                constraints->max_height = attr_list[i].value.value.i;
                break;
            }
        }
        if (pix_fmt_count == 0) {
            // Nothing usable found.  Presumably there exists something which
            // works, so leave the set null to indicate unknown.
            constraints->valid_sw_formats = NULL;
        } else {
            constraints->valid_sw_formats = av_malloc_array(pix_fmt_count + 1,
                                                            sizeof(pix_fmt));
            if (!constraints->valid_sw_formats) {
                err = AVERROR(ENOMEM);
                goto fail;
            }

            for (i = j = 0; i < attr_count; i++) {
                if (attr_list[i].type != VASurfaceAttribPixelFormat)
                    continue;
                fourcc = attr_list[i].value.value.i;
                pix_fmt = vaapi_pix_fmt_from_fourcc(fourcc);
                if (pix_fmt != AV_PIX_FMT_NONE)
                    constraints->valid_sw_formats[j++] = pix_fmt;
            }
            av_assert0(j == pix_fmt_count);
            constraints->valid_sw_formats[j] = AV_PIX_FMT_NONE;
        }
    } else {
        // No configuration supplied.
        // Return the full set of image formats known by the implementation.
        constraints->valid_sw_formats = av_malloc_array(ctx->nb_formats + 1,
                                                        sizeof(pix_fmt));
        if (!constraints->valid_sw_formats) {
            err = AVERROR(ENOMEM);
            goto fail;
        }
        for (i = 0; i < ctx->nb_formats; i++)
            constraints->valid_sw_formats[i] = ctx->formats[i].pix_fmt;
        constraints->valid_sw_formats[i] = AV_PIX_FMT_NONE;
    }

    constraints->valid_hw_formats = av_malloc_array(2, sizeof(pix_fmt));
    if (!constraints->valid_hw_formats) {
        err = AVERROR(ENOMEM);
        goto fail;
    }
    constraints->valid_hw_formats[0] = AV_PIX_FMT_VAAPI;
    constraints->valid_hw_formats[1] = AV_PIX_FMT_NONE;

    err = 0;
fail:
    av_freep(&attr_list);
    return err;
}

static const struct {
    const char *friendly_name;
    const char *match_string;
    unsigned int quirks;
} vaapi_driver_quirks_table[] = {
    {
        "Intel i965 (Quick Sync)",
        "i965",
        AV_VAAPI_DRIVER_QUIRK_RENDER_PARAM_BUFFERS,
    },
    {
        "Intel iHD",
        "ubit",
        AV_VAAPI_DRIVER_QUIRK_ATTRIB_MEMTYPE,
    },
    {
        "VDPAU wrapper",
        "Splitted-Desktop Systems VDPAU backend for VA-API",
        AV_VAAPI_DRIVER_QUIRK_SURFACE_ATTRIBUTES,
    },
};

static int vaapi_device_init(AVHWDeviceContext *hwdev)
{
    VAAPIDeviceContext *ctx = hwdev->internal->priv;
    AVVAAPIDeviceContext *hwctx = hwdev->hwctx;
    VAImageFormat *image_list = NULL;
    VAStatus vas;
    const char *vendor_string;
    int err, i, image_count;
    enum AVPixelFormat pix_fmt;
    unsigned int fourcc;

    image_count = vaMaxNumImageFormats(hwctx->display);
    if (image_count <= 0) {
        err = AVERROR(EIO);
        goto fail;
    }
    image_list = av_malloc(image_count * sizeof(*image_list));
    if (!image_list) {
        err = AVERROR(ENOMEM);
        goto fail;
    }
    vas = vaQueryImageFormats(hwctx->display, image_list, &image_count);
    if (vas != VA_STATUS_SUCCESS) {
        err = AVERROR(EIO);
        goto fail;
    }

    ctx->formats  = av_malloc(image_count * sizeof(*ctx->formats));
    if (!ctx->formats) {
        err = AVERROR(ENOMEM);
        goto fail;
    }
    ctx->nb_formats = 0;
    for (i = 0; i < image_count; i++) {
        fourcc  = image_list[i].fourcc;
        pix_fmt = vaapi_pix_fmt_from_fourcc(fourcc);
        if (pix_fmt == AV_PIX_FMT_NONE) {
            av_log(hwdev, AV_LOG_DEBUG, "Format %#x -> unknown.\n",
                   fourcc);
        } else {
            av_log(hwdev, AV_LOG_DEBUG, "Format %#x -> %s.\n",
                   fourcc, av_get_pix_fmt_name(pix_fmt));
            ctx->formats[ctx->nb_formats].pix_fmt      = pix_fmt;
            ctx->formats[ctx->nb_formats].image_format = image_list[i];
            ++ctx->nb_formats;
        }
    }

    if (hwctx->driver_quirks & AV_VAAPI_DRIVER_QUIRK_USER_SET) {
        av_log(hwdev, AV_LOG_VERBOSE, "Not detecting driver: "
               "quirks set by user.\n");
    } else {
        // Detect the driver in use and set quirk flags if necessary.
        vendor_string = vaQueryVendorString(hwctx->display);
        hwctx->driver_quirks = 0;
        if (vendor_string) {
            for (i = 0; i < FF_ARRAY_ELEMS(vaapi_driver_quirks_table); i++) {
                if (strstr(vendor_string,
                           vaapi_driver_quirks_table[i].match_string)) {
                    av_log(hwdev, AV_LOG_VERBOSE, "Matched \"%s\" as known "
                           "driver \"%s\".\n", vendor_string,
                           vaapi_driver_quirks_table[i].friendly_name);
                    hwctx->driver_quirks |=
                        vaapi_driver_quirks_table[i].quirks;
                    break;
                }
            }
            if (!(i < FF_ARRAY_ELEMS(vaapi_driver_quirks_table))) {
                av_log(hwdev, AV_LOG_VERBOSE, "Unknown driver \"%s\", "
                       "assuming standard behaviour.\n", vendor_string);
            }
        }
    }

    av_free(image_list);
    return 0;
fail:
    av_freep(&ctx->formats);
    av_free(image_list);
    return err;
}

static void vaapi_device_uninit(AVHWDeviceContext *hwdev)
{
    VAAPIDeviceContext *ctx = hwdev->internal->priv;

    av_freep(&ctx->formats);
}

static void vaapi_buffer_free(void *opaque, uint8_t *data)
{
    AVHWFramesContext     *hwfc = opaque;
    AVVAAPIDeviceContext *hwctx = hwfc->device_ctx->hwctx;
    VASurfaceID surface_id;
    VAStatus vas;

    surface_id = (VASurfaceID)(uintptr_t)data;

    vas = vaDestroySurfaces(hwctx->display, &surface_id, 1);
    if (vas != VA_STATUS_SUCCESS) {
        av_log(hwfc, AV_LOG_ERROR, "Failed to destroy surface %#x: "
               "%d (%s).\n", surface_id, vas, vaErrorStr(vas));
    }
}

static AVBufferRef *vaapi_pool_alloc(void *opaque, int size)
{
    AVHWFramesContext     *hwfc = opaque;
    VAAPIFramesContext     *ctx = hwfc->internal->priv;
    AVVAAPIDeviceContext *hwctx = hwfc->device_ctx->hwctx;
    AVVAAPIFramesContext  *avfc = hwfc->hwctx;
    VASurfaceID surface_id;
    VAStatus vas;
    AVBufferRef *ref;

    if (hwfc->initial_pool_size > 0 &&
        avfc->nb_surfaces >= hwfc->initial_pool_size)
        return NULL;

    vas = vaCreateSurfaces(hwctx->display, ctx->rt_format,
                           hwfc->width, hwfc->height,
                           &surface_id, 1,
                           ctx->attributes, ctx->nb_attributes);
    if (vas != VA_STATUS_SUCCESS) {
        av_log(hwfc, AV_LOG_ERROR, "Failed to create surface: "
               "%d (%s).\n", vas, vaErrorStr(vas));
        return NULL;
    }
    av_log(hwfc, AV_LOG_DEBUG, "Created surface %#x.\n", surface_id);

    ref = av_buffer_create((uint8_t*)(uintptr_t)surface_id,
                           sizeof(surface_id), &vaapi_buffer_free,
                           hwfc, AV_BUFFER_FLAG_READONLY);
    if (!ref) {
        vaDestroySurfaces(hwctx->display, &surface_id, 1);
        return NULL;
    }

    if (hwfc->initial_pool_size > 0) {
        // This is a fixed-size pool, so we must still be in the initial
        // allocation sequence.
        av_assert0(avfc->nb_surfaces < hwfc->initial_pool_size);
        avfc->surface_ids[avfc->nb_surfaces] = surface_id;
        ++avfc->nb_surfaces;
    }

    return ref;
}

static int vaapi_frames_init(AVHWFramesContext *hwfc)
{
    AVVAAPIFramesContext  *avfc = hwfc->hwctx;
    VAAPIFramesContext     *ctx = hwfc->internal->priv;
    AVVAAPIDeviceContext *hwctx = hwfc->device_ctx->hwctx;
    VAImageFormat *expected_format;
    AVBufferRef *test_surface = NULL;
    VASurfaceID test_surface_id;
    VAImage test_image;
    VAStatus vas;
    int err, i;
    unsigned int fourcc, rt_format;

    for (i = 0; i < FF_ARRAY_ELEMS(vaapi_format_map); i++) {
        if (vaapi_format_map[i].pix_fmt == hwfc->sw_format) {
            fourcc    = vaapi_format_map[i].fourcc;
            rt_format = vaapi_format_map[i].rt_format;
            break;
        }
    }
    if (i >= FF_ARRAY_ELEMS(vaapi_format_map)) {
        av_log(hwfc, AV_LOG_ERROR, "Unsupported format: %s.\n",
               av_get_pix_fmt_name(hwfc->sw_format));
        return AVERROR(EINVAL);
    }

    if (!hwfc->pool) {
        if (!(hwctx->driver_quirks & AV_VAAPI_DRIVER_QUIRK_SURFACE_ATTRIBUTES)) {
            int need_memory_type = !(hwctx->driver_quirks & AV_VAAPI_DRIVER_QUIRK_ATTRIB_MEMTYPE);
            int need_pixel_format = 1;
            for (i = 0; i < avfc->nb_attributes; i++) {
                if (ctx->attributes[i].type == VASurfaceAttribMemoryType)
                    need_memory_type  = 0;
                if (ctx->attributes[i].type == VASurfaceAttribPixelFormat)
                    need_pixel_format = 0;
            }
            ctx->nb_attributes =
                avfc->nb_attributes + need_memory_type + need_pixel_format;

            ctx->attributes = av_malloc(ctx->nb_attributes *
                                        sizeof(*ctx->attributes));
            if (!ctx->attributes) {
                err = AVERROR(ENOMEM);
                goto fail;
            }

            for (i = 0; i < avfc->nb_attributes; i++)
                ctx->attributes[i] = avfc->attributes[i];
            if (need_memory_type) {
                ctx->attributes[i++] = (VASurfaceAttrib) {
                    .type          = VASurfaceAttribMemoryType,
                    .flags         = VA_SURFACE_ATTRIB_SETTABLE,
                    .value.type    = VAGenericValueTypeInteger,
                    .value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_VA,
                };
            }
            if (need_pixel_format) {
                ctx->attributes[i++] = (VASurfaceAttrib) {
                    .type          = VASurfaceAttribPixelFormat,
                    .flags         = VA_SURFACE_ATTRIB_SETTABLE,
                    .value.type    = VAGenericValueTypeInteger,
                    .value.value.i = fourcc,
                };
            }
            av_assert0(i == ctx->nb_attributes);
        } else {
            ctx->attributes = NULL;
            ctx->nb_attributes = 0;
        }

        ctx->rt_format = rt_format;

        if (hwfc->initial_pool_size > 0) {
            // This pool will be usable as a render target, so we need to store
            // all of the surface IDs somewhere that vaCreateContext() calls
            // will be able to access them.
            avfc->nb_surfaces = 0;
            avfc->surface_ids = av_malloc(hwfc->initial_pool_size *
                                          sizeof(*avfc->surface_ids));
            if (!avfc->surface_ids) {
                err = AVERROR(ENOMEM);
                goto fail;
            }
        } else {
            // This pool allows dynamic sizing, and will not be usable as a
            // render target.
            avfc->nb_surfaces = 0;
            avfc->surface_ids = NULL;
        }

        hwfc->internal->pool_internal =
            av_buffer_pool_init2(sizeof(VASurfaceID), hwfc,
                                 &vaapi_pool_alloc, NULL);
        if (!hwfc->internal->pool_internal) {
            av_log(hwfc, AV_LOG_ERROR, "Failed to create VAAPI surface pool.\n");
            err = AVERROR(ENOMEM);
            goto fail;
        }
    }

    // Allocate a single surface to test whether vaDeriveImage() is going
    // to work for the specific configuration.
    if (hwfc->pool) {
        test_surface = av_buffer_pool_get(hwfc->pool);
        if (!test_surface) {
            av_log(hwfc, AV_LOG_ERROR, "Unable to allocate a surface from "
                   "user-configured buffer pool.\n");
            err = AVERROR(ENOMEM);
            goto fail;
        }
    } else {
        test_surface = av_buffer_pool_get(hwfc->internal->pool_internal);
        if (!test_surface) {
            av_log(hwfc, AV_LOG_ERROR, "Unable to allocate a surface from "
                   "internal buffer pool.\n");
            err = AVERROR(ENOMEM);
            goto fail;
        }
    }
    test_surface_id = (VASurfaceID)(uintptr_t)test_surface->data;

    ctx->derive_works = 0;

    err = vaapi_get_image_format(hwfc->device_ctx,
                                 hwfc->sw_format, &expected_format);
    if (err == 0) {
        vas = vaDeriveImage(hwctx->display, test_surface_id, &test_image);
        if (vas == VA_STATUS_SUCCESS) {
            if (expected_format->fourcc == test_image.format.fourcc) {
                av_log(hwfc, AV_LOG_DEBUG, "Direct mapping possible.\n");
                ctx->derive_works = 1;
            } else {
                av_log(hwfc, AV_LOG_DEBUG, "Direct mapping disabled: "
                       "derived image format %08x does not match "
                       "expected format %08x.\n",
                       expected_format->fourcc, test_image.format.fourcc);
            }
            vaDestroyImage(hwctx->display, test_image.image_id);
        } else {
            av_log(hwfc, AV_LOG_DEBUG, "Direct mapping disabled: "
                   "deriving image does not work: "
                   "%d (%s).\n", vas, vaErrorStr(vas));
        }
    } else {
        av_log(hwfc, AV_LOG_DEBUG, "Direct mapping disabled: "
               "image format is not supported.\n");
    }

    av_buffer_unref(&test_surface);
    return 0;

fail:
    av_buffer_unref(&test_surface);
    av_freep(&avfc->surface_ids);
    av_freep(&ctx->attributes);
    return err;
}

static void vaapi_frames_uninit(AVHWFramesContext *hwfc)
{
    AVVAAPIFramesContext *avfc = hwfc->hwctx;
    VAAPIFramesContext    *ctx = hwfc->internal->priv;

    av_freep(&avfc->surface_ids);
    av_freep(&ctx->attributes);
}

static int vaapi_get_buffer(AVHWFramesContext *hwfc, AVFrame *frame)
{
    frame->buf[0] = av_buffer_pool_get(hwfc->pool);
    if (!frame->buf[0])
        return AVERROR(ENOMEM);

    frame->data[3] = frame->buf[0]->data;
    frame->format  = AV_PIX_FMT_VAAPI;
    frame->width   = hwfc->width;
    frame->height  = hwfc->height;

    return 0;
}

static int vaapi_transfer_get_formats(AVHWFramesContext *hwfc,
                                      enum AVHWFrameTransferDirection dir,
                                      enum AVPixelFormat **formats)
{
    VAAPIDeviceContext *ctx = hwfc->device_ctx->internal->priv;
    enum AVPixelFormat *pix_fmts, preferred_format;
    int i, k;

    preferred_format = hwfc->sw_format;

    pix_fmts = av_malloc((ctx->nb_formats + 1) * sizeof(*pix_fmts));
    if (!pix_fmts)
        return AVERROR(ENOMEM);

    pix_fmts[0] = preferred_format;
    k = 1;
    for (i = 0; i < ctx->nb_formats; i++) {
        if (ctx->formats[i].pix_fmt == preferred_format)
            continue;
        av_assert0(k < ctx->nb_formats);
        pix_fmts[k++] = ctx->formats[i].pix_fmt;
    }
    av_assert0(k == ctx->nb_formats);
    pix_fmts[k] = AV_PIX_FMT_NONE;

    *formats = pix_fmts;
    return 0;
}

static void vaapi_unmap_frame(AVHWFramesContext *hwfc,
                              HWMapDescriptor *hwmap)
{
    AVVAAPIDeviceContext *hwctx = hwfc->device_ctx->hwctx;
    VAAPIMapping           *map = hwmap->priv;
    VASurfaceID surface_id;
    VAStatus vas;

    surface_id = (VASurfaceID)(uintptr_t)hwmap->source->data[3];
    av_log(hwfc, AV_LOG_DEBUG, "Unmap surface %#x.\n", surface_id);

    vas = vaUnmapBuffer(hwctx->display, map->image.buf);
    if (vas != VA_STATUS_SUCCESS) {
        av_log(hwfc, AV_LOG_ERROR, "Failed to unmap image from surface "
               "%#x: %d (%s).\n", surface_id, vas, vaErrorStr(vas));
    }

    if ((map->flags & AV_HWFRAME_MAP_WRITE) &&
        !(map->flags & AV_HWFRAME_MAP_DIRECT)) {
        vas = vaPutImage(hwctx->display, surface_id, map->image.image_id,
                         0, 0, hwfc->width, hwfc->height,
                         0, 0, hwfc->width, hwfc->height);
        if (vas != VA_STATUS_SUCCESS) {
            av_log(hwfc, AV_LOG_ERROR, "Failed to write image to surface "
                   "%#x: %d (%s).\n", surface_id, vas, vaErrorStr(vas));
        }
    }

    vas = vaDestroyImage(hwctx->display, map->image.image_id);
    if (vas != VA_STATUS_SUCCESS) {
        av_log(hwfc, AV_LOG_ERROR, "Failed to destroy image from surface "
               "%#x: %d (%s).\n", surface_id, vas, vaErrorStr(vas));
    }

    av_free(map);
}

static int vaapi_map_frame(AVHWFramesContext *hwfc,
                           AVFrame *dst, const AVFrame *src, int flags)
{
    AVVAAPIDeviceContext *hwctx = hwfc->device_ctx->hwctx;
    VAAPIFramesContext *ctx = hwfc->internal->priv;
    VASurfaceID surface_id;
    VAImageFormat *image_format;
    VAAPIMapping *map;
    VAStatus vas;
    void *address = NULL;
    int err, i;

    surface_id = (VASurfaceID)(uintptr_t)src->data[3];
    av_log(hwfc, AV_LOG_DEBUG, "Map surface %#x.\n", surface_id);

    if (!ctx->derive_works && (flags & AV_HWFRAME_MAP_DIRECT)) {
        // Requested direct mapping but it is not possible.
        return AVERROR(EINVAL);
    }
    if (dst->format == AV_PIX_FMT_NONE)
        dst->format = hwfc->sw_format;
    if (dst->format != hwfc->sw_format && (flags & AV_HWFRAME_MAP_DIRECT)) {
        // Requested direct mapping but the formats do not match.
        return AVERROR(EINVAL);
    }

    err = vaapi_get_image_format(hwfc->device_ctx, dst->format, &image_format);
    if (err < 0) {
        // Requested format is not a valid output format.
        return AVERROR(EINVAL);
    }

    map = av_malloc(sizeof(*map));
    if (!map)
        return AVERROR(ENOMEM);
    map->flags = flags;
    map->image.image_id = VA_INVALID_ID;

    vas = vaSyncSurface(hwctx->display, surface_id);
    if (vas != VA_STATUS_SUCCESS) {
        av_log(hwfc, AV_LOG_ERROR, "Failed to sync surface "
               "%#x: %d (%s).\n", surface_id, vas, vaErrorStr(vas));
        err = AVERROR(EIO);
        goto fail;
    }

    // The memory which we map using derive need not be connected to the CPU
    // in a way conducive to fast access.  On Gen7-Gen9 Intel graphics, the
    // memory is mappable but not cached, so normal memcpy()-like access is
    // very slow to read it (but writing is ok).  It is possible to read much
    // faster with a copy routine which is aware of the limitation, but we
    // assume for now that the user is not aware of that and would therefore
    // prefer not to be given direct-mapped memory if they request read access.
    if (ctx->derive_works && dst->format == hwfc->sw_format &&
        ((flags & AV_HWFRAME_MAP_DIRECT) || !(flags & AV_HWFRAME_MAP_READ))) {
        vas = vaDeriveImage(hwctx->display, surface_id, &map->image);
        if (vas != VA_STATUS_SUCCESS) {
            av_log(hwfc, AV_LOG_ERROR, "Failed to derive image from "
                   "surface %#x: %d (%s).\n",
                   surface_id, vas, vaErrorStr(vas));
            err = AVERROR(EIO);
            goto fail;
        }
        if (map->image.format.fourcc != image_format->fourcc) {
            av_log(hwfc, AV_LOG_ERROR, "Derive image of surface %#x "
                   "is in wrong format: expected %#08x, got %#08x.\n",
                   surface_id, image_format->fourcc, map->image.format.fourcc);
            err = AVERROR(EIO);
            goto fail;
        }
        map->flags |= AV_HWFRAME_MAP_DIRECT;
    } else {
        vas = vaCreateImage(hwctx->display, image_format,
                            hwfc->width, hwfc->height, &map->image);
        if (vas != VA_STATUS_SUCCESS) {
            av_log(hwfc, AV_LOG_ERROR, "Failed to create image for "
                   "surface %#x: %d (%s).\n",
                   surface_id, vas, vaErrorStr(vas));
            err = AVERROR(EIO);
            goto fail;
        }
        if (!(flags & AV_HWFRAME_MAP_OVERWRITE)) {
            vas = vaGetImage(hwctx->display, surface_id, 0, 0,
                             hwfc->width, hwfc->height, map->image.image_id);
            if (vas != VA_STATUS_SUCCESS) {
                av_log(hwfc, AV_LOG_ERROR, "Failed to read image from "
                       "surface %#x: %d (%s).\n",
                       surface_id, vas, vaErrorStr(vas));
                err = AVERROR(EIO);
                goto fail;
            }
        }
    }

    vas = vaMapBuffer(hwctx->display, map->image.buf, &address);
    if (vas != VA_STATUS_SUCCESS) {
        av_log(hwfc, AV_LOG_ERROR, "Failed to map image from surface "
               "%#x: %d (%s).\n", surface_id, vas, vaErrorStr(vas));
        err = AVERROR(EIO);
        goto fail;
    }

    err = ff_hwframe_map_create(src->hw_frames_ctx,
                                dst, src, &vaapi_unmap_frame, map);
    if (err < 0)
        goto fail;

    dst->width  = src->width;
    dst->height = src->height;

    for (i = 0; i < map->image.num_planes; i++) {
        dst->data[i] = (uint8_t*)address + map->image.offsets[i];
        dst->linesize[i] = map->image.pitches[i];
    }
    if (
#ifdef VA_FOURCC_YV16
        map->image.format.fourcc == VA_FOURCC_YV16 ||
#endif
        map->image.format.fourcc == VA_FOURCC_YV12) {
        // Chroma planes are YVU rather than YUV, so swap them.
        FFSWAP(uint8_t*, dst->data[1], dst->data[2]);
    }

    return 0;

fail:
    if (map) {
        if (address)
            vaUnmapBuffer(hwctx->display, map->image.buf);
        if (map->image.image_id != VA_INVALID_ID)
            vaDestroyImage(hwctx->display, map->image.image_id);
        av_free(map);
    }
    return err;
}

static int vaapi_transfer_data_from(AVHWFramesContext *hwfc,
                                    AVFrame *dst, const AVFrame *src)
{
    AVFrame *map;
    int err;

    if (dst->width > hwfc->width || dst->height > hwfc->height)
        return AVERROR(EINVAL);

    map = av_frame_alloc();
    if (!map)
        return AVERROR(ENOMEM);
    map->format = dst->format;

    err = vaapi_map_frame(hwfc, map, src, AV_HWFRAME_MAP_READ);
    if (err)
        goto fail;

    map->width  = dst->width;
    map->height = dst->height;

    err = av_frame_copy(dst, map);
    if (err)
        goto fail;

    err = 0;
fail:
    av_frame_free(&map);
    return err;
}

static int vaapi_transfer_data_to(AVHWFramesContext *hwfc,
                                  AVFrame *dst, const AVFrame *src)
{
    AVFrame *map;
    int err;

    if (src->width > hwfc->width || src->height > hwfc->height)
        return AVERROR(EINVAL);

    map = av_frame_alloc();
    if (!map)
        return AVERROR(ENOMEM);
    map->format = src->format;

    err = vaapi_map_frame(hwfc, map, dst, AV_HWFRAME_MAP_WRITE | AV_HWFRAME_MAP_OVERWRITE);
    if (err)
        goto fail;

    map->width  = src->width;
    map->height = src->height;

    err = av_frame_copy(map, src);
    if (err)
        goto fail;

    err = 0;
fail:
    av_frame_free(&map);
    return err;
}

static int vaapi_map_from(AVHWFramesContext *hwfc, AVFrame *dst,
                          const AVFrame *src, int flags)
{
    int err;

    if (dst->format != AV_PIX_FMT_NONE) {
        err = vaapi_get_image_format(hwfc->device_ctx, dst->format, NULL);
        if (err < 0)
            return AVERROR(ENOSYS);
    }

    err = vaapi_map_frame(hwfc, dst, src, flags);
    if (err)
        return err;

    err = av_frame_copy_props(dst, src);
    if (err)
        return err;

    return 0;
}

static void vaapi_device_free(AVHWDeviceContext *ctx)
{
    AVVAAPIDeviceContext *hwctx = ctx->hwctx;
    VAAPIDevicePriv      *priv  = ctx->user_opaque;

    if (hwctx->display)
        vaTerminate(hwctx->display);

#if HAVE_VAAPI_X11
    if (priv->x11_display)
        XCloseDisplay(priv->x11_display);
#endif

    if (priv->drm_fd >= 0)
        close(priv->drm_fd);

    av_freep(&priv);
}

static int vaapi_device_create(AVHWDeviceContext *ctx, const char *device,
                               AVDictionary *opts, int flags)
{
    AVVAAPIDeviceContext *hwctx = ctx->hwctx;
    VAAPIDevicePriv *priv;
    VADisplay display = 0;
    VAStatus vas;
    int major, minor;

    priv = av_mallocz(sizeof(*priv));
    if (!priv)
        return AVERROR(ENOMEM);

    priv->drm_fd = -1;

    ctx->user_opaque = priv;
    ctx->free        = vaapi_device_free;

#if HAVE_VAAPI_X11
    if (!display && !(device && device[0] == '/')) {
        // Try to open the device as an X11 display.
        priv->x11_display = XOpenDisplay(device);
        if (!priv->x11_display) {
            av_log(ctx, AV_LOG_VERBOSE, "Cannot open X11 display "
                   "%s.\n", XDisplayName(device));
        } else {
            display = vaGetDisplay(priv->x11_display);
            if (!display) {
                av_log(ctx, AV_LOG_ERROR, "Cannot open a VA display "
                       "from X11 display %s.\n", XDisplayName(device));
                return AVERROR_UNKNOWN;
            }

            av_log(ctx, AV_LOG_VERBOSE, "Opened VA display via "
                   "X11 display %s.\n", XDisplayName(device));
        }
    }
#endif

#if HAVE_VAAPI_DRM
    if (!display) {
        // Try to open the device as a DRM path.
        // Default to using the first render node if the user did not
        // supply a path.
        const char *path = device ? device : "/dev/dri/renderD128";
        priv->drm_fd = open(path, O_RDWR);
        if (priv->drm_fd < 0) {
            av_log(ctx, AV_LOG_VERBOSE, "Cannot open DRM device %s.\n",
                   path);
        } else {
            display = vaGetDisplayDRM(priv->drm_fd);
            if (!display) {
                av_log(ctx, AV_LOG_ERROR, "Cannot open a VA display "
                       "from DRM device %s.\n", path);
                return AVERROR_UNKNOWN;
            }

            av_log(ctx, AV_LOG_VERBOSE, "Opened VA display via "
                   "DRM device %s.\n", path);
        }
    }
#endif

    if (!display) {
        av_log(ctx, AV_LOG_ERROR, "No VA display found for "
               "device: %s.\n", device ? device : "");
        return AVERROR(EINVAL);
    }

    hwctx->display = display;

    vas = vaInitialize(display, &major, &minor);
    if (vas != VA_STATUS_SUCCESS) {
        av_log(ctx, AV_LOG_ERROR, "Failed to initialise VAAPI "
               "connection: %d (%s).\n", vas, vaErrorStr(vas));
        return AVERROR(EIO);
    }
    av_log(ctx, AV_LOG_VERBOSE, "Initialised VAAPI connection: "
           "version %d.%d\n", major, minor);

    return 0;
}

const HWContextType ff_hwcontext_type_vaapi = {
    .type                   = AV_HWDEVICE_TYPE_VAAPI,
    .name                   = "VAAPI",

    .device_hwctx_size      = sizeof(AVVAAPIDeviceContext),
    .device_priv_size       = sizeof(VAAPIDeviceContext),
    .device_hwconfig_size   = sizeof(AVVAAPIHWConfig),
    .frames_hwctx_size      = sizeof(AVVAAPIFramesContext),
    .frames_priv_size       = sizeof(VAAPIFramesContext),

    .device_create          = &vaapi_device_create,
    .device_init            = &vaapi_device_init,
    .device_uninit          = &vaapi_device_uninit,
    .frames_get_constraints = &vaapi_frames_get_constraints,
    .frames_init            = &vaapi_frames_init,
    .frames_uninit          = &vaapi_frames_uninit,
    .frames_get_buffer      = &vaapi_get_buffer,
    .transfer_get_formats   = &vaapi_transfer_get_formats,
    .transfer_data_to       = &vaapi_transfer_data_to,
    .transfer_data_from     = &vaapi_transfer_data_from,
    .map_to                 = NULL,
    .map_from               = &vaapi_map_from,

    .pix_fmts = (const enum AVPixelFormat[]) {
        AV_PIX_FMT_VAAPI,
        AV_PIX_FMT_NONE
    },
};
