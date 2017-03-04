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

__kernel void overlay_nv12_rgba(__write_only image2d_t dst_y,
                                __write_only image2d_t dst_uv,
                                __read_only  image2d_t src_y,
                                __read_only  image2d_t src_uv,
                                __read_only  image2d_t overlay_rgba,
                                int x_position,
                                int y_position)
{
    const sampler_t sampler = (CLK_NORMALIZED_COORDS_FALSE |
                               CLK_FILTER_NEAREST);
    int i;

    int2 overlay_size = get_image_dim(overlay_rgba);

    int2 loc_y[4];
    int2 loc_uv = (int2)(get_global_id(0), get_global_id(1));
    int2 loc_overlay = (int2)(x_position, y_position);
    float4 in_y[4];
    float4 in_uv;
    for (i = 0; i < 4; i++) {
        loc_y[i] = 2 * loc_uv + (int2)(i & 1, !!(i & 2));
        in_y[i] = read_imagef(src_y, sampler, loc_y[i]);
    }
    in_uv = read_imagef(src_uv, sampler, loc_uv);

    if (loc_y[0].x <  x_position ||
        loc_y[0].y <  y_position ||
        loc_y[3].x >= overlay_size.x + x_position ||
        loc_y[3].y >= overlay_size.y + y_position) {
        for (i = 0; i < 4; i++)
            write_imagef(dst_y, loc_y[i], in_y[i]);
        write_imagef(dst_uv, loc_uv, in_uv);
        return;
    }

    float4 in_yuv[4];
    float4 uval_rgb[4], oval_rgb[4];
    float4 out_rgb[4], out_yuv[4];
    float4 out_uv = 0.0f;
    for (i = 0; i < 4; i++) {
        in_yuv[i].x = in_y[i].x;
        in_yuv[i].yz = in_uv.xy;

        uval_rgb[i] = yuv_to_rgb_input(in_yuv[i]);
        oval_rgb[i] = read_imagef(overlay_rgba, sampler, loc_y[i] - loc_overlay);

        out_rgb[i] = uval_rgb[i] * (1.0f - oval_rgb[i].w) +
                     oval_rgb[i] * oval_rgb[i].w;
        out_yuv[i] = rgb_to_yuv_output(out_rgb[i]);

        write_imagef(dst_y, loc_y[i], out_yuv[i].x);
        out_uv.xy += out_yuv[i].yz;
    }
    write_imagef(dst_uv, loc_uv, 0.25f * out_uv);
}
