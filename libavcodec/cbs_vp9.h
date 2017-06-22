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

#ifndef AVCODEC_CBS_VP9_H
#define AVCODEC_CBS_VP9_H

#include <stdint.h>


enum {
    VP9_MAX_SEGMENTS = 8,
    VP9_SEG_LVL_MAX  = 4,
    VP9_MIN_TILE_WIDTH_B64 = 4,
    VP9_MAX_TILE_WIDTH_B64 = 64,
};


typedef struct VP9RawColorConfig {
    uint8_t ten_or_twelve_bit;

    uint8_t color_space;
    uint8_t color_range;
    uint8_t subsampling_x;
    uint8_t subsampling_y;
} VP9RawColorConfig;

typedef struct VP9RawFrameHeader {
    uint8_t frame_marker;
    uint8_t profile_low_bit;
    uint8_t profile_high_bit;
    uint8_t profile_reserved_zero;

    uint8_t show_existing_frame;
    uint8_t frame_to_show_map_idx;

    uint8_t frame_type;
    uint8_t show_frame;
    uint8_t error_resilient_mode;

    // Color config.
    uint8_t ten_or_twelve_bit;
    uint8_t color_space;
    uint8_t color_range;
    uint8_t subsampling_x;
    uint8_t subsampling_y;
    uint8_t color_config_reserved_zero;

    uint8_t refresh_frame_flags;

    uint8_t intra_only;
    uint8_t reset_frame_context;

    uint8_t ref_frame_idx[3];
    uint8_t ref_frame_sign_bias[3];

    uint8_t allow_high_precision_mv;

    uint8_t refresh_frame_context;
    uint8_t frame_parallel_decoding_mode;

    uint8_t frame_context_idx;

    // Frame/render size.
    uint8_t found_ref[3];
    uint16_t frame_width_minus_1;
    uint16_t frame_height_minus_1;
    uint8_t render_and_frame_size_different;
    uint16_t render_width_minus_1;
    uint16_t render_height_minus_1;

    // Interpolation filter.
    uint8_t is_filter_switchable;
    uint8_t raw_interpolation_filter_type;

    // Loop filter params.
    uint8_t loop_filter_level;
    uint8_t loop_filter_sharpness;
    uint8_t loop_filter_delta_enabled;
    uint8_t loop_filter_delta_update;
    uint8_t update_ref_delta[4];
    uint8_t loop_filter_ref_deltas[4];
    uint8_t update_mode_delta[2];
    uint8_t loop_filter_mode_deltas[2];

    // Quantization params.
    uint8_t base_q_idx;
    uint8_t delta_q_y_dc;
    uint8_t delta_q_uv_dc;
    uint8_t delta_q_uv_ac;

    // Segmentation params.
    uint8_t segmentation_enabled;
    uint8_t segmentation_update_map;
    uint8_t segmentation_tree_probs[7];
    uint8_t segmentation_temporal_update;
    uint8_t segmentation_pred_prob[3];
    uint8_t segmentation_update_data;
    uint8_t segmentation_abs_or_delta_update;
    uint8_t feature_enabled[8][4];
    uint8_t feature_value[8][4];
    uint8_t feature_sign[8][4];

    // Tile info.
    uint8_t tile_cols_log2;
    uint8_t tile_rows_log2;

    uint16_t header_size_in_bytes;
} VP9RawFrameHeader;

typedef struct VP9RawFrame {
    VP9RawFrameHeader header;

    uint8_t *data;
    size_t   data_size;
    int      data_bit_start;
} VP9RawFrame;

typedef struct VP9RawSuperframeIndex {
    uint8_t superframe_marker;
    uint8_t bytes_per_framesize_minus_1;
    uint8_t frames_in_superframe_minus_1;
    uint32_t frame_sizes[8];
} VP9RawSuperframeIndex;

typedef struct VP9RawSuperframe {
    VP9RawFrame frames[8];
    VP9RawSuperframeIndex index;
} VP9RawSuperframe;


typedef struct CodedBitstreamVP9Context {
    uint16_t mi_cols;
    uint16_t mi_rows;

    uint16_t sb64_cols;
    uint16_t sb64_rows;
} CodedBitstreamVP9Context;


#endif /* AVCODEC_CBS_VP9_H */
