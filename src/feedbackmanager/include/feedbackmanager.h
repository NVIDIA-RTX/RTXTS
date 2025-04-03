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

#include <stdint.h>
#include <vector>
#include <memory>
#include <nvrhi/nvrhi.h>

namespace nvfeedback
{
    struct FeedbackTextureTileInfo
    {
        uint32_t mip;
        uint32_t xInTexels;
        uint32_t yInTexels;
        uint32_t widthInTexels;
        uint32_t heightInTexels;

        bool operator==(const FeedbackTextureTileInfo& b) const
        {
            return mip == b.mip &&
                xInTexels == b.xInTexels &&
                yInTexels == b.yInTexels &&
                widthInTexels == b.widthInTexels &&
                heightInTexels == b.heightInTexels;
        }
    };

    class FeedbackTexture
    {
    public:
        virtual unsigned long AddRef() = 0;
        virtual unsigned long Release() = 0;

        virtual nvrhi::TextureHandle GetReservedTexture() = 0;
        virtual nvrhi::SamplerFeedbackTextureHandle GetSamplerFeedbackTexture() = 0;
        virtual nvrhi::TextureHandle GetMinMipTexture() = 0;
        virtual bool IsTilePacked(uint32_t tileIndex) = 0;
        virtual void GetTileInfo(uint32_t tileIndex, std::vector<FeedbackTextureTileInfo>& tiles) = 0;
    };

    struct FeedbackManagerStats
    {
        uint64_t heapAllocationInBytes; // The amount of heap space allocated in bytes
        uint32_t tilesTotal;            // Total number of tiles tracked in all textures
        uint32_t tilesRequested;        // Number of tiles actively being requested for rendering
        uint32_t tilesAllocated;        // Number of tiles allocated in heaps
        uint32_t tilesIdle;             // Number of tiles no longer being requested but not freed
        uint32_t tilesStandby;          // Number of tiles in the standby queue

        double cputimeBeginFrame;
        double cputimeUpdateTileMappings;
        double cputimeResolve;

        double cputimeDxUpdateTileMappings;
        double cputimeDxResolve;

        uint32_t numUpdateTileMappingsCalls;
    };

    struct FeedbackUpdateConfig
    {
        uint32_t frameIndex; // Current frame index, must fall within the range of 0-numFramesInFlight
        uint32_t maxTexturesToUpdate; // Max textures to update, 0=unlimited
        float tileTimeoutSeconds; // Timeout of tile allocation in seconds
        bool defragmentHeaps; // Enable defragmentation of heaps
        bool releaseEmptyHeaps; // Enable releasing of empty heaps
        uint32_t maxStandbyTiles; // Maximum number of tiles in the standby queue
    };

    struct FeedbackTextureUpdate
    {
        FeedbackTexture* texture;
        std::vector<uint32_t> tileIndices;
    };

    struct FeedbackTextureCollection
    {
        std::vector<FeedbackTextureUpdate> textures;
    };

    struct FeedbackManagerDesc
    {
        uint32_t numFramesInFlight; // Number of frames in flight, affects the latency of readback
        uint32_t heapSizeInTiles; // The size of each heap in tiles
    };

    // FeedbackManager interfaces between application code using NVRHI and the RTXTS library
    class FeedbackManager
    {
    public:
        virtual ~FeedbackManager() {};

        // Creates a FeedbackTexture
        virtual bool CreateTexture(const nvrhi::TextureDesc& desc, FeedbackTexture** ppTex) = 0;

        // Call at the beginning of the frame. Reads back the feedback resources from N frames ago.
        virtual void BeginFrame(nvrhi::ICommandList* commandList, const FeedbackUpdateConfig& config, FeedbackTextureCollection* results) = 0;

        // Call for tiles which ready to have their data filled on this frame's GPU timeline
        virtual void UpdateTileMappings(nvrhi::ICommandList* commandList, FeedbackTextureCollection* tilesReady) = 0;

        // After rendering, resolve the sampler feedback maps
        virtual void ResolveFeedback(nvrhi::ICommandList* commandList) = 0;

        // Small cleanup at the end of the frame
        virtual void EndFrame() = 0;

        // Returns statistics of the operations performed during this frame
        virtual FeedbackManagerStats GetStats() = 0;
    };

    // Creates a FeedbackManager
    FeedbackManager* CreateFeedbackManager(nvrhi::IDevice* device, const FeedbackManagerDesc& desc);
}
