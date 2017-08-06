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

#include <string.h>

#include "libavutil/avassert.h"
#include "libavutil/common.h"

#include "cbs.h"
#include "cbs_internal.h"
#include "golomb.h"


static const CodedBitstreamType *cbs_type_table[] = {
};

int ff_cbs_init(CodedBitstreamContext *ctx,
                enum AVCodecID codec_id, void *log_ctx)
{
    const CodedBitstreamType *type;
    int i;

    type = NULL;
    for (i = 0; i < FF_ARRAY_ELEMS(cbs_type_table); i++) {
        if (cbs_type_table[i]->codec_id == codec_id) {
            type = cbs_type_table[i];
            break;
        }
    }
    if (!type)
        return AVERROR(EINVAL);

    ctx->log_ctx = log_ctx;

    ctx->codec = type;

    ctx->priv_data = av_mallocz(ctx->codec->priv_data_size);
    if (!ctx->priv_data)
        return AVERROR(ENOMEM);

    ctx->decompose_unit_types = NULL;

    ctx->trace_enable = 0;
    ctx->trace_level  = AV_LOG_TRACE;

    return 0;
}

void ff_cbs_close(CodedBitstreamContext *ctx)
{
    if (ctx->codec && ctx->codec->close)
        ctx->codec->close(ctx);

    av_freep(&ctx->priv_data);
}

static void cbs_unit_uninit(CodedBitstreamContext *ctx,
                            CodedBitstreamUnit *unit)
{
    if (ctx->codec->free_unit && unit->content && !unit->content_external)
        ctx->codec->free_unit(unit);

    av_freep(&unit->data);
    unit->data_size = 0;
    unit->data_bit_padding = 0;
}

void ff_cbs_fragment_uninit(CodedBitstreamContext *ctx,
                            CodedBitstreamFragment *frag)
{
    int i;

    for (i = 0; i < frag->nb_units; i++)
        cbs_unit_uninit(ctx, &frag->units[i]);
    av_freep(&frag->units);
    frag->nb_units = 0;

    av_freep(&frag->data);
    frag->data_size = 0;
    frag->data_bit_padding = 0;
}

static int cbs_read_fragment_content(CodedBitstreamContext *ctx,
                                     CodedBitstreamFragment *frag)
{
    int err, i, j;

    for (i = 0; i < frag->nb_units; i++) {
        if (ctx->decompose_unit_types) {
            for (j = 0; j < ctx->nb_decompose_unit_types; j++) {
                if (ctx->decompose_unit_types[j] == frag->units[i].type)
                    break;
            }
            if (j >= ctx->nb_decompose_unit_types)
                continue;
        }

        err = ctx->codec->read_unit(ctx, &frag->units[i]);
        if (err == AVERROR(ENOSYS)) {
            av_log(ctx->log_ctx, AV_LOG_WARNING,
                   "Decomposition unimplemented for unit %d "
                   "(type %d).\n", i, frag->units[i].type);
        } else if (err < 0) {
            av_log(ctx->log_ctx, AV_LOG_ERROR, "Failed to read unit %d "
                   "(type %d).\n", i, frag->units[i].type);
            return err;
        }
    }

    return 0;
}

int ff_cbs_read_extradata(CodedBitstreamContext *ctx,
                          CodedBitstreamFragment *frag,
                          const AVCodecParameters *par)
{
    int err;

    memset(frag, 0, sizeof(*frag));

    frag->data      = par->extradata;
    frag->data_size = par->extradata_size;

    err = ctx->codec->split_fragment(ctx, frag, 1);
    if (err < 0)
        return err;

    frag->data      = NULL;
    frag->data_size = 0;

    return cbs_read_fragment_content(ctx, frag);
}

int ff_cbs_read_packet(CodedBitstreamContext *ctx,
                       CodedBitstreamFragment *frag,
                       const AVPacket *pkt)
{
    int err;

    memset(frag, 0, sizeof(*frag));

    frag->data      = pkt->data;
    frag->data_size = pkt->size;

    err = ctx->codec->split_fragment(ctx, frag, 0);
    if (err < 0)
        return err;

    frag->data      = NULL;
    frag->data_size = 0;

    return cbs_read_fragment_content(ctx, frag);
}

int ff_cbs_read(CodedBitstreamContext *ctx,
                CodedBitstreamFragment *frag,
                const uint8_t *data, size_t size)
{
    int err;

    memset(frag, 0, sizeof(*frag));

    // (We won't write to this during split.)
    frag->data      = (uint8_t*)data;
    frag->data_size = size;

    err = ctx->codec->split_fragment(ctx, frag, 0);
    if (err < 0)
        return err;

    frag->data      = NULL;
    frag->data_size = 0;

    return cbs_read_fragment_content(ctx, frag);
}


int ff_cbs_write_fragment_data(CodedBitstreamContext *ctx,
                               CodedBitstreamFragment *frag)
{
    int err, i;

    for (i = 0; i < frag->nb_units; i++) {
        if (!frag->units[i].content)
            continue;

        err = ctx->codec->write_unit(ctx, &frag->units[i]);
        if (err < 0) {
            av_log(ctx->log_ctx, AV_LOG_ERROR, "Failed to write unit %d "
                   "(type %d).\n", i, frag->units[i].type);
            return err;
        }
    }

    err = ctx->codec->assemble_fragment(ctx, frag);
    if (err < 0) {
        av_log(ctx->log_ctx, AV_LOG_ERROR, "Failed to assemble fragment.\n");
        return err;
    }

    return 0;
}

int ff_cbs_write_extradata(CodedBitstreamContext *ctx,
                           AVCodecParameters *par,
                           CodedBitstreamFragment *frag)
{
    int err;

    err = ff_cbs_write_fragment_data(ctx, frag);
    if (err < 0)
        return err;

    av_freep(&par->extradata);

    par->extradata = av_malloc(frag->data_size +
                               AV_INPUT_BUFFER_PADDING_SIZE);
    if (!par->extradata)
        return AVERROR(ENOMEM);

    memcpy(par->extradata, frag->data, frag->data_size);
    memset(par->extradata + frag->data_size, 0,
           AV_INPUT_BUFFER_PADDING_SIZE);
    par->extradata_size = frag->data_size;

    return 0;
}

int ff_cbs_write_packet(CodedBitstreamContext *ctx,
                       AVPacket *pkt,
                       CodedBitstreamFragment *frag)
{
    int err;

    err = ff_cbs_write_fragment_data(ctx, frag);
    if (err < 0)
        return err;

    av_new_packet(pkt, frag->data_size);
    if (err < 0)
        return err;

    memcpy(pkt->data, frag->data, frag->data_size);
    pkt->size = frag->data_size;

    return 0;
}


void ff_cbs_trace_header(CodedBitstreamContext *ctx,
                         const char *name)
{
    if (!ctx->trace_enable)
        return;

    av_log(ctx->log_ctx, ctx->trace_level, "%s\n", name);
}

void ff_cbs_trace_syntax_element(CodedBitstreamContext *ctx, int position,
                                 const char *name, const char *bits,
                                 int64_t value)
{
    size_t name_len, bits_len;
    int pad;

    if (!ctx->trace_enable)
        return;

    av_assert0(value >= INT_MIN && value <= UINT32_MAX);

    name_len = strlen(name);
    bits_len = strlen(bits);

    if (name_len + bits_len > 60)
        pad = bits_len + 2;
    else
        pad = 61 - name_len;

    av_log(ctx->log_ctx, ctx->trace_level, "%-10d  %s%*s = %"PRId64"\n",
           position, name, pad, bits, value);
}

int ff_cbs_read_unsigned(CodedBitstreamContext *ctx, BitstreamContext *bc,
                         int width, const char *name, uint32_t *write_to,
                         uint32_t range_min, uint32_t range_max)
{
    uint32_t value;
    int position;

    av_assert0(width <= 32);

    if (ctx->trace_enable)
        position = bitstream_tell(bc);

    value = bitstream_read(bc, width);

    if (ctx->trace_enable) {
        char bits[33];
        int i;
        for (i = 0; i < width; i++)
            bits[i] = value >> (width - i - 1) & 1 ? '1' : '0';
        bits[i] = 0;

        ff_cbs_trace_syntax_element(ctx, position, name, bits, value);
    }

    if (value < range_min || value > range_max) {
        av_log(ctx->log_ctx, AV_LOG_ERROR, "%s out of range: "
               "%"PRIu32", but must be in [%"PRIu32",%"PRIu32"].\n",
               name, value, range_min, range_max);
        return AVERROR_INVALIDDATA;
    }

    *write_to = value;
    return 0;
}

int ff_cbs_read_ue_golomb(CodedBitstreamContext *ctx, BitstreamContext *bc,
                          const char *name, uint32_t *write_to,
                          uint32_t range_min, uint32_t range_max)
{
    uint32_t value;
    int position;

    if (ctx->trace_enable) {
        char bits[65];
        unsigned int k;
        int i, j;

        position = bitstream_tell(bc);

        for (i = 0; i < 32; i++) {
            k = bitstream_read_bit(bc);
            bits[i] = k ? '1' : '0';
            if (k)
                break;
        }
        if (i >= 32) {
            av_log(ctx->log_ctx, AV_LOG_ERROR, "Invalid ue-golomb "
                   "code found while reading %s: "
                   "more than 31 zeroes.\n", name);
            return AVERROR_INVALIDDATA;
        }
        value = 1;
        for (j = 0; j < i; j++) {
            k = bitstream_read_bit(bc);
            bits[i + j + 1] = k ? '1' : '0';
            value = value << 1 | k;
        }
        bits[i + j + 1] = 0;
        --value;

        ff_cbs_trace_syntax_element(ctx, position, name, bits, value);
    } else {
        value = get_ue_golomb_long(bc);
    }

    if (value < range_min || value > range_max) {
        av_log(ctx->log_ctx, AV_LOG_ERROR, "%s out of range: "
               "%"PRIu32", but must be in [%"PRIu32",%"PRIu32"].\n",
               name, value, range_min, range_max);
        return AVERROR_INVALIDDATA;
    }

    *write_to = value;
    return 0;
}

int ff_cbs_read_se_golomb(CodedBitstreamContext *ctx, BitstreamContext *bc,
                          const char *name, int32_t *write_to,
                          int32_t range_min, int32_t range_max)
{
    int32_t value;
    int position;

    if (ctx->trace_enable) {
        char bits[65];
        uint32_t v;
        unsigned int k;
        int i, j;

        position = bitstream_tell(bc);

        for (i = 0; i < 32; i++) {
            k = bitstream_read_bit(bc);
            bits[i] = k ? '1' : '0';
            if (k)
                break;
        }
        if (i >= 32) {
            av_log(ctx->log_ctx, AV_LOG_ERROR, "Invalid se-golomb "
                   "code found while reading %s: "
                   "more than 31 zeroes.\n", name);
            return AVERROR_INVALIDDATA;
        }
        v = 1;
        for (j = 0; j < i; j++) {
            k = bitstream_read_bit(bc);
            bits[i + j + 1] = k ? '1' : '0';
            v = v << 1 | k;
        }
        bits[i + j + 1] = 0;
        if (v & 1)
            value = -(int32_t)(v / 2);
        else
            value = v / 2;

        ff_cbs_trace_syntax_element(ctx, position, name, bits, value);
    } else {
        value = get_se_golomb_long(bc);
    }

    if (value < range_min || value > range_max) {
        av_log(ctx->log_ctx, AV_LOG_ERROR, "%s out of range: "
               "%"PRId32", but must be in [%"PRId32",%"PRId32"].\n",
               name, value, range_min, range_max);
        return AVERROR_INVALIDDATA;
    }

    *write_to = value;
    return 0;
}

int ff_cbs_write_unsigned(CodedBitstreamContext *ctx, PutBitContext *pbc,
                          int width, const char *name, uint32_t value,
                          uint32_t range_min, uint32_t range_max)
{
    av_assert0(width <= 32);

    if (value < range_min || value > range_max) {
        av_log(ctx->log_ctx, AV_LOG_ERROR, "%s out of range: "
               "%"PRIu32", but must be in [%"PRIu32",%"PRIu32"].\n",
               name, value, range_min, range_max);
        return AVERROR_INVALIDDATA;
    }

    if (put_bits_left(pbc) < width)
        return AVERROR(ENOSPC);

    if (ctx->trace_enable) {
        char bits[33];
        int i;
        for (i = 0; i < width; i++)
            bits[i] = value >> (width - i - 1) & 1 ? '1' : '0';
        bits[i] = 0;

        ff_cbs_trace_syntax_element(ctx, put_bits_count(pbc), name, bits, value);
    }

    if (width < 32)
        put_bits(pbc, width, value);
    else
        put_bits32(pbc, value);

    return 0;
}

int ff_cbs_write_ue_golomb(CodedBitstreamContext *ctx, PutBitContext *pbc,
                           const char *name, uint32_t value,
                           uint32_t range_min, uint32_t range_max)
{
    int len;

    if (value < range_min || value > range_max) {
        av_log(ctx->log_ctx, AV_LOG_ERROR, "%s out of range: "
               "%"PRIu32", but must be in [%"PRIu32",%"PRIu32"].\n",
               name, value, range_min, range_max);
        return AVERROR_INVALIDDATA;
    }
    av_assert0(value != UINT32_MAX);

    len = av_log2(value + 1);
    if (put_bits_left(pbc) < 2 * len + 1)
        return AVERROR(ENOSPC);

    if (ctx->trace_enable) {
        char bits[65];
        int i;

        for (i = 0; i < len; i++)
            bits[i] = '0';
        bits[len] = '1';
        for (i = 0; i < len; i++)
            bits[len + i + 1] = (value + 1) >> (len - i - 1) & 1 ? '1' : '0';
        bits[len + len + 1] = 0;

        ff_cbs_trace_syntax_element(ctx, put_bits_count(pbc), name, bits, value);
    }

    put_bits(pbc, len, 0);
    if (len + 1 < 32)
        put_bits(pbc, len + 1, value + 1);
    else
        put_bits32(pbc, value + 1);

    return 0;
}

int ff_cbs_write_se_golomb(CodedBitstreamContext *ctx, PutBitContext *pbc,
                           const char *name, int32_t value,
                           int32_t range_min, int32_t range_max)
{
    int len;
    uint32_t uvalue;

    if (value < range_min || value > range_max) {
        av_log(ctx->log_ctx, AV_LOG_ERROR, "%s out of range: "
               "%"PRId32", but must be in [%"PRId32",%"PRId32"].\n",
               name, value, range_min, range_max);
        return AVERROR_INVALIDDATA;
    }
    av_assert0(value != INT32_MIN);

    if (value == 0)
        uvalue = 0;
    else if (value > 0)
        uvalue = 2 * (uint32_t)value - 1;
    else
        uvalue = 2 * (uint32_t)-value;

    len = av_log2(uvalue + 1);
    if (put_bits_left(pbc) < 2 * len + 1)
        return AVERROR(ENOSPC);

    if (ctx->trace_enable) {
        char bits[65];
        int i;

        for (i = 0; i < len; i++)
            bits[i] = '0';
        bits[len] = '1';
        for (i = 0; i < len; i++)
            bits[len + i + 1] = (uvalue + 1) >> (len - i - 1) & 1 ? '1' : '0';
        bits[len + len + 1] = 0;

        ff_cbs_trace_syntax_element(ctx, put_bits_count(pbc), name, bits, value);
    }

    put_bits(pbc, len, 0);
    if (len + 1 < 32)
        put_bits(pbc, len + 1, uvalue + 1);
    else
        put_bits32(pbc, uvalue + 1);

    return 0;
}


static int cbs_insert_unit(CodedBitstreamContext *ctx,
                           CodedBitstreamFragment *frag,
                           int position)
{
    CodedBitstreamUnit *units;

    units = av_malloc_array(frag->nb_units + 1, sizeof(*units));
    if (!units)
        return AVERROR(ENOMEM);

    if (position > 0)
        memcpy(units, frag->units, position * sizeof(*units));
    if (position < frag->nb_units)
        memcpy(units + position + 1, frag->units + position,
               (frag->nb_units - position) * sizeof(*units));

    memset(units + position, 0, sizeof(*units));

    av_freep(&frag->units);
    frag->units = units;
    ++frag->nb_units;

    return 0;
}

int ff_cbs_insert_unit_content(CodedBitstreamContext *ctx,
                               CodedBitstreamFragment *frag,
                               int position, uint32_t type,
                               void *content)
{
    int err;

    if (position == -1)
        position = frag->nb_units;
    av_assert0(position >= 0 && position <= frag->nb_units);

    err = cbs_insert_unit(ctx, frag, position);
    if (err < 0)
        return err;

    frag->units[position].type    = type;
    frag->units[position].content = content;
    frag->units[position].content_external = 1;

    return 0;
}

int ff_cbs_insert_unit_data(CodedBitstreamContext *ctx,
                            CodedBitstreamFragment *frag,
                            int position, uint32_t type,
                            uint8_t *data, size_t data_size)
{
    int err;

    if (position == -1)
        position = frag->nb_units;
    av_assert0(position >= 0 && position <= frag->nb_units);

    err = cbs_insert_unit(ctx, frag, position);
    if (err < 0)
        return err;

    frag->units[position].type      = type;
    frag->units[position].data      = data;
    frag->units[position].data_size = data_size;

    return 0;
}

int ff_cbs_delete_unit(CodedBitstreamContext *ctx,
                       CodedBitstreamFragment *frag,
                       int position)
{
    if (position < 0 || position >= frag->nb_units)
        return AVERROR(EINVAL);

    cbs_unit_uninit(ctx, &frag->units[position]);

    --frag->nb_units;

    if (frag->nb_units == 0) {
        av_freep(&frag->units);

    } else {
        memmove(frag->units + position,
                frag->units + position + 1,
                (frag->nb_units - position) * sizeof(*frag->units));

        // Don't bother reallocating the unit array.
    }

    return 0;
}
