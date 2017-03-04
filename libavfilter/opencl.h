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

#ifndef AVFILTER_OPENCL_H
#define AVFILTER_OPENCL_H

#include "libavutil/buffer.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_opencl.h"
#include "libavutil/pixfmt.h"

#include "avfilter.h"

typedef struct OpenCLFilterContext {
    const AVClass     *class;

    AVBufferRef       *device_ref;
    AVHWDeviceContext *device;
    AVOpenCLDeviceContext *hwctx;

    cl_program         program;

    enum AVPixelFormat output_format;
    int                output_width;
    int                output_height;
} OpenCLFilterContext;

/**
 * Return that all inputs and outputs support only AV_PIX_FMT_OPENCL.
 */
int ff_opencl_filter_query_formats(AVFilterContext *avctx);

/**
 * Check that the input link contains a suitable hardware frames
 * context and extract the device from it.
 */
int ff_opencl_filter_config_input(AVFilterLink *inlink);

/**
 * Create a suitable hardware frames context for the output.
 */
int ff_opencl_filter_config_output(AVFilterLink *outlink);

int ff_opencl_filter_init(AVFilterContext *avctx);

void ff_opencl_filter_uninit(AVFilterContext *avctx);

int ff_opencl_filter_load_program(AVFilterContext *avctx,
                                  const char **program_source_array,
                                  int nb_strings);

int ff_opencl_filter_load_program_from_file(AVFilterContext *avctx,
                                            const char *filename);

const char *ff_opencl_make_rgbyuv(const char *called_name,
                                  enum AVColorSpace colorspace,
                                  int to_yuv);

#endif /* AVFILTER_OPENCL_H */
