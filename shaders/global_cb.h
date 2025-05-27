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

#pragma once

#define GBUFFER_BINDING_GLOBAL_CONSTANTS 3

#define GBUFFER_BINDING_FEEDBACK_CONSTANTS 4

#define GBUFFER_BINDING_MATERIAL_DIFFUSE_FEEDBACKTEXTURE 0
#define GBUFFER_BINDING_MATERIAL_SPECULAR_FEEDBACKTEXTURE 1
#define GBUFFER_BINDING_MATERIAL_NORMAL_FEEDBACKTEXTURE 2
#define GBUFFER_BINDING_MATERIAL_EMISSIVE_FEEDBACKTEXTURE 3
#define GBUFFER_BINDING_MATERIAL_OCCLUSION_FEEDBACKTEXTURE 4
#define GBUFFER_BINDING_MATERIAL_TRANSMISSION_FEEDBACKTEXTURE 5
#define GBUFFER_BINDING_MATERIAL_OPACITY_FEEDBACKTEXTURE 6

#define GBUFFER_BINDING_MATERIAL_DIFFUSE_MINMIPTEXTURE 6
#define GBUFFER_BINDING_MATERIAL_SPECULAR_MINMIPTEXTURE 7
#define GBUFFER_BINDING_MATERIAL_NORMAL_MINMIPTEXTURE 8
#define GBUFFER_BINDING_MATERIAL_EMISSIVE_MINMIPTEXTURE 9
#define GBUFFER_BINDING_MATERIAL_OCCLUSION_MINMIPTEXTURE 10
#define GBUFFER_BINDING_MATERIAL_TRANSMISSION_MINMIPTEXTURE 11
#define GBUFFER_BINDING_MATERIAL_OPACITY_MINMIPTEXTURE 12

#define GBUFFER_BINDING_MATERIAL_SAMPLER_MINMIP 1

struct GlobalConstants
{
    uint		frameIndex;
    uint		showUnmappedRegions;
    uint		enableDebug;
    float		feedbackThreshold;
};

