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

#include <donut/engine/TextureCache.h>

#include <donut/engine/SceneTypes.h>
#include <donut/core/log.h>

#include <nvrhi/nvrhi.h>
#include <atomic>
#include <filesystem>
#include <unordered_map>
#include <memory>
#include <shared_mutex>
#include <queue>

#ifdef DONUT_WITH_TASKFLOW
namespace tf
{
    class Executor;
}
#endif

namespace donut::vfs
{
    class IBlob;
    class IFileSystem;
}

namespace donut::engine
{
    class CommonRenderPasses;
}

class TextureCacheFeedback : public donut::engine::TextureCache
{
public:
    TextureCacheFeedback(
        nvrhi::IDevice* device,
        std::shared_ptr<donut::vfs::IFileSystem> fs,
        std::shared_ptr<donut::engine::DescriptorTableManager> descriptorTable);
    virtual ~TextureCacheFeedback();

    // Synchronous read and decode, synchronous upload and mip generation on a given command list (must be open).
    // The `passes` argument is optional, and mip generation is disabled if it's NULL.
    virtual std::shared_ptr<donut::engine::LoadedTexture> LoadTextureFromFile(
        const std::filesystem::path& path,
        bool sRGB,
        donut::engine::CommonRenderPasses* passes,
        nvrhi::ICommandList* commandList);

    // Synchronous read and decode, deferred upload and mip generation (in the ProcessRenderingThreadCommands queue).
    virtual std::shared_ptr<donut::engine::LoadedTexture> LoadTextureFromFileDeferred(
        const std::filesystem::path& path,
        bool sRGB);

#ifdef DONUT_WITH_TASKFLOW
    // Asynchronous read and decode, deferred upload and mip generation (in the ProcessRenderingThreadCommands queue).
    virtual std::shared_ptr<donut::engine::LoadedTexture> LoadTextureFromFileAsync(
        const std::filesystem::path& path,
        bool sRGB,
        tf::Executor& executor);
#endif
};
