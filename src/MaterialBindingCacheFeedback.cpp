/*
 * Copyright (c) 2025, NVIDIA CORPORATION.  All rights reserved.
 *
 * NVIDIA CORPORATION and its licensors retain all intellectual property
 * and proprietary rights in and to this software, related documentation
 * and any modifications thereto.  Any use, reproduction, disclosure or
 * distribution of this software and related documentation without an express
 * license agreement from NVIDIA CORPORATION is strictly prohibited.
 */

#include "MaterialBindingCacheFeedback.h"
#include <donut/core/log.h>

MaterialBindingCacheFeedback::MaterialBindingCacheFeedback(
    nvrhi::IDevice* device,
    FeedbackTextureMaps* feedbackMaps,
    nvrhi::ShaderType shaderType, 
    uint32_t registerSpace,
    const std::vector<MaterialResourceBindingFeedback>& bindings,
    nvrhi::ISampler* sampler,
    nvrhi::ISampler* samplerMinMip,
    bool trackLiveness)
    : m_Device(device)
    , m_feedbackMaps(feedbackMaps)
    , m_ShaderType(shaderType)
    , m_BindingDesc(bindings)
    , m_Sampler(sampler)
    , m_SamplerMinMip(samplerMinMip)
    , m_TrackLiveness(trackLiveness)
{
    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = shaderType;
    layoutDesc.registerSpace = registerSpace;

    {
        nvrhi::TextureDesc textureDesc = {};
        textureDesc.width = 8;
        textureDesc.height = 8;
        textureDesc.format = nvrhi::Format::R32_FLOAT;
        textureDesc.initialState = nvrhi::ResourceStates::ShaderResource;
        textureDesc.keepInitialState = true;
        m_FallbackTexture = m_Device->createTexture(textureDesc);

        nvrhi::SamplerFeedbackTextureDesc samplerFeedbackTextureDesc = {};
        samplerFeedbackTextureDesc.samplerFeedbackFormat = nvrhi::SamplerFeedbackFormat::MinMipOpaque;
        samplerFeedbackTextureDesc.samplerFeedbackMipRegionX = 4;
        samplerFeedbackTextureDesc.samplerFeedbackMipRegionY = 4;
        samplerFeedbackTextureDesc.samplerFeedbackMipRegionZ = 1;
        m_FallbackSamplerFeedbackTexture = m_Device->createSamplerFeedbackTexture(m_FallbackTexture, samplerFeedbackTextureDesc);
    }

    for (const auto& item : bindings)
    {
        nvrhi::BindingLayoutItem layoutItem{};
        layoutItem.slot = item.slot;
        
        switch (item.resource)
        {
        case MaterialResourceFeedback::ConstantBuffer:
            layoutItem.type = nvrhi::ResourceType::ConstantBuffer;
            break;
        case MaterialResourceFeedback::DiffuseTexture:
        case MaterialResourceFeedback::SpecularTexture:
        case MaterialResourceFeedback::NormalTexture:
        case MaterialResourceFeedback::EmissiveTexture:
        case MaterialResourceFeedback::OcclusionTexture:
        case MaterialResourceFeedback::TransmissionTexture:
        case MaterialResourceFeedback::DiffuseTextureMinMip:
        case MaterialResourceFeedback::SpecularTextureMinMip:
        case MaterialResourceFeedback::NormalTextureMinMip:
        case MaterialResourceFeedback::EmissiveTextureMinMip:
        case MaterialResourceFeedback::OcclusionTextureMinMip:
        case MaterialResourceFeedback::TransmissionTextureMinMip:
            layoutItem.type = nvrhi::ResourceType::Texture_SRV;
            break;
        case MaterialResourceFeedback::DiffuseTextureFeedback:
        case MaterialResourceFeedback::SpecularTextureFeedback:
        case MaterialResourceFeedback::NormalTextureFeedback:
        case MaterialResourceFeedback::EmissiveTextureFeedback:
        case MaterialResourceFeedback::OcclusionTextureFeedback:
        case MaterialResourceFeedback::TransmissionTextureFeedback:
            layoutItem.type = nvrhi::ResourceType::SamplerFeedbackTexture_UAV;
            break;
        case MaterialResourceFeedback::Sampler:
        case MaterialResourceFeedback::SamplerMinMip:
            layoutItem.type = nvrhi::ResourceType::Sampler;
            break;
        default:
            donut::log::error("MaterialBindingCache: unknown MaterialResource value (%d)", item.resource);
            return;
        }

        layoutDesc.bindings.push_back(layoutItem);
    }

    m_BindingLayout = m_Device->createBindingLayout(layoutDesc);
}

nvrhi::IBindingLayout* MaterialBindingCacheFeedback::GetLayout() const
{
    return m_BindingLayout;
}

nvrhi::IBindingSet* MaterialBindingCacheFeedback::GetMaterialBindingSet(const donut::engine::Material* material)
{
    std::lock_guard<std::mutex> lockGuard(m_Mutex);

    nvrhi::BindingSetHandle& bindingSet = m_BindingSets[material];

    if (bindingSet)
        return bindingSet;

    bindingSet = CreateMaterialBindingSet(material);

    return bindingSet;
}

void MaterialBindingCacheFeedback::Clear()
{
    std::lock_guard<std::mutex> lockGuard(m_Mutex);

    m_BindingSets.clear();
}

nvrhi::BindingSetItem MaterialBindingCacheFeedback::GetTextureBindingSetItem(uint32_t slot, const std::shared_ptr<donut::engine::LoadedTexture>& texture) const
{
    if (!texture)
        return nvrhi::BindingSetItem::Texture_SRV(slot, m_FallbackTexture.Get());
    else if (texture->texture)
        return nvrhi::BindingSetItem::Texture_SRV(slot, texture->texture);

    donut::engine::LoadedTexture* ptr = texture.get();
    return nvrhi::BindingSetItem::Texture_SRV(slot, m_feedbackMaps->m_feedbackTexturesBySource[ptr]->m_feedbackTexture->GetReservedTexture());
}

nvrhi::BindingSetItem MaterialBindingCacheFeedback::GetTextureFeedbackBindingSetItem(uint32_t slot, const std::shared_ptr<donut::engine::LoadedTexture>& texture) const
{
    if (!texture || texture->texture)
        return nvrhi::BindingSetItem::SamplerFeedbackTexture_UAV(slot, m_FallbackSamplerFeedbackTexture.Get());

    return nvrhi::BindingSetItem::SamplerFeedbackTexture_UAV(slot, m_feedbackMaps->m_feedbackTexturesBySource[texture.get()]->m_feedbackTexture->GetSamplerFeedbackTexture());
}

nvrhi::BindingSetItem MaterialBindingCacheFeedback::GetTextureMinMipBindingSetItem(uint32_t slot, const std::shared_ptr<donut::engine::LoadedTexture>& texture) const
{
    if (!texture || texture->texture)
        return nvrhi::BindingSetItem::Texture_SRV(slot, m_FallbackTexture.Get());

    return nvrhi::BindingSetItem::Texture_SRV(slot, m_feedbackMaps->m_feedbackTexturesBySource[texture.get()]->m_feedbackTexture->GetMinMipTexture());
}

nvrhi::BindingSetHandle MaterialBindingCacheFeedback::CreateMaterialBindingSet(const donut::engine::Material* material)
{
    nvrhi::BindingSetDesc bindingSetDesc;
    bindingSetDesc.trackLiveness = m_TrackLiveness;

    for (const auto& item : m_BindingDesc)
    {
        nvrhi::BindingSetItem setItem;

        switch (item.resource)
        {
        case MaterialResourceFeedback::ConstantBuffer:
            setItem = nvrhi::BindingSetItem::ConstantBuffer(
                item.slot, 
                material->materialConstants);
            break;

        case MaterialResourceFeedback::Sampler:
            setItem = nvrhi::BindingSetItem::Sampler(
                item.slot,
                m_Sampler);
            break;

        case MaterialResourceFeedback::SamplerMinMip:
            setItem = nvrhi::BindingSetItem::Sampler(
                item.slot,
                m_SamplerMinMip);
            break;

        case MaterialResourceFeedback::DiffuseTexture:
            setItem = GetTextureBindingSetItem(item.slot, material->baseOrDiffuseTexture);
            break;

        case MaterialResourceFeedback::SpecularTexture:
            setItem = GetTextureBindingSetItem(item.slot, material->metalRoughOrSpecularTexture);
            break;

        case MaterialResourceFeedback::NormalTexture:
            setItem = GetTextureBindingSetItem(item.slot, material->normalTexture);
            break;

        case MaterialResourceFeedback::EmissiveTexture:
            setItem = GetTextureBindingSetItem(item.slot, material->emissiveTexture);
            break;

        case MaterialResourceFeedback::OcclusionTexture:
            setItem = GetTextureBindingSetItem(item.slot, material->occlusionTexture);
            break;

        case MaterialResourceFeedback::TransmissionTexture:
            setItem = GetTextureBindingSetItem(item.slot, material->transmissionTexture);
            break;

        case MaterialResourceFeedback::DiffuseTextureFeedback:
            setItem = GetTextureFeedbackBindingSetItem(item.slot, material->baseOrDiffuseTexture);
            break;

        case MaterialResourceFeedback::SpecularTextureFeedback:
            setItem = GetTextureFeedbackBindingSetItem(item.slot, material->metalRoughOrSpecularTexture);
            break;

        case MaterialResourceFeedback::NormalTextureFeedback:
            setItem = GetTextureFeedbackBindingSetItem(item.slot, material->normalTexture);
            break;

        case MaterialResourceFeedback::EmissiveTextureFeedback:
            setItem = GetTextureFeedbackBindingSetItem(item.slot, material->emissiveTexture);
            break;

        case MaterialResourceFeedback::OcclusionTextureFeedback:
            setItem = GetTextureFeedbackBindingSetItem(item.slot, material->occlusionTexture);
            break;

        case MaterialResourceFeedback::TransmissionTextureFeedback:
            setItem = GetTextureFeedbackBindingSetItem(item.slot, material->transmissionTexture);
            break;

        case MaterialResourceFeedback::DiffuseTextureMinMip:
            setItem = GetTextureMinMipBindingSetItem(item.slot, material->baseOrDiffuseTexture);
            break;

        case MaterialResourceFeedback::SpecularTextureMinMip:
            setItem = GetTextureMinMipBindingSetItem(item.slot, material->metalRoughOrSpecularTexture);
            break;

        case MaterialResourceFeedback::NormalTextureMinMip:
            setItem = GetTextureMinMipBindingSetItem(item.slot, material->normalTexture);
            break;

        case MaterialResourceFeedback::EmissiveTextureMinMip:
            setItem = GetTextureMinMipBindingSetItem(item.slot, material->emissiveTexture);
            break;

        case MaterialResourceFeedback::OcclusionTextureMinMip:
            setItem = GetTextureMinMipBindingSetItem(item.slot, material->occlusionTexture);
            break;

        case MaterialResourceFeedback::TransmissionTextureMinMip:
            setItem = GetTextureMinMipBindingSetItem(item.slot, material->transmissionTexture);
            break;

        default:
            donut::log::error("MaterialBindingCache: unknown MaterialResource value (%d)", item.resource);
            return nullptr;
        }

        bindingSetDesc.bindings.push_back(setItem);
    }

    return m_Device->createBindingSet(bindingSetDesc, m_BindingLayout);
}
