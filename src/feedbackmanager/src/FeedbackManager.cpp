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

#include "../include/FeedbackManager.h"
#include "FeedbackManagerInternal.h"

#include <map>
#include <assert.h>

namespace nvfeedback
{
    FeedbackManagerImpl::FeedbackManagerImpl(nvrhi::IDevice* device, const FeedbackManagerDesc& desc) :
        m_device(device),
        m_desc(desc),
        m_numFramesInFlight(desc.numFramesInFlight),
        m_frameIndex(0),
        m_statsLastFrame()
    {
        m_texturesToReadback.resize(m_numFramesInFlight);
        ZeroMemory(&m_statsLastFrame, sizeof(FeedbackManagerStats));

        m_heapAllocator = std::make_shared<HeapAllocator>(m_device, desc.heapSizeInTiles * D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES, desc.numFramesInFlight);

        rtxts::TiledTextureManagerDesc tiledTextureManagerDesc = {};
        tiledTextureManagerDesc.heapTilesCapacity = desc.heapSizeInTiles;
        m_tiledTextureManager = std::shared_ptr<rtxts::TiledTextureManager>(CreateTiledTextureManager(tiledTextureManagerDesc));
    }

    FeedbackManagerImpl::~FeedbackManagerImpl()
    {
    }

    bool FeedbackManagerImpl::CreateTexture(const nvrhi::TextureDesc& desc, FeedbackTexture** ppTex)
    {
        FeedbackTextureImpl* feedbackTexture = new FeedbackTextureImpl(desc, this, m_tiledTextureManager.get(), m_device, m_numFramesInFlight);
        m_textures.push_back(feedbackTexture);
        m_texturesRingbuffer.push_back(feedbackTexture);
        *ppTex = feedbackTexture;
        return true;
    }

    bool FeedbackManagerImpl::CreateTextureSet(FeedbackTextureSet** ppTexSet)
    {
        if (!ppTexSet)
        {
            return false;
        }
        
        FeedbackTextureSetImpl* textureSet = new FeedbackTextureSetImpl(this, m_device, m_numFramesInFlight);
        
        *ppTexSet = textureSet;
        return true;
    }

    void FeedbackManagerImpl::UnregisterTexture(FeedbackTextureImpl* feedbackTexture)
    {
        m_textures.erase(std::remove(m_textures.begin(), m_textures.end(), feedbackTexture), m_textures.end());
        
        m_texturesRingbuffer.erase(std::remove(m_texturesRingbuffer.begin(), m_texturesRingbuffer.end(), feedbackTexture), m_texturesRingbuffer.end());

        for (auto& vec : m_texturesToReadback)
        {
            auto it = std::find(vec.begin(), vec.end(), feedbackTexture);
            if (it != vec.end())
                vec.erase(it);
        }

        auto it = std::find(m_minMipDirtyTextures.begin(), m_minMipDirtyTextures.end(), feedbackTexture);
        if (it != m_minMipDirtyTextures.end())
            m_minMipDirtyTextures.erase(it);
    }

    void FeedbackManagerImpl::UpdateTextureRingBufferState(FeedbackTextureImpl* pTex, bool includeInRingBuffer)
    {
        auto it = std::find(m_texturesRingbuffer.begin(), m_texturesRingbuffer.end(), pTex);
        if (includeInRingBuffer && it == m_texturesRingbuffer.end())
        {
            m_texturesRingbuffer.push_back(pTex);
        }
        else if (!includeInRingBuffer && it != m_texturesRingbuffer.end())
        {
            m_texturesRingbuffer.erase(it);
        }
    }

    void FeedbackManagerImpl::BeginFrame(nvrhi::ICommandList* commandList, const FeedbackUpdateConfig& config, FeedbackTextureCollection* results)
    {
        m_timerBeginFrame.Begin();

        m_frameIndex = config.frameIndex % m_numFramesInFlight;

        m_updateConfigThisFrame = config;

        rtxts::TiledTextureManagerConfig tiledTextureManagerConfig = {};
        tiledTextureManagerConfig.numExtraStandbyTiles = config.numExtraStandbyTiles;
        m_tiledTextureManager->SetConfig(tiledTextureManagerConfig);

        auto& readbackTextures = m_texturesToReadback[m_frameIndex];
        if (!readbackTextures.empty())
        {
            float timeStamp = float(GetTickCount64()) / 1000.0f;
            uint32_t texturesNum = uint32_t(readbackTextures.size());
            for (uint32_t iReadbackTexture = 0; iReadbackTexture < texturesNum; ++iReadbackTexture)
            {
                FeedbackTextureImpl* readbackTexture = readbackTextures[iReadbackTexture];
                uint8_t* pReadbackData = (uint8_t*)m_device->mapBuffer(readbackTexture->GetFeedbackResolveBuffer(m_frameIndex), nvrhi::CpuAccessMode::Read);

                rtxts::SamplerFeedbackDesc samplerFeedbackDesc = {};
                samplerFeedbackDesc.pMinMipData = (uint8_t*)(pReadbackData);
                m_tiledTextureManager->UpdateWithSamplerFeedback(readbackTexture->GetTiledTextureId(), samplerFeedbackDesc, timeStamp, m_updateConfigThisFrame.tileTimeoutSeconds);

                m_device->unmapBuffer(readbackTexture->GetFeedbackResolveBuffer(m_frameIndex));

                // If this is a primary texture, make followers match its state
                if (readbackTexture->IsPrimaryTexture())
                {
                    auto textureSets = readbackTexture->GetPrimaryTextureSets();
                    for (auto textureSet : textureSets)
                    {
                        uint32_t numTextures = textureSet->GetNumTextures();
                        uint32_t primaryTextureIndex = textureSet->GetPrimaryTextureIndex();
                        for (uint32_t iTextureSet = 0; iTextureSet < numTextures; ++iTextureSet)
                        {
                            if (iTextureSet == primaryTextureIndex)
                                continue;

                            // Make the follower texture match the primary texture requested tile state
                            FeedbackTexture* follower = textureSet->GetTexture(iTextureSet);
                            FeedbackTextureImpl* followerImpl = static_cast<FeedbackTextureImpl*>(follower);
                            m_tiledTextureManager->MatchPrimaryTexture(
                                readbackTexture->GetTiledTextureId(),
                                followerImpl->GetTiledTextureId(),
                                timeStamp,
                                m_updateConfigThisFrame.tileTimeoutSeconds);
                        }
                    }
                }
            }
        }

        // Collect textures to read back
        readbackTextures.clear();
        {
            uint32_t updatesLeft = m_updateConfigThisFrame.maxTexturesToUpdate;
            for (auto& feedbackTexture : m_texturesRingbuffer)
            {
                if (m_updateConfigThisFrame.maxTexturesToUpdate > 0 && updatesLeft == 0)
                    break;
                commandList->clearSamplerFeedbackTexture(feedbackTexture->GetSamplerFeedbackTexture());
                readbackTextures.push_back(feedbackTexture);
                updatesLeft--;
            }
        }

        // Trim standby tiles if requested
        if (m_updateConfigThisFrame.trimStandbyTiles)
        {
            m_tiledTextureManager->TrimStandbyTiles();
        }

        // Now check how many heaps the tiled texture manager needs
        uint32_t numRequiredHeaps = m_tiledTextureManager->GetNumDesiredHeaps();
        if (numRequiredHeaps > m_heapAllocator->GetNumHeaps())
        {
            while (m_heapAllocator->GetNumHeaps() < numRequiredHeaps)
            {
                uint32_t heapId;
                m_heapAllocator->AllocateHeap(heapId);
                m_tiledTextureManager->AddHeap(heapId);
            }
        }
        else if (m_updateConfigThisFrame.releaseEmptyHeaps)
        {
            std::vector<uint32_t> emptyHeaps;
            m_tiledTextureManager->GetEmptyHeaps(emptyHeaps);
            for (auto& heapId : emptyHeaps)
            {
                m_tiledTextureManager->RemoveHeap(heapId);
                m_heapAllocator->ReleaseHeap(heapId, m_frameIndex);
            }
        }

        // Now let the tiled texture manager allocate
        m_tiledTextureManager->AllocateRequestedTiles();

        // Get tiles to unmap and map from the tiled texture manager
        // TODO: The current code does not merge unmapping and mapping tiles for the same textures. It would be more optimal.
        std::vector<uint32_t> tilesRequestedNew;
        std::vector<uint32_t> tilesToUnmap;
        for (auto& feedbackTexture : m_textures)
        {
            // Unmap tiles
            m_tiledTextureManager->GetTilesToUnmap(feedbackTexture->GetTiledTextureId(), tilesToUnmap);
            if (!tilesToUnmap.empty())
            {
                const auto& tilesCoordinates = m_tiledTextureManager->GetTileCoordinates(feedbackTexture->GetTiledTextureId());
                uint32_t tileToUnmapNum = (uint32_t)tilesToUnmap.size();

                nvrhi::TiledTextureRegion tiledTextureRegion = {};
                tiledTextureRegion.tilesNum = 1;

                nvrhi::TextureTilesMapping textureTilesMapping = {};
                textureTilesMapping.numTextureRegions = tileToUnmapNum;
                std::vector<nvrhi::TiledTextureCoordinate> tiledTextureCoordinates(textureTilesMapping.numTextureRegions);
                std::vector<nvrhi::TiledTextureRegion> tiledTextureRegions(textureTilesMapping.numTextureRegions, tiledTextureRegion);
                textureTilesMapping.tiledTextureCoordinates = tiledTextureCoordinates.data();
                textureTilesMapping.tiledTextureRegions = tiledTextureRegions.data();

                uint32_t tilesProcessedNum = 0;
                for (auto& tileIndex : tilesToUnmap)
                {
                    // Process only unpacked tiles
                    nvrhi::TiledTextureCoordinate& tiledTextureCoordinate = tiledTextureCoordinates[tilesProcessedNum];
                    tiledTextureCoordinate.mipLevel = tilesCoordinates[tileIndex].mipLevel;
                    tiledTextureCoordinate.arrayLevel = 0;
                    tiledTextureCoordinate.x = tilesCoordinates[tileIndex].x;
                    tiledTextureCoordinate.y = tilesCoordinates[tileIndex].y;
                    tiledTextureCoordinate.z = 0;

                    tilesProcessedNum++;
                }

                m_device->updateTextureTileMappings(feedbackTexture->GetReservedTexture(), &textureTilesMapping, 1);

                m_minMipDirtyTextures.insert(feedbackTexture);
            }

            // Collect new tiles to stream in
            m_tiledTextureManager->GetTilesToMap(feedbackTexture->GetTiledTextureId(), tilesRequestedNew);
            if (!tilesRequestedNew.empty())
            {
                FeedbackTextureUpdate update;
                update.texture = feedbackTexture;
                for (auto& tileIndex : tilesRequestedNew)
                {
#if _DEBUG
                    assert(std::find(update.tileIndices.begin(), update.tileIndices.end(), tileIndex) == update.tileIndices.end());
#endif
                    update.tileIndices.push_back(tileIndex);
                }
                results->textures.push_back(update);
            }
        }

        // Defragmentation phase
        if (m_updateConfigThisFrame.defragmentHeaps)
        {
            // Defragment up to 16 tiles per frame
            const uint32_t numTiles = 16;
            m_tiledTextureManager->DefragmentTiles(numTiles);
        }

        m_timerBeginFrame.End();
    }

    void FeedbackManagerImpl::UpdateTileMappings(nvrhi::ICommandList* commandList, FeedbackTextureCollection* tilesReady)
    {
        m_timerUpdateTileMappings.Begin();

        for (auto& texUpdate : tilesReady->textures)
        {
            FeedbackTextureImpl* texture = dynamic_cast<FeedbackTextureImpl*>(texUpdate.texture);
            m_minMipDirtyTextures.insert(texture);

            uint32_t tiledTextureId = texture->GetTiledTextureId();
            m_tiledTextureManager->UpdateTilesMapping(tiledTextureId, texUpdate.tileIndices);

            const auto& tilesCoordinates = m_tiledTextureManager->GetTileCoordinates(tiledTextureId);
            const auto& tilesAllocations = m_tiledTextureManager->GetTileAllocations(tiledTextureId);

            std::map<nvrhi::HeapHandle, std::vector<uint32_t>> heapTilesMapping;
            for (auto tileIndex : texUpdate.tileIndices)
            {
                nvrhi::HeapHandle heap = m_heapAllocator->GetHeapHandle(tilesAllocations[tileIndex].heapId);
                if (heapTilesMapping.find(heap) == heapTilesMapping.end())
                    heapTilesMapping[heap] = std::vector<uint32_t>();
                heapTilesMapping[heap].push_back(tileIndex);
            }

            // Now loop heaps
            for (auto& pair : heapTilesMapping)
            {
                nvrhi::HeapHandle heap = pair.first;
                auto& heapTiles = pair.second;
                uint32_t numTiles = (uint32_t)heapTiles.size();

                std::vector<nvrhi::TiledTextureCoordinate> tiledTextureCoordinates;
                std::vector<nvrhi::TiledTextureRegion> tiledTextureRegions;
                std::vector<uint64_t> byteOffsets;

                for (UINT i = 0; i < numTiles; i++)
                {
                    uint32_t tileIndex = heapTiles[i];

                    nvrhi::TiledTextureCoordinate tiledTextureCoordinate = {};
                    tiledTextureCoordinate.mipLevel = tilesCoordinates[tileIndex].mipLevel;
                    tiledTextureCoordinate.x = tilesCoordinates[tileIndex].x;
                    tiledTextureCoordinate.y = tilesCoordinates[tileIndex].y;
                    tiledTextureCoordinate.z = 0;
                    tiledTextureCoordinates.push_back(tiledTextureCoordinate);

                    nvrhi::TiledTextureRegion tiledTextureRegion = {};
                    tiledTextureRegion.tilesNum = 1;
                    tiledTextureRegions.push_back(tiledTextureRegion);

                    byteOffsets.push_back(tilesAllocations[tileIndex].heapTileIndex * D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES);
                }

                nvrhi::TextureTilesMapping textureTilesMapping = {};
                textureTilesMapping.numTextureRegions = (uint32_t)tiledTextureCoordinates.size();
                textureTilesMapping.tiledTextureCoordinates = tiledTextureCoordinates.data();
                textureTilesMapping.tiledTextureRegions = tiledTextureRegions.data();
                textureTilesMapping.byteOffsets = byteOffsets.data();
                textureTilesMapping.heap = heap;

                m_device->updateTextureTileMappings(texture->GetReservedTexture(), &textureTilesMapping, 1);
            }
        }

        if (!m_minMipDirtyTextures.empty())
        {
            const bool useAutomaticBarriers = false;
            commandList->setEnableAutomaticBarriers(useAutomaticBarriers);
            if (!useAutomaticBarriers)
            {
                for (auto& feedbackTexture : m_minMipDirtyTextures)
                    commandList->setTextureState(feedbackTexture->GetMinMipTexture(), nvrhi::AllSubresources, nvrhi::ResourceStates::CopyDest);
            }

            std::vector<uint8_t> minMipData(4096);
            std::vector<uint8_t> uploadData(4096 * 4);
            for (auto& texture : m_minMipDirtyTextures)
            {
                m_tiledTextureManager->WriteMinMipData(texture->GetTiledTextureId(), minMipData.data());
                rtxts::TextureDesc desc = m_tiledTextureManager->GetTextureDesc(texture->GetTiledTextureId(), rtxts::TextureTypes::eMinMipTexture);
                uint32_t rowPitch = (desc.textureOrMipRegionWidth * sizeof(float) + 0xFF) & ~0xFF;

                uint8_t* pUploadData = uploadData.data();
                for (uint32_t y = 0; y < desc.textureOrMipRegionHeight; ++y)
                {
                    float* pDataFloat = reinterpret_cast<float*>(pUploadData);
                    for (uint32_t x = 0; x < desc.textureOrMipRegionWidth; ++x)
                        pDataFloat[x] = minMipData[y * desc.textureOrMipRegionWidth + x];

                    pUploadData += rowPitch;
                }

                commandList->writeTexture(texture->GetMinMipTexture(), 0, 0, uploadData.data(), rowPitch);
            }

            if (!useAutomaticBarriers)
            {
                for (auto& feedbackTexture : m_minMipDirtyTextures)
                    commandList->setTextureState(feedbackTexture->GetMinMipTexture(), nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
            }

            m_minMipDirtyTextures.clear();

            // Restore the automatic barriers mode
            commandList->setEnableAutomaticBarriers(true);
        }

        m_timerUpdateTileMappings.End();
    }

    void FeedbackManagerImpl::ResolveFeedback(nvrhi::ICommandList* commandList)
    {
        auto& readbackTextures = m_texturesToReadback[m_frameIndex];
        if (readbackTextures.empty())
        {
            m_timerResolve.Clear();
            return;
        }

        m_timerResolve.Begin();

        const bool useAutomaticBarriers = false;
        commandList->setEnableAutomaticBarriers(useAutomaticBarriers);

        if (!useAutomaticBarriers)
        {
            for (auto& feedbackTexture : readbackTextures)
                commandList->setSamplerFeedbackTextureState(feedbackTexture->GetSamplerFeedbackTexture(), nvrhi::ResourceStates::ResolveSource);
        }

        {
            uint32_t textureNum = uint32_t(readbackTextures.size());
            for (uint32_t i = 0; i < textureNum; ++i)
                commandList->decodeSamplerFeedbackTexture(readbackTextures[i]->GetFeedbackResolveBuffer(m_frameIndex), readbackTextures[i]->GetSamplerFeedbackTexture(), nvrhi::Format::R8_UINT);
        }

        if (!useAutomaticBarriers)
        {
            for (auto& feedbackTexture : readbackTextures)
                commandList->setSamplerFeedbackTextureState(feedbackTexture->GetSamplerFeedbackTexture(), nvrhi::ResourceStates::UnorderedAccess);
        }

        // Restore the automatic barriers mode
        commandList->setEnableAutomaticBarriers(true);

        m_timerResolve.End();
    }

    void FeedbackManagerImpl::EndFrame()
    {
        // Cycle textures which were updated in this frame to the back of the ringbuffer
        if (m_texturesRingbuffer.size() > 0 && m_updateConfigThisFrame.maxTexturesToUpdate > 0)
        {
            uint32_t numTexturesToUpdate = m_updateConfigThisFrame.maxTexturesToUpdate;
            for (uint32_t i = 0; i < numTexturesToUpdate; i++)
            {
                FeedbackTextureImpl* tex = m_texturesRingbuffer.front();
                m_texturesRingbuffer.pop_front();
                m_texturesRingbuffer.push_back(tex);
            }
        }

        // Save stats
        m_statsLastFrame.heapAllocationInBytes = m_heapAllocator->GetTotalAllocatedBytes();

        m_statsLastFrame.cputimeBeginFrame = m_timerBeginFrame.GetTime();
        m_statsLastFrame.cputimeUpdateTileMappings = m_timerUpdateTileMappings.GetTime();
        m_statsLastFrame.cputimeResolve = m_timerResolve.GetTime();

        {
            rtxts::Statistics statistics = m_tiledTextureManager->GetStatistics();
            m_statsLastFrame.tilesAllocated = statistics.allocatedTilesNum;
            m_statsLastFrame.tilesTotal = statistics.totalTilesNum;
            m_statsLastFrame.heapTilesFree = statistics.heapFreeTilesNum;
            m_statsLastFrame.tilesStandby = statistics.standbyTilesNum;
        }
    }

    FeedbackManagerStats FeedbackManagerImpl::GetStats()
    {
        return m_statsLastFrame;
    }

    // CreateFeedbackManager
    FeedbackManager* CreateFeedbackManager(nvrhi::IDevice* device, const FeedbackManagerDesc& desc)
    {
        return new FeedbackManagerImpl(device, desc);
    }

    HeapAllocator::HeapAllocator(nvrhi::IDevice* device, uint64_t heapSizeInBytes, uint32_t framesInFlight)
        : m_device(device)
        , m_framesInFlight(framesInFlight)
        , m_heapSizeInBytes(heapSizeInBytes)
        , m_totalAllocatedBytes(0)
        , m_numHeaps(0)
    {
    }

    void HeapAllocator::AllocateHeap(uint32_t& heapId)
    {
        nvrhi::HeapDesc heapDesc = {};
        heapDesc.capacity = m_heapSizeInBytes;
        heapDesc.type = nvrhi::HeapType::DeviceLocal;

        // TODO: Calling createHeap should ideally be called asynchronously to offload the critical path
        nvrhi::HeapHandle heap = m_device->createHeap(heapDesc);

        nvrhi::BufferDesc bufferDesc = {};
        bufferDesc.byteSize = m_heapSizeInBytes;
        bufferDesc.isVirtual = true;
        bufferDesc.initialState = nvrhi::ResourceStates::CopySource;
        bufferDesc.keepInitialState = true;
        nvrhi::BufferHandle buffer = m_device->createBuffer(bufferDesc);

        m_device->bindBufferMemory(buffer, heap, 0);

        if (m_freeHeapIds.empty())
        {
            heapId = (uint32_t)m_heaps.size();
            m_heaps.push_back(heap);
            m_buffers.push_back(buffer);
        }
        else
        {
            heapId = m_freeHeapIds.back();
            m_freeHeapIds.pop_back();
            m_heaps[heapId] = heap;
            m_buffers[heapId] = buffer;
        }

        m_totalAllocatedBytes += m_heapSizeInBytes;
        m_numHeaps++;
    }

    void HeapAllocator::ReleaseHeap(uint32_t heapId, uint32_t frameIndex)
    {
        m_freeHeapIds.push_back(heapId);

        uint32_t framesBucket = frameIndex % m_framesInFlight;
        m_buffersToRelease[framesBucket].push_back(m_buffers[heapId]);
        m_heapsToRelease[framesBucket].push_back(m_heaps[heapId]);

        m_heaps[heapId] = nullptr;
        m_buffers[heapId] = nullptr;

        m_totalAllocatedBytes -= m_heapSizeInBytes;
        m_numHeaps--;
    }
}
