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

#include <donut/engine/View.h>
#include <donut/engine/SceneTypes.h>
#include <donut/render/GeometryPasses.h>
#include <memory>
#include <mutex>

#include "MaterialBindingCacheFeedback.h"

namespace donut::engine
{
    class ShaderFactory;
    class CommonRenderPasses;
    class FramebufferFactory;
    class MaterialBindingCache;
    struct Material;
}

class GBufferFillPassFeedback : public donut::render::IGeometryPass
{
public:
    union PipelineKey
    {
        struct
        {
            nvrhi::RasterCullMode cullMode : 2;
            bool writeFeedback : 1;
            bool alphaTested : 1;
            bool frontCounterClockwise : 1;
            bool reverseDepth : 1;
        } bits;
        uint32_t value;

        static constexpr size_t Count = 1 << 6;
    };

    class Context : public donut::render::GeometryPassContext
    {
    public:
        PipelineKey keyTemplate;

        Context()
        {
            keyTemplate.value = 0;
        }
    };

    struct CreateParameters
    {
        bool enableSinglePassCubemap = false;
        bool enableDepthWrite = true;
        bool enableMotionVectors = false;
        bool trackLiveness = true;
        uint32_t stencilWriteMask = 0;
        uint32_t numConstantBufferVersions = 16;
    };

    // These variables are set by the application
    bool m_writeFeedback = true;
    uint32_t m_frameIndex = 0;
    bool m_showUnmappedRegions = false;
    float m_feedbackThreshold = 1.0f;
    bool m_enableDebug = false;

    GBufferFillPassFeedback(nvrhi::IDevice* device, std::shared_ptr<donut::engine::CommonRenderPasses> commonPasses, FeedbackTextureMaps* feedbackMaps);

    virtual void Init(
        donut::engine::ShaderFactory& shaderFactory,
        const CreateParameters& params);

    void ResetBindingCache() const;
    
    // IGeometryPass implementation

    [[nodiscard]] donut::engine::ViewType::Enum GetSupportedViewTypes() const override;
    void SetupView(donut::render::GeometryPassContext& context, nvrhi::ICommandList* commandList, const donut::engine::IView* view, const donut::engine::IView* viewPrev) override;
    bool SetupMaterial(donut::render::GeometryPassContext& context, const donut::engine::Material* material, nvrhi::RasterCullMode cullMode, nvrhi::GraphicsState& state) override;
    void SetupInputBuffers(donut::render::GeometryPassContext& context, const donut::engine::BufferGroup* buffers, nvrhi::GraphicsState& state) override;
    void SetPushConstants(donut::render::GeometryPassContext& context, nvrhi::ICommandList* commandList, nvrhi::GraphicsState& state, nvrhi::DrawArguments& args) override { }

protected:
    nvrhi::DeviceHandle m_Device;
    nvrhi::InputLayoutHandle m_InputLayout;
    nvrhi::ShaderHandle m_VertexShader;
    nvrhi::ShaderHandle m_PixelShader;
    nvrhi::ShaderHandle m_PixelShaderAlphaTested;
    nvrhi::ShaderHandle m_PixelShaderFeedback;
    nvrhi::ShaderHandle m_PixelShaderFeedbackAlphaTested;
    nvrhi::ShaderHandle m_GeometryShader;
    nvrhi::BindingLayoutHandle m_ViewBindingLayout;
    nvrhi::BufferHandle m_GBufferCB;
    nvrhi::BufferHandle m_GlobalCB;
    nvrhi::BindingSetHandle m_ViewBindings;
    donut::engine::ViewType::Enum m_SupportedViewTypes = donut::engine::ViewType::PLANAR;
    nvrhi::GraphicsPipelineHandle m_Pipelines[PipelineKey::Count];
    std::mutex m_Mutex;

    std::shared_ptr<donut::engine::CommonRenderPasses> m_CommonPasses;
    std::shared_ptr<MaterialBindingCacheFeedback> m_MaterialBindings;
    FeedbackTextureMaps* m_feedbackMaps;

    bool m_EnableDepthWrite = true;
    uint32_t m_StencilWriteMask = 0;
    
    virtual nvrhi::ShaderHandle CreateVertexShader(donut::engine::ShaderFactory& shaderFactory, const CreateParameters& params);
    virtual nvrhi::ShaderHandle CreateGeometryShader(donut::engine::ShaderFactory& shaderFactory, const CreateParameters& params);
    virtual nvrhi::ShaderHandle CreatePixelShader(donut::engine::ShaderFactory& shaderFactory, const CreateParameters& params, bool writeFeedback, bool alphaTested);
    virtual nvrhi::InputLayoutHandle CreateInputLayout(nvrhi::IShader* vertexShader, const CreateParameters& params);
    virtual void CreateViewBindings(nvrhi::BindingLayoutHandle& layout, nvrhi::BindingSetHandle& set, const CreateParameters& params);
    virtual std::shared_ptr<MaterialBindingCacheFeedback> CreateMaterialBindingCache(donut::engine::CommonRenderPasses& commonPasses);
    virtual nvrhi::GraphicsPipelineHandle CreateGraphicsPipeline(PipelineKey key, nvrhi::IFramebuffer* sampleFramebuffer);
};

class MaterialIDPassFeedback : public GBufferFillPassFeedback
{
protected:
    nvrhi::ShaderHandle CreatePixelShader(donut::engine::ShaderFactory& shaderFactory, const CreateParameters& params, bool writeFeedback, bool alphaTested) override;

public:
    using GBufferFillPassFeedback::GBufferFillPassFeedback;
};
