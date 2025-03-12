/*
 * Copyright (c) 2025, NVIDIA CORPORATION.  All rights reserved.
 *
 * NVIDIA CORPORATION and its licensors retain all intellectual property
 * and proprietary rights in and to this software, related documentation
 * and any modifications thereto.  Any use, reproduction, disclosure or
 * distribution of this software and related documentation without an express
 * license agreement from NVIDIA CORPORATION is strictly prohibited.
 */

#include "feedbacktexture.h"
#include "feedbackmanager_internal.h"
#include <nvrhi/d3d12.h>

#include <array>

namespace nvfeedback
{
    FeedbackTextureImpl::FeedbackTextureImpl(const nvrhi::TextureDesc& desc, FeedbackManagerImpl* pFeedbackManager, rtxts::TiledTextureManager* tiledTextureManager, nvrhi::IDevice* device, uint32_t numReadbacks) :
        m_pFeedbackManager(pFeedbackManager),
        m_refCount(1)
    {
        // Reserved texture
        {
            nvrhi::TextureDesc textureDesc = desc;
            textureDesc.isTiled = true;
            textureDesc.initialState = nvrhi::ResourceStates::ShaderResource;
            textureDesc.keepInitialState = true;
            textureDesc.debugName = "Reserved texture";
            m_reservedTexture = device->createTexture(textureDesc);
        }

        // Get tiling info
        m_numTiles = 0;
        m_packedMipDesc = {};
        m_tileShape = {};
        uint32_t mipLevels = desc.mipLevels;
        std::array<nvrhi::SubresourceTiling, 16> tilingsInfo;
        device->getTextureTiling(m_reservedTexture, &m_numTiles, &m_packedMipDesc, &m_tileShape, &mipLevels, tilingsInfo.data());

        rtxts::TiledLevelDesc tiledLevelDescs[16];
        rtxts::TiledTextureDesc tiledTextureDesc = {};
        tiledTextureDesc.textureWidth = desc.width;
        tiledTextureDesc.textureHeight = desc.height;
        tiledTextureDesc.tiledLevelDescs = tiledLevelDescs;
        tiledTextureDesc.regularMipLevelsNum = m_packedMipDesc.numStandardMips;
        tiledTextureDesc.packedMipLevelsNum = m_packedMipDesc.numPackedMips;
        tiledTextureDesc.packedTilesNum = m_packedMipDesc.numTilesForPackedMips;
        tiledTextureDesc.tileWidth = m_tileShape.widthInTexels;
        tiledTextureDesc.tileHeight = m_tileShape.heightInTexels;

        for (uint32_t i = 0; i < tiledTextureDesc.regularMipLevelsNum; ++i)
        {
            tiledLevelDescs[i].widthInTiles = tilingsInfo[i].widthInTiles;
            tiledLevelDescs[i].heightInTiles = tilingsInfo[i].heightInTiles;
        }

        tiledTextureManager->AddTiledTexture(tiledTextureDesc, m_tiledTextureId);

        if (!m_tiledTextureId)
            return;
        
        rtxts::TextureDesc feedbackDesc = tiledTextureManager->GetTextureDesc(m_tiledTextureId, rtxts::eFeedbackTexture);
        if (device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D12)
        {
            nvrhi::d3d12::IDevice* deviceD3D12 = static_cast<nvrhi::d3d12::IDevice*>(device);
            nvrhi::SamplerFeedbackTextureDesc samplerFeedbackTextureDesc = {};
            samplerFeedbackTextureDesc.samplerFeedbackFormat = nvrhi::SamplerFeedbackFormat::MinMipOpaque;
            samplerFeedbackTextureDesc.samplerFeedbackMipRegionX = feedbackDesc.textureOrMipRegionWidth;
            samplerFeedbackTextureDesc.samplerFeedbackMipRegionY = feedbackDesc.textureOrMipRegionHeight;
            samplerFeedbackTextureDesc.samplerFeedbackMipRegionZ = m_tileShape.depthInTexels;
            samplerFeedbackTextureDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
            samplerFeedbackTextureDesc.keepInitialState = true;
            m_feedbackTexture = deviceD3D12->createSamplerFeedbackTexture(m_reservedTexture, samplerFeedbackTextureDesc);
        }

        // Resolve / Readback buffer
        uint32_t readbackBuffersNum = 1;
        readbackBuffersNum = numReadbacks;
        m_feedbackResolveBuffers.resize(readbackBuffersNum);
        for (uint32_t i = 0; i < readbackBuffersNum; i++)
        {
            uint32_t feedbackTilesX = (desc.width - 1) / feedbackDesc.textureOrMipRegionWidth + 1;
            uint32_t feedbackTilesY = (desc.height - 1) / feedbackDesc.textureOrMipRegionHeight + 1;

            nvrhi::BufferDesc bufferDesc = {};
            bufferDesc.byteSize = feedbackTilesX * feedbackTilesY;
            bufferDesc.cpuAccess = nvrhi::CpuAccessMode::Read;
            bufferDesc.initialState = nvrhi::ResourceStates::ResolveDest;
            bufferDesc.debugName = "Resolve Buffer";
            m_feedbackResolveBuffers[i] = device->createBuffer(bufferDesc);
        }

        // MinMip texture
        {
            rtxts::TextureDesc minMipDesc = tiledTextureManager->GetTextureDesc(m_tiledTextureId, rtxts::eMinMipTexture);

            nvrhi::TextureDesc textureDesc = {};
            textureDesc.width = minMipDesc.textureOrMipRegionWidth;
            textureDesc.height = minMipDesc.textureOrMipRegionHeight;
            textureDesc.format = nvrhi::Format::R32_FLOAT;
            textureDesc.initialState = nvrhi::ResourceStates::ShaderResource;
            textureDesc.keepInitialState = true;
            textureDesc.debugName = "MinMip Texture";
            m_minMipTexture = device->createTexture(textureDesc);
        }
    }

    FeedbackTextureImpl::~FeedbackTextureImpl()
    {
        m_pFeedbackManager->UnregisterTexture(this);
    }

    nvrhi::TextureHandle FeedbackTextureImpl::GetReservedTexture()
    {
        return m_reservedTexture;
    }

    nvrhi::SamplerFeedbackTextureHandle FeedbackTextureImpl::GetSamplerFeedbackTexture()
    {
        return m_feedbackTexture;
    }

    nvrhi::TextureHandle FeedbackTextureImpl::GetMinMipTexture()
    {
        return m_minMipTexture;
    }
}
