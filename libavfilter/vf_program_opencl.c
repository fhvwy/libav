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

#include "libavutil/buffer.h"
#include "libavutil/common.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_opencl.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"
#include "libavutil/pixdesc.h"
#include "libavutil/opt.h"

#include "avfilter.h"
#include "internal.h"
#include "opencl.h"
#include "video.h"

typedef struct ProgramOpenCLContext {
    OpenCLFilterContext ocf;

    int              initialised;
    cl_uint          index;
    cl_kernel        kernel;
    cl_command_queue command_queue;

    const char      *source_file;
    const char      *kernel_name;
} ProgramOpenCLContext;

static int program_opencl_init(AVFilterContext *avctx)
{
    ProgramOpenCLContext *ctx = avctx->priv;
    cl_int cle;
    int err;

    err = ff_opencl_filter_load_program_from_file(avctx, ctx->source_file);
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

    ctx->kernel = clCreateKernel(ctx->ocf.program, ctx->kernel_name, &cle);
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

static int program_opencl_filter_frame(AVFilterLink *inlink, AVFrame *input)
{
    AVFilterContext    *avctx = inlink->dst;
    AVFilterLink     *outlink = avctx->outputs[0];
    ProgramOpenCLContext *ctx = avctx->priv;
    AVFrame *output = NULL;
    cl_int cle;
    size_t global_work[2];
    cl_mem src, dst;
    int err, plane;

    av_log(ctx, AV_LOG_DEBUG, "Filter input: %s, %ux%u (%"PRId64").\n",
           av_get_pix_fmt_name(input->format),
           input->width, input->height, input->pts);

    if (!input->hw_frames_ctx)
        return AVERROR(EINVAL);

    if (!ctx->initialised) {
        err = program_opencl_init(avctx);
        if (err < 0)
            goto fail;
    }

    output = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!output) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    for (plane = 0; plane < FF_ARRAY_ELEMS(output->data); plane++) {
        src = (cl_mem) input->data[plane];
        dst = (cl_mem)output->data[plane];

        if (!dst)
            break;

        cle = clSetKernelArg(ctx->kernel, 0, sizeof(cl_mem), &dst);
        if (cle != CL_SUCCESS) {
            av_log(avctx, AV_LOG_ERROR, "Failed to set kernel "
                   "destination image argument: %d.\n", cle);
            goto fail;
        }
        cle = clSetKernelArg(ctx->kernel, 1, sizeof(cl_mem), &src);
        if (cle != CL_SUCCESS) {
            av_log(avctx, AV_LOG_ERROR, "Failed to set kernel "
                   "source image argument: %d.\n", cle);
            goto fail;
        }
        cle = clSetKernelArg(ctx->kernel, 2, sizeof(cl_uint), &ctx->index);
        if (cle != CL_SUCCESS) {
            av_log(avctx, AV_LOG_ERROR, "Failed to set kernel "
                   "index argument: %d.\n", cle);
            goto fail;
        }

        cle = clGetImageInfo(dst, CL_IMAGE_WIDTH,  sizeof(size_t),
                             &global_work[0], NULL);
        cle = clGetImageInfo(dst, CL_IMAGE_HEIGHT, sizeof(size_t),
                             &global_work[1], NULL);

        av_log(avctx, AV_LOG_DEBUG, "Run kernel on plane %d "
               "(%zux%zu).\n", plane, global_work[0], global_work[1]);

        cle = clEnqueueNDRangeKernel(ctx->command_queue, ctx->kernel, 2, NULL,
                                     global_work, NULL, 0, NULL, NULL);
        if (cle != CL_SUCCESS) {
            av_log(avctx, AV_LOG_ERROR, "Failed to enqueue kernel: %d.\n",
                   cle);
            err = AVERROR(EIO);
            goto fail;
        }
    }

    cle = clFinish(ctx->command_queue);
    if (cle != CL_SUCCESS) {
        av_log(avctx, AV_LOG_ERROR, "Failed to finish command queue: %d.\n",
               cle);
        err = AVERROR(EIO);
        goto fail;
    }

    err = av_frame_copy_props(output, input);
    if (err < 0)
        goto fail;

    av_frame_free(&input);

    ++ctx->index;

    av_log(ctx, AV_LOG_DEBUG, "Filter output: %s, %ux%u (%"PRId64").\n",
           av_get_pix_fmt_name(output->format),
           output->width, output->height, output->pts);

    return ff_filter_frame(outlink, output);

fail:
    clFinish(ctx->command_queue);
    av_frame_free(&input);
    av_frame_free(&output);
    return err;
}

static av_cold void program_opencl_uninit(AVFilterContext *avctx)
{
    ProgramOpenCLContext *ctx = avctx->priv;
    cl_int cle;

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

#define OFFSET(x) offsetof(ProgramOpenCLContext, x)
#define FLAGS (AV_OPT_FLAG_VIDEO_PARAM)
static const AVOption program_opencl_options[] = {
    { "source", "OpenCL program source file",
      OFFSET(source_file),  AV_OPT_TYPE_STRING, { .str = 0 }, .flags = FLAGS },
    { "kernel", "Kernel name in program",
      OFFSET(kernel_name), AV_OPT_TYPE_STRING, { .str = 0 }, .flags = FLAGS },
    { "w", "Output video width",
      OFFSET(ocf.output_width), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, INT_MAX, .flags = FLAGS },
    { "h", "Output video height",
      OFFSET(ocf.output_height),AV_OPT_TYPE_INT, { .i64 = 0 }, 0, INT_MAX, .flags = FLAGS },
    { NULL },
};

static const AVClass program_opencl_class = {
    .class_name = "program_opencl",
    .item_name  = av_default_item_name,
    .option     = program_opencl_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static const AVFilterPad program_opencl_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = &program_opencl_filter_frame,
        .config_props = &ff_opencl_filter_config_input,
    },
    { NULL }
};

static const AVFilterPad program_opencl_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = &ff_opencl_filter_config_output,
    },
    { NULL }
};

AVFilter ff_vf_program_opencl = {
    .name           = "program_opencl",
    .description    = NULL_IF_CONFIG_SMALL("Filter using an OpenCL program"),
    .priv_size      = sizeof(ProgramOpenCLContext),
    .priv_class     = &program_opencl_class,
    .init           = &ff_opencl_filter_init,
    .uninit         = &program_opencl_uninit,
    .query_formats  = &ff_opencl_filter_query_formats,
    .inputs         = program_opencl_inputs,
    .outputs        = program_opencl_outputs,
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
};
