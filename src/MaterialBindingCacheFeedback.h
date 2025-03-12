/*
 * Copyright (c) 2025, NVIDIA CORPORATION.  All rights reserved.
 *
 * NVIDIA CORPORATION and its licensors retain all intellectual property
 * and proprietary rights in and to this software, related documentation
 * and any modifications thereto.  Any use, reproduction, disclosure or
 * distribution of this software and related documentation without an express
 * license agreement from NVIDIA CORPORATION is strictly prohibited.
 */

#pragma once

#include <donut/engine/SceneTypes.h>
#include <donut/engine/TextureCache.h>
#include <nvrhi/nvrhi.h>
#include <unordered_map>
#include <mutex>

#include "feedbackmanager/include/feedbackmanager.h"
#include "rtxts-ttm/tiledTextureManager.h"

struct FeedbackTextureWrapper
{
    nvrhi::RefCountPtr<nvfeedback::FeedbackTexture> m_feedbackTexture;
    std::shared_ptr<donut::engine::TextureData> m_sourceTexture;
};

struct FeedbackTextureMaps
{
    std::unordered_map<std::string, std::shared_ptr<FeedbackTextureWrapper>> m_feedbackTexturesByName;
    std::unordered_map<nvfeedback::FeedbackTexture*, std::shared_ptr<FeedbackTextureWrapper>> m_feedbackTexturesByFeedback;
    std::unordered_map<donut::engine::LoadedTexture*, std::shared_ptr<FeedbackTextureWrapper>> m_feedbackTexturesBySource;
};

enum class MaterialResourceFeedback
{
    ConstantBuffer,
    Sampler,
    SamplerMinMip,
    DiffuseTexture,
    SpecularTexture,
    NormalTexture,
    EmissiveTexture,
    OcclusionTexture,
    TransmissionTexture,
    DiffuseTextureFeedback,
    SpecularTextureFeedback,
    NormalTextureFeedback,
    EmissiveTextureFeedback,
    OcclusionTextureFeedback,
    TransmissionTextureFeedback,
    DiffuseTextureMinMip,
    SpecularTextureMinMip,
    NormalTextureMinMip,
    EmissiveTextureMinMip,
    OcclusionTextureMinMip,
    TransmissionTextureMinMip
};

struct MaterialResourceBindingFeedback
{
    MaterialResourceFeedback resource;
    uint32_t slot; // type depends on resource
};

class MaterialBindingCacheFeedback
{
private:
    nvrhi::DeviceHandle m_Device;
    FeedbackTextureMaps* m_feedbackMaps;
    nvrhi::BindingLayoutHandle m_BindingLayout;
    std::unordered_map<const donut::engine::Material*, nvrhi::BindingSetHandle> m_BindingSets;
    nvrhi::ShaderType m_ShaderType;
    std::vector<MaterialResourceBindingFeedback> m_BindingDesc;
    nvrhi::SamplerFeedbackTextureHandle m_FallbackSamplerFeedbackTexture;
    nvrhi::TextureHandle m_FallbackTexture;
    nvrhi::SamplerHandle m_Sampler;
    nvrhi::SamplerHandle m_SamplerMinMip;
    std::mutex m_Mutex;
    bool m_TrackLiveness;

    nvrhi::BindingSetHandle CreateMaterialBindingSet(const donut::engine::Material* material);
    nvrhi::BindingSetItem GetTextureBindingSetItem(uint32_t slot, const std::shared_ptr<donut::engine::LoadedTexture>& texture) const;
    nvrhi::BindingSetItem GetTextureFeedbackBindingSetItem(uint32_t slot, const std::shared_ptr<donut::engine::LoadedTexture>& texture) const;
    nvrhi::BindingSetItem GetTextureMinMipBindingSetItem(uint32_t slot, const std::shared_ptr<donut::engine::LoadedTexture>& texture) const;

public:
    MaterialBindingCacheFeedback(
        nvrhi::IDevice* device, 
        FeedbackTextureMaps* feedbackMaps,
        nvrhi::ShaderType shaderType, 
        uint32_t registerSpace,
        const std::vector<MaterialResourceBindingFeedback>& bindings,
        nvrhi::ISampler* sampler,
        nvrhi::ISampler* samplerMinMip,
        bool trackLiveness = true);

    nvrhi::IBindingLayout* GetLayout() const;
    nvrhi::IBindingSet* GetMaterialBindingSet(const donut::engine::Material* material);
    void Clear();
};
