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

static inline float4 yuv_to_rgb_bt470bg(float4 yuv)
{
    float4 yuv2r = (float4)(1.0,  0.0,   +1.140, 0.0);
    float4 yuv2g = (float4)(1.0, -0.396, -0.581, 0.0);
    float4 yuv2b = (float4)(1.0, +2.029,  0.0,   0.0);
    float4 rgb;
    yuv -= (float4)(0.0, 0.5, 0.5, 0.0);
    rgb.x = dot(yuv, yuv2r);
    rgb.y = dot(yuv, yuv2g);
    rgb.z = dot(yuv, yuv2b);
    return rgb;
}

static inline float4 rgb_to_yuv_bt470bg(float4 rgb)
{
    float4 rgb2y = (float4)(+0.299, +0.587, +0.114, 0.0);
    float4 rgb2u = (float4)(-0.147, -0.289, +0.436, 0.0);
    float4 rgb2v = (float4)(+0.615, -0.515, -0.100, 0.0);
    float4 yuv;
    yuv.x = dot(rgb, rgb2y);
    yuv.y = 0.5f + dot(rgb, rgb2u);
    yuv.z = 0.5f + dot(rgb, rgb2v);
    return yuv;
}

static inline float4 yuv_to_rgb_smpte170m(float4 yuv)
{
    float4 yuv2r = (float4)(1.0,  0.0,   +1.403, 0.0);
    float4 yuv2g = (float4)(1.0, -0.344, -0.714, 0.0);
    float4 yuv2b = (float4)(1.0, +1.773,  0.0,   0.0);
    float4 rgb;
    yuv -= (float4)(0.0, 0.5, 0.5, 0.0);
    rgb.x = dot(yuv, yuv2r);
    rgb.y = dot(yuv, yuv2g);
    rgb.z = dot(yuv, yuv2b);
    return rgb;
}

static inline float4 rgb_to_yuv_smpte170m(float4 rgb)
{
    float4 rgb2y = (float4)(+0.299, +0.587, +0.114, 0.0);
    float4 rgb2u = (float4)(-0.169, -0.331, +0.500, 0.0);
    float4 rgb2v = (float4)(+0.500, -0.419, -0.081, 0.0);
    float4 yuv;
    yuv.x = dot(rgb, rgb2y);
    yuv.y = 0.5f + dot(rgb, rgb2u);
    yuv.z = 0.5f + dot(rgb, rgb2v);
    return yuv;
}

static inline float4 yuv_to_rgb_bt709(float4 yuv)
{
    float4 yuv2r = (float4)(1.0,  0.0,    +1.5701, 0.0);
    float4 yuv2g = (float4)(1.0, -0.1870, -0.4664, 0.0);
    float4 yuv2b = (float4)(1.0, +1.8556,  0.0,    0.0);
    float4 rgb;
    yuv -= (float4)(0.0, 0.5, 0.5, 0.0);
    rgb.x = dot(yuv, yuv2r);
    rgb.y = dot(yuv, yuv2g);
    rgb.z = dot(yuv, yuv2b);
    return rgb;
}

static inline float4 rgb_to_yuv_bt709(float4 rgb)
{
    float4 rgb2y = (float4)(+0.2215, +0.7154, +0.0721, 0.0);
    float4 rgb2u = (float4)(-0.1145, -0.3855, +0.5000, 0.0);
    float4 rgb2v = (float4)(+0.5016, -0.4556, -0.0459, 0.0);
    float4 yuv;
    yuv.x = dot(rgb, rgb2y);
    yuv.y = 0.5f + dot(rgb, rgb2u);
    yuv.z = 0.5f + dot(rgb, rgb2v);
    return yuv;
}

static inline float4 yuv_to_rgb_bt2020(float4 yuv)
{
    float4 yuv2r = (float4)(1.0,  0.0,     +1.4746,  0.0);
    float4 yuv2g = (float4)(1.0, -0.16455, -0.57135, 0.0);
    float4 yuv2b = (float4)(1.0, +1.8814,   0.0,     0.0);
    float4 rgb;
    yuv -= (float4)(0.0, 0.5, 0.5, 0.0);
    rgb.x = dot(yuv, yuv2r);
    rgb.y = dot(yuv, yuv2g);
    rgb.z = dot(yuv, yuv2b);
    return rgb;
}

static inline float4 rgb_to_yuv_bt2020(float4 rgb)
{
    float4 rgb2y = (float4)(+0.2627,  +0.6780,  +0.0593,  0.0);
    float4 rgb2u = (float4)(-0.13963, -0.36037, +0.5,     0.0);
    float4 rgb2v = (float4)(+0.5,     -0.45979, -0.04021, 0.0);
    float4 yuv;
    yuv.x = dot(rgb, rgb2y);
    yuv.y = 0.5f + dot(rgb, rgb2u);
    yuv.z = 0.5f + dot(rgb, rgb2v);
    return yuv;
}
