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

#include <string>
#include <vector>
#include <memory>
#include <chrono>
#include <set>
#include <map>
#include <assert.h>
#include <functional>
#include <algorithm>

#include <wrl/client.h>

#include "../include/FeedbackManager.h"
#include "FeedbackTexture.h"
#include "FeedbackTextureSet.h"

#include "rtxts-ttm/TiledTextureManager.h"

#include <d3d12.h>
#include <nvrhi/nvrhi.h>

using namespace Microsoft::WRL;

namespace nvfeedback
{
    // A really simple timer which holds just one sample
    class SimpleTimer
    {
    public:
        SimpleTimer() :
            m_begin(0),
            m_end(0),
            m_frequency(0)
        {
            QueryPerformanceFrequency((LARGE_INTEGER*)&m_frequency);
        }

        void Clear()
        {
            m_begin = 0;
            m_end = 0;
        }

        void Begin()
        {
            QueryPerformanceCounter((LARGE_INTEGER*)&m_begin);
        }

        void End()
        {
            QueryPerformanceCounter((LARGE_INTEGER*)&m_end);
        }

        double GetTime()
        {
            uint64_t delta = m_end - m_begin;
            return double(delta) / double(m_frequency);
        }

    private:
        uint64_t m_begin;
        uint64_t m_end;
        uint64_t m_frequency;
    };

    class HeapAllocator
    {
    public:
        HeapAllocator(nvrhi::IDevice* device, uint64_t heapSizeInBytes, uint32_t framesInFlight);
        ~HeapAllocator() {};

        void AllocateHeap(uint32_t& heapId);
        void ReleaseHeap(uint32_t heapId, uint32_t frameIndex);
        nvrhi::HeapHandle GetHeapHandle(uint32_t heapId) { return m_heaps[heapId]; }
        nvrhi::BufferHandle GetBufferHandle(uint32_t heapId) { return m_buffers[heapId]; }

        uint64_t GetTotalAllocatedBytes() { return m_totalAllocatedBytes; }

        uint32_t GetNumHeaps() { return m_numHeaps; }

    private:
        uint32_t m_framesInFlight;
        nvrhi::DeviceHandle m_device;
        std::vector<nvrhi::HeapHandle> m_heaps;
        std::vector<nvrhi::BufferHandle> m_buffers;

        std::vector<uint32_t> m_freeHeapIds;

        uint64_t m_heapSizeInBytes;

        uint32_t m_numHeaps;
        uint64_t m_totalAllocatedBytes;

        std::map<uint32_t, std::vector<nvrhi::HeapHandle>> m_heapsToRelease;
        std::map<uint32_t, std::vector<nvrhi::BufferHandle>> m_buffersToRelease;
    };

    class FeedbackManagerImpl : public FeedbackManager
    {
    public:
        // Public

        ~FeedbackManagerImpl() override;
        bool CreateTexture(const nvrhi::TextureDesc& desc, FeedbackTexture** ppTex) override;
        bool CreateTextureSet(FeedbackTextureSet** ppTexSet) override;
        void BeginFrame(nvrhi::ICommandList* commandList, const FeedbackUpdateConfig& config, FeedbackTextureCollection* results) override;
        void UpdateTileMappings(nvrhi::ICommandList* commandList, FeedbackTextureCollection* tilesReady) override;
        void ResolveFeedback(nvrhi::ICommandList* commandList) override;
        void EndFrame() override;
        FeedbackManagerStats GetStats() override;

        // Internal

        FeedbackManagerImpl(nvrhi::IDevice* device, const FeedbackManagerDesc& desc);
        void UnregisterTexture(FeedbackTextureImpl* pTex);

        void UpdateTextureRingBufferState(FeedbackTextureImpl* pTex, bool includeInRingBuffer);

        rtxts::TiledTextureManager* GetTiledTextureManager() { return m_tiledTextureManager.get(); }

    private:
        FeedbackManagerDesc m_desc;
        FeedbackUpdateConfig m_updateConfigThisFrame;

        uint32_t m_numFramesInFlight;
        uint32_t m_frameIndex;

        nvrhi::DeviceHandle m_device;

        std::vector<FeedbackTextureImpl*> m_textures;
        std::list<FeedbackTextureImpl*> m_texturesRingbuffer;
        std::vector<std::vector<FeedbackTextureImpl*>> m_texturesToReadback;

        FeedbackManagerStats m_statsLastFrame;

        SimpleTimer m_timerBeginFrame;
        SimpleTimer m_timerUpdateTileMappings;
        SimpleTimer m_timerResolve;

        std::shared_ptr<HeapAllocator> m_heapAllocator;
        std::shared_ptr<rtxts::TiledTextureManager> m_tiledTextureManager;
        std::set<FeedbackTextureImpl*> m_minMipDirtyTextures;
    };
}
