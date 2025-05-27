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

#include "FeedbackTexture.h"
#include "FeedbackManagerInternal.h"
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
        std::vector<FeedbackTextureSetImpl*> textureSets = m_textureSets;
        for (auto textureSet : textureSets)
        {
            RemoveFromTextureSet(textureSet);
        }
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

    bool FeedbackTextureImpl::IsTilePacked(uint32_t tileIndex)
    {
        return tileIndex >= GetPackedMipInfo().startTileIndexInOverallResource;
    }

    void FeedbackTextureImpl::GetTileInfo(uint32_t tileIndex, std::vector<FeedbackTextureTileInfo>& tiles)
    {
        tiles.clear();

        auto& tileShape = GetTileShape();
        auto& packedMipInfo = GetPackedMipInfo();
        const nvrhi::TextureDesc textureDesc = GetReservedTexture()->getDesc();
        bool isBlockCompressed = (textureDesc.format >= nvrhi::Format::BC1_UNORM && textureDesc.format <= nvrhi::Format::BC7_UNORM_SRGB);

        uint32_t firstPackedTileIndex = packedMipInfo.startTileIndexInOverallResource;

        if (IsTilePacked(tileIndex))
        {
            for (uint32_t mip = packedMipInfo.numStandardMips; mip < uint32_t(packedMipInfo.numStandardMips + packedMipInfo.numPackedMips); mip++)
            {
                uint32_t width = std::max(textureDesc.width >> mip, 1u);
                uint32_t height = std::max(textureDesc.height >> mip, 1u);

                // Round up subresource size for BC compressed formats to match block sizes
                if (isBlockCompressed)
                {
                    // Round up to 4x4 blocks
                    width = ((width + 3) / 4) * 4;
                    height = ((height + 3) / 4) * 4;
                }

                FeedbackTextureTileInfo tile;
                tile.xInTexels = 0;
                tile.yInTexels = 0;
                tile.mip = mip;
                tile.widthInTexels = width;
                tile.heightInTexels = height;
                tiles.push_back(tile);
            }
        }
        else
        {
            const auto& tileCoord = m_pFeedbackManager->GetTiledTextureManager()->GetTileCoordinates(m_tiledTextureId);
            uint32_t tileX = tileCoord[tileIndex].x;
            uint32_t tileY = tileCoord[tileIndex].y;
            uint32_t mip = tileCoord[tileIndex].mipLevel;
            uint32_t width = tileShape.widthInTexels;
            uint32_t height = tileShape.heightInTexels;

            uint32_t subresourceWidth = std::max(textureDesc.width >> mip, 1u);
            uint32_t subresourceHeight = std::max(textureDesc.height >> mip, 1u);

            // Round up subresource size for BC compressed formats to match block sizes
            if (isBlockCompressed)
            {
                // Round up to 4x4 blocks
                subresourceWidth = ((subresourceWidth + 3) / 4) * 4;
                subresourceHeight = ((subresourceHeight + 3) / 4) * 4;
            }

            uint32_t x = tileX * width;
            uint32_t y = tileY * height;

            // Make sure the tile (for filling out the data) doesn't extend past the actual subresource
            if (x + width > subresourceWidth)
                width = subresourceWidth - x;
            if (y + height > subresourceHeight)
                height = subresourceHeight - y;

            FeedbackTextureTileInfo tile;
            tile.xInTexels = x;
            tile.yInTexels = y;
            tile.mip = mip;
            tile.widthInTexels = width;
            tile.heightInTexels = height;
            tiles.push_back(tile);
        }
    }

    uint32_t FeedbackTextureImpl::GetNumTextureSets() const
    {
        return (uint32_t)m_textureSets.size();
    }

    FeedbackTextureSet* FeedbackTextureImpl::GetTextureSet(uint32_t index) const
    {
        return m_textureSets[index];
    }
    
    bool FeedbackTextureImpl::AddToTextureSet(FeedbackTextureSetImpl* textureSet)
    {
        if (!textureSet)
        {
            return false;
        }
        auto it = std::find(m_textureSets.begin(), m_textureSets.end(), textureSet);
        if (it == m_textureSets.end())
        {
            m_textureSets.push_back(textureSet);
        }
        UpdateTextureSets();
        return true;
    }
    
    bool FeedbackTextureImpl::RemoveFromTextureSet(FeedbackTextureSetImpl* textureSet)
    {
        if (!textureSet)
        {
            return false;
        }
        auto it = std::find(m_textureSets.begin(), m_textureSets.end(), textureSet);
        if (it == m_textureSets.end())
        {
            return false;
        }
        m_textureSets.erase(it);
        UpdateTextureSets();
        return true;
    }

    void FeedbackTextureImpl::UpdateTextureSets()
    {
        // Figure out in which texture sets this texture is the primary texture for sampler feedback
        m_primaryTextureSets.clear();
        for (const auto& textureSet : m_textureSets)
        {
            if (textureSet->GetPrimaryTexture() == this)
            {
                m_primaryTextureSets.push_back(textureSet);
            }
        }

        // Ensure this texture is in the ringbuffer, unless we use texture sets and are never a primary texture
        bool needsRingBuffer = m_textureSets.size() == 0 || IsPrimaryTexture();
        m_pFeedbackManager->UpdateTextureRingBufferState(this, needsRingBuffer);
    }
    
    bool FeedbackTextureImpl::IsPrimaryTexture() const
    {
        return m_primaryTextureSets.size() > 0;
    }
}
