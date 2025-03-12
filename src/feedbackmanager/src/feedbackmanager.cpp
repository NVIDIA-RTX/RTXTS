/*
 * Copyright (c) 2025, NVIDIA CORPORATION.  All rights reserved.
 *
 * NVIDIA CORPORATION and its licensors retain all intellectual property
 * and proprietary rights in and to this software, related documentation
 * and any modifications thereto.  Any use, reproduction, disclosure or
 * distribution of this software and related documentation without an express
 * license agreement from NVIDIA CORPORATION is strictly prohibited.
 */

#include "../include/feedbackmanager.h"
#include "feedbackmanager_internal.h"

#include <map>

namespace nvfeedback
{
    FeedbackManagerImpl::FeedbackManagerImpl(nvrhi::IDevice* device, const FeedbackManagerDesc& desc) :
        m_device(device),
        m_desc(desc),
        m_numFramesInFlight(desc.numFramesInFlight),
        m_frameIndex(0),
        m_statsTilesRequested(0),
        m_statsTilesIdle(0),
        m_statsLastFrame()
    {
        m_texturesToReadback.resize(m_numFramesInFlight);
        ZeroMemory(&m_statsLastFrame, sizeof(FeedbackManagerStats));

        {
            nvrhi::BufferDesc bufferDesc = {};
            bufferDesc.byteSize = D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
            bufferDesc.initialState = nvrhi::ResourceStates::CopyDest;
            bufferDesc.keepInitialState = true;
            m_defragBuffer = m_device->createBuffer(bufferDesc);
        }

        m_heapAllocator = std::make_shared<HeapAllocatorImpl>(m_device);

        rtxts::TiledTextureManagerDesc tiledTextureManagerDesc = {};
        tiledTextureManagerDesc.pHeapAllocator = m_heapAllocator.get();
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

        uint32_t tiledTextureId = feedbackTexture->GetTiledTextureId();
        if (tiledTextureId)
        {
            m_feedbackTexturesMapping[tiledTextureId] = feedbackTexture;
        }

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

    void FeedbackManagerImpl::BeginFrame(nvrhi::ICommandList* commandList, const FeedbackUpdateConfig& config, FeedbackTextureCollection* results)
    {
        // Instrumentation
        m_cputimeDxUpdateTileMappings = 0;
        m_numUpdateTileMappingsCalls = 0;
        m_timerBeginFrame.Begin();

        m_frameIndex = config.frameIndex % m_numFramesInFlight;

        m_updateConfigThisFrame = config;

        m_statsTilesRequested = 0;
        m_statsTilesIdle = 0;

        auto& readbackTextures = m_texturesToReadback[m_frameIndex];
        if (!readbackTextures.empty())
        {
            float timeStamp = float(GetTickCount64()) / 1000.0f;
            uint32_t texturesNum = uint32_t(readbackTextures.size());
            for (uint32_t i = 0; i < texturesNum; ++i)
            {
                uint8_t* pReadbackData = (uint8_t*)m_device->mapBuffer(readbackTextures[i]->GetFeedbackResolveBuffer(m_frameIndex), nvrhi::CpuAccessMode::Read);

                rtxts::SamplerFeedbackDesc samplerFeedbackDesc = {};
                samplerFeedbackDesc.pMinMipData = (uint8_t*)(pReadbackData);
                m_tiledTextureManager->UpdateWithSamplerFeedback(readbackTextures[i]->GetTiledTextureId(), samplerFeedbackDesc, timeStamp, m_updateConfigThisFrame.tileTimeoutSeconds);

                std::vector<rtxts::TileType> tilesToUnmap;
                m_tiledTextureManager->GetTilesToUnmap(readbackTextures[i]->GetTiledTextureId(), tilesToUnmap);
                if (!tilesToUnmap.empty())
                {
                    const auto& tilesCoordinates = m_tiledTextureManager->GetTileCoordinates(readbackTextures[i]->GetTiledTextureId());
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

                    m_device->updateTextureTileMappings(readbackTextures[i]->GetReservedTexture(), &textureTilesMapping, 1);

                    m_minMipDirtyTextures.insert(readbackTextures[i]);
                }

                m_device->unmapBuffer(readbackTextures[i]->GetFeedbackResolveBuffer(m_frameIndex));
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

        for (auto& feedbackTexture : m_textures)
        {
            std::vector<rtxts::TileType> tilesRequestedNew;
            m_tiledTextureManager->GetTilesToMap(feedbackTexture->GetTiledTextureId(), tilesRequestedNew);

            if (!tilesRequestedNew.empty())
            {
                const nvrhi::TextureDesc textureDesc = feedbackTexture->GetReservedTexture()->getDesc();
                bool isBlockCompressed = (textureDesc.format >= nvrhi::Format::BC1_UNORM && textureDesc.format <= nvrhi::Format::BC7_UNORM_SRGB);

                auto& tileShape = feedbackTexture->GetTileShape();
                auto& packedMipInfo = feedbackTexture->GetPackedMipInfo();

                uint32_t firstPackedTileIndex = packedMipInfo.startTileIndexInOverallResource;
                bool seenPackedTiles = false;
                FeedbackTextureUpdate update;
                update.texture = feedbackTexture;
                for (auto& tileIndex : tilesRequestedNew)
                {
                    if (!seenPackedTiles && tileIndex >= firstPackedTileIndex)
                    {
                        uint32_t currentTileIndex = firstPackedTileIndex;
                        uint32_t processedTilesNum = 0;
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

                            FeedbackTextureTile tile;
                            tile.x = 0;
                            tile.y = 0;
                            tile.mip = mip;
                            tile.width = width;
                            tile.height = height;
                            tile.isPacked = true;
                            update.tiles.push_back(tile);
                            update.tileIndices.push_back(currentTileIndex); // Sequentially assign tile indices from packed mip levels(the exact mapping is undefined)

                            if (++processedTilesNum < packedMipInfo.numTilesForPackedMips)
                                currentTileIndex++;
                        }
                        seenPackedTiles = true;
                    }
                    else if (tileIndex < firstPackedTileIndex)
                    {
                        const auto& tileCoord = m_tiledTextureManager->GetTileCoordinates(feedbackTexture->GetTiledTextureId());
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

                        FeedbackTextureTile tile;
                        tile.x = x;
                        tile.y = y;
                        tile.mip = mip;
                        tile.width = width;
                        tile.height = height;
                        tile.isPacked = false;
                        update.tiles.push_back(tile);
                        update.tileIndices.push_back(tileIndex);
                    }
                }
                results->textures.push_back(update);
            }
        }

        // Defragmentation phase
        m_pDefragTexture = nullptr;
        if (m_updateConfigThisFrame.defragmentHeaps)
        {
            rtxts::TileAllocation prevTileAllocation;
            rtxts::TileAllocationInHeap tileAllocation = m_tiledTextureManager->GetFragmentedTextureTile(prevTileAllocation);
            if (tileAllocation.textureId)
            {
                m_pDefragTexture = m_feedbackTexturesMapping[tileAllocation.textureId];
                m_defragTile = tileAllocation.textureTileIndex;
            }

            if (m_pDefragTexture)
            {
                // Save the tile into m_defragBuffer
                commandList->copyBuffer(m_defragBuffer, 0, m_heapAllocator->GetBufferHandle(prevTileAllocation.heapId), prevTileAllocation.heapTileIndex * D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES, D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES);
            }
        }

        m_timerBeginFrame.End();
    }

    void FeedbackManagerImpl::UpdateTileMappings(nvrhi::ICommandList* commandList, FeedbackTextureCollection* tilesReady)
    {
        m_timerUpdateTileMappings.Begin();

        // Copy tilesReady because we might modify it for defragmentation
        FeedbackTextureCollection tilesToMap = *tilesReady;

        if (m_pDefragTexture)
        {
            // Insert the tile being defragmented into the list of tiles to be mapped
            auto it = std::find_if(tilesToMap.textures.begin(), tilesToMap.textures.end(),
                [&](const FeedbackTextureUpdate& u) { return u.texture == m_pDefragTexture; });
            if (it != tilesToMap.textures.end())
            {
                FeedbackTextureUpdate& update = *it;
                if (std::find(update.tileIndices.begin(), update.tileIndices.end(), m_defragTile) == update.tileIndices.end())
                {
                    update.tileIndices.push_back(m_defragTile);
                }
            }
            else
            {
                FeedbackTextureUpdate feedbackTextureUpdate;
                feedbackTextureUpdate.texture = m_pDefragTexture;
                feedbackTextureUpdate.tileIndices.push_back(m_defragTile);
                tilesToMap.textures.push_back(feedbackTextureUpdate);
            }
        }

        for (auto& texUpdate : tilesToMap.textures)
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

        if (m_pDefragTexture)
        {
            auto& alloc = m_tiledTextureManager->GetTileAllocations(m_pDefragTexture->GetTiledTextureId())[m_defragTile];
            // Restore the tile from m_defragBuffer
            commandList->copyBuffer(m_heapAllocator->GetBufferHandle(alloc.heapId), alloc.heapTileIndex * D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES, m_defragBuffer, 0, D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES);
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
        m_timerResolve.Begin();

        auto& readbackTextures = m_texturesToReadback[m_frameIndex];
        if (readbackTextures.empty())
        {
            m_timerApiResolve.Clear();
            return;
        }

        const bool useAutomaticBarriers = false;
        commandList->setEnableAutomaticBarriers(useAutomaticBarriers);

        if (!useAutomaticBarriers)
        {
            for (auto& feedbackTexture : readbackTextures)
                commandList->setSamplerFeedbackTextureState(feedbackTexture->GetSamplerFeedbackTexture(), nvrhi::ResourceStates::ResolveSource);
        }

        m_timerApiResolve.Begin();
        {
            uint32_t textureNum = uint32_t(readbackTextures.size());
            for (uint32_t i = 0; i < textureNum; ++i)
                commandList->decodeSamplerFeedbackTexture(readbackTextures[i]->GetFeedbackResolveBuffer(m_frameIndex), readbackTextures[i]->GetSamplerFeedbackTexture(), nvrhi::Format::R8_UINT);
        }
        m_timerApiResolve.End();

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
        if (m_updateConfigThisFrame.releaseEmptyHeaps)
        {
            m_heapAllocator->ReleaseEmptyHeaps();
        }

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
        m_statsLastFrame.tilesRequested = m_statsTilesRequested;
        m_statsLastFrame.tilesIdle = m_statsTilesIdle;

        m_statsLastFrame.heapAllocationInBytes = m_heapAllocator->GetTotalAllocatedBytes();

        m_statsLastFrame.cputimeBeginFrame = m_timerBeginFrame.GetTime();
        m_statsLastFrame.cputimeUpdateTileMappings = m_timerUpdateTileMappings.GetTime();
        m_statsLastFrame.cputimeDxUpdateTileMappings = m_cputimeDxUpdateTileMappings;
        m_statsLastFrame.cputimeResolve = m_timerResolve.GetTime();
        m_statsLastFrame.cputimeDxResolve = m_timerApiResolve.GetTime();

        m_statsLastFrame.numUpdateTileMappingsCalls = m_numUpdateTileMappingsCalls;

        {
            rtxts::Statistics statistics = m_tiledTextureManager->GetStatistics();
            m_statsLastFrame.tilesAllocated = statistics.allocatedTilesNum;
            m_statsLastFrame.tilesTotal = statistics.totalTilesNum;
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

    HeapAllocatorImpl::HeapAllocatorImpl(nvrhi::IDevice* device)
        : m_device(device)
        , m_totalAllocatedBytes(0)
    {
    }

    void HeapAllocatorImpl::AllocateHeap(uint64_t heapSizeInBytes, uint32_t& heapId)
    {
        if (m_freeHeapIndices.empty())
        {
            m_freeHeapIndices.push_back((uint32_t)m_heaps.size());
            m_heaps.push_back(nullptr);
            m_buffers.push_back(nullptr);
        }

        uint32_t heapIndex = m_freeHeapIndices.back();
        if (!m_heaps[heapIndex])
        {
            nvrhi::HeapDesc heapDesc = {};
            heapDesc.capacity = heapSizeInBytes;
            heapDesc.type = nvrhi::HeapType::DeviceLocal;
            m_heaps[heapIndex] = m_device->createHeap(heapDesc);

            nvrhi::BufferDesc bufferDesc = {};
            bufferDesc.byteSize = heapSizeInBytes;
            bufferDesc.isVirtual = true;
            bufferDesc.initialState = nvrhi::ResourceStates::CopySource;
            bufferDesc.keepInitialState = true;
            m_buffers[heapIndex] = m_device->createBuffer(bufferDesc);

            m_totalAllocatedBytes += heapSizeInBytes;

            m_device->bindBufferMemory(m_buffers[heapIndex], m_heaps[heapIndex], 0);
        }

        m_freeHeapIndices.pop_back();

        heapId = heapIndex + 1;
    }

    void HeapAllocatorImpl::ReleaseHeap(uint32_t heapId)
    {
        if (heapId && m_heaps[heapId - 1])
        {
            uint32_t heapIndex = heapId - 1;
            m_freeHeapIndices.push_back(heapIndex);
        }
    }

    void HeapAllocatorImpl::ReleaseEmptyHeaps()
    {
        for (auto heapIndex : m_freeHeapIndices)
        {
            if (m_heaps[heapIndex])
            {
                m_totalAllocatedBytes -= m_buffers[heapIndex]->getDesc().byteSize;

                m_heaps[heapIndex] = nullptr;
                m_buffers[heapIndex] = nullptr;
            }
        }
    }
}
