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

#include "cbs.h"
#include "cbs_internal.h"
#include "cbs_vp9.h"
#include "internal.h"


static int cbs_vp9_read_s(CodedBitstreamContext *ctx, BitstreamContext *bc,
                          int width, const char *name, int32_t *write_to)
{
    uint32_t magnitude;
    int position, sign;
    int32_t result;

    if (ctx->trace_enable)
        position = bitstream_tell(bc);

    magnitude = bitstream_read(bc, width);
    sign      = bitstream_read_bit(bc);
    result    = sign ? -(int32_t)magnitude : magnitude;

    if (ctx->trace_enable) {
        char bits[33];
        int i;
        for (i = 0; i < width; i++)
            bits[i] = magnitude >> (width - i - 1) & 1 ? '1' : '0';
        bits[i] = sign ? '1' : '0';
        bits[i + 1] = 0;

        ff_cbs_trace_syntax_element(ctx, position, name, bits, result);
    }

    *write_to = result;
    return 0;
}

static int cbs_vp9_write_s(CodedBitstreamContext *ctx, PutBitContext *pbc,
                           int width, const char *name, int32_t value)
{
    uint32_t magnitude;
    int sign;

    sign      = value < 0;
    magnitude = sign ? -value : value;

    if (ctx->trace_enable) {
        char bits[33];
        int i;
        for (i = 0; i < width; i++)
            bits[i] = magnitude >> (width - i - 1) & 1 ? '1' : '0';
        bits[i] = sign ? '1' : '0';
        bits[i + 1] = 0;

        ff_cbs_trace_syntax_element(ctx, put_bits_count(pbc), name, bits, value);
    }

    put_bits(pbc, width, magnitude);
    put_bits(pbc, 1, sign);

    return 0;
}

static int cbs_vp9_read_le(CodedBitstreamContext *ctx, BitstreamContext *bc,
                           int width, const char *name, uint32_t *write_to)
{
    uint32_t result;
    int position, b;

    av_assert0(width % 8 == 0);

    if (ctx->trace_enable)
        position = bitstream_tell(bc);

    result = 0;
    for (b = 0; b < width; b += 8)
        result |= bitstream_read(bc, 8) << b;

    if (ctx->trace_enable) {
        char bits[33];
        int i;
        for (b = 0; b < width; b += 8)
            for (i = 0; i < 8; i++)
                bits[b + i] = result >> (b + i) & 1 ? '1' : '0';
        bits[b] = 0;

        ff_cbs_trace_syntax_element(ctx, position, name, bits, result);
    }

    *write_to = result;
    return 0;
}

static int cbs_vp9_write_le(CodedBitstreamContext *ctx, PutBitContext *pbc,
                            int width, const char *name, uint32_t value)
{
    int b;

    av_assert0(width % 8 == 0);

    if (ctx->trace_enable) {
        char bits[33];
        int i;
        for (b = 0; b < width; b += 8)
            for (i = 0; i < 8; i++)
                bits[b + i] = value >> (b + i) & 1 ? '1' : '0';
        bits[b] = 0;

        ff_cbs_trace_syntax_element(ctx, put_bits_count(pbc), name, bits, value);
    }

    for (b = 0; b < width; b += 8)
        put_bits(pbc, 8, value >> b & 0xff);

    return 0;
}

#define HEADER(name) do { \
        ff_cbs_trace_header(ctx, name); \
    } while (0)

#define CHECK(call) do { \
        err = (call); \
        if (err < 0) \
            return err; \
    } while (0)

#define FUNC_NAME(rw, codec, name) cbs_ ## codec ## _ ## rw ## _ ## name
#define FUNC_VP9(rw, name) FUNC_NAME(rw, vp9, name)
#define FUNC(name) FUNC_VP9(READWRITE, name)


#define READ
#define READWRITE read
#define RWContext BitstreamContext

#define xf(width, name, var) do { \
        uint32_t value = 0; \
        CHECK(ff_cbs_read_unsigned(ctx, rw, width, #name, \
                                   &value, 0, (1 << width) - 1)); \
        var = value; \
    } while (0)
#define xs(width, name, var) do { \
        int32_t value = 0; \
        CHECK(cbs_vp9_read_s(ctx, rw, width, #name, &value)); \
        var = value; \
    } while (0)

#define f(width, name) \
        xf(width, name, current->name)
#define s(width, name) \
        xs(width, name, current->name)

#define infer(name, value) do { \
        current->name = value; \
    } while (0)

#define delta_q(name) do { \
        uint8_t delta_coded; \
        int8_t delta_q; \
        xf(1, name.delta_coded, delta_coded); \
        if (delta_coded) \
            xs(4, name.delta_q, delta_q); \
        else \
            delta_q = 0; \
        current->name = delta_q; \
    } while (0)

#define prob(name) do { \
        uint8_t prob_coded; \
        int8_t prob; \
        xf(1, name.prob_coded, prob_coded); \
        if (prob_coded) \
            xf(8, name.prob, prob); \
        else \
            prob = 255; \
        current->name = prob; \
    } while (0)

#define byte_alignment(rw) (bitstream_tell(rw) % 8)

#include "cbs_vp9_syntax.c"

#undef READ
#undef READWRITE
#undef RWContext
#undef xf
#undef xs
#undef f
#undef s
#undef infer
#undef byte_alignment


#define WRITE
#define READWRITE write
#define RWContext PutBitContext

#define xf(width, name, var) do { \
        CHECK(ff_cbs_write_unsigned(ctx, rw, width, #name, \
                                    var, 0, (1 << width) - 1)); \
    } while (0)
#define xs(width, name, var) do { \
        CHECK(cbs_vp9_write_s(ctx, rw, width, #name, var)); \
    } while (0)

#define f(width, name) \
        xf(width, name, current->name)
#define s(width, name) \
        xs(width, name, current->name)

#define infer(name, value) do { \
        if (current->name != (value)) { \
            av_log(ctx->log_ctx, AV_LOG_WARNING, "Warning: " \
                   "%s does not match inferred value: " \
                   "%"PRId64", but should be %"PRId64".\n", \
                   #name, (int64_t)current->name, (int64_t)(value)); \
        } \
    } while (0)

#define byte_alignment(rw) (put_bits_count(rw) % 8)

#include "cbs_vp9_syntax.c"

#undef READ
#undef READWRITE
#undef RWContext
#undef xf
#undef xs
#undef f
#undef s
#undef infer
#undef byte_alignment


static int cbs_vp9_split_fragment(CodedBitstreamContext *ctx,
                                  CodedBitstreamFragment *frag,
                                  int header)
{
    uint8_t superframe_header;
    uint8_t *data;
    int err;

    superframe_header = frag->data[frag->data_size - 1];

    if ((superframe_header & 0xe0) == 0xc0) {
        VP9RawSuperframeIndex sfi;
        BitstreamContext bc;
        size_t index_size, pos;
        int i;

        index_size = 2 + (((superframe_header & 0x18) >> 3) + 1) *
                          ((superframe_header & 0x07) + 1);

        err = bitstream_init(&bc, frag->data + frag->data_size - index_size,
                             8 * index_size);
        if (err < 0)
            return err;

        err = cbs_vp9_read_superframe_index(ctx, &bc, &sfi);
        if (err < 0)
            return err;

        pos = 0;
        for (i = 0; i <= sfi.frames_in_superframe_minus_1; i++) {
            if (pos + sfi.frame_sizes[i] + index_size > frag->data_size) {
                av_log(ctx->log_ctx, AV_LOG_ERROR, "Frame %d too large "
                       "in superframe: %"PRIu32" bytes.\n",
                       i, sfi.frame_sizes[i]);
                return AVERROR_INVALIDDATA;
            }

            data = av_malloc(sfi.frame_sizes[i]);
            if (!data)
                return AVERROR(ENOMEM);
            memcpy(data, frag->data + pos, sfi.frame_sizes[i]);

            err = ff_cbs_insert_unit_data(ctx, frag, -1, 0,
                                          data, sfi.frame_sizes[i]);
            if (err < 0) {
                av_freep(&data);
                return err;
            }

            pos += sfi.frame_sizes[i];
        }
        if (pos + index_size != frag->data_size) {
            av_log(ctx->log_ctx, AV_LOG_WARNING, "Extra padding at "
                   "end of superframe: %zu bytes.\n",
                   frag->data_size - (pos + index_size));
        }

        return 0;

    } else {
        data = av_malloc(frag->data_size);
        if (!data)
            return AVERROR(ENOMEM);
        memcpy(data, frag->data, frag->data_size);

        err = ff_cbs_insert_unit_data(ctx, frag, -1, 0,
                                      data, frag->data_size);
        if (err < 0) {
            av_freep(&data);
            return err;
        }
    }

    return 0;
}

static int cbs_vp9_read_unit(CodedBitstreamContext *ctx,
                             CodedBitstreamUnit *unit)
{
    VP9RawFrame *frame;
    BitstreamContext bc;
    int err;

    err = bitstream_init(&bc, unit->data, 8 * unit->data_size);
    if (err < 0)
        return err;

    frame = av_mallocz(sizeof(*frame));
    if (!frame)
        return AVERROR(ENOMEM);

    err = cbs_vp9_read_frame(ctx, &bc, frame);
    if (err < 0) {
        av_freep(&frame);
        return err;
    }

    unit->content = frame;
    return 0;
}

static int cbs_vp9_write_unit(CodedBitstreamContext *ctx,
                              CodedBitstreamUnit *unit)
{
    PutBitContext pbc;
    uint8_t *data;
    size_t size;
    int err;

    data = av_malloc(1 << 24);
    if (!data)
        return AVERROR(ENOMEM);

    init_put_bits(&pbc, data, 1 << 24);

    err = cbs_vp9_write_frame(ctx, &pbc, unit->content);
    if (err < 0) {
        av_free(data);
        return err;
    }

    // Frame must be byte-aligned.
    av_assert0(put_bits_count(&pbc) % 8 == 0);

    size = put_bits_count(&pbc) / 8;
    flush_put_bits(&pbc);

    av_freep(&unit->data);
    unit->data = av_realloc(data, size);
    if (!unit->data)
        return AVERROR(ENOMEM);
    unit->data_size = size;

    return 0;
}

static int cbs_vp9_assemble_fragment(CodedBitstreamContext *ctx,
                                     CodedBitstreamFragment *frag)
{
    uint8_t *data;
    size_t size;
    int err;

    if (frag->nb_units == 1) {
        // Output is just the content of the single fragment.

        size = frag->units[0].data_size;
        data = av_malloc(size);
        if (!data)
            return AVERROR(ENOMEM);

        memcpy(data, frag->units[0].data, size);
    } else {
        // Build superframe out of frames.

        VP9RawSuperframeIndex sfi;
        PutBitContext pbc;
        size_t max, pos;
        int i, size_len;

        if (frag->nb_units > 8) {
            av_log(ctx->log_ctx, AV_LOG_ERROR, "Too many frames to "
                   "make superframe: %d.\n", frag->nb_units);
            return AVERROR(EINVAL);
        }

        max = 0;
        for (i = 0; i < frag->nb_units; i++)
            if (max < frag->units[i].data_size)
                max = frag->units[i].data_size;

        size_len = av_log2(max) / 8;
        if (size_len > 4) {
            av_log(ctx->log_ctx, AV_LOG_ERROR, "Frame too large: "
                   "%zu bytes.\n", max);
            return AVERROR(EINVAL);
        }

        sfi.superframe_marker            = 6;
        sfi.bytes_per_framesize_minus_1  = size_len - 1;
        sfi.frames_in_superframe_minus_1 = frag->nb_units - 1;

        size = 2;
        for (i = 0; i < frag->nb_units; i++) {
            size += size_len + frag->units[i].data_size;
            sfi.frame_sizes[i] = frag->units[i].data_size;
        }

        data = av_malloc(size);
        if (!data)
            return AVERROR(ENOMEM);

        pos = 0;
        for (i = 0; i < frag->nb_units; i++) {
            av_assert0(size - pos > frag->units[i].data_size);
            memcpy(data + pos, frag->units[i].data,
                   frag->units[i].data_size);
            pos += frag->units[i].data_size;
        }
        av_assert0(size - pos == 2 + frag->nb_units * size_len);

        init_put_bits(&pbc, data + pos, size - pos);

        err = cbs_vp9_write_superframe_index(ctx, &pbc, &sfi);
        if (err < 0) {
            av_log(ctx->log_ctx, AV_LOG_ERROR, "Failed to write "
                   "superframe index.\n");
            av_freep(&data);
            return err;
        }

        av_assert0(put_bits_left(&pbc) == 0);
        flush_put_bits(&pbc);
    }

    av_freep(&frag->data);

    frag->data      = data;
    frag->data_size = size;

    return 0;
}

static void cbs_vp9_free_unit(CodedBitstreamUnit *unit)
{
}

const CodedBitstreamType ff_cbs_type_vp9 = {
    .codec_id          = AV_CODEC_ID_VP9,

    .priv_data_size    = sizeof(CodedBitstreamVP9Context),

    .split_fragment    = &cbs_vp9_split_fragment,
    .read_unit         = &cbs_vp9_read_unit,
    .write_unit        = &cbs_vp9_write_unit,
    .assemble_fragment = &cbs_vp9_assemble_fragment,

    .free_unit         = &cbs_vp9_free_unit,
};
