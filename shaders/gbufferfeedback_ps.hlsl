/*
 * SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: LicenseRef-NvidiaProprietary
 *
 * NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
 * property and proprietary rights in and to this material, related
 * documentation and any modifications thereto. Any use, reproduction,
 * disclosure or distribution of this material and related documentation
 * without an express license agreement from NVIDIA CORPORATION or
 * its affiliates is strictly prohibited.
 */

#pragma pack_matrix(row_major)

#include <donut/shaders/gbuffer_cb.h>
#include "global_cb.h"
#include "feedback_cb.h"

// Declare the constants that drive material bindings in 'material_bindings.hlsli'
// to match the bindings explicitly declared in 'gbuffer_cb.h'.

#define MATERIAL_REGISTER_SPACE     GBUFFER_SPACE_MATERIAL
#define MATERIAL_CB_SLOT            GBUFFER_BINDING_MATERIAL_CONSTANTS
#define MATERIAL_DIFFUSE_SLOT       GBUFFER_BINDING_MATERIAL_DIFFUSE_TEXTURE
#define MATERIAL_SPECULAR_SLOT      GBUFFER_BINDING_MATERIAL_SPECULAR_TEXTURE
#define MATERIAL_NORMALS_SLOT       GBUFFER_BINDING_MATERIAL_NORMAL_TEXTURE
#define MATERIAL_EMISSIVE_SLOT      GBUFFER_BINDING_MATERIAL_EMISSIVE_TEXTURE
#define MATERIAL_OCCLUSION_SLOT     GBUFFER_BINDING_MATERIAL_OCCLUSION_TEXTURE
#define MATERIAL_TRANSMISSION_SLOT  GBUFFER_BINDING_MATERIAL_TRANSMISSION_TEXTURE

#define MATERIAL_SAMPLER_REGISTER_SPACE GBUFFER_SPACE_VIEW
#define MATERIAL_SAMPLER_SLOT           GBUFFER_BINDING_MATERIAL_SAMPLER

#include <donut/shaders/scene_material.hlsli>
#include <donut/shaders/material_bindings.hlsli>
#include <donut/shaders/motion_vectors.hlsli>
#include <donut/shaders/forward_vertex.hlsli>
#include <donut/shaders/binding_helpers.hlsli>

DECLARE_CBUFFER(GBufferFillConstants, c_GBuffer, GBUFFER_BINDING_VIEW_CONSTANTS, GBUFFER_SPACE_VIEW);
DECLARE_CBUFFER(GlobalConstants, c_Global, GBUFFER_BINDING_GLOBAL_CONSTANTS, GBUFFER_SPACE_VIEW);
DECLARE_CBUFFER(FeedbackConstants, c_Feedback, GBUFFER_BINDING_FEEDBACK_CONSTANTS, GBUFFER_SPACE_MATERIAL);

FeedbackTexture2D<SAMPLER_FEEDBACK_MIN_MIP> t_BaseOrDiffuseFeedback : REGISTER_UAV(GBUFFER_BINDING_MATERIAL_DIFFUSE_FEEDBACKTEXTURE, GBUFFER_SPACE_MATERIAL);
FeedbackTexture2D<SAMPLER_FEEDBACK_MIN_MIP> t_MetalRoughOrSpecularFeedback : REGISTER_UAV(GBUFFER_BINDING_MATERIAL_SPECULAR_FEEDBACKTEXTURE, GBUFFER_SPACE_MATERIAL);
FeedbackTexture2D<SAMPLER_FEEDBACK_MIN_MIP> t_NormalFeedback : REGISTER_UAV(GBUFFER_BINDING_MATERIAL_NORMAL_FEEDBACKTEXTURE, GBUFFER_SPACE_MATERIAL);
FeedbackTexture2D<SAMPLER_FEEDBACK_MIN_MIP> t_EmissiveFeedback : REGISTER_UAV(GBUFFER_BINDING_MATERIAL_EMISSIVE_FEEDBACKTEXTURE, GBUFFER_SPACE_MATERIAL);
FeedbackTexture2D<SAMPLER_FEEDBACK_MIN_MIP> t_OcclusionFeedback : REGISTER_UAV(GBUFFER_BINDING_MATERIAL_OCCLUSION_FEEDBACKTEXTURE, GBUFFER_SPACE_MATERIAL);
FeedbackTexture2D<SAMPLER_FEEDBACK_MIN_MIP> t_TransmissionFeedback : REGISTER_UAV(GBUFFER_BINDING_MATERIAL_TRANSMISSION_FEEDBACKTEXTURE, GBUFFER_SPACE_MATERIAL);

Texture2D<float> t_BaseOrDiffuseMinMip: REGISTER_SRV(GBUFFER_BINDING_MATERIAL_DIFFUSE_MINMIPTEXTURE, GBUFFER_SPACE_MATERIAL);
Texture2D<float> t_MetalRoughOrSpecularMinMip : REGISTER_SRV(GBUFFER_BINDING_MATERIAL_SPECULAR_MINMIPTEXTURE, GBUFFER_SPACE_MATERIAL);
Texture2D<float> t_NormalMinMip : REGISTER_SRV(GBUFFER_BINDING_MATERIAL_NORMAL_MINMIPTEXTURE, GBUFFER_SPACE_MATERIAL);
Texture2D<float> t_EmissiveMinMip : REGISTER_SRV(GBUFFER_BINDING_MATERIAL_EMISSIVE_MINMIPTEXTURE, GBUFFER_SPACE_MATERIAL);
Texture2D<float> t_OcclusionMinMip : REGISTER_SRV(GBUFFER_BINDING_MATERIAL_OCCLUSION_MINMIPTEXTURE, GBUFFER_SPACE_MATERIAL);
Texture2D<float> t_TransmissionMinMip : REGISTER_SRV(GBUFFER_BINDING_MATERIAL_TRANSMISSION_MINMIPTEXTURE, GBUFFER_SPACE_MATERIAL);

SamplerState s_MaterialSamplerMinMip   : REGISTER_SAMPLER(GBUFFER_BINDING_MATERIAL_SAMPLER_MINMIP, MATERIAL_SAMPLER_REGISTER_SPACE);

// Toggle these defines to use a loop to find the highest resident mip level
// otherwise the "MinMip" texture will be used
#define USE_MIN_MIP_SAMPLER 1
#define USE_MIN_MIP_TEXTURE 1

float4 SampleTiledTexture(float2 texCoord, Texture2D tex, Texture2D<float> texMinMip, bool isDiffuse = false)
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

    return col;
}

float4 SampleWithFeedback(float2 texCoord, Texture2D tex, Texture2D<float> texMinMip, FeedbackTexture2D<SAMPLER_FEEDBACK_MIN_MIP> texFeedback, bool enableFeedback, bool isDiffuse = false)
{
    float4 col = SampleTiledTexture(texCoord, tex, texMinMip, isDiffuse);

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

    if (c_Feedback.useTextureSet)
    {
        // TextureSet codepath where the diffuse texture is the primary texture and we use its sampler feedbakc resource
        
        // NOTE: Normally requiring a diffuse texture and using TextureSets is something the engine can guarantee on build, but
        // because theoretically a material might not have a diffuse texture we need to check for it here.

        if (g_Material.flags & MaterialFlags_UseBaseOrDiffuseTexture)
        {
            values.baseOrDiffuse = SampleTiledTexture(texCoord, t_BaseOrDiffuse, t_BaseOrDiffuseMinMip, true);
        }

        if (g_Material.flags & MaterialFlags_UseMetalRoughOrSpecularTexture)
        {
            values.metalRoughOrSpecular = SampleTiledTexture(texCoord, t_MetalRoughOrSpecular, t_MetalRoughOrSpecularMinMip);
        }

        if (g_Material.flags & MaterialFlags_UseEmissiveTexture)
        {
            values.emissive = SampleTiledTexture(texCoord, t_Emissive, t_EmissiveMinMip);
        }

        if (g_Material.flags & MaterialFlags_UseNormalTexture)
        {
            values.normal = SampleTiledTexture(texCoord, t_Normal, t_NormalMinMip);
        }

        if (g_Material.flags & MaterialFlags_UseOcclusionTexture)
        {
            values.occlusion = SampleTiledTexture(texCoord, t_Occlusion, t_OcclusionMinMip);
        }

        if (g_Material.flags & MaterialFlags_UseTransmissionTexture)
        {
            values.transmission = SampleTiledTexture(texCoord, t_Transmission, t_TransmissionMinMip);
        }

#if WRITEFEEDBACK
#ifndef SPIRV
        if (enableFeedback)
            t_BaseOrDiffuseFeedback.WriteSamplerFeedback(t_BaseOrDiffuse, s_MaterialSampler, texCoord);
#endif // SPIRV
#endif // WRITEFEEDBACK
    }
    else
    {
        // Individual textures all have smapler feedback textures

        // This is a "fallback" path only for the unusual materials without diffuse textures

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
    o_motion = GetMotionVector(i_position.xyz, i_vtx.prevPos, c_GBuffer.view, c_GBuffer.viewPrev);
#endif
}
