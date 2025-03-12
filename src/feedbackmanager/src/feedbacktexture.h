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

#include <nvrhi/nvrhi.h>

#include "../include/feedbackmanager.h"
#include "rtxts-ttm/tiledTextureManager.h"

#include <vector>
#include <atomic>

namespace nvfeedback
{
    class FeedbackManagerImpl;
    class FeedbackTextureImpl;

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

        // Internal methods
        FeedbackTextureImpl(const nvrhi::TextureDesc& desc, FeedbackManagerImpl* pFeedbackManager, rtxts::TiledTextureManager* tiledTextureManager, nvrhi::IDevice* device, uint32_t numReadbacks);
        ~FeedbackTextureImpl();

        nvrhi::BufferHandle GetFeedbackResolveBuffer(uint32_t frameIndex) { return m_feedbackResolveBuffers[frameIndex]; }

        uint32_t GetNumTiles() { return m_numTiles; }
        const nvrhi::TileShape& GetTileShape() const { return m_tileShape; }
        const nvrhi::PackedMipDesc& GetPackedMipInfo() const { return m_packedMipDesc; }

        uint32_t GetTiledTextureId() { return m_tiledTextureId; }

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
    };
}
