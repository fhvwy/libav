/*
 * Intel MediaSDK QSV based HEVC encoder
 *
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


#include <stdint.h>
#include <sys/types.h>

#include <mfx/mfxvideo.h>

#include "libavutil/common.h"
#include "libavutil/opt.h"

#include "avcodec.h"
#include "cbs.h"
#include "cbs_h265.h"
#include "get_bits.h"
#include "hevc.h"
#include "hevcdec.h"
#include "internal.h"
#include "qsv.h"
#include "qsv_internal.h"
#include "qsvenc.h"

enum LoadPlugin {
    LOAD_PLUGIN_NONE,
    LOAD_PLUGIN_HEVC_SW,
    LOAD_PLUGIN_HEVC_HW,
};

typedef struct QSVHEVCEncContext {
    AVClass *class;
    QSVEncContext qsv;
    int load_plugin;
} QSVHEVCEncContext;

static int generate_fake_vps(QSVEncContext *q, AVCodecContext *avctx)
{
    CodedBitstreamContext cbc;
    CodedBitstreamFragment ps;
    const H265RawSPS *sps;
    H265RawVPS vps;
    uint8_t *data = NULL;
    size_t data_size;
    int err, sps_pos, i;

    if (!avctx->extradata_size) {
        av_log(avctx, AV_LOG_ERROR,
               "No parameter sets returned by libmfx.\n");
        return AVERROR_UNKNOWN;
    }

    err = ff_cbs_init(&cbc, AV_CODEC_ID_HEVC, avctx);
    if (err < 0)
        return err;

    err = ff_cbs_read(&cbc, &ps, avctx->extradata, avctx->extradata_size);
    if (err < 0) {
        av_log(avctx, AV_LOG_ERROR,
               "Error reading parameter sets returned by libmfx.\n");
        ff_cbs_close(&cbc);
        return err;
    }

    sps = NULL;
    for (sps_pos = 0; sps_pos < ps.nb_units; sps_pos++) {
        if (ps.units[sps_pos].type == HEVC_NAL_SPS) {
            sps = ps.units[sps_pos].content;
            break;
        }
    }
    if (!sps) {
        av_log(avctx, AV_LOG_ERROR, "No SPS returned by libmfx.\n");
        goto fail;
    }

    vps = (H265RawVPS) {
        .nal_unit_header = {
            .nal_unit_type         = HEVC_NAL_VPS,
            .nuh_layer_id          = 0,
            .nuh_temporal_id_plus1 = 1,
        },
        .vps_video_parameter_set_id    = sps->sps_video_parameter_set_id,
        .vps_base_layer_internal_flag  = 1,
        .vps_base_layer_available_flag = 1,
        .vps_max_layers_minus1         = 0,
        .vps_max_sub_layers_minus1     = sps->sps_max_sub_layers_minus1,
        .vps_temporal_id_nesting_flag  =
            sps->sps_max_sub_layers_minus1 == 0 ? 1 : 0,

        .profile_tier_level = sps->profile_tier_level,

        // Sub-layer info copied from SPS below.

        .vps_max_layer_id             = 0,
        .vps_num_layer_sets_minus1    = 0,
        .layer_id_included_flag[0][0] = 1,

        // Timing/HRD info copied from VUI below if present.
    };

    vps.vps_sub_layer_ordering_info_present_flag =
        sps->sps_sub_layer_ordering_info_present_flag;
    for (i = 0; i < HEVC_MAX_SUB_LAYERS; i++) {
        vps.vps_max_dec_pic_buffering_minus1[i] =
            sps->sps_max_dec_pic_buffering_minus1[i];
        vps.vps_max_num_reorder_pics[i] =
            sps->sps_max_num_reorder_pics[i];
        vps.vps_max_latency_increase_plus1[i] =
            sps->sps_max_latency_increase_plus1[i];
    }

    if (sps->vui_parameters_present_flag &&
        sps->vui.vui_timing_info_present_flag) {
        const H265RawVUI *vui = &sps->vui;

        vps.vps_num_units_in_tick = vui->vui_num_units_in_tick;
        vps.vps_time_scale        = vui->vui_time_scale;
        vps.vps_poc_proportional_to_timing_flag =
            vui->vui_poc_proportional_to_timing_flag;
        vps.vps_num_ticks_poc_diff_one_minus1 =
            vui->vui_num_ticks_poc_diff_one_minus1;

        if (vui->vui_hrd_parameters_present_flag) {
            vps.vps_num_hrd_parameters = 1;
            vps.hrd_layer_set_idx[0]   = 0;
            vps.cprms_present_flag[0]  = 1;
            vps.hrd_parameters[0]      = vui->hrd_parameters;
        }
    } else {
        vps.vps_timing_info_present_flag = 0;
    }


    err = ff_cbs_insert_unit_content(&cbc, &ps, sps_pos, HEVC_NAL_VPS, &vps);
    if (err < 0) {
        av_log(avctx, AV_LOG_ERROR,
               "Error inserting new VPS into parameter sets.\n");
        goto fail;
    }

    err = ff_cbs_write_fragment_data(&cbc, &ps);
    if (err < 0) {
        av_log(avctx, AV_LOG_ERROR, "Error writing new parameter sets.\n");
        goto fail;
    }

    data_size = ps.data_size + AV_INPUT_BUFFER_PADDING_SIZE;
    data      = av_mallocz(data_size);
    if (!data) {
        err = AVERROR(ENOMEM);
        goto fail;
    }
    memcpy(data, ps.data, data_size);

    av_freep(&avctx->extradata);
    avctx->extradata      = data;
    avctx->extradata_size = data_size;

    err = 0;
    data = NULL;

fail:
    ff_cbs_fragment_uninit(&cbc, &ps);
    ff_cbs_close(&cbc);
    av_freep(&data);
    return err;
}

static av_cold int qsv_enc_init(AVCodecContext *avctx)
{
    QSVHEVCEncContext *q = avctx->priv_data;
    int ret;

    if (q->load_plugin != LOAD_PLUGIN_NONE) {
        static const char * const uid_hevcenc_sw = "2fca99749fdb49aeb121a5b63ef568f7";
        static const char * const uid_hevcenc_hw = "6fadc791a0c2eb479ab6dcd5ea9da347";

        if (q->qsv.load_plugins[0]) {
            av_log(avctx, AV_LOG_WARNING,
                   "load_plugins is not empty, but load_plugin is not set to 'none'."
                   "The load_plugin value will be ignored.\n");
        } else {
            av_freep(&q->qsv.load_plugins);

            if (q->load_plugin == LOAD_PLUGIN_HEVC_SW)
                q->qsv.load_plugins = av_strdup(uid_hevcenc_sw);
            else
                q->qsv.load_plugins = av_strdup(uid_hevcenc_hw);

            if (!q->qsv.load_plugins)
                return AVERROR(ENOMEM);
        }
    }

    ret = ff_qsv_enc_init(avctx, &q->qsv);
    if (ret < 0)
        return ret;

    ret = generate_fake_vps(&q->qsv, avctx);
    if (ret < 0) {
        ff_qsv_enc_close(avctx, &q->qsv);
        return ret;
    }

    return 0;
}

static int qsv_enc_frame(AVCodecContext *avctx, AVPacket *pkt,
                         const AVFrame *frame, int *got_packet)
{
    QSVHEVCEncContext *q = avctx->priv_data;

    return ff_qsv_encode(avctx, &q->qsv, pkt, frame, got_packet);
}

static av_cold int qsv_enc_close(AVCodecContext *avctx)
{
    QSVHEVCEncContext *q = avctx->priv_data;

    return ff_qsv_enc_close(avctx, &q->qsv);
}

#define OFFSET(x) offsetof(QSVHEVCEncContext, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    QSV_COMMON_OPTS

    { "load_plugin", "A user plugin to load in an internal session", OFFSET(load_plugin), AV_OPT_TYPE_INT, { .i64 = LOAD_PLUGIN_HEVC_SW }, LOAD_PLUGIN_NONE, LOAD_PLUGIN_HEVC_HW, VE, "load_plugin" },
    { "none",     NULL, 0, AV_OPT_TYPE_CONST, { .i64 = LOAD_PLUGIN_NONE },    0, 0, VE, "load_plugin" },
    { "hevc_sw",  NULL, 0, AV_OPT_TYPE_CONST, { .i64 = LOAD_PLUGIN_HEVC_SW }, 0, 0, VE, "load_plugin" },
    { "hevc_hw",  NULL, 0, AV_OPT_TYPE_CONST, { .i64 = LOAD_PLUGIN_HEVC_HW }, 0, 0, VE, "load_plugin" },

    { "load_plugins", "A :-separate list of hexadecimal plugin UIDs to load in an internal session",
        OFFSET(qsv.load_plugins), AV_OPT_TYPE_STRING, { .str = "" }, 0, 0, VE },

    { "profile", NULL, OFFSET(qsv.profile), AV_OPT_TYPE_INT, { .i64 = MFX_PROFILE_UNKNOWN }, 0, INT_MAX, VE, "profile" },
    { "unknown", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MFX_PROFILE_UNKNOWN      }, INT_MIN, INT_MAX,     VE, "profile" },
    { "main",    NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MFX_PROFILE_HEVC_MAIN    }, INT_MIN, INT_MAX,     VE, "profile" },
    { "main10",  NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MFX_PROFILE_HEVC_MAIN10  }, INT_MIN, INT_MAX,     VE, "profile" },
    { "mainsp",  NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MFX_PROFILE_HEVC_MAINSP  }, INT_MIN, INT_MAX,     VE, "profile" },

    { NULL },
};

static const AVClass class = {
    .class_name = "hevc_qsv encoder",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static const AVCodecDefault qsv_enc_defaults[] = {
    { "b",         "1M"    },
    { "refs",      "0"     },
    // same as the x264 default
    { "g",         "248"   },
    { "bf",        "8"     },

    { "flags",     "+cgop" },
#if FF_API_PRIVATE_OPT
    { "b_strategy", "-1"   },
#endif
    { NULL },
};

AVCodec ff_hevc_qsv_encoder = {
    .name           = "hevc_qsv",
    .long_name      = NULL_IF_CONFIG_SMALL("HEVC (Intel Quick Sync Video acceleration)"),
    .priv_data_size = sizeof(QSVHEVCEncContext),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_HEVC,
    .init           = qsv_enc_init,
    .encode2        = qsv_enc_frame,
    .close          = qsv_enc_close,
    .capabilities   = AV_CODEC_CAP_DELAY,
    .pix_fmts       = (const enum AVPixelFormat[]){ AV_PIX_FMT_NV12,
                                                    AV_PIX_FMT_P010,
                                                    AV_PIX_FMT_QSV,
                                                    AV_PIX_FMT_NONE },
    .priv_class     = &class,
    .defaults       = qsv_enc_defaults,
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,
};
