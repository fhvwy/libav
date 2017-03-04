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

#include <stdio.h>
#include <string.h>

#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_opencl.h"
#include "libavutil/mem.h"

#include "avfilter.h"
#include "formats.h"
#include "opencl.h"

int ff_opencl_filter_query_formats(AVFilterContext *avctx)
{
    const static enum AVPixelFormat formats[] = {
        AV_PIX_FMT_OPENCL,
        AV_PIX_FMT_NONE,
    };
    int i;

    for (i = 0; i < avctx->nb_inputs; i++) {
        ff_formats_ref(ff_make_format_list(formats),
                       &avctx->inputs[i]->out_formats);
    }

    for (i = 0; i < avctx->nb_outputs; i++) {
        ff_formats_ref(ff_make_format_list(formats),
                       &avctx->outputs[i]->in_formats);
    }

    return 0;
}

int ff_opencl_filter_config_input(AVFilterLink *inlink)
{
    AVFilterContext   *avctx = inlink->dst;
    OpenCLFilterContext *ctx = avctx->priv;
    AVHWFramesContext *input_frames;

    if (!inlink->hw_frames_ctx) {
        av_log(avctx, AV_LOG_ERROR, "OpenCL filtering requires a "
               "hardware frames context on the input.\n");
        return AVERROR(EINVAL);
    }

    // Extract the device and default output format from the first input.
    if (avctx->inputs[0] != inlink)
        return 0;

    input_frames = (AVHWFramesContext*)inlink->hw_frames_ctx->data;

    if (input_frames->format != AV_PIX_FMT_OPENCL)
        return AVERROR(EINVAL);

    ctx->device_ref = av_buffer_ref(input_frames->device_ref);
    if (!ctx->device_ref)
        return AVERROR(ENOMEM);
    ctx->device = input_frames->device_ctx;
    ctx->hwctx  = ctx->device->hwctx;

    // Default output parameters match input parameters.
    if (ctx->output_format == AV_PIX_FMT_NONE)
        ctx->output_format = input_frames->sw_format;
    if (!ctx->output_width)
        ctx->output_width  = inlink->w;
    if (!ctx->output_height)
        ctx->output_height = inlink->h;

    return 0;
}

int ff_opencl_filter_config_output(AVFilterLink *outlink)
{
    AVFilterContext   *avctx = outlink->src;
    OpenCLFilterContext *ctx = avctx->priv;
    AVBufferRef       *output_frames_ref = NULL;
    AVHWFramesContext *output_frames;
    int err;

    av_buffer_unref(&outlink->hw_frames_ctx);

    output_frames_ref = av_hwframe_ctx_alloc(ctx->device_ref);
    if (!output_frames_ref) {
        err = AVERROR(ENOMEM);
        goto fail;
    }
    output_frames = (AVHWFramesContext*)output_frames_ref->data;

    output_frames->format    = AV_PIX_FMT_OPENCL;
    output_frames->sw_format = ctx->output_format;
    output_frames->width     = ctx->output_width;
    output_frames->height    = ctx->output_height;

    err = av_hwframe_ctx_init(output_frames_ref);
    if (err < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to initialise output "
               "frames: %d.\n", err);
        goto fail;
    }

    outlink->hw_frames_ctx = output_frames_ref;
    outlink->w = ctx->output_width;
    outlink->h = ctx->output_height;

    return 0;
fail:
    av_buffer_unref(&output_frames_ref);
    return err;
}

int ff_opencl_filter_init(AVFilterContext *avctx)
{
    OpenCLFilterContext *ctx = avctx->priv;

    ctx->output_format = AV_PIX_FMT_NONE;

    return 0;
}

void ff_opencl_filter_uninit(AVFilterContext *avctx)
{
    OpenCLFilterContext *ctx = avctx->priv;
    cl_int cle;

    if (ctx->program) {
        cle = clReleaseProgram(ctx->program);
        if (cle != CL_SUCCESS)
            av_log(avctx, AV_LOG_ERROR, "Failed to release "
                   "program: %d.\n", cle);
    }

    av_buffer_unref(&ctx->device_ref);
}

const char *ff_opencl_make_rgbyuv(const char *called_name,
                                  enum AVColorSpace colorspace,
                                  int to_yuv)
{
    const char *function;
    const char *suffix;
    char tmp[256];

    if (to_yuv)
        function = "rgb_to_yuv";
    else
        function = "yuv_to_rgb";

    switch (colorspace) {
    case AVCOL_SPC_BT709:
    default:
        suffix = "bt709";
        break;
    case AVCOL_SPC_BT470BG:
        suffix = "bt470bg";
        break;
    case AVCOL_SPC_SMPTE170M:
    case AVCOL_SPC_SMPTE240M:
        suffix = "smpte170m";
        break;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL:
        suffix = "bt2020";
        break;
    }

    snprintf(tmp, sizeof(tmp), "#line %d \"%s\"\n"
             "#define %s_%s %s_%s\n", __LINE__, __FILE__,
             function, called_name, function, suffix);

    return av_strdup(tmp);
}

static int opencl_filter_load_program(AVFilterContext *avctx,
                                      const char **program_source_array,
                                      int nb_strings)
{
    OpenCLFilterContext *ctx = avctx->priv;
    cl_int cle;

    ctx->program = clCreateProgramWithSource(ctx->hwctx->context, nb_strings,
                                             program_source_array,
                                             NULL, &cle);
    if (!ctx->program) {
        av_log(avctx, AV_LOG_ERROR, "Failed to create program: %d.\n", cle);
        return AVERROR(EIO);
    }

    cle = clBuildProgram(ctx->program, 1, &ctx->hwctx->device_id,
                         NULL, NULL, NULL);
    if (cle != CL_SUCCESS) {
        av_log(avctx, AV_LOG_ERROR, "Failed to build program: %d.\n", cle);

        if (cle == CL_BUILD_PROGRAM_FAILURE) {
            char *log;
            size_t log_length;

            clGetProgramBuildInfo(ctx->program, ctx->hwctx->device_id,
                                  CL_PROGRAM_BUILD_LOG, 0, NULL, &log_length);

            log = av_malloc(log_length);
            if (log) {
                cle = clGetProgramBuildInfo(ctx->program,
                                            ctx->hwctx->device_id,
                                            CL_PROGRAM_BUILD_LOG,
                                            log_length, log, NULL);
                if (cle == CL_SUCCESS)
                    av_log(avctx, AV_LOG_ERROR, "Build log:\n%s\n", log);
            }

            av_free(log);
        }

        clReleaseProgram(ctx->program);
        return AVERROR(EIO);
    }

    return 0;
}

int ff_opencl_filter_load_program(AVFilterContext *avctx,
                                  const char **program_source_array,
                                  int nb_strings)
{
    return opencl_filter_load_program(avctx, program_source_array, nb_strings);
}

int ff_opencl_filter_load_program_from_file(AVFilterContext *avctx,
                                            const char *filename)
{
    FILE *file;
    char *src = NULL;
    size_t pos, len, rb;
    const char *src_const;
    int err;

    file = fopen(filename, "r");
    if (!file) {
        av_log(avctx, AV_LOG_ERROR, "Unable to open program "
               "source file \"%s\".\n", filename);
        return AVERROR(ENOENT);
    }

    len = 1 << 16;
    pos = 0;
    while (1) {
        err = av_reallocp(&src, len);
        if (err < 0)
            goto fail;
        rb = fread(src + pos, 1, len - pos - 1, file);
        if (rb == 0 && ferror(file)) {
            err = AVERROR(EIO);
            goto fail;
        }
        pos += rb;
        if (pos < len)
            break;
        len <<= 1;
    }
    src[pos] = 0;

    src_const = src;

    err = opencl_filter_load_program(avctx, &src_const, 1);
fail:
    fclose(file);
    av_freep(&src);
    return err;
}
