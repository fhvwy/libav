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

#ifndef AVCODEC_CBS_INTERNAL_H
#define AVCODEC_CBS_INTERNAL_H

#include "avcodec.h"
#include "bitstream.h"
#include "cbs.h"
#include "put_bits.h"


typedef struct CodedBitstreamType {
    enum AVCodecID codec_id;

    size_t priv_data_size;

    // Split frag->data into coded bitstream units, creating the
    // frag->units array.  Fill data but not content on each unit.
    int (*split_fragment)(CodedBitstreamContext *ctx,
                          CodedBitstreamFragment *frag,
                          int header);

    // Read the unit->data bitstream and decompose it, creating
    // unit->content.
    int (*read_unit)(CodedBitstreamContext *ctx,
                     CodedBitstreamUnit *unit);

    // Write the unit->data bitstream from unit->content.
    int (*write_unit)(CodedBitstreamContext *ctx,
                      CodedBitstreamUnit *unit);

    // Read the data from all of frag->units and assemble it into
    // a bitstream for the whole fragment.
    int (*assemble_fragment)(CodedBitstreamContext *ctx,
                             CodedBitstreamFragment *frag);

    // Free the content and data of a single unit.
    void (*free_unit)(CodedBitstreamUnit *unit);

    // Free the codec internal state.
    void (*close)(CodedBitstreamContext *ctx);
} CodedBitstreamType;


// Helper functions for trace output.

void ff_cbs_trace_header(CodedBitstreamContext *ctx,
                         const char *name);

void ff_cbs_trace_syntax_element(CodedBitstreamContext *ctx,
                                 int position, const char *name,
                                 const char *bitstring, int64_t value);


// Helper functions for read/write of common bitstream elements, including
// generation of trace output.

int ff_cbs_read_unsigned(CodedBitstreamContext *ctx, BitstreamContext *bc,
                         int width, const char *name, uint32_t *write_to,
                         uint32_t range_min, uint32_t range_max);

int ff_cbs_read_ue_golomb(CodedBitstreamContext *ctx, BitstreamContext *bc,
                          const char *name, uint32_t *write_to,
                          uint32_t range_min, uint32_t range_max);

int ff_cbs_read_se_golomb(CodedBitstreamContext *ctx, BitstreamContext *bc,
                          const char *name, int32_t *write_to,
                          int32_t range_min, int32_t range_max);

int ff_cbs_write_unsigned(CodedBitstreamContext *ctx, PutBitContext *pbc,
                          int width, const char *name, uint32_t value,
                          uint32_t range_min, uint32_t range_max);

int ff_cbs_write_ue_golomb(CodedBitstreamContext *ctx, PutBitContext *pbc,
                           const char *name, uint32_t value,
                           uint32_t range_min, uint32_t range_max);

int ff_cbs_write_se_golomb(CodedBitstreamContext *ctx, PutBitContext *pbc,
                           const char *name, int32_t value,
                           int32_t range_min, int32_t range_max);


extern const CodedBitstreamType ff_cbs_type_h264;
extern const CodedBitstreamType ff_cbs_type_h265;


#endif /* AVCODEC_CBS_INTERNAL_H */
