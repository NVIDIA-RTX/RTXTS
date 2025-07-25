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

#include <donut/shaders/scene_material.hlsli>
#include <donut/shaders/forward_vertex.hlsli>
#include <donut/shaders/material_bindings.hlsli>

void main(
    in float4 i_position : SV_Position,
    in SceneVertex i_vtx,
    in uint i_instance : INSTANCE,
    out uint4 o_output : SV_Target0
)
{
    o_output.x = uint(g_Material.materialID);
    o_output.y = i_instance;
    o_output.zw = 0;
}
