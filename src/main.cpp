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

#include <donut/core/vfs/VFS.h>
#include <donut/core/log.h>
#include <donut/core/string_utils.h>
#include <donut/engine/CommonRenderPasses.h>
#include <donut/engine/ConsoleInterpreter.h>
#include <donut/engine/ConsoleObjects.h>
#include <donut/engine/FramebufferFactory.h>
#include <donut/engine/Scene.h>
#include <donut/engine/ShaderFactory.h>
#include <donut/engine/TextureCache.h>
#include <donut/render/BloomPass.h>
#include <donut/render/CascadedShadowMap.h>
#include <donut/render/DeferredLightingPass.h>
#include <donut/render/DepthPass.h>
#include <donut/render/DrawStrategy.h>
#include <donut/render/ForwardShadingPass.h>
#include <donut/render/GBuffer.h>
#include <donut/render/GBufferFillPass.h>
#include <donut/render/PixelReadbackPass.h>
#include <donut/render/SkyPass.h>
#include <donut/render/SsaoPass.h>
#include <donut/render/TemporalAntiAliasingPass.h>
#include <donut/render/ToneMappingPasses.h>
#include <donut/render/MipMapGenPass.h>
#include <donut/app/ApplicationBase.h>
#include <donut/app/UserInterfaceUtils.h>
#include <donut/app/Camera.h>
#include <donut/app/DeviceManager.h>
#include <donut/app/imgui_console.h>
#include <donut/app/imgui_renderer.h>
#include <nvrhi/utils.h>
#include <nvrhi/common/misc.h>

#ifdef DONUT_WITH_TASKFLOW
#include <taskflow/taskflow.hpp>
#endif

using namespace donut;
using namespace donut::math;
using namespace donut::app;
using namespace donut::vfs;
using namespace donut::engine;
using namespace donut::render;

// NOTE: This is currently required to talk directly to the d3d12 commandlist for requireTextureState and commitBarriers
#include "../external/donut/nvrhi/src/d3d12/d3d12-backend.h"

#include <string>
#include <vector>
#include <memory>
#include <chrono>

#include "GBufferFillPassFeedback.h"
#include "TextureCacheFeedback.h"
#include "../shaders/feedback_cb.h"
#include "Profiler.h"
#include "feedbackmanager/include/feedbackmanager.h"
#include "rtxts-ttm/tiledTextureManager.h"

using namespace nvfeedback;

extern "C"
{
    __declspec(dllexport) extern const UINT D3D12SDKVersion = 614;
    __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";

    __declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
}

// Simple performance timer with max/average values for a number of samples
// Used for CPU profiling
class SimplePerf
{
public:
    SimplePerf() :
        m_maxValue(0.0),
        m_sum(0.0),
        m_maxNumSamples(100)
    {
    }

    void AddSample(double t)
    {
        // Add the new sample
        m_samples.push_back(t);
        m_sum += t;
        m_maxValue = std::max(m_maxValue, t);
        LimitSampleCount();
    }

    double GetMax() const
    {
        return m_maxValue;
    }

    double GetAverage() const
    {
        return m_samples.empty() ? 0.0 : m_sum / (double)m_samples.size();
    }

    void SetMaxNumSamples(uint32_t maxNumSamples)
    {
        m_maxNumSamples = maxNumSamples;
        LimitSampleCount();
    }

private:
    uint32_t m_maxNumSamples;
    std::deque<double> m_samples;
    double m_maxValue;
    double m_sum;

    void LimitSampleCount()
    {
        // Keep only the last m_maxNumSamples samples
        bool newMaxNeeded = false;
        
        while (m_samples.size() > m_maxNumSamples)
        {
            double oldestSample = m_samples.front();
            m_sum -= oldestSample;
            m_samples.pop_front();
            
            // Find new maximum only if we removed the current max value
            if (oldestSample == m_maxValue)
            {
                newMaxNeeded = true;
            }
        }

        if (newMaxNeeded)
        {
            m_maxValue = 0.0;
            for (const double& sample : m_samples)
            {
                m_maxValue = std::max(m_maxValue, sample);
            }
        }
    }
};

// Render targets for the g-buffer pass
class RenderTargets : public GBufferRenderTargets
{
public:
    nvrhi::TextureHandle hdrColor;
    nvrhi::TextureHandle ldrColor;
    nvrhi::TextureHandle materialIDs;
    nvrhi::TextureHandle resolvedColor;
    nvrhi::TextureHandle temporalFeedback1;
    nvrhi::TextureHandle temporalFeedback2;
    nvrhi::TextureHandle ambientOcclusion;

    nvrhi::HeapHandle heap;

    std::shared_ptr<FramebufferFactory> forwardFramebuffer;
    std::shared_ptr<FramebufferFactory> hdrFramebuffer;
    std::shared_ptr<FramebufferFactory> ldrFramebuffer;
    std::shared_ptr<FramebufferFactory> resolvedFramebuffer;
    std::shared_ptr<FramebufferFactory> materialIDFramebuffer;

    void Init(
        nvrhi::IDevice* device,
        dm::uint2 size,
        dm::uint sampleCount,
        bool enableMotionVectors,
        bool useReverseProjection) override
    {
        GBufferRenderTargets::Init(device, size, sampleCount, enableMotionVectors, useReverseProjection);

        nvrhi::TextureDesc desc;
        desc.width = size.x;
        desc.height = size.y;
        desc.isRenderTarget = true;
        desc.useClearValue = true;
        desc.clearValue = nvrhi::Color(1.f);
        desc.sampleCount = sampleCount;
        desc.dimension = sampleCount > 1 ? nvrhi::TextureDimension::Texture2DMS : nvrhi::TextureDimension::Texture2D;
        desc.keepInitialState = true;
        desc.isVirtual = device->queryFeatureSupport(nvrhi::Feature::VirtualResources);

        desc.clearValue = nvrhi::Color(0.f);
        desc.isTypeless = false;
        desc.isUAV = sampleCount == 1;
        desc.format = nvrhi::Format::RGBA16_FLOAT;
        desc.initialState = nvrhi::ResourceStates::RenderTarget;
        desc.debugName = "HdrColor";
        hdrColor = device->createTexture(desc);

        desc.format = nvrhi::Format::RG16_UINT;
        desc.isUAV = false;
        desc.debugName = "MaterialIDs";
        materialIDs = device->createTexture(desc);

        // The render targets below this point are non-MSAA
        desc.sampleCount = 1;
        desc.dimension = nvrhi::TextureDimension::Texture2D;

        desc.format = nvrhi::Format::RGBA16_FLOAT;
        desc.isUAV = true;
        desc.mipLevels = uint32_t(floorf(::log2f(float(std::max(desc.width, desc.height)))) + 1.f); // Used to test the MipMapGen pass
        desc.debugName = "ResolvedColor";
        resolvedColor = device->createTexture(desc);

        desc.format = nvrhi::Format::RGBA16_SNORM;
        desc.mipLevels = 1;
        desc.debugName = "TemporalFeedback1";
        temporalFeedback1 = device->createTexture(desc);
        desc.debugName = "TemporalFeedback2";
        temporalFeedback2 = device->createTexture(desc);

        desc.format = nvrhi::Format::SRGBA8_UNORM;
        desc.isUAV = false;
        desc.debugName = "LdrColor";
        ldrColor = device->createTexture(desc);

        desc.format = nvrhi::Format::R8_UNORM;
        desc.isUAV = true;
        desc.debugName = "AmbientOcclusion";
        ambientOcclusion = device->createTexture(desc);

        if (desc.isVirtual)
        {
            uint64_t heapSize = 0;
            nvrhi::ITexture* const textures[] = {
                hdrColor,
                materialIDs,
                resolvedColor,
                temporalFeedback1,
                temporalFeedback2,
                ldrColor,
                ambientOcclusion
            };

            for (auto texture : textures)
            {
                nvrhi::MemoryRequirements memReq = device->getTextureMemoryRequirements(texture);
                heapSize = nvrhi::align(heapSize, memReq.alignment);
                heapSize += memReq.size;
            }

            nvrhi::HeapDesc heapDesc;
            heapDesc.type = nvrhi::HeapType::DeviceLocal;
            heapDesc.capacity = heapSize;
            heapDesc.debugName = "RenderTargetHeap";

            heap = device->createHeap(heapDesc);

            uint64_t offset = 0;
            for (auto texture : textures)
            {
                nvrhi::MemoryRequirements memReq = device->getTextureMemoryRequirements(texture);
                offset = nvrhi::align(offset, memReq.alignment);

                device->bindTextureMemory(texture, heap, offset);

                offset += memReq.size;
            }
        }

        forwardFramebuffer = std::make_shared<FramebufferFactory>(device);
        forwardFramebuffer->RenderTargets = { hdrColor };
        forwardFramebuffer->DepthTarget = Depth;

        hdrFramebuffer = std::make_shared<FramebufferFactory>(device);
        hdrFramebuffer->RenderTargets = { hdrColor };

        ldrFramebuffer = std::make_shared<FramebufferFactory>(device);
        ldrFramebuffer->RenderTargets = { ldrColor };

        resolvedFramebuffer = std::make_shared<FramebufferFactory>(device);
        resolvedFramebuffer->RenderTargets = { resolvedColor };

        materialIDFramebuffer = std::make_shared<FramebufferFactory>(device);
        materialIDFramebuffer->RenderTargets = { materialIDs };
        materialIDFramebuffer->DepthTarget = Depth;
    }

    [[nodiscard]] bool IsUpdateRequired(uint2 size, uint sampleCount) const
    {
        if (any(m_Size != size) || m_SampleCount != sampleCount)
            return true;

        return false;
    }

    void Clear(nvrhi::ICommandList* commandList) override
    {
        GBufferRenderTargets::Clear(commandList);

        commandList->clearTextureFloat(hdrColor, nvrhi::AllSubresources, nvrhi::Color(0.f));
    }
};

enum class AntiAliasingMode
{
    NONE,
    TEMPORAL
};

struct UIData
{
    bool                                showUI = true;
    bool                                showConsole = false;
    bool                                enableSsao = true;
    SsaoParameters                      ssaoParams;
    ToneMappingParameters               toneMappingParams;
    TemporalAntiAliasingParameters      temporalAntiAliasingParams;
    SkyParameters                       skyParams;
    enum AntiAliasingMode               antiAliasingMode = AntiAliasingMode::NONE;
    enum TemporalAntiAliasingJitter     temporalAntiAliasingJitter = TemporalAntiAliasingJitter::Halton;
    bool                                enableVsync = false;
    bool                                shaderReloadRequested = false;
    bool                                enableProceduralSky = true;
    bool                                enableBloom = true;
    float                               bloomSigma = 32.f;
    float                               bloomAlpha = 0.05f;
    bool                                enableTranslucency = true;
    bool                                enableMaterialEvents = false;
    bool                                enableShadows = true;
    float                               ambientIntensity = 1.0f;
    float                               csmExponent = 4.f;
    bool                                enableAnimations = false;
    std::shared_ptr<Material>           selectedMaterial;
    std::shared_ptr<SceneGraphNode>     selectedNode;
    std::string                         screenshotFileName;
    std::shared_ptr<SceneCamera>        activeSceneCamera;
    bool                                writeFeedback = true;
    bool                                useTextureSets = true;
    bool                                compactMemory = false;
    bool                                showUnmappedRegions = false;
    bool                                enableStochasticFeedback = false;
    float                               feedbackProbabilityThreshold = 0.005f;
    bool                                enableDebug = false;
    int                                 texturesPerFrame = 10;
    int                                 tilesPerFrame = 256;
    float                               tileTimeout = 1.0f;
    int                                 numExtraStandbyTiles = 2000;
};

// Helper class for uploading tiles to the GPU
class TileUploadHelper
{
public:
    TileUploadHelper(nvrhi::IDevice* device, uint32_t maxTiles, uint32_t framesInFlight)
        : m_device(device)
        , m_maxTiles(maxTiles)
    {
        m_framesInFlight = framesInFlight;
        m_tileCount.resize(m_framesInFlight, 0);
        m_uploadBuffers.resize(m_framesInFlight);

        for (uint32_t i = 0; i < m_framesInFlight; ++i)
        {
            nvrhi::BufferDesc bufferDesc = {};
            bufferDesc.byteSize = maxTiles * D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
            bufferDesc.debugName = "TileDataUploadBuffer";
            bufferDesc.keepInitialState = true;
            bufferDesc.cpuAccess = nvrhi::CpuAccessMode::Write;

            m_uploadBuffers[i] = device->createBuffer(bufferDesc);
        }
    }

    void BeginFrame(uint32_t frameIndex)
    {
        m_frameIndex = frameIndex % m_framesInFlight;
        m_tileCount[m_frameIndex] = 0;
    }

    uint32_t NumTilesMax() const
    {
        return m_maxTiles;
    }

    bool UploadTile(ID3D12GraphicsCommandList* commandList, ID3D12Resource* destTexture, nvfeedback::FeedbackTextureTileInfo tile, const char* dataMipBase, nvrhi::TileShape tileShape, uint32_t rowPitchSource)
    {
        uint32_t& tileCount = m_tileCount[m_frameIndex];
        if (tileCount >= m_maxTiles)
            return false;

        uint32_t bufferOffset = tileCount * D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
        ++tileCount;

        uint8_t* mappedData = (uint8_t*)m_device->mapBuffer(m_uploadBuffers[m_frameIndex], nvrhi::CpuAccessMode::Write);
        mappedData += bufferOffset;

        // Compute pitches and offsets in 4x4 blocks
        // Note: The "tile" being copied here might be smaller than a tiled resource tile, for example non-pow2 textures
        uint32_t tileBlocksWidth = tile.widthInTexels / 4;
        uint32_t tileBlocksHeight = tile.heightInTexels / 4;
        uint32_t shapeBlocksWidth = tileShape.widthInTexels / 4;
        uint32_t shapeBlocksHeight = tileShape.heightInTexels / 4;
        uint32_t bytesPerBlock = D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES / (shapeBlocksWidth * shapeBlocksHeight);
        uint32_t sourceBlockX = tile.xInTexels / 4;
        uint32_t sourceBlockY = tile.yInTexels / 4;
        uint32_t rowPitchTile = tileBlocksWidth * bytesPerBlock;
        for (uint32_t blockRow = 0; blockRow < tileBlocksHeight; blockRow++)
        {
            uint32_t readOffset = (sourceBlockY + blockRow) * rowPitchSource + sourceBlockX * bytesPerBlock;
            uint32_t writeOffset = blockRow * rowPitchTile;
            memcpy(mappedData + writeOffset, dataMipBase + readOffset, rowPitchTile);
        }

        m_device->unmapBuffer(m_uploadBuffers[m_frameIndex]);

        D3D12_TEXTURE_COPY_LOCATION srcLocation = {};
        srcLocation.pResource = m_uploadBuffers[m_frameIndex]->getNativeObject(nvrhi::ObjectTypes::D3D12_Resource);
        srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcLocation.PlacedFootprint.Offset = bufferOffset;
        srcLocation.PlacedFootprint.Footprint.Format = destTexture->GetDesc().Format;
        srcLocation.PlacedFootprint.Footprint.Width = tile.widthInTexels;
        srcLocation.PlacedFootprint.Footprint.Height = tile.heightInTexels;
        srcLocation.PlacedFootprint.Footprint.Depth = 1;
        srcLocation.PlacedFootprint.Footprint.RowPitch = rowPitchTile;

        D3D12_TEXTURE_COPY_LOCATION dstLocation = {};
        dstLocation.pResource = destTexture;
        dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dstLocation.SubresourceIndex = tile.mip;

        D3D12_BOX sourceBox = {};
        sourceBox.left = 0;
        sourceBox.top = 0;
        sourceBox.front = 0;
        sourceBox.right = tile.widthInTexels;
        sourceBox.bottom = tile.heightInTexels;
        sourceBox.back = 1;

        commandList->CopyTextureRegion(&dstLocation, tile.xInTexels, tile.yInTexels, 0, &srcLocation, &sourceBox);

        return true;
    }

private:
    nvrhi::IDevice* m_device;
    uint32_t m_maxTiles;

    std::vector<nvrhi::BufferHandle> m_uploadBuffers;
    std::vector<uint32_t> m_tileCount;
    uint32_t m_framesInFlight = 0;
    uint32_t m_frameIndex = -1;
};

struct RequestedTile
{
    nvfeedback::FeedbackTexture* texture;
    uint32_t tileIndex;
};

// Main application class
class SampleApp : public ApplicationBase
{
public:
    typedef ApplicationBase Super;

    std::shared_ptr<RootFileSystem>     m_rootFileSystem;
    std::vector<std::string>            m_sceneFilesAvailable;
    std::string                         m_currentSceneName;
    std::shared_ptr<Scene>              m_scene;
    std::shared_ptr<ShaderFactory>      m_shaderFactory;
    std::shared_ptr<DirectionalLight>   m_sunLight;
    std::shared_ptr<CascadedShadowMap>  m_shadowMap;
    std::shared_ptr<FramebufferFactory> m_depthFramebuffer;
    std::shared_ptr<DepthPass>          m_depthPass;
    std::shared_ptr<FramebufferFactory> m_shadowFramebuffer;
    std::shared_ptr<DepthPass>          m_shadowDepthPass;
    std::shared_ptr<InstancedOpaqueDrawStrategy> m_opaqueDrawStrategy;
    std::shared_ptr<TransparentDrawStrategy> m_transparentDrawStrategy;
    std::unique_ptr<RenderTargets>      m_renderTargets;
    std::shared_ptr<ForwardShadingPass> m_forwardPass;
    std::unique_ptr<GBufferFillPassFeedback> m_gBufferPass;
    std::unique_ptr<GBufferFillPassFeedback> m_gBufferReadDepthPass;
    std::unique_ptr<DeferredLightingPass> m_deferredLightingPass;
    std::unique_ptr<SkyPass>            m_skyPass;
    std::unique_ptr<TemporalAntiAliasingPass> m_temporalAntiAliasingPass;
    std::unique_ptr<BloomPass>          m_bloomPass;
    std::unique_ptr<ToneMappingPass>    m_toneMappingPass;
    std::unique_ptr<SsaoPass>           m_ssaoPass;
    std::unique_ptr<MaterialIDPassFeedback> m_materialIDPass;
    std::unique_ptr<PixelReadbackPass>  m_pixelReadbackPass;

    std::shared_ptr<IView>              m_view;
    std::shared_ptr<IView>              m_viewPrevious;

    nvrhi::CommandListHandle            m_commandList;

    bool                                m_previousViewsValid = false;
    FirstPersonCamera                   m_firstPersonCamera;
    BindingCache                        m_bindingCache;

    float                               m_cameraVerticalFov = 60.f;
    float3                              m_ambientTop = 0.f;
    float3                              m_ambientBottom = 0.f;
    uint2                               m_pickPosition = 0u;
    bool                                m_pick = false;

    float                               m_wallclockTime = 0.f;

    UIData&                             m_ui;

    // Tiled resources & Sampler feedback
    bool m_recreateFeedbackTextures = true;
    bool m_recreateFeedbackTextureSets = true;
    bool m_textureSetsEnabled = false;
    bool m_cameraCut = false;
    std::shared_ptr<FeedbackManager> m_feedbackManager;
    FeedbackTextureMaps m_feedbackTextureMaps;
    std::queue<RequestedTile> m_requestedTiles;
    TileUploadHelper m_tileUploadHelper;

    // Simple perf counters
    SimplePerf m_perfFeedbackBegin;
    SimplePerf m_perfFeedbackUpdateTileMappings;
    SimplePerf m_perfFeedbackResolve;

    // GPU timing
    AveragingTimerQuery m_timerGbuffer;
    AveragingTimerQuery m_timerResolve;

public:

    SampleApp(DeviceManager* deviceManager, UIData& ui, const std::string& sceneName)
        : Super(deviceManager)
        , m_ui(ui)
        , m_bindingCache(deviceManager->GetDevice())
        , m_tileUploadHelper(deviceManager->GetDevice(), m_ui.tilesPerFrame, deviceManager->GetBackBufferCount())
        , m_timerGbuffer(deviceManager->GetDevice())
        , m_timerResolve(deviceManager->GetDevice())
    { 
        std::shared_ptr<NativeFileSystem> nativeFS = std::make_shared<NativeFileSystem>();

        std::filesystem::path mediaPath = app::GetDirectoryWithExecutable().parent_path() / "media";
        std::filesystem::path frameworkShaderPath = app::GetDirectoryWithExecutable() / "shaders/framework" / app::GetShaderTypeName(GetDevice()->getGraphicsAPI());
        std::filesystem::path appShaderPath = app::GetDirectoryWithExecutable() / "shaders/app" / app::GetShaderTypeName(GetDevice()->getGraphicsAPI());

        m_rootFileSystem = std::make_shared<RootFileSystem>();
        m_rootFileSystem->mount("/media", mediaPath);
        m_rootFileSystem->mount("/shaders/app", appShaderPath);
        m_rootFileSystem->mount("/shaders/donut", frameworkShaderPath);
        m_rootFileSystem->mount("/native", nativeFS);

        std::filesystem::path scenePath = "/media";
        std::vector<std::string> sceneFilesAvailable = FindScenes(*m_rootFileSystem, scenePath);

        const std::string mediaExt = ".scene.json";
        for (auto& sceneFileName : sceneFilesAvailable)
        {
            std::string longExt = (sceneFileName.size() <= mediaExt.length()) ? ("") : (sceneFileName.substr(sceneFileName.length() - mediaExt.length()));
            if (longExt == mediaExt)
                m_sceneFilesAvailable.push_back(sceneFileName);
        }

        if (sceneName.empty() && m_sceneFilesAvailable.empty())
        {
            log::fatal("No scene file found in media folder '%s'\n"
                "Please make sure that folder contains valid scene files.", scenePath.generic_string().c_str());
        }

        m_TextureCache = std::make_shared<TextureCacheFeedback>(GetDevice(), m_rootFileSystem, nullptr);

        m_shaderFactory = std::make_shared<ShaderFactory>(GetDevice(), m_rootFileSystem, "/shaders");
        m_CommonPasses = std::make_shared<CommonRenderPasses>(GetDevice(), m_shaderFactory);

        m_opaqueDrawStrategy = std::make_shared<InstancedOpaqueDrawStrategy>();
        m_transparentDrawStrategy = std::make_shared<TransparentDrawStrategy>();

        m_shadowMap = std::make_shared<CascadedShadowMap>(GetDevice(), 2048, 4, 0, nvrhi::Format::D32);
        m_shadowMap->SetupProxyViews();

        m_shadowFramebuffer = std::make_shared<FramebufferFactory>(GetDevice());
        m_shadowFramebuffer->DepthTarget = m_shadowMap->GetTexture();

        DepthPass::CreateParameters shadowDepthParams;
        shadowDepthParams.slopeScaledDepthBias = 4.f;
        shadowDepthParams.depthBias = 100;
        m_shadowDepthPass = std::make_shared<DepthPass>(GetDevice(), m_CommonPasses);
        m_shadowDepthPass->Init(*m_shaderFactory, shadowDepthParams);

        m_commandList = GetDevice()->createCommandList();

        m_firstPersonCamera.SetMoveSpeed(3.0f);

        SetAsynchronousLoadingEnabled(true);

        // Load default scene if not provided
        if (sceneName.empty())
            SetCurrentSceneName(app::FindPreferredScene(m_sceneFilesAvailable, "media/Bistro.scene.json"));
        else
            SetCurrentSceneName("/native/" + sceneName);
    }

    std::shared_ptr<vfs::IFileSystem> GetRootFs() const
    {
        return m_rootFileSystem;
    }

    BaseCamera& GetActiveCamera() const
    {
        return (BaseCamera&)m_firstPersonCamera;
    }

    std::vector<std::string> const& GetAvailableScenes() const
    {
        return m_sceneFilesAvailable;
    }

    std::string GetCurrentSceneName() const
    {
        return m_currentSceneName;
    }

    void SetCurrentSceneName(const std::string& sceneName)
    {
        if (m_currentSceneName == sceneName)
            return;

        m_currentSceneName = sceneName;

        BeginLoadingScene(m_rootFileSystem, m_currentSceneName);
    }

    void CopyActiveCameraToFirstPerson()
    {
        if (m_ui.activeSceneCamera)
        {
            dm::affine3 viewToWorld = m_ui.activeSceneCamera->GetViewToWorldMatrix();
            dm::float3 cameraPos = viewToWorld.m_translation;
            m_firstPersonCamera.LookAt(cameraPos, cameraPos + viewToWorld.m_linear.row2, viewToWorld.m_linear.row1);
        }
    }

    virtual bool KeyboardUpdate(int key, int scancode, int action, int mods) override
    {
        if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS)
        {
            m_ui.showUI = !m_ui.showUI;
            return true;
        }

        if (key == GLFW_KEY_GRAVE_ACCENT && action == GLFW_PRESS)
        {
            m_ui.showConsole = !m_ui.showConsole;
            return true;
        }

        if (key == GLFW_KEY_SPACE && action == GLFW_PRESS)
        {
            m_ui.enableAnimations = !m_ui.enableAnimations;
            return true;
        }

        GetActiveCamera().KeyboardUpdate(key, scancode, action, mods);

        return true;
    }

    virtual bool MousePosUpdate(double xpos, double ypos) override
    {
        GetActiveCamera().MousePosUpdate(xpos, ypos);

        m_pickPosition = uint2(static_cast<uint>(xpos), static_cast<uint>(ypos));

        return true;
    }

    virtual bool MouseButtonUpdate(int button, int action, int mods) override
    {
        GetActiveCamera().MouseButtonUpdate(button, action, mods);

        if (action == GLFW_PRESS && button == GLFW_MOUSE_BUTTON_2)
            m_pick = true;

        return true;
    }

    virtual bool MouseScrollUpdate(double xoffset, double yoffset) override
    {
        GetActiveCamera().MouseScrollUpdate(xoffset, yoffset);

        return true;
    }

    virtual void Animate(float fElapsedTimeSeconds) override
    {
        GetActiveCamera().Animate(fElapsedTimeSeconds);

        if (m_toneMappingPass)
            m_toneMappingPass->AdvanceFrame(fElapsedTimeSeconds);

        if (IsSceneLoaded() && m_ui.enableAnimations)
        {
            m_wallclockTime += fElapsedTimeSeconds;

            for (const auto& anim : m_scene->GetSceneGraph()->GetAnimations())
            {
                float duration = anim->GetDuration();
                float integral;
                float animationTime = std::modf(m_wallclockTime / duration, &integral) * duration;
                (void)anim->Apply(animationTime);
            }
        }
    }

    virtual void SceneUnloading() override
    {
        GetDevice()->waitForIdle();
        GetDevice()->runGarbageCollection();

        m_shaderFactory->ClearCache();
        m_bindingCache.Clear();

        if (m_forwardPass) m_forwardPass->ResetBindingCache();
        if (m_deferredLightingPass) m_deferredLightingPass->ResetBindingCache();
        if (m_gBufferPass) m_gBufferPass->ResetBindingCache();
        if (m_gBufferReadDepthPass) m_gBufferReadDepthPass->ResetBindingCache();
        if (m_depthPass) m_depthPass->ResetBindingCache();
        if (m_shadowDepthPass) m_shadowDepthPass->ResetBindingCache();

        m_sunLight.reset();
        m_ui.selectedMaterial = nullptr;
        m_ui.selectedNode = nullptr;
        m_feedbackTextureMaps.m_feedbackTextureSetsByMaterial.clear();
        m_feedbackTextureMaps.m_feedbackTexturesByFeedback.clear();
        m_feedbackTextureMaps.m_feedbackTexturesByName.clear();
        m_feedbackTextureMaps.m_feedbackTexturesBySource.clear();
        m_feedbackTextureMaps.m_materialConstantsFeedback.clear();
        m_requestedTiles = {};

        m_feedbackManager.reset();
    }

    virtual bool LoadScene(std::shared_ptr<IFileSystem> fs, const std::filesystem::path& fileName) override
    {
        using namespace std::chrono;

        Scene* scene = new Scene(GetDevice(), *m_shaderFactory, fs, m_TextureCache, nullptr, nullptr);

        auto startTime = high_resolution_clock::now();

        if (scene->Load(fileName))
        {
            m_scene = std::unique_ptr<Scene>(scene);

            auto endTime = high_resolution_clock::now();
            auto duration = duration_cast<milliseconds>(endTime - startTime).count();
            log::info("Scene loading time: %llu ms", duration);

            return true;
        }

        return false;
    }
    
    virtual void SceneLoaded() override
    {
        Super::SceneLoaded();

        m_scene->FinishedLoading(GetFrameIndex());

        m_wallclockTime = 0.f;
        m_previousViewsValid = false;

        for (auto light : m_scene->GetSceneGraph()->GetLights())
        {
            if (light->GetLightType() == LightType_Directional)
            {
                m_sunLight = std::static_pointer_cast<DirectionalLight>(light);
                break;
            }
        }

        if (!m_sunLight)
        {
            m_sunLight = std::make_shared<DirectionalLight>();
            m_sunLight->angularSize = 0.53f;
            m_sunLight->irradiance = 1.f;

            auto node = std::make_shared<SceneGraphNode>();
            node->SetLeaf(m_sunLight);
            m_sunLight->SetDirection(dm::double3(0.1, -0.9, 0.1));
            m_sunLight->SetName("Sun");
            m_scene->GetSceneGraph()->Attach(m_scene->GetSceneGraph()->GetRootNode(), node);
        }

        m_sunLight->SetDirection(dm::double3(-0.049f, -0.87f, 0.48f));

        auto cameras = m_scene->GetSceneGraph()->GetCameras();
        if (!cameras.empty())
        {
            m_ui.activeSceneCamera = cameras[0];
        }
        else
        {
            m_ui.activeSceneCamera.reset();
            m_firstPersonCamera.LookAt(float3(0.f, 1.8f, 0.f), float3(1.f, 1.8f, 0.f));
        }

        FeedbackManagerDesc fmDesc = {};
        fmDesc.numFramesInFlight = GetDeviceManager()->GetBackBufferCount();
        fmDesc.heapSizeInTiles = 1024; // 64MiB heap size
        m_feedbackManager = std::shared_ptr<FeedbackManager>(CreateFeedbackManager(GetDevice(), fmDesc));

        m_recreateFeedbackTextures = true;
        m_recreateFeedbackTextureSets = true;
        m_cameraCut = true;

        CopyActiveCameraToFirstPerson();
    }

    std::shared_ptr<TextureCache> GetTextureCache()
    {
        return m_TextureCache;
    }

    std::shared_ptr<Scene> GetScene()
    {
        return m_scene;
    }

    bool SetupView()
    {
        float2 renderTargetSize = float2(m_renderTargets->GetSize());

        if (m_temporalAntiAliasingPass)
            m_temporalAntiAliasingPass->SetJitter(m_ui.temporalAntiAliasingJitter);

        float2 pixelOffset = m_ui.antiAliasingMode == AntiAliasingMode::TEMPORAL && m_temporalAntiAliasingPass 
            ? m_temporalAntiAliasingPass->GetCurrentPixelOffset() 
            : float2(0.f);

        std::shared_ptr<PlanarView> planarView = std::dynamic_pointer_cast<PlanarView, IView>(m_view);

        dm::affine3 viewMatrix;
        float verticalFov = dm::radians(m_cameraVerticalFov);
        float zNear = 0.01f;
        viewMatrix = GetActiveCamera().GetWorldToViewMatrix();

        bool topologyChanged = false;

        if (!planarView)
        {
            m_view = planarView = std::make_shared<PlanarView>();
            m_viewPrevious = std::make_shared<PlanarView>();
            topologyChanged = true;
        }

        float4x4 projection = perspProjD3DStyleReverse(verticalFov, renderTargetSize.x / renderTargetSize.y, zNear);

        planarView->SetViewport(nvrhi::Viewport(renderTargetSize.x, renderTargetSize.y));
        planarView->SetPixelOffset(pixelOffset);

        planarView->SetMatrices(viewMatrix, projection);
        planarView->UpdateCache();

        if (topologyChanged)
            *std::static_pointer_cast<PlanarView>(m_viewPrevious) = *std::static_pointer_cast<PlanarView>(m_view);

        return topologyChanged;
    }

    void CreateRenderPasses(bool& exposureResetRequired)
    {
        uint32_t motionVectorStencilMask = 0x01;

        m_depthFramebuffer = std::make_shared<FramebufferFactory>(GetDevice());
        m_depthFramebuffer->DepthTarget = m_renderTargets->Depth;

        DepthPass::CreateParameters depthParams;
        depthParams.trackLiveness = false;
        m_depthPass = std::make_shared<DepthPass>(GetDevice(), m_CommonPasses);
        m_depthPass->Init(*m_shaderFactory, depthParams);

        ForwardShadingPass::CreateParameters ForwardParams;
        ForwardParams.trackLiveness = false;
        m_forwardPass = std::make_unique<ForwardShadingPass>(GetDevice(), m_CommonPasses);
        m_forwardPass->Init(*m_shaderFactory, ForwardParams);

        GBufferFillPassFeedback::CreateParameters GBufferParams;
        GBufferParams.enableDepthWrite = true;
        GBufferParams.enableMotionVectors = true;
        GBufferParams.stencilWriteMask = motionVectorStencilMask;
        m_gBufferPass = std::make_unique<GBufferFillPassFeedback>(GetDevice(), m_CommonPasses, &m_feedbackTextureMaps);
        m_gBufferPass->Init(*m_shaderFactory, GBufferParams);

        GBufferParams.enableDepthWrite = false;
        m_gBufferReadDepthPass = std::make_unique<GBufferFillPassFeedback>(GetDevice(), m_CommonPasses, &m_feedbackTextureMaps);
        m_gBufferReadDepthPass->Init(*m_shaderFactory, GBufferParams);

        GBufferParams.enableMotionVectors = false;
        m_materialIDPass = std::make_unique<MaterialIDPassFeedback>(GetDevice(), m_CommonPasses, &m_feedbackTextureMaps);
        m_materialIDPass->Init(*m_shaderFactory, GBufferParams);

        m_pixelReadbackPass = std::make_unique<PixelReadbackPass>(GetDevice(), m_shaderFactory, m_renderTargets->materialIDs, nvrhi::Format::RGBA32_UINT);

        m_deferredLightingPass = std::make_unique<DeferredLightingPass>(GetDevice(), m_CommonPasses);
        m_deferredLightingPass->Init(m_shaderFactory);

        m_skyPass = std::make_unique<SkyPass>(GetDevice(), m_shaderFactory, m_CommonPasses, m_renderTargets->forwardFramebuffer, *m_view);

        {
            TemporalAntiAliasingPass::CreateParameters taaParams;
            taaParams.sourceDepth = m_renderTargets->Depth;
            taaParams.motionVectors = m_renderTargets->MotionVectors;
            taaParams.unresolvedColor = m_renderTargets->hdrColor;
            taaParams.resolvedColor = m_renderTargets->resolvedColor;
            taaParams.feedback1 = m_renderTargets->temporalFeedback1;
            taaParams.feedback2 = m_renderTargets->temporalFeedback2;
            taaParams.motionVectorStencilMask = motionVectorStencilMask;
            taaParams.useCatmullRomFilter = true;

            m_temporalAntiAliasingPass = std::make_unique<TemporalAntiAliasingPass>(GetDevice(), m_shaderFactory, m_CommonPasses, *m_view, taaParams);
        }

        m_ssaoPass = std::make_unique<SsaoPass>(GetDevice(), m_shaderFactory, m_CommonPasses, m_renderTargets->Depth, m_renderTargets->GBufferNormals, m_renderTargets->ambientOcclusion);

        nvrhi::BufferHandle exposureBuffer = nullptr;
        if (m_toneMappingPass)
            exposureBuffer = m_toneMappingPass->GetExposureBuffer();
        else
            exposureResetRequired = true;

        ToneMappingPass::CreateParameters toneMappingParams;
        toneMappingParams.exposureBufferOverride = exposureBuffer;
        m_toneMappingPass = std::make_unique<ToneMappingPass>(GetDevice(), m_shaderFactory, m_CommonPasses, m_renderTargets->ldrFramebuffer, *m_view, toneMappingParams);

        m_bloomPass = std::make_unique<BloomPass>(GetDevice(), m_shaderFactory, m_CommonPasses, m_renderTargets->resolvedFramebuffer, *m_view);

        m_previousViewsValid = false;
    }

    virtual void RenderSplashScreen(nvrhi::IFramebuffer* framebuffer) override
    {
        nvrhi::ITexture* framebufferTexture = framebuffer->getDesc().colorAttachments[0].texture;
        m_commandList->open();
        m_commandList->clearTextureFloat(framebufferTexture, nvrhi::AllSubresources, nvrhi::Color(0.f));
        m_commandList->close();
        GetDevice()->executeCommandList(m_commandList);
        GetDeviceManager()->SetVsyncEnabled(true);
    }

    // Make sure before rendering that all feedback textures have been created
    void EnsureFeedbackTextures()
    {
        if (!m_recreateFeedbackTextures)
            return;
        m_recreateFeedbackTextures = false;

        nvrhi::DeviceHandle device = GetDevice();
        device->waitForIdle();
        device->runGarbageCollection();

        if (m_gBufferPass) m_gBufferPass->ResetBindingCache();
        if (m_gBufferReadDepthPass) m_gBufferReadDepthPass->ResetBindingCache();

        m_feedbackTextureMaps.m_feedbackTexturesByFeedback.clear();
        m_feedbackTextureMaps.m_feedbackTexturesByName.clear();
        m_feedbackTextureMaps.m_feedbackTexturesBySource.clear();
        m_feedbackTextureMaps.m_materialConstantsFeedback.clear();

        // Generate all the reserved and feedback textures

        {
            nvrhi::CommandListHandle commandList = device->createCommandList();
            commandList->open();

            // Normal textures based on the scene
            auto& cache = GetTextureCache();
            for (auto it = cache->begin(); it != cache->end(); ++it)
            {
                auto& name = it->first;
                std::shared_ptr<donut::engine::TextureData> texture = it->second;

                uint textureWidth = texture->width;
                uint textureHeight = texture->height;

                bool isBlockCompressed = (texture->format >= nvrhi::Format::BC1_UNORM && texture->format <= nvrhi::Format::BC7_UNORM_SRGB);
                if (isBlockCompressed)
                {
                    textureWidth = (textureWidth + 3) & ~3;
                    textureHeight = (textureHeight + 3) & ~3;
                }

                nvrhi::TextureDesc textureDesc;
                textureDesc.format = texture->format;
                textureDesc.width = textureWidth;
                textureDesc.height = textureHeight;
                textureDesc.depth = texture->depth;
                textureDesc.arraySize = texture->arraySize;
                textureDesc.dimension = texture->dimension;
                textureDesc.mipLevels = texture->mipLevels;
                textureDesc.debugName = texture->path;
                textureDesc.isRenderTarget = texture->isRenderTarget;

                bool useTiledTexture = isBlockCompressed && textureDesc.depth == 1 && textureDesc.arraySize == 1;
                if (!useTiledTexture)
                {
                    texture->texture = device->createTexture(textureDesc);
                    commandList->beginTrackingTextureState(texture->texture, nvrhi::AllSubresources, nvrhi::ResourceStates::Common);

                    const char* dataPointer = static_cast<const char*>(texture->data->data());
                    for (uint32_t arraySlice = 0; arraySlice < texture->arraySize; arraySlice++)
                    {
                        for (uint32_t mipLevel = 0; mipLevel < texture->mipLevels; mipLevel++)
                        {
                            const TextureSubresourceData& layout = texture->dataLayout[arraySlice][mipLevel];
                            commandList->writeTexture(texture->texture, arraySlice, mipLevel, dataPointer + layout.dataOffset, layout.rowPitch, layout.depthPitch);
                        }
                    }

                    commandList->setPermanentTextureState(texture->texture, nvrhi::ResourceStates::ShaderResource);

                    continue;
                }

                nvrhi::RefCountPtr<FeedbackTexture> feedbackTexture;
                m_feedbackManager->CreateTexture(textureDesc, &feedbackTexture);

                auto wrapper = std::make_shared<FeedbackTextureWrapper>();
                wrapper->m_feedbackTexture = feedbackTexture;
                wrapper->m_sourceTexture = texture;
                m_feedbackTextureMaps.m_feedbackTexturesByName[name] = wrapper;
                m_feedbackTextureMaps.m_feedbackTexturesByFeedback[feedbackTexture] = wrapper;
                m_feedbackTextureMaps.m_feedbackTexturesBySource[texture.get()] = wrapper;
            }

            commandList->close();
            device->executeCommandList(commandList);

            log::info("Created %d tiled textures", m_feedbackTextureMaps.m_feedbackTexturesByName.size());
        }
    }

    // After all feedback textures have been created, create texture sets if enabled
    void EnsureTextureSets()
    {
        if (m_ui.useTextureSets != m_textureSetsEnabled)
        {
            m_textureSetsEnabled = m_ui.useTextureSets;
            m_recreateFeedbackTextureSets = true;
        }

        if (!m_recreateFeedbackTextureSets)
            return;
            
        m_recreateFeedbackTextureSets = false;

        nvrhi::DeviceHandle device = GetDevice();
        device->waitForIdle();
        device->runGarbageCollection();

        log::info("Clearing texture sets");
        m_feedbackTextureMaps.m_feedbackTextureSetsByMaterial.clear();
        m_feedbackTextureMaps.m_materialConstantsFeedback.clear();

        if (m_gBufferPass) m_gBufferPass->ResetBindingCache();
        if (m_gBufferReadDepthPass) m_gBufferReadDepthPass->ResetBindingCache();

        auto& materials = GetScene()->GetSceneGraph()->GetMaterials();
        if (m_ui.useTextureSets)
        {
            // Now construct texture sets, one per material
            for (auto& material : materials)
            {
                if (!material->baseOrDiffuseTexture)
                {
                    // Texture sets are only supported for materials with a diffuse texture due to pairing in the shader
                    continue;
                }

                if (m_feedbackTextureMaps.m_feedbackTexturesBySource.find(material->baseOrDiffuseTexture.get()) == m_feedbackTextureMaps.m_feedbackTexturesBySource.end())
                {
                    // This material does not have feedback textures
                    continue;
                }

                nvrhi::RefCountPtr<FeedbackTextureSet> textureSet;
                m_feedbackManager->CreateTextureSet(&textureSet);

                auto addTextureToSet = [&](const std::shared_ptr<LoadedTexture>& texture) {
                    if (!texture) return;
                    auto it = m_feedbackTextureMaps.m_feedbackTexturesBySource.find(texture.get());
                    if (it != m_feedbackTextureMaps.m_feedbackTexturesBySource.end())
                    {
                        textureSet->AddTexture(it->second->m_feedbackTexture);
                    }
                };

                // Add the diffuse texture first, which is currently always the primary texture
                addTextureToSet(material->baseOrDiffuseTexture);
                addTextureToSet(material->metalRoughOrSpecularTexture); 
                addTextureToSet(material->normalTexture);
                addTextureToSet(material->emissiveTexture);
                addTextureToSet(material->occlusionTexture);
                addTextureToSet(material->transmissionTexture);
                addTextureToSet(material->opacityTexture);

                // Do one more check to ensure no follower textures are larger than the primary texture
                bool rejectTextureSet = false;
                auto primaryTexture = textureSet->GetPrimaryTexture()->GetReservedTexture();
                uint32_t primaryWidth = primaryTexture->getDesc().width;
                uint32_t primaryHeight = primaryTexture->getDesc().height;
                uint32_t primaryMipLevels = primaryTexture->getDesc().mipLevels;
                uint32_t numTextures = textureSet->GetNumTextures();
                for (uint32_t i = 0; i < numTextures; i++)
                {
                    auto followerTexture = textureSet->GetTexture(i)->GetReservedTexture();
                    uint32_t width = followerTexture->getDesc().width;
                    uint32_t height = followerTexture->getDesc().height;
                    uint32_t mipLevels = followerTexture->getDesc().mipLevels;

                    if (width > primaryWidth || height > primaryHeight || mipLevels > primaryMipLevels)
                    {
                        // Reject this texture set because it has a follower texture that is larger than the primary texture
                        rejectTextureSet = true;
                        break;
                    }
                }

                if (rejectTextureSet)
                    continue; // Reject the texture set, its destructor will clean up state in FeedbackTextures

                // Store the texture set, and the map from material to texture set
                m_feedbackTextureMaps.m_feedbackTextureSetsByMaterial[material.get()] = textureSet;
            }
            log::info("Created %d texture sets", m_feedbackTextureMaps.m_feedbackTextureSetsByMaterial.size());
        }

        // Now create the FeedbackConstants constant buffers, one per material
        {
            nvrhi::CommandListHandle commandList = device->createCommandList();
            commandList->open();

            for (auto& material : materials)
            {
                nvrhi::BufferDesc bufferDesc;
                bufferDesc.byteSize = sizeof(FeedbackConstants);
                bufferDesc.debugName = material->name + "_FeedbackConstants";
                bufferDesc.isConstantBuffer = true;
                bufferDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
                bufferDesc.keepInitialState = true;
                bufferDesc.isVirtual = false;
                nvrhi::BufferHandle cb = device->createBuffer(bufferDesc);

                // Check if this material is using a texture set by looking it up in the map
                bool useTextureSet = m_feedbackTextureMaps.m_feedbackTextureSetsByMaterial.find(material.get()) != m_feedbackTextureMaps.m_feedbackTextureSetsByMaterial.end();

                FeedbackConstants feedbackConstants;
                feedbackConstants.useTextureSet = useTextureSet;
                commandList->writeBuffer(cb, &feedbackConstants, sizeof(FeedbackConstants));

                // Store the constant buffer in the map
                m_feedbackTextureMaps.m_materialConstantsFeedback[material.get()] = cb;
            }

            commandList->close();
            device->executeCommandList(commandList);
        }

        // Just for information, count textures with and without texture sets, and also how many textures are "primary"
        size_t texturesWithSets = 0;
        size_t texturesWithoutSets = 0;
        std::set<nvfeedback::FeedbackTexture*> uniquePrimaryTextures;
        for (const auto& pair : m_feedbackTextureMaps.m_feedbackTexturesByFeedback)
        {
            nvfeedback::FeedbackTexture* feedbackTexture = pair.first;
            if (feedbackTexture->GetNumTextureSets() > 0)
            {
                texturesWithSets++;
                for (uint32_t i = 0; i < feedbackTexture->GetNumTextureSets(); i++) 
                {
                    nvfeedback::FeedbackTextureSet* textureSet = feedbackTexture->GetTextureSet(i);
                    uniquePrimaryTextures.insert(textureSet->GetPrimaryTexture());
                }
            }
            else
            {
                texturesWithoutSets++;
            }
        }
        log::info("Feedback textures with texture sets: %zu", texturesWithSets);
        log::info("Feedback textures without texture sets: %zu", texturesWithoutSets);
        log::info("Unique primary textures: %zu", uniquePrimaryTextures.size());
    }

    // At the beginning of the frame, read back and process sampler feedback
    void ProcessFeedbackBeforeRender()
    {
        nvrhi::DeviceHandle device = GetDevice();

        m_tileUploadHelper.BeginFrame(GetFrameIndex());

        // Collection of packed tiles requested, typically right after loading a scene
        // These always get uploaded using the slower but more flexible packed mip codepath
        std::vector<RequestedTile> requestedPackedTiles;

        // Begin frame, readback feedback
        {
            m_commandList->open();

            FeedbackTextureCollection updatedTextures = {};
            FeedbackUpdateConfig fconfig = {};
            fconfig.frameIndex = GetDeviceManager()->GetCurrentBackBufferIndex();
            fconfig.maxTexturesToUpdate = std::max(m_ui.texturesPerFrame, 0);
            fconfig.tileTimeoutSeconds = std::max(m_ui.tileTimeout, 0.0f);
            fconfig.defragmentHeaps = m_ui.compactMemory;
            fconfig.trimStandbyTiles = m_ui.compactMemory;
            fconfig.releaseEmptyHeaps = m_ui.compactMemory;
            fconfig.numExtraStandbyTiles = m_ui.numExtraStandbyTiles;
            if (m_cameraCut)
            {
                fconfig.maxTexturesToUpdate = 0;
                m_cameraCut = false;
            }
            m_feedbackManager->BeginFrame(m_commandList, fconfig, &updatedTextures);

            // Collect all tiles and store them in the queue
            for (FeedbackTextureUpdate& texUpdate : updatedTextures.textures)
            {
                RequestedTile reqTile;
                reqTile.texture = texUpdate.texture;
                for (uint32_t i = 0; i < texUpdate.tileIndices.size(); i++)
                {
                    reqTile.tileIndex = texUpdate.tileIndices[i];
                    if (texUpdate.texture->IsTilePacked(reqTile.tileIndex))
                        requestedPackedTiles.push_back(reqTile);
                    else
                        m_requestedTiles.push(reqTile);
                }
            }

            m_commandList->close();
            device->executeCommandList(m_commandList);
        }

        // Figure out which tiles to map and upload this frame
        FeedbackTextureCollection tilesThisFrame;
        if (!requestedPackedTiles.empty() || !m_requestedTiles.empty())
        {
            // Compute how many tiles we will upload this frame
            uint32_t countUpload = std::min((uint32_t)m_requestedTiles.size(), m_tileUploadHelper.NumTilesMax());
            countUpload = std::min(countUpload, (uint32_t)m_ui.tilesPerFrame);

            // This schedules a tile to be uploaded this frame
            auto scheduleTileForUpload = [&](const RequestedTile& reqTile)
            {
                // Find if we already have this texture in tilesThisFrame
                FeedbackTextureUpdate* pTexUpdate = nullptr;
                for (uint32_t t = 0; t < tilesThisFrame.textures.size(); t++)
                {
                    if (tilesThisFrame.textures[t].texture == reqTile.texture)
                    {
                        pTexUpdate = &tilesThisFrame.textures[t];
                        break;
                    }
                }

                if (pTexUpdate == nullptr)
                {
                    // First time we see this texture this frame
                    FeedbackTextureUpdate texUpdate;
                    texUpdate.texture = reqTile.texture;
                    tilesThisFrame.textures.push_back(texUpdate);
                    pTexUpdate = &tilesThisFrame.textures.back();
                }

                pTexUpdate->tileIndices.push_back(reqTile.tileIndex);
            };

            // Upload all packed tiles this frame
            for (auto& packedTile : requestedPackedTiles)
                scheduleTileForUpload(packedTile);

            // Upload only countUpload regular tiles
            for (uint32_t i = 0; i < countUpload; i++)
            {
                scheduleTileForUpload(m_requestedTiles.front());
                m_requestedTiles.pop();
            }
        }

        // Call UpdateTileMappings always (it might be needed for defragmentation)
        {
            m_commandList->open();

            m_feedbackManager->UpdateTileMappings(m_commandList, &tilesThisFrame);

            m_commandList->close();

            // Execute the command list here
            // This synchronizes the command lists work before and after calling the UpdateTileMappings API
            GetDevice()->executeCommandList(m_commandList);
        }

        // Upload the tiles to the GPU and copy them into the resources
        if (tilesThisFrame.textures.size() > 0)
        {
            m_commandList->open();
            ID3D12GraphicsCommandList* pCommandList = m_commandList->getNativeObject(nvrhi::ObjectTypes::D3D12_GraphicsCommandList);

            std::vector<nvfeedback::FeedbackTextureTileInfo> tiles;

            for (FeedbackTextureUpdate& texUpdate : tilesThisFrame.textures)
            {
                auto& wrapper = m_feedbackTextureMaps.m_feedbackTexturesByFeedback[texUpdate.texture];
                auto& reservedTexture = wrapper->m_feedbackTexture->GetReservedTexture();

                // Get tiling info
                uint32_t numTiles = 0;
                nvrhi::PackedMipDesc packedMipDesc = {};
                nvrhi::TileShape tileShape = {};
                uint32_t mipLevels = reservedTexture->getDesc().mipLevels;
                std::array<nvrhi::SubresourceTiling, 16> tilingsInfo;
                device->getTextureTiling(reservedTexture, &numTiles, &packedMipDesc, &tileShape, &mipLevels, tilingsInfo.data());

                // NOTE: This is currently required to talk directly to the d3d12 commandlist for requireTextureState and commitBarriers
                // This hack is incompatible with the NVRHI validation layer
                nvrhi::d3d12::CommandList* d3d12CommandList = dynamic_cast<nvrhi::d3d12::CommandList*>(m_commandList.Get());
                d3d12CommandList->requireTextureState(reservedTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::CopyDest);
                d3d12CommandList->commitBarriers();

                auto& textureData = wrapper->m_sourceTexture;
                ID3D12Resource* pResource = reservedTexture->getNativeObject(nvrhi::ObjectTypes::D3D12_Resource);

                for (auto& tileIndex : texUpdate.tileIndices)
                {
                    texUpdate.texture->GetTileInfo(tileIndex, tiles);
                    for (auto& tile : tiles)
                    {
                        if (texUpdate.texture->IsTilePacked(tileIndex))
                        {
                            // Flexible, but slower, path for uploading packed mips
                            const TextureSubresourceData& layout = textureData->dataLayout[0][tile.mip];
                            const char* dataPointer = static_cast<const char*>(textureData->data->data());

                            m_commandList->writeTexture(reservedTexture, 0, tile.mip, dataPointer + layout.dataOffset,
                                layout.rowPitch, layout.depthPitch);
                        }
                        else
                        {
                            // More efficient path for uploading regular tiles
                            const TextureSubresourceData& layout = textureData->dataLayout[0][tile.mip];
                            const char* mipBase = static_cast<const char*>(textureData->data->data()) + layout.dataOffset;
                            bool uploadSuccess = m_tileUploadHelper.UploadTile(pCommandList, pResource, tile, mipBase, tileShape,(uint32_t)layout.rowPitch);
                            assert(uploadSuccess);
                        }
                    }
                }
            }

            m_commandList->close();
            device->executeCommandList(m_commandList);
        }
    }

    // After rendering, resolve feedback and do some housekeeping
    void ProcessFeedbackAfterRender()
    {
        m_commandList->open();

        // Resolve feedback after rendering
        m_timerResolve.beginQuery(m_commandList);
        m_feedbackManager->ResolveFeedback(m_commandList);
        m_timerResolve.endQuery(m_commandList);

        // End frame logic
        m_feedbackManager->EndFrame();

        m_commandList->close();
        GetDevice()->executeCommandList(m_commandList);

        // Update CPU time stats
        FeedbackManagerStats stats = m_feedbackManager->GetStats();
        m_perfFeedbackBegin.AddSample(stats.cputimeBeginFrame);
        m_perfFeedbackUpdateTileMappings.AddSample(stats.cputimeUpdateTileMappings);
        m_perfFeedbackResolve.AddSample(stats.cputimeResolve);

        // Get frames per second and adjust max num samples to roughly match it
        float const frameTime = (float)GetDeviceManager()->GetAverageFrameTimeSeconds();
        float const framesPerSecond = (frameTime > 0.f) ? 1.f / frameTime : 0.f;
        uint32_t newMaxNumSamples = std::clamp((uint32_t)framesPerSecond, 1U, 1000U);

        m_perfFeedbackBegin.SetMaxNumSamples(newMaxNumSamples);
        m_perfFeedbackUpdateTileMappings.SetMaxNumSamples(newMaxNumSamples);
        m_perfFeedbackResolve.SetMaxNumSamples(newMaxNumSamples);
    }

    // Main render function
    virtual void RenderScene(nvrhi::IFramebuffer* framebuffer) override
    {
        // Make sure feedback textures are created
        EnsureFeedbackTextures();

        // Make sure texture sets are created
        EnsureTextureSets();

        // Perform all the feedback/tiled code before rendering the frame
        ProcessFeedbackBeforeRender();

        // Begin rendering the frame
        int windowWidth, windowHeight;
        GetDeviceManager()->GetWindowDimensions(windowWidth, windowHeight);
        nvrhi::Viewport windowViewport = nvrhi::Viewport(float(windowWidth), float(windowHeight));
        nvrhi::Viewport renderViewport = windowViewport;

        m_scene->RefreshSceneGraph(GetFrameIndex());

        bool exposureResetRequired = false;

        // Create render passes if needed
        {
            uint width = windowWidth;
            uint height = windowHeight;
            uint sampleCount = 1;

            bool needNewPasses = false;
            if (!m_renderTargets || m_renderTargets->IsUpdateRequired(uint2(width, height), sampleCount))
            {
                m_renderTargets = nullptr;
                m_bindingCache.Clear();
                m_renderTargets = std::make_unique<RenderTargets>();
                m_renderTargets->Init(GetDevice(), uint2(width, height), sampleCount, true, true);

                needNewPasses = true;
            }

            if (SetupView())
            {
                needNewPasses = true;
            }

            if (m_ui.shaderReloadRequested)
            {
                m_shaderFactory->ClearCache();
                needNewPasses = true;
            }

            if (needNewPasses)
                CreateRenderPasses(exposureResetRequired);

            m_ui.shaderReloadRequested = false;
        }

        // Open the command list for rendering
        m_commandList->open();

        m_scene->RefreshBuffers(m_commandList, GetFrameIndex());

        nvrhi::ITexture* framebufferTexture = framebuffer->getDesc().colorAttachments[0].texture;
        m_commandList->clearTextureFloat(framebufferTexture, nvrhi::AllSubresources, nvrhi::Color(0.f));

        m_ambientTop = m_ui.ambientIntensity * m_ui.skyParams.skyColor * m_ui.skyParams.brightness;
        m_ambientBottom = m_ui.ambientIntensity * m_ui.skyParams.groundColor * m_ui.skyParams.brightness;

        // Render shadows
        if (m_ui.enableShadows)
        {
            m_sunLight->shadowMap = m_shadowMap;
            box3 sceneBounds = m_scene->GetSceneGraph()->GetRootNode()->GetGlobalBoundingBox();

            frustum projectionFrustum = m_view->GetProjectionFrustum();
            const float maxShadowDistance = 100.f;

            dm::affine3 viewMatrixInv = m_view->GetChildView(ViewType::PLANAR, 0)->GetInverseViewMatrix();

            float zRange = length(sceneBounds.diagonal()) * 0.5f;
            m_shadowMap->SetupForPlanarViewStable(*m_sunLight, projectionFrustum, viewMatrixInv, maxShadowDistance, zRange, zRange, m_ui.csmExponent);

            m_shadowMap->Clear(m_commandList);

            DepthPass::Context context;

            RenderCompositeView(m_commandList,
                &m_shadowMap->GetView(), nullptr,
                *m_shadowFramebuffer,
                m_scene->GetSceneGraph()->GetRootNode(),
                *m_opaqueDrawStrategy,
                *m_shadowDepthPass,
                context,
                "ShadowMap",
                m_ui.enableMaterialEvents);
        }
        else
        {
            m_sunLight->shadowMap = nullptr;
        }

        m_renderTargets->Clear(m_commandList);

        if (exposureResetRequired)
            m_toneMappingPass->ResetExposure(m_commandList, 0.5f);

        ForwardShadingPass::Context forwardContext;

        if (m_ui.enableTranslucency)
        {
            std::vector<std::shared_ptr<LightProbe>> lightProbes;
            m_forwardPass->PrepareLights(forwardContext, m_commandList, m_scene->GetSceneGraph()->GetLights(), m_ambientTop, m_ambientBottom, lightProbes);
        }

        // Gbuffer pass with sampler feedback
        {
            uint32_t backBufferIndex = GetDeviceManager()->GetCurrentBackBufferIndex();

            m_timerGbuffer.beginQuery(m_commandList);

            auto* gBufferPass = m_gBufferPass.get();
            GBufferFillPassFeedback::Context gBufferFillPassFeedbackContext;
            gBufferPass->m_writeFeedback = m_ui.writeFeedback;
            gBufferPass->m_frameIndex = GetFrameIndex();
            gBufferPass->m_showUnmappedRegions = m_ui.showUnmappedRegions;
            gBufferPass->m_feedbackThreshold = m_ui.enableStochasticFeedback ? m_ui.feedbackProbabilityThreshold : 1.0f;
            gBufferPass->m_enableDebug = m_ui.enableDebug;
            RenderCompositeView(m_commandList,
                m_view.get(), m_viewPrevious.get(), 
                *m_renderTargets->GBufferFramebuffer, 
                m_scene->GetSceneGraph()->GetRootNode(),
                *m_opaqueDrawStrategy,
                *gBufferPass,
                gBufferFillPassFeedbackContext,
                "GBufferFill",
                m_ui.enableMaterialEvents);

            m_timerGbuffer.endQuery(m_commandList);

            nvrhi::ITexture* ambientOcclusionTarget = nullptr;
            if (m_ui.enableSsao && m_ssaoPass)
            {
                m_ssaoPass->Render(m_commandList, m_ui.ssaoParams, *m_view);
                ambientOcclusionTarget = m_renderTargets->ambientOcclusion;
            }

            DeferredLightingPass::Inputs deferredInputs;
            deferredInputs.SetGBuffer(*m_renderTargets);
            deferredInputs.ambientOcclusion = m_ui.enableSsao ? m_renderTargets->ambientOcclusion : nullptr;
            deferredInputs.ambientColorTop = m_ambientTop;
            deferredInputs.ambientColorBottom = m_ambientBottom;
            deferredInputs.lights = &m_scene->GetSceneGraph()->GetLights();
            deferredInputs.lightProbes = nullptr;
            deferredInputs.output = m_renderTargets->hdrColor;

            m_deferredLightingPass->Render(m_commandList, *m_view, deferredInputs);
        }

        // User requested to pick which material is under the cursor
        if (m_pick)
        {
            m_commandList->clearTextureUInt(m_renderTargets->materialIDs, nvrhi::AllSubresources, 0xffff);

            MaterialIDPassFeedback::Context materialIDPassFeedbackContext;

            RenderCompositeView(m_commandList, 
                m_view.get(), m_viewPrevious.get(),
                *m_renderTargets->materialIDFramebuffer,
                m_scene->GetSceneGraph()->GetRootNode(),
                *m_opaqueDrawStrategy,
                *m_materialIDPass,
                materialIDPassFeedbackContext,
                "MaterialID");

            if (m_ui.enableTranslucency)
            {
                RenderCompositeView(m_commandList,
                    m_view.get(), m_viewPrevious.get(),
                    *m_renderTargets->materialIDFramebuffer,
                    m_scene->GetSceneGraph()->GetRootNode(),
                    *m_transparentDrawStrategy,
                    *m_materialIDPass,
                    materialIDPassFeedbackContext,
                    "MaterialID - Translucent");
            }

            m_pixelReadbackPass->Capture(m_commandList, m_pickPosition);
        }

        if (m_ui.enableProceduralSky)
            m_skyPass->Render(m_commandList, *m_view, *m_sunLight, m_ui.skyParams);

        if (m_ui.enableTranslucency)
        {
            RenderCompositeView(m_commandList,
                m_view.get(), m_viewPrevious.get(),
                *m_renderTargets->forwardFramebuffer,
                m_scene->GetSceneGraph()->GetRootNode(),
                *m_transparentDrawStrategy,
                *m_forwardPass,
                forwardContext,
                "ForwardTransparent",
                m_ui.enableMaterialEvents);
        }

        nvrhi::ITexture* finalHdrColor = m_renderTargets->hdrColor;

        // TAA or regular HDR resolve
        if (m_ui.antiAliasingMode == AntiAliasingMode::TEMPORAL)
        {
            if (m_previousViewsValid)
            {
                m_temporalAntiAliasingPass->RenderMotionVectors(m_commandList, *m_view, *m_viewPrevious);
            }

            m_temporalAntiAliasingPass->TemporalResolve(m_commandList, m_ui.temporalAntiAliasingParams, m_previousViewsValid, *m_view, *m_view);

            finalHdrColor = m_renderTargets->resolvedColor;

            if (m_ui.enableBloom)
                m_bloomPass->Render(m_commandList, m_renderTargets->resolvedFramebuffer, *m_view, m_renderTargets->resolvedColor, m_ui.bloomSigma, m_ui.bloomAlpha);

            m_previousViewsValid = true;
        }
        else
        {
            std::shared_ptr<FramebufferFactory> finalHdrFramebuffer = m_renderTargets->hdrFramebuffer;

            if (m_renderTargets->GetSampleCount() > 1)
            {
                auto subresources = nvrhi::TextureSubresourceSet(0, 1, 0, 1);
                m_commandList->resolveTexture(m_renderTargets->resolvedColor, subresources, m_renderTargets->hdrColor, subresources);
                finalHdrColor = m_renderTargets->resolvedColor;
                finalHdrFramebuffer = m_renderTargets->resolvedFramebuffer;
            }

            if (m_ui.enableBloom)
                m_bloomPass->Render(m_commandList, finalHdrFramebuffer, *m_view, finalHdrColor, m_ui.bloomSigma, m_ui.bloomAlpha);

            m_previousViewsValid = false;
        }

        auto toneMappingParams = m_ui.toneMappingParams;
        if (exposureResetRequired)
        {
            toneMappingParams.eyeAdaptationSpeedUp = 0.f;
            toneMappingParams.eyeAdaptationSpeedDown = 0.f;
        }
        m_toneMappingPass->SimpleRender(m_commandList, toneMappingParams, *m_view, finalHdrColor);

        m_CommonPasses->BlitTexture(m_commandList, framebuffer, m_renderTargets->ldrColor, &m_bindingCache);

        // Visualize tile streaming state, at least for the diffuse texture
        auto selectedMaterial = m_ui.selectedMaterial;
        if (selectedMaterial && selectedMaterial->baseOrDiffuseTexture)
        {
            auto& name = selectedMaterial->baseOrDiffuseTexture->path;
            if (m_feedbackTextureMaps.m_feedbackTexturesByName.find(name) != m_feedbackTextureMaps.m_feedbackTexturesByName.end())
            {
                auto feedback = m_feedbackTextureMaps.m_feedbackTexturesByName[name];
                uint32_t mipLevelNum = std::min(feedback->m_sourceTexture->mipLevels, 8u);

                float size = 400.0f;
                float margin = 10.0f;
                float x = margin;
                for (uint32_t mip = 0; mip < mipLevelNum; mip++)
                {
                    nvrhi::Viewport viewport = nvrhi::Viewport(
                        x,
                        x + size,
                        windowViewport.maxY - size - margin,
                        windowViewport.maxY - margin,
                        0.f, 1.f
                    );

                    x += size + margin;
                    size /= 2.0f;

                    engine::BlitParameters blitParams;
                    blitParams.targetFramebuffer = framebuffer;
                    blitParams.targetViewport = viewport;
                    blitParams.sourceTexture = feedback->m_feedbackTexture->GetReservedTexture();
                    blitParams.sourceMip = mip;
                    m_CommonPasses->BlitTexture(m_commandList, blitParams, &m_bindingCache);
                }
            }
        }

        // Close the main render commandlist and execute it
        m_commandList->close();
        GetDevice()->executeCommandList(m_commandList);

        // Update the GPU timers
        m_timerGbuffer.update();
        m_timerResolve.update();

        // Now that the frame is rendered, resolve sampler feedback
        ProcessFeedbackAfterRender();

        if (!m_ui.screenshotFileName.empty())
        {
            SaveTextureToFile(GetDevice(), m_CommonPasses.get(), framebufferTexture, nvrhi::ResourceStates::RenderTarget, m_ui.screenshotFileName.c_str());
            m_ui.screenshotFileName = "";
        }

        if (m_pick)
        {
            m_pick = false;
            uint4 pixelValue = m_pixelReadbackPass->ReadUInts();
            m_ui.selectedMaterial = nullptr;
            m_ui.selectedNode = nullptr;

            for (const auto& material : m_scene->GetSceneGraph()->GetMaterials())
            {
                if (material->materialID == int(pixelValue.x))
                {
                    m_ui.selectedMaterial = material;
                    break;
                }
            }

            for (const auto& instance : m_scene->GetSceneGraph()->GetMeshInstances())
            {
                if (instance->GetInstanceIndex() == int(pixelValue.y))
                {
                    m_ui.selectedNode = instance->GetNodeSharedPtr();
                    break;
                }
            }
        }

        m_temporalAntiAliasingPass->AdvanceFrame();
        std::swap(m_view, m_viewPrevious);

        GetDeviceManager()->SetVsyncEnabled(m_ui.enableVsync);
    }

    std::shared_ptr<ShaderFactory> GetShaderFactory()
    {
        return m_shaderFactory;
    }
};

// UI renderer for the application
class UIRenderer : public ImGui_Renderer
{
private:
    std::shared_ptr<SampleApp> m_app;
    std::shared_ptr<engine::Light> m_selectedLight;
    std::shared_ptr<donut::app::RegisteredFont> m_fontDroidSansMono;
    std::shared_ptr<donut::app::RegisteredFont> m_fontDroidSansMonoLarge;

    UIData& m_ui;
    nvrhi::CommandListHandle m_commandList;

public:
    UIRenderer(DeviceManager* deviceManager, std::shared_ptr<SampleApp> app, UIData& ui)
        : ImGui_Renderer(deviceManager)
        , m_app(app)
        , m_ui(ui)
    {
        m_commandList = GetDevice()->createCommandList();

        std::shared_ptr<donut::vfs::NativeFileSystem> nativeFileSystem = std::make_shared<donut::vfs::NativeFileSystem>();
        m_fontDroidSansMono = CreateFontFromFile(*nativeFileSystem, GetDirectoryWithExecutable().parent_path() / "media/fonts/DroidSans/DroidSans-Mono.ttf", 16.0f);
        m_fontDroidSansMonoLarge = CreateFontFromFile(*nativeFileSystem, GetDirectoryWithExecutable().parent_path() / "media/fonts/DroidSans/DroidSans-Mono.ttf", 20.0f);

        ImGui::GetIO().IniFilename = nullptr;
    }

protected:
    virtual void buildUI(void) override
    {
        if (!m_ui.showUI)
            return;

        const auto& io = ImGui::GetIO();

        int width, height;
        GetDeviceManager()->GetWindowDimensions(width, height);

        if (m_app->IsSceneLoading())
        {
            BeginFullScreenWindow();

            ImGui::PushFont(m_fontDroidSansMono->GetScaledFont());

            char messageBuffer[256];
            const auto& stats = Scene::GetLoadingStats();
            snprintf(messageBuffer, std::size(messageBuffer), "Loading scene %s, please wait...\nObjects: %d/%d, Textures: %d/%d",
                m_app->GetCurrentSceneName().c_str(), stats.ObjectsLoaded.load(), stats.ObjectsTotal.load(), m_app->GetTextureCache()->GetNumberOfLoadedTextures(), m_app->GetTextureCache()->GetNumberOfRequestedTextures());

            DrawScreenCenteredText(messageBuffer);

            ImGui::PopFont();
            EndFullScreenWindow();

            return;
        }

        ImGui::PushFont(m_fontDroidSansMono->GetScaledFont());

        std::string resolution = std::to_string(width) + " x " + std::to_string(height);

        ImGui::SetNextWindowPos(ImVec2(10.f, 10.f), 0);
        ImGui::Begin("Settings", 0, ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::Text("Renderer: %s, %s", GetDeviceManager()->GetRendererString(), resolution.c_str());

        float const frameTime = (float)GetDeviceManager()->GetAverageFrameTimeSeconds();
        float const framesPerSecond = (frameTime > 0.f) ? 1.f / frameTime : 0.f;
        ImGui::Text("Frame Time: %.2f ms %.1f FPS (CPU)", frameTime * 1000.0f, framesPerSecond);

        auto renderTime = m_app->m_timerGbuffer.getAverageTime();
        if (renderTime.has_value())
        {
            ImGui::Text("G-Buffer Pass: %.2f ms (GPU)", renderTime.value() * 1e3f);
        }

        auto resolveTime = m_app->m_timerResolve.getAverageTime();
        if (resolveTime.has_value())
        {
            ImGui::Text("Resolve Pass: %.2f ms (GPU)", resolveTime.value() * 1e3f);
        }

        const std::string currentScene = m_app->GetCurrentSceneName();
        if (ImGui::BeginCombo("Scene", currentScene.c_str()))
        {
            const std::vector<std::string>& scenes = m_app->GetAvailableScenes();
            for (const std::string& scene : scenes)
            {
                bool is_selected = scene == currentScene;
                if (ImGui::Selectable(scene.c_str(), is_selected))
                    m_app->SetCurrentSceneName(scene);
                if (is_selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

#if _DEBUG
        if (ImGui::Button("Reload Shaders"))
            m_ui.shaderReloadRequested = true;
#endif // _DEBUG

        ImGui::Checkbox("VSync", &m_ui.enableVsync);

        if (ImGui::CollapsingHeader("CPU Profiling"))
        {
            double tBeginMax = m_app->m_perfFeedbackBegin.GetMax();
            double tBeginAvg = m_app->m_perfFeedbackBegin.GetAverage();
            double tUpdateTileMappingsMax = m_app->m_perfFeedbackUpdateTileMappings.GetMax();
            double tUpdateTileMappingsAvg = m_app->m_perfFeedbackUpdateTileMappings.GetAverage();
            double tResolveMax = m_app->m_perfFeedbackResolve.GetMax();
            double tResolveAvg = m_app->m_perfFeedbackResolve.GetAverage();
            ImGui::Text("BeginFrame max: %.3f ms, avg: %.3f ms", tBeginMax * 1e3, tBeginAvg * 1e3);
            ImGui::Text("UpdateTileMappings max: %.3f ms, avg: %.3f ms", tUpdateTileMappingsMax * 1e3, tUpdateTileMappingsAvg * 1e3);
            ImGui::Text("Resolve max: %.3f ms, avg: %.3f ms", tResolveMax * 1e3, tResolveAvg * 1e3);
        }

#if _DEBUG
        if (ImGui::CollapsingHeader("Rendering Settings"))
        {
            ImGui::Checkbox("Animations", &m_ui.enableAnimations);

            ImGui::Combo("AA Mode", (int*)&m_ui.antiAliasingMode, "None\0TemporalAA\0");
            ImGui::Combo("TAA Camera Jitter", (int*)&m_ui.temporalAntiAliasingJitter, "MSAA\0Halton\0R2\0White Noise\0");

            ImGui::SliderFloat("Ambient Intensity", &m_ui.ambientIntensity, 0.f, 1.f);

            ImGui::Checkbox("Enable Procedural Sky", &m_ui.enableProceduralSky);
            if (m_ui.enableProceduralSky && ImGui::CollapsingHeader("Sky Parameters"))
            {
                ImGui::SliderFloat("Brightness", &m_ui.skyParams.brightness, 0.f, 1.f);
                ImGui::SliderFloat("Glow Size", &m_ui.skyParams.glowSize, 0.f, 90.f);
                ImGui::SliderFloat("Glow Sharpness", &m_ui.skyParams.glowSharpness, 1.f, 10.f);
                ImGui::SliderFloat("Glow Intensity", &m_ui.skyParams.glowIntensity, 0.f, 1.f);
                ImGui::SliderFloat("Horizon Size", &m_ui.skyParams.horizonSize, 0.f, 90.f);
            }

            ImGui::Checkbox("Enable SSAO", &m_ui.enableSsao);
            ImGui::Checkbox("Enable Bloom", &m_ui.enableBloom);
            ImGui::DragFloat("Bloom Sigma", &m_ui.bloomSigma, 0.01f, 0.1f, 100.f);
            ImGui::DragFloat("Bloom Alpha", &m_ui.bloomAlpha, 0.01f, 0.01f, 1.0f);
            ImGui::Checkbox("Enable Shadows", &m_ui.enableShadows);
            ImGui::Checkbox("Enable Translucency", &m_ui.enableTranslucency);

            ImGui::Separator();
            ImGui::Checkbox("Temporal AA Clamping", &m_ui.temporalAntiAliasingParams.enableHistoryClamping);
        }
#endif // _DEBUG

        FeedbackManagerStats stats = {};
        if (m_app->m_feedbackManager)
            stats = m_app->m_feedbackManager->GetStats();

        ImGui::Separator();
        ImGui::Checkbox("Write Feedback", &m_ui.writeFeedback);
        ImGui::Checkbox("Use Texture Sets", &m_ui.useTextureSets);
        ImGui::Checkbox("Compact memory (pause/loading screen)", &m_ui.compactMemory);

        ImGui::Checkbox("Highlight Unmapped Regions", &m_ui.showUnmappedRegions);
        ImGui::Checkbox("Enable Stochastic Feedback", &m_ui.enableStochasticFeedback);
        ImGui::SliderFloat("Feedback Probability", &m_ui.feedbackProbabilityThreshold, 0.0f, 0.1f);
#if _DEBUG
        ImGui::Checkbox("Enable Debug", &m_ui.enableDebug);
#endif // _DEBUG

        ImGui::SliderInt("Textures Per Frame", &m_ui.texturesPerFrame, 0, 32);
        ImGui::SliderInt("Tiles Per Frame", &m_ui.tilesPerFrame, 1, 100);
        ImGui::SliderFloat("Tile Timeout Seconds", &m_ui.tileTimeout, 0, 1.0f);
        ImGui::SliderInt("Extra Standby Tiles", &m_ui.numExtraStandbyTiles, 0, 2000);

        ImGui::Separator();
        constexpr double mebibyte = 1024 * 1024;
        ImGui::Text("Tiled Textures: %d / %d", m_app->m_feedbackTextureMaps.m_feedbackTexturesByName.size(), m_app->GetTextureCache()->GetNumberOfLoadedTextures());
        double tilesTotalMibs = double(uint64_t(stats.tilesTotal) * uint64_t(D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES)) / mebibyte;
        ImGui::Text("Tiles Total: %d (%.0f MiB)", stats.tilesTotal, tilesTotalMibs);
        ImGui::Text("Tiles Allocated: %d (%.0f MiB)", stats.tilesAllocated, double(uint64_t(stats.tilesAllocated) * uint64_t(D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES)) / mebibyte);
        ImGui::Text("Tiles Standby: %d (%.0f MiB)", stats.tilesStandby, double(uint64_t(stats.tilesStandby) * uint64_t(D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES)) / mebibyte);
        double tilesHeapAllocatedMib = double(stats.heapAllocationInBytes) / mebibyte;
        ImGui::Text("Heap Allocation: %.0f MiB", tilesHeapAllocatedMib);
        ImGui::Text("Heap Free Tiles: %d (%.0f MiB)", stats.heapTilesFree, double(uint64_t(stats.heapTilesFree)* uint64_t(D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES)) / mebibyte);

        ImGui::Separator();

        if (stats.tilesTotal)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 255, 0, 255));
            ImGui::Text("Memory Savings: %.2fx (%.0f MiB)", tilesTotalMibs / tilesHeapAllocatedMib, tilesTotalMibs - tilesHeapAllocatedMib);
            ImGui::PopStyleColor();
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 20, 20, 255));
            ImGui::Text("No tiled resources Loaded\nOnly scenes with block-compressed textures are currently supported");
            ImGui::PopStyleColor();
        }

        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 0, 255));
        ImGui::Text("Right-click to visualize tiles residency");
        ImGui::PopStyleColor();

        ImGui::End();

        ImGui::PopFont();
    }
};

bool ProcessCommandLine(int argc, const char* const* argv, DeviceCreationParameters& deviceParams, std::string& sceneName)
{
    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "-width"))
        {
            deviceParams.backBufferWidth = std::stoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "-height"))
        {
            deviceParams.backBufferHeight = std::stoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "-fullscreen"))
        {
            deviceParams.startFullscreen = true;
        }
        else if (!strcmp(argv[i], "-debug"))
        {
            deviceParams.enableDebugRuntime = true;
        }
        else if (!strcmp(argv[i], "-no-vsync"))
        {
            deviceParams.vsyncEnabled = false;
        }
        else if (argv[i][0] != '-')
        {
            sceneName = argv[i];
        }
    }

    return true;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    DeviceCreationParameters deviceParams;
    deviceParams.backBufferWidth = 1920;
    deviceParams.backBufferHeight = 1080;
    deviceParams.swapChainSampleCount = 1;
    deviceParams.swapChainBufferCount = 3;
    deviceParams.startFullscreen = false;
    deviceParams.vsyncEnabled = false;

    std::string sceneName;
    if (!ProcessCommandLine(__argc, __argv, deviceParams, sceneName))
    {
        log::error("Failed to process the command line.");
        return 1;
    }

    DeviceManager* deviceManager = DeviceManager::Create(nvrhi::GraphicsAPI::D3D12);
    const char* apiString = nvrhi::utils::GraphicsAPIToString(deviceManager->GetGraphicsAPI());

    std::string windowTitle = "RTXTS Sample (" + std::string(apiString) + ")";

    if (!deviceManager->CreateWindowDeviceAndSwapChain(deviceParams, windowTitle.c_str()))
    {
        log::error("Cannot initialize a %s graphics device with the requested parameters", apiString);
        return 1;
    }

    {
        UIData uiData;
        std::shared_ptr<SampleApp> demo = std::make_shared<SampleApp>(deviceManager, uiData, sceneName);
        std::shared_ptr<UIRenderer> gui = std::make_shared<UIRenderer>(deviceManager, demo, uiData);

        gui->Init(demo->GetShaderFactory());

        deviceManager->AddRenderPassToBack(demo.get());
        deviceManager->AddRenderPassToBack(gui.get());

        deviceManager->RunMessageLoop();
    }

    deviceManager->Shutdown();
    delete deviceManager;

    return 0;
}
