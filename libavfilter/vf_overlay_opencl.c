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

#include "libavutil/avassert.h"
#include "libavutil/buffer.h"
#include "libavutil/common.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_opencl.h"
#include "libavutil/log.h"
#include "libavutil/mathematics.h"
#include "libavutil/mem.h"
#include "libavutil/pixdesc.h"
#include "libavutil/opt.h"

#include "avfilter.h"
#include "internal.h"
#include "opencl.h"
#include "opencl_source.h"
#include "video.h"

typedef struct OverlayOpenCLContext {
    OpenCLFilterContext ocf;

    int              initialised;
    cl_kernel        kernel;
    cl_command_queue command_queue;

    AVFrame         *main;
    AVFrame         *overlay;
    AVFrame         *overlay_next;

    int              x_position;
    int              y_position;
} OverlayOpenCLContext;


static int overlay_opencl_load(AVFilterContext *avctx,
                               enum AVColorSpace colorspace)
{
    OverlayOpenCLContext *ctx = avctx->priv;
    cl_int cle;
    const char *source[4];
    int err;

    source[0] = ff_opencl_source_rgbyuv;
    source[1] = ff_opencl_make_rgbyuv("input",  colorspace, 0);
    source[2] = ff_opencl_make_rgbyuv("output", colorspace, 1);
    source[3] = ff_opencl_source_overlay;

    err = ff_opencl_filter_load_program(avctx, source, 4);

    av_freep(&source[1]);
    av_freep(&source[2]);

    if (err < 0)
        goto fail;

    ctx->command_queue = clCreateCommandQueue(ctx->ocf.hwctx->context,
                                              ctx->ocf.hwctx->device_id,
                                              0, &cle);
    if (!ctx->command_queue) {
        av_log(avctx, AV_LOG_ERROR, "Failed to create OpenCL "
               "command queue: %d.\n", cle);
        err = AVERROR(EIO);
        goto fail;
    }

    ctx->kernel = clCreateKernel(ctx->ocf.program, "overlay_nv12_rgba", &cle);
    if (!ctx->kernel) {
        av_log(avctx, AV_LOG_ERROR, "Failed to create kernel: %d.\n", cle);
        err = AVERROR(EIO);
        goto fail;
    }

    ctx->initialised = 1;
    return 0;

fail:
    if (ctx->command_queue)
        clReleaseCommandQueue(ctx->command_queue);
    if (ctx->kernel)
        clReleaseKernel(ctx->kernel);
    return err;
}

static int overlay_opencl_filter_main(AVFilterLink *inlink, AVFrame *input)
{
    AVFilterContext    *avctx = inlink->dst;
    OverlayOpenCLContext *ctx = avctx->priv;

    av_log(avctx, AV_LOG_DEBUG, "Filter main: %s, %ux%u (%"PRId64").\n",
           av_get_pix_fmt_name(input->format),
           input->width, input->height, input->pts);

    av_assert0(!ctx->main);
    ctx->main = input;

    return 0;
}

static int overlay_opencl_filter_overlay(AVFilterLink *inlink, AVFrame *input)
{
    AVFilterContext    *avctx = inlink->dst;
    OverlayOpenCLContext *ctx = avctx->priv;

    av_log(avctx, AV_LOG_DEBUG, "Filter overlay: %s, %ux%u (%"PRId64").\n",
           av_get_pix_fmt_name(input->format),
           input->width, input->height, input->pts);

    av_assert0(!ctx->overlay_next);
    ctx->overlay_next = input;

    return 0;
}

static int overlay_opencl_request_frame(AVFilterLink *outlink)
{
    AVFilterContext    *avctx = outlink->src;
    OverlayOpenCLContext *ctx = avctx->priv;
    AVFrame *output;
    cl_mem mem;
    cl_int cle, x, y;
    size_t global_work[2];
    int kernel_arg = 0;
    int err;

    av_log(avctx, AV_LOG_DEBUG, "Filter request frame.\n");

    if (!ctx->main) {
        err = ff_request_frame(avctx->inputs[0]);
        if (err < 0)
            return err;
    }
    if (!ctx->main)
        return AVERROR(EAGAIN);

    if (!ctx->initialised) {
        err = overlay_opencl_load(avctx, ctx->main->colorspace);
        if (err < 0)
            return err;
    }

    if (!ctx->overlay_next) {
        err = ff_request_frame(avctx->inputs[1]);
        if (err < 0)
            return err;
    }

    while (!ctx->overlay ||
           av_compare_ts(ctx->main->pts,
                         avctx->inputs[0]->time_base,
                         ctx->overlay_next->pts,
                         avctx->inputs[1]->time_base) > 0) {
        av_frame_free(&ctx->overlay);
        ctx->overlay = ctx->overlay_next;
        ctx->overlay_next = NULL;

        err = ff_request_frame(avctx->inputs[1]);
        if (err < 0)
            return err;
    }

    output = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!output) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    mem = (cl_mem)output->data[0];
    cle = clSetKernelArg(ctx->kernel, kernel_arg++, sizeof(cl_mem), &mem);
    if (cle != CL_SUCCESS) goto fail_kernel_arg;
    mem = (cl_mem)output->data[1];
    cle = clSetKernelArg(ctx->kernel, kernel_arg++, sizeof(cl_mem), &mem);
    if (cle != CL_SUCCESS) goto fail_kernel_arg;
    mem = (cl_mem)ctx->main->data[0];
    cle = clSetKernelArg(ctx->kernel, kernel_arg++, sizeof(cl_mem), &mem);
    if (cle != CL_SUCCESS) goto fail_kernel_arg;
    mem = (cl_mem)ctx->main->data[1];
    cle = clSetKernelArg(ctx->kernel, kernel_arg++, sizeof(cl_mem), &mem);
    if (cle != CL_SUCCESS) goto fail_kernel_arg;
    mem = (cl_mem)ctx->overlay->data[0];
    cle = clSetKernelArg(ctx->kernel, kernel_arg++, sizeof(cl_mem), &mem);
    if (cle != CL_SUCCESS) goto fail_kernel_arg;

    x = ctx->x_position;
    y = ctx->y_position;
    cle = clSetKernelArg(ctx->kernel, kernel_arg++, sizeof(cl_int), &x);
    if (cle != CL_SUCCESS) goto fail_kernel_arg;
    cle = clSetKernelArg(ctx->kernel, kernel_arg++, sizeof(cl_int), &y);
    if (cle != CL_SUCCESS) goto fail_kernel_arg;

    // The kernel processes a 2x2 block.
    global_work[0] = output->width  / 2;
    global_work[1] = output->height / 2;

    cle = clEnqueueNDRangeKernel(ctx->command_queue, ctx->kernel, 2, NULL,
                                 global_work, NULL, 0, NULL, NULL);
    if (cle != CL_SUCCESS) {
        av_log(avctx, AV_LOG_ERROR, "Failed to enqueue "
               "overlay kernel: %d.\n", cle);
        err = AVERROR(EIO);
        goto fail;
    }

    cle = clFinish(ctx->command_queue);
    if (cle != CL_SUCCESS) {
        av_log(avctx, AV_LOG_ERROR, "Failed to finish "
               "command queue: %d.\n", cle);
        err = AVERROR(EIO);
        goto fail;
    }

    err = av_frame_copy_props(output, ctx->main);

    av_frame_free(&ctx->main);

    av_log(avctx, AV_LOG_DEBUG, "Filter output: %s, %ux%u (%"PRId64").\n",
           av_get_pix_fmt_name(output->format),
           output->width, output->height, output->pts);

    return ff_filter_frame(outlink, output);
fail_kernel_arg:
    av_log(avctx, AV_LOG_ERROR, "Failed to set kernel arg %d: %d.\n",
           kernel_arg, cle);
    err = AVERROR(EIO);
fail:
    return err;
}

static av_cold void overlay_opencl_uninit(AVFilterContext *avctx)
{
    OverlayOpenCLContext *ctx = avctx->priv;
    cl_int cle;

    av_frame_free(&ctx->main);
    av_frame_free(&ctx->overlay);
    av_frame_free(&ctx->overlay_next);

    if (ctx->kernel) {
        cle = clReleaseKernel(ctx->kernel);
        if (cle != CL_SUCCESS)
            av_log(avctx, AV_LOG_ERROR, "Failed to release "
                   "kernel: %d.\n", cle);
    }

    if (ctx->command_queue) {
        cle = clReleaseCommandQueue(ctx->command_queue);
        if (cle != CL_SUCCESS)
            av_log(avctx, AV_LOG_ERROR, "Failed to release "
                   "command queue: %d.\n", cle);
    }

    ff_opencl_filter_uninit(avctx);
}

#define OFFSET(x) offsetof(OverlayOpenCLContext, x)
#define FLAGS (AV_OPT_FLAG_VIDEO_PARAM)
static const AVOption overlay_opencl_options[] = {
    { "x", "Overlay x position",
      OFFSET(x_position), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, INT_MAX, .flags = FLAGS },
    { "y", "Overlay y position",
      OFFSET(y_position), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, INT_MAX, .flags = FLAGS },
    { NULL },
};

static const AVClass overlay_opencl_class = {
    .class_name = "overlay_opencl",
    .item_name  = av_default_item_name,
    .option     = overlay_opencl_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static const AVFilterPad overlay_opencl_inputs[] = {
    {
        .name         = "main",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = &ff_opencl_filter_config_input,
        .filter_frame = &overlay_opencl_filter_main,
    },
    {
        .name         = "overlay",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = &ff_opencl_filter_config_input,
        .filter_frame = &overlay_opencl_filter_overlay,
        .needs_fifo   = 1,
    },
    { NULL }
};

static const AVFilterPad overlay_opencl_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .config_props  = &ff_opencl_filter_config_output,
        .request_frame = &overlay_opencl_request_frame,
    },
    { NULL }
};

AVFilter ff_vf_overlay_opencl = {
    .name           = "overlay_opencl",
    .description    = NULL_IF_CONFIG_SMALL("Overlay one video on top of another"),
    .priv_size      = sizeof(OverlayOpenCLContext),
    .priv_class     = &overlay_opencl_class,
    .init           = &ff_opencl_filter_init,
    .uninit         = &overlay_opencl_uninit,
    .query_formats  = &ff_opencl_filter_query_formats,
    .inputs         = overlay_opencl_inputs,
    .outputs        = overlay_opencl_outputs,
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
};
