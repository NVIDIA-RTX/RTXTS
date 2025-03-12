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

#include <string>
#include <vector>
#include <memory>
#include <chrono>
#include <set>
#include <assert.h>
#include <functional>
#include <algorithm>

#include <wrl/client.h>

#include "../include/feedbackmanager.h"
#include "feedbacktexture.h"

#include "rtxts-ttm/tiledTextureManager.h"

#include <d3d12.h>
#include <nvrhi/nvrhi.h>

using namespace Microsoft::WRL;

namespace nvfeedback
{
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

    class HeapAllocatorImpl : public rtxts::HeapAllocator
    {
    public:
        HeapAllocatorImpl(nvrhi::IDevice* device);
        ~HeapAllocatorImpl() {};

        void AllocateHeap(uint64_t heapSizeInBytes, uint32_t& heapId);
        void ReleaseHeap(uint32_t heapId);
        nvrhi::HeapHandle GetHeapHandle(uint32_t heapId) { return heapId ? m_heaps[heapId - 1] : nullptr; }
        nvrhi::BufferHandle GetBufferHandle(uint32_t heapId) { return heapId ? m_buffers[heapId - 1] : nullptr; }

        uint64_t GetTotalAllocatedBytes() { return m_totalAllocatedBytes; }
        void ReleaseEmptyHeaps();

    private:
        nvrhi::DeviceHandle m_device;
        std::vector<uint32_t> m_freeHeapIndices;
        std::vector<nvrhi::HeapHandle> m_heaps;
        std::vector<nvrhi::BufferHandle> m_buffers;
        uint64_t m_totalAllocatedBytes;
    };

    class FeedbackManagerImpl : public FeedbackManager
    {
    public:
        // Public

        ~FeedbackManagerImpl() override;
        bool CreateTexture(const nvrhi::TextureDesc& desc, FeedbackTexture** ppTex) override;
        void BeginFrame(nvrhi::ICommandList* commandList, const FeedbackUpdateConfig& config, FeedbackTextureCollection* results) override;
        void UpdateTileMappings(nvrhi::ICommandList* commandList, FeedbackTextureCollection* tilesReady) override;
        void ResolveFeedback(nvrhi::ICommandList* commandList) override;
        void EndFrame() override;
        virtual FeedbackManagerStats GetStats() override;

        // Internal

        FeedbackManagerImpl(nvrhi::IDevice* device, const FeedbackManagerDesc& desc);
        void UnregisterTexture(FeedbackTextureImpl* pTex);

        void AddStat_UpdateTileMappingCall(uint32_t count) { m_numUpdateTileMappingsCalls += count; }
        void AddStat_UpdateTileMappingTime(double time) { m_cputimeDxUpdateTileMappings += time; }

    private:
        FeedbackManagerDesc m_desc;
        FeedbackUpdateConfig m_updateConfigThisFrame;

        uint32_t m_numFramesInFlight;
        uint32_t m_frameIndex;

        nvrhi::DeviceHandle m_device;

        std::vector<FeedbackTextureImpl*> m_textures;
        std::list<FeedbackTextureImpl*> m_texturesRingbuffer;
        std::vector<std::vector<FeedbackTextureImpl*>> m_texturesToReadback;

        nvrhi::BufferHandle m_defragBuffer; // Defragmentation tile save/restore buffer
        FeedbackTextureImpl *m_pDefragTexture; // Which texture we are defragmenting
        uint32_t m_defragTile; // Which tile in the texture is being defragmented

        uint32_t m_statsTilesRequested;
        uint32_t m_statsTilesIdle;
        FeedbackManagerStats m_statsLastFrame;

        SimpleTimer m_timerBeginFrame;
        SimpleTimer m_timerUpdateTileMappings;
        double m_cputimeDxUpdateTileMappings;
        SimpleTimer m_timerResolve;
        SimpleTimer m_timerApiResolve;
        uint32_t m_numUpdateTileMappingsCalls;

        std::shared_ptr<HeapAllocatorImpl> m_heapAllocator;
        std::shared_ptr<rtxts::TiledTextureManager> m_tiledTextureManager;
        std::set<FeedbackTextureImpl*> m_minMipDirtyTextures;

        // Used in defragmentation phase
        std::unordered_map<uint32_t, FeedbackTextureImpl*> m_feedbackTexturesMapping;
    };
}
