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

#include <nvrhi/nvrhi.h>

#include "../include/FeedbackManager.h"
#include "rtxts-ttm/TiledTextureManager.h"

#include <vector>
#include <atomic>
#include <unordered_map>

namespace nvfeedback
{
    class FeedbackManagerImpl;
    class FeedbackTextureImpl;
    class FeedbackTextureSetImpl;

    struct TextureAndTile
    {
        FeedbackTextureImpl* tex;
        uint32_t tile;

        TextureAndTile(FeedbackTextureImpl* _tex, uint32_t _tile) :
            tex(_tex),
            tile(_tile)
        {
        }
    };

    class FeedbackTextureImpl : public FeedbackTexture
    {
    public:
        unsigned long AddRef() override
        {
            return ++m_refCount;
        }

        unsigned long Release() override
        {
            unsigned long result = --m_refCount;
            if (result == 0) {
                delete this;
            }
            return result;
        }

        nvrhi::TextureHandle GetReservedTexture() override;
        nvrhi::SamplerFeedbackTextureHandle GetSamplerFeedbackTexture() override;
        nvrhi::TextureHandle GetMinMipTexture() override;
        bool IsTilePacked(uint32_t tileIndex) override;
        void GetTileInfo(uint32_t tileIndex, std::vector<FeedbackTextureTileInfo>& tiles) override;
        uint32_t GetNumTextureSets() const override;
        FeedbackTextureSet* GetTextureSet(uint32_t index) const override;

        // Internal methods
        FeedbackTextureImpl(const nvrhi::TextureDesc& desc, FeedbackManagerImpl* pFeedbackManager, rtxts::TiledTextureManager* tiledTextureManager, nvrhi::IDevice* device, uint32_t numReadbacks);
        ~FeedbackTextureImpl();

        nvrhi::BufferHandle GetFeedbackResolveBuffer(uint32_t frameIndex) { return m_feedbackResolveBuffers[frameIndex]; }

        uint32_t GetNumTiles() { return m_numTiles; }
        const nvrhi::TileShape& GetTileShape() const { return m_tileShape; }
        const nvrhi::PackedMipDesc& GetPackedMipInfo() const { return m_packedMipDesc; }

        uint32_t GetTiledTextureId() { return m_tiledTextureId; }
        
        // Methods for texture set management
        bool AddToTextureSet(FeedbackTextureSetImpl* textureSet);
        bool RemoveFromTextureSet(FeedbackTextureSetImpl* textureSet);
        void UpdateTextureSets();
        
        // Check if this texture is a primary texture in any of its texture sets
        bool IsPrimaryTexture() const;
        
        // Get all texture sets this texture belongs to in vectors, for internal use only
        const std::vector<FeedbackTextureSetImpl*>& GetTextureSets() const { return m_textureSets; }
        const std::vector<FeedbackTextureSetImpl*>& GetPrimaryTextureSets() const { return m_primaryTextureSets; }

    private:

        std::atomic<unsigned long> m_refCount;

        FeedbackManagerImpl* m_pFeedbackManager;

        nvrhi::TextureHandle m_reservedTexture;
        nvrhi::SamplerFeedbackTextureHandle m_feedbackTexture;
        std::vector<nvrhi::BufferHandle> m_feedbackResolveBuffers;
        nvrhi::TextureHandle m_minMipTexture;

        uint32_t m_numTiles = 0;
        nvrhi::PackedMipDesc m_packedMipDesc;
        nvrhi::TileShape m_tileShape;

        uint32_t m_tiledTextureId = 0;
        
        // Members for texture set management
        std::vector<FeedbackTextureSetImpl*> m_textureSets;
        std::vector<FeedbackTextureSetImpl*> m_primaryTextureSets;
    };
}
