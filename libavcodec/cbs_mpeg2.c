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
#include "cbs_mpeg2.h"
#include "internal.h"


#define HEADER(name) do { \
        ff_cbs_trace_header(ctx, name); \
    } while (0)

#define CHECK(call) do { \
        err = (call); \
        if (err < 0) \
            return err; \
    } while (0)

#define FUNC_NAME(rw, codec, name) cbs_ ## codec ## _ ## rw ## _ ## name
#define FUNC_MPEG2(rw, name) FUNC_NAME(rw, mpeg2, name)
#define FUNC(name) FUNC_MPEG2(READWRITE, name)


#define READ
#define READWRITE read
#define RWContext BitstreamContext

#define xui(width, name, var) do { \
        uint32_t value = 0; \
        CHECK(ff_cbs_read_unsigned(ctx, rw, width, #name, \
                                   &value, 0, (1 << width) - 1)); \
        var = value; \
    } while (0)

#define ui(width, name) \
        xui(width, name, current->name)

#define marker_bit() do { \
        av_unused int one = 1; \
        CHECK(ff_cbs_read_unsigned(ctx, rw, 1, "marker_bit", &one, 1, 1)); \
    } while (0)

#define nextbits(width, compare, var) (var = bitstream_peek(rw, width), \
                                       var == (compare))

#include "cbs_mpeg2_syntax.c"

#undef READ
#undef READWRITE
#undef RWContext
#undef xui
#undef ui
#undef marker_bit
#undef nextbits


#define WRITE
#define READWRITE write
#define RWContext PutBitContext

#define xui(width, name, var) do { \
        CHECK(ff_cbs_write_unsigned(ctx, rw, width, #name, \
                                    var, 0, (1 << width) - 1)); \
    } while (0)

#define ui(width, name) \
        xui(width, name, current->name)

#define marker_bit() do { \
        CHECK(ff_cbs_write_unsigned(ctx, rw, 1, "marker_bit", 1, 1, 1)); \
    } while (0)

#define nextbits(width, compare, var) (var)

#include "cbs_mpeg2_syntax.c"

#undef READ
#undef READWRITE
#undef RWContext
#undef xui
#undef ui
#undef marker_bit
#undef nextbits


static int cbs_mpeg2_split_fragment(CodedBitstreamContext *ctx,
                                    CodedBitstreamFragment *frag,
                                    int header)
{
    const uint8_t *start, *end;
    uint8_t *unit_data;
    uint32_t start_code = -1, next_start_code = -1;
    size_t unit_size;
    int err, i, unit_type;

    if (frag->nb_units != 0)
        return AVERROR(EINVAL);

    start = avpriv_find_start_code(frag->data, frag->data + frag->data_size,
                                   &start_code);
    for (i = 0;; i++) {
        end = avpriv_find_start_code(start, frag->data + frag->data_size,
                                     &next_start_code);

        unit_type = start_code & 0xff;

        if (end == frag->data + frag->data_size)
            unit_size = end - (start - 1);
        else
            unit_size = (end - 4) - (start - 1);

        unit_data = av_malloc(unit_size);
        if (!unit_data)
            return AVERROR(ENOMEM);
        memcpy(unit_data, start - 1, unit_size);

        err = ff_cbs_insert_unit_data(ctx, frag, i, unit_type,
                                      unit_data, unit_size);
        if (err < 0)
            return err;

        if (end == frag->data + frag->data_size)
            break;

        start_code = next_start_code;
        start = end;
    }

    return 0;
}

static int cbs_mpeg2_read_unit(CodedBitstreamContext *ctx,
                               CodedBitstreamUnit *unit)
{
    BitstreamContext bc;
    int err;

    err = bitstream_init(&bc, unit->data, 8 * unit->data_size);
    if (err < 0)
        return err;

    if (unit->type >= 0x01 && unit->type <= 0xaf) {
        MPEG2RawSlice *slice;
        int pos, len;

        slice = av_mallocz(sizeof(*slice));
        if (!slice)
            return AVERROR(ENOMEM);
        err = cbs_mpeg2_read_slice_header(ctx, &bc, &slice->header);
        if (err < 0) {
            av_free(slice);
            return err;
        }

        pos = bitstream_tell(&bc);
        len = unit->data_size;

        slice->data_size = len - pos / 8;
        slice->data = av_malloc(slice->data_size);
        if (!slice->data) {
            av_free(slice);
            return AVERROR(ENOMEM);
        }

        memcpy(slice->data,
               unit->data + pos / 8, slice->data_size);
        slice->data_bit_start = pos % 8;

        unit->content = slice;

    } else {
        switch (unit->type) {
#define START(start_code, type, func) \
        case start_code: \
            { \
                type *header; \
                header = av_mallocz(sizeof(*header)); \
                if (!header) \
                    return AVERROR(ENOMEM); \
                err = cbs_mpeg2_read_ ## func(ctx, &bc, header); \
                if (err < 0) { \
                    av_free(header); \
                    return err; \
                } \
                unit->content = header; \
            } \
            break;
            START(0x00, MPEG2RawPictureHeader,  picture_header);
            START(0xb2, MPEG2RawUserData,       user_data);
            START(0xb3, MPEG2RawSequenceHeader, sequence_header);
            START(0xb5, MPEG2RawExtensionData,  extension_data);
            START(0xb8, MPEG2RawGroupOfPicturesHeader, group_of_pictures_header);
#undef START
        default:
            av_log(ctx->log_ctx, AV_LOG_ERROR, "Unknown start code %02x.\n",
                   unit->type);
            return AVERROR_INVALIDDATA;
        }
    }

    return 0;
}

static int cbs_mpeg2_write_unit(CodedBitstreamContext *ctx,
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

    if (unit->type >= 0x01 && unit->type <= 0xaf) {
        MPEG2RawSlice *slice = unit->content;
        BitstreamContext bc;
        size_t bits_left;

        err = cbs_mpeg2_write_slice_header(ctx, &pbc, &slice->header);
        if (err < 0)
            goto fail;

        if (slice->data) {
            bitstream_init(&bc, slice->data, slice->data_size * 8);
            bitstream_skip(&bc, slice->data_bit_start);

            while (bitstream_bits_left(&bc) > 15)
                put_bits(&pbc, 16, bitstream_read(&bc, 16));

            bits_left = bitstream_bits_left(&bc);
            put_bits(&pbc, bits_left, bitstream_read(&bc, bits_left));

            // Align with zeroes.
            while (put_bits_count(&pbc) % 8 != 0)
                put_bits(&pbc, 1, 0);
        }
    } else {
        switch (unit->type) {
#define START(start_code, type, func) \
        case start_code: \
            err = cbs_mpeg2_write_ ## func(ctx, &pbc, unit->content); \
            if (err < 0) \
                goto fail; \
            break;
        START(0x00, MPEG2RawPictureHeader,  picture_header);
        START(0xb2, MPEG2RawUserData,       user_data);
        START(0xb3, MPEG2RawSequenceHeader, sequence_header);
        START(0xb5, MPEG2RawExtensionData,  extension_data);
        START(0xb8, MPEG2RawGroupOfPicturesHeader, group_of_pictures_header);
#undef START
        default:
            av_log(ctx->log_ctx, AV_LOG_ERROR, "Write unimplemented for start "
                   "code %02x.\n", unit->type);
            err = AVERROR_PATCHWELCOME;
            goto fail;
        }
    }

    if (put_bits_count(&pbc) % 8)
        unit->data_bit_padding = 8 - put_bits_count(&pbc) % 8;
    else
        unit->data_bit_padding = 0;

    size = (put_bits_count(&pbc) + 7) / 8;
    flush_put_bits(&pbc);

    av_freep(&unit->data);

    unit->data = av_realloc(data, size);
    if (!unit->data) {
        err = AVERROR(ENOMEM);
        goto fail;
    }
    unit->data_size = size;

    return 0;

fail:
    av_free(data);
    return err;
}

static int cbs_mpeg2_assemble_fragment(CodedBitstreamContext *ctx,
                                       CodedBitstreamFragment *frag)
{
    uint8_t *data;
    size_t size, dp, sp;
    int i;

    size = 0;
    for (i = 0; i < frag->nb_units; i++)
        size += 3 + frag->units[i].data_size;

    data = av_malloc(size);
    if (!data)
        return AVERROR(ENOMEM);

    dp = 0;
    for (i = 0; i < frag->nb_units; i++) {
        CodedBitstreamUnit *unit = &frag->units[i];

        data[dp++] = 0;
        data[dp++] = 0;
        data[dp++] = 1;

        for (sp = 0; sp < unit->data_size; sp++)
            data[dp++] = unit->data[sp];
    }

    av_assert0(dp == size);

    frag->data      = data;
    frag->data_size = size;

    return 0;
}

static void cbs_mpeg2_free_unit(CodedBitstreamUnit *unit)
{
}

const CodedBitstreamType ff_cbs_type_mpeg2 = {
    .codec_id          = AV_CODEC_ID_MPEG2VIDEO,

    .priv_data_size    = 1, //sizeof(CodedBitstreamMPEG2Context),

    .split_fragment    = &cbs_mpeg2_split_fragment,
    .read_unit         = &cbs_mpeg2_read_unit,
    .write_unit        = &cbs_mpeg2_write_unit,
    .assemble_fragment = &cbs_mpeg2_assemble_fragment,

    .free_unit         = &cbs_mpeg2_free_unit,
};
