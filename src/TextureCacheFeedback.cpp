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

#include "TextureCacheFeedback.h"

#include <donut/engine/CommonRenderPasses.h>
#include <donut/engine/ConsoleObjects.h>
#include <donut/engine/DDSFile.h>
#include <donut/core/vfs/VFS.h>
#include <donut/core/log.h>

#ifdef DONUT_WITH_TASKFLOW
#include <taskflow/taskflow.hpp>
#endif

#include <algorithm>
#include <chrono>
#include <regex>

using namespace donut;
using namespace donut::math;
using namespace donut::vfs;
using namespace donut::engine;

TextureCacheFeedback::TextureCacheFeedback(
    nvrhi::IDevice* device,
    std::shared_ptr<IFileSystem> fs,
    std::shared_ptr<DescriptorTableManager> descriptorTable)
    : TextureCache(device, fs, descriptorTable)
{
}

TextureCacheFeedback::~TextureCacheFeedback()
{
}

std::shared_ptr<LoadedTexture> TextureCacheFeedback::LoadTextureFromFile(
    const std::filesystem::path& path,
    bool sRGB,
    CommonRenderPasses* passes,
    nvrhi::ICommandList* commandList)
{
    // TODO: Implement without finalization
    return TextureCache::LoadTextureFromFile(path, sRGB, passes, commandList);
}

std::shared_ptr<LoadedTexture> TextureCacheFeedback::LoadTextureFromFileDeferred(
    const std::filesystem::path& path,
    bool sRGB)
{
    // TODO: Implement without finalization
    return TextureCache::LoadTextureFromFileDeferred(path, sRGB);
}

#ifdef DONUT_WITH_TASKFLOW
std::shared_ptr<LoadedTexture> TextureCacheFeedback::LoadTextureFromFileAsync(
    const std::filesystem::path& path,
    bool sRGB,
    tf::Executor& executor)
{
    std::shared_ptr<TextureData> texture;

    if (FindTextureInCache(path, texture))
        return texture;

    texture->forceSRGB = sRGB;
    texture->path = path.generic_string();

    executor.async([this, texture, path]()
        {
            auto fileData = ReadTextureFile(path);
            if (fileData)
            {
                if (FillTextureData(fileData, texture, path.extension().generic_string(), ""))
                {
                    TextureLoaded(texture);
                }
            }

            ++m_TexturesLoaded;
        });

    return texture;
}

#endif

