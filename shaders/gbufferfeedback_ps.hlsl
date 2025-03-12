/*
* Copyright (c) 2014-2021, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/

#pragma pack_matrix(row_major)

#include <donut/shaders/scene_material.hlsli>
#include <donut/shaders/material_bindings.hlsli>
#include <donut/shaders/motion_vectors.hlsli>
#include <donut/shaders/forward_vertex.hlsli>
#include <donut/shaders/gbuffer_cb.h>
#include <donut/shaders/vulkan.hlsli>

#include "global_cb.h"

cbuffer c_GBuffer : register(b1 VK_DESCRIPTOR_SET(1))
{
    GBufferFillConstants c_GBuffer;
};

cbuffer c_Global : register(b3 VK_DESCRIPTOR_SET(3))
{
    GlobalConstants c_Global;
};

FeedbackTexture2D<SAMPLER_FEEDBACK_MIN_MIP> t_BaseOrDiffuseFeedback : register(u0);
FeedbackTexture2D<SAMPLER_FEEDBACK_MIN_MIP> t_MetalRoughOrSpecularFeedback : register(u1);
FeedbackTexture2D<SAMPLER_FEEDBACK_MIN_MIP> t_NormalFeedback : register(u2);
FeedbackTexture2D<SAMPLER_FEEDBACK_MIN_MIP> t_EmissiveFeedback : register(u3);
FeedbackTexture2D<SAMPLER_FEEDBACK_MIN_MIP> t_OcclusionFeedback : register(u4);
FeedbackTexture2D<SAMPLER_FEEDBACK_MIN_MIP> t_TransmissionFeedback : register(u5);

Texture2D<float> t_BaseOrDiffuseMinMip: register(t6);
Texture2D<float> t_MetalRoughOrSpecularMinMip : register(t7);
Texture2D<float> t_NormalMinMip : register(t8);
Texture2D<float> t_EmissiveMinMip : register(t9);
Texture2D<float> t_OcclusionMinMip : register(t10);
Texture2D<float> t_TransmissionMinMip : register(t11);

SamplerState s_MaterialSamplerMinMip : register(s1);

// Toggle these defines to use a loop to find the highest resident mip level
// otherwise the "MinMip" texture will be used
#define USE_MIN_MIP_SAMPLER 1
#define USE_MIN_MIP_TEXTURE 1

float4 SampleWithFeedback(float2 texCoord, Texture2D tex, Texture2D<float> texMinMip, FeedbackTexture2D<SAMPLER_FEEDBACK_MIN_MIP> texFeedback, bool enableFeedback, bool isDiffuse = false)
{
    int2 offsetZero = int2(0, 0);
    uint status;
    float4 col = tex.Sample(s_MaterialSampler, texCoord, offsetZero, 0, status); // Opportunistic sample
    if (!CheckAccessFullyMapped(status))
    {
        // Miss path with MinMip clamp
#if USE_MIN_MIP_SAMPLER
        float clamp = texMinMip.Sample(s_MaterialSamplerMinMip, texCoord);
#else // !USE_MIN_MIP_SAMPLER
        float4 clamp4 = texMinMip.Gather(s_MaterialSampler, texCoord);
        float clamp = max(clamp4.x, max(clamp4.y, max(clamp4.z, clamp4.w)));
#endif // !USE_MIN_MIP_SAMPLER

#if USE_MIN_MIP_TEXTURE
        col = tex.Sample(s_MaterialSampler, texCoord, offsetZero, clamp, status);
#else // !USE_MIN_MIP_TEXTURE
        uint level = tex.CalculateLevelOfDetail(s_MaterialSampler, texCoord);
        for (uint i = level + 1; i < 16; ++i)
        {
            col = tex.Sample(s_MaterialSampler, texCoord, offsetZero, i, status);
            if (CheckAccessFullyMapped(status))
                break;
        }
#endif // !USE_MIN_MIP_TEXTURE

        if (isDiffuse)
            col.xyz = lerp(col.xyz, float3(1, 0, 0), c_Global.showUnmappedRegions * 0.99f);
    }

#if WRITEFEEDBACK
#ifndef SPIRV
    if (enableFeedback)
        texFeedback.WriteSamplerFeedback(tex, s_MaterialSampler, texCoord);
#endif // SPIRV
#endif // WRITEFEEDBACK

    return col;
}

MaterialTextureSample SampleMaterialTexturesFeedback(float2 texCoord, bool enableFeedback)
{
    MaterialTextureSample values = DefaultMaterialTextures();

    if (g_Material.flags & MaterialFlags_UseBaseOrDiffuseTexture)
    {
        values.baseOrDiffuse = SampleWithFeedback(texCoord, t_BaseOrDiffuse, t_BaseOrDiffuseMinMip, t_BaseOrDiffuseFeedback, enableFeedback, true);
    }

    if (g_Material.flags & MaterialFlags_UseMetalRoughOrSpecularTexture)
    {
        values.metalRoughOrSpecular = SampleWithFeedback(texCoord, t_MetalRoughOrSpecular, t_MetalRoughOrSpecularMinMip, t_MetalRoughOrSpecularFeedback, enableFeedback);
    }

    if (g_Material.flags & MaterialFlags_UseEmissiveTexture)
    {
        values.emissive = SampleWithFeedback(texCoord, t_Emissive, t_EmissiveMinMip, t_EmissiveFeedback, enableFeedback);
    }

    if (g_Material.flags & MaterialFlags_UseNormalTexture)
    {
        values.normal = SampleWithFeedback(texCoord, t_Normal, t_NormalMinMip, t_NormalFeedback, enableFeedback);
    }

    if (g_Material.flags & MaterialFlags_UseOcclusionTexture)
    {
        values.occlusion = SampleWithFeedback(texCoord, t_Occlusion, t_OcclusionMinMip, t_OcclusionFeedback, enableFeedback);
    }

    if (g_Material.flags & MaterialFlags_UseTransmissionTexture)
    {
        values.transmission = SampleWithFeedback(texCoord, t_Transmission, t_TransmissionMinMip, t_TransmissionFeedback, enableFeedback);
    }

    return values;
}

uint ProceduralJenkinsHash(uint x)
{
    x += x << 10;
    x ^= x >> 6;
    x += x << 3;
    x ^= x >> 11;
    x += x << 15;

    return x;
}

uint ProceduralInitRng(in uint2 pixel, in uint2 resolution, in uint frame)
{
    uint rngState = dot(pixel, uint2(1, resolution.x)) ^ ProceduralJenkinsHash(frame);

    return ProceduralJenkinsHash(rngState);
}

float ProceduralUintToFloat(uint x)
{
    return asfloat(0x3f800000 | (x >> 9)) - 1.f;
}

uint ProceduralXorShift(inout uint rngState)
{
    rngState ^= rngState << 13;
    rngState ^= rngState >> 17;
    rngState ^= rngState << 5;

    return rngState;
}

float ProceduralRand(inout uint rngState)
{
    return ProceduralUintToFloat(ProceduralXorShift(rngState));
}

#if !ALPHA_TESTED
[earlydepthstencil]
#endif
void main(
    in float4 i_position : SV_Position,
	in SceneVertex i_vtx,
#if MOTION_VECTORS
    in float3 i_prevWorldPos : PREV_WORLD_POS,
#endif
    in bool i_isFrontFace : SV_IsFrontFace,
    out float4 o_channel0 : SV_Target0,
    out float4 o_channel1 : SV_Target1,
    out float4 o_channel2 : SV_Target2,
    out float4 o_channel3 : SV_Target3
#if MOTION_VECTORS
    , out float3 o_motion : SV_Target4
#endif
)
{
    bool enableFeedback = true;
    if (c_Global.feedbackThreshold < 1.0)
    {
        uint2 uniformQuadCoord = uint2(i_position.xy) / 2;

        uint rngState = ProceduralInitRng(uniformQuadCoord, uint2(4096, 1), c_Global.frameIndex);
        float random = ProceduralRand(rngState);
        enableFeedback = random < c_Global.feedbackThreshold ? true : false;
    }

    MaterialTextureSample textures = SampleMaterialTexturesFeedback(i_vtx.texCoord, enableFeedback);

    MaterialSample surface = EvaluateSceneMaterial(i_vtx.normal, i_vtx.tangent, g_Material, textures);

#if ALPHA_TESTED
    if (g_Material.domain != MaterialDomain_Opaque)
        clip(surface.opacity - g_Material.alphaCutoff);
#endif

    if (!i_isFrontFace)
        surface.shadingNormal = -surface.shadingNormal;

    o_channel0.xyz = surface.diffuseAlbedo;
    o_channel0.w = surface.opacity;
    o_channel1.xyz = surface.specularF0;
    o_channel1.w = surface.occlusion;
    o_channel2.xyz = surface.shadingNormal;
    o_channel2.w = surface.roughness;
    o_channel3.xyz = surface.emissiveColor;
    o_channel3.w = 0;

#if MOTION_VECTORS
    o_motion = GetMotionVector(i_position.xyz, i_prevWorldPos, c_GBuffer.view, c_GBuffer.viewPrev);
#endif
}
