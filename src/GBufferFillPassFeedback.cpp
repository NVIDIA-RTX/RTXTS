/*
 * Copyright (c) 2025, NVIDIA CORPORATION.  All rights reserved.
 *
 * NVIDIA CORPORATION and its licensors retain all intellectual property
 * and proprietary rights in and to this software, related documentation
 * and any modifications thereto.  Any use, reproduction, disclosure or
 * distribution of this software and related documentation without an express
 * license agreement from NVIDIA CORPORATION is strictly prohibited.
 */

#include <donut/render/DrawStrategy.h>
#include <donut/engine/FramebufferFactory.h>
#include <donut/engine/ShaderFactory.h>
#include <donut/engine/ShadowMap.h>
#include <donut/engine/SceneTypes.h>
#include <donut/engine/CommonRenderPasses.h>
#include <donut/core/log.h>
#include <nvrhi/utils.h>
#include <utility>

using namespace donut::math;
#include <donut/shaders/gbuffer_cb.h>
#include "../shaders/global_cb.h"

using namespace donut::engine;
using namespace donut::render;

#include "MaterialBindingCacheFeedback.h"
#include "GBufferFillPassFeedback.h"

GBufferFillPassFeedback::GBufferFillPassFeedback(nvrhi::IDevice* device, std::shared_ptr<CommonRenderPasses> commonPasses, FeedbackTextureMaps* feedbackMaps)
    : m_Device(device)
    , m_CommonPasses(std::move(commonPasses))
    , m_feedbackMaps(feedbackMaps)
{

}

void GBufferFillPassFeedback::Init(ShaderFactory& shaderFactory, const CreateParameters& params)
{
    m_SupportedViewTypes = ViewType::PLANAR;
    if (params.enableSinglePassCubemap)
        m_SupportedViewTypes = ViewType::Enum(m_SupportedViewTypes | ViewType::CUBEMAP);
    
    m_VertexShader = CreateVertexShader(shaderFactory, params);
    m_InputLayout = CreateInputLayout(m_VertexShader, params);
    m_GeometryShader = CreateGeometryShader(shaderFactory, params);
    m_PixelShaderFeedback = CreatePixelShader(shaderFactory, params, true, false);
    m_PixelShaderFeedbackAlphaTested = CreatePixelShader(shaderFactory, params, true, true);
    m_PixelShader = CreatePixelShader(shaderFactory, params, false, false);
    m_PixelShaderAlphaTested = CreatePixelShader(shaderFactory, params, false, true);

    m_MaterialBindings = CreateMaterialBindingCache(*m_CommonPasses);

    m_GBufferCB = m_Device->createBuffer(nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(GBufferFillConstants), "GBufferFillConstants", params.numConstantBufferVersions));
    m_GlobalCB = m_Device->createBuffer(nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(GlobalConstants), "GlobalConstants", params.numConstantBufferVersions));
    CreateViewBindings(m_ViewBindingLayout, m_ViewBindings, params);

    m_EnableDepthWrite = params.enableDepthWrite;
    m_StencilWriteMask = params.stencilWriteMask;
}

void GBufferFillPassFeedback::ResetBindingCache() const
{
    m_MaterialBindings->Clear();
}

nvrhi::ShaderHandle GBufferFillPassFeedback::CreateVertexShader(ShaderFactory& shaderFactory, const CreateParameters& params)
{
    std::vector<ShaderMacro> VertexShaderMacros;
    VertexShaderMacros.push_back(ShaderMacro("MOTION_VECTORS", params.enableMotionVectors ? "1" : "0"));
    return shaderFactory.CreateShader("app/gbufferfeedback_vs.hlsl", "main", &VertexShaderMacros, nvrhi::ShaderType::Vertex);
}

nvrhi::ShaderHandle GBufferFillPassFeedback::CreateGeometryShader(ShaderFactory& shaderFactory, const CreateParameters& params)
{

    ShaderMacro MotionVectorsMacro("MOTION_VECTORS", params.enableMotionVectors ? "1" : "0");

    if (params.enableSinglePassCubemap)
    {
        // MVs will not work with cubemap views because:
        // 1. cubemap_gs does not pass through the previous position attribute;
        // 2. Computing correct MVs for a cubemap is complicated and not implemented.
        assert(!params.enableMotionVectors);

        nvrhi::ShaderDesc desc(nvrhi::ShaderType::Geometry);
        desc.fastGSFlags = nvrhi::FastGeometryShaderFlags(
            nvrhi::FastGeometryShaderFlags::ForceFastGS |
            nvrhi::FastGeometryShaderFlags::UseViewportMask |
            nvrhi::FastGeometryShaderFlags::OffsetTargetIndexByViewportIndex);

        desc.pCoordinateSwizzling = CubemapView::GetCubemapCoordinateSwizzle();

        return shaderFactory.CreateShader("donut/passes/cubemap_gs.hlsl", "main", nullptr, desc);
    }
    else
    {
        return nullptr;
    }
}

nvrhi::ShaderHandle GBufferFillPassFeedback::CreatePixelShader(ShaderFactory& shaderFactory, const CreateParameters& params, bool writeFeedback, bool alphaTested)
{
    std::vector<ShaderMacro> PixelShaderMacros;
    PixelShaderMacros.push_back(ShaderMacro("MOTION_VECTORS", params.enableMotionVectors ? "1" : "0"));
    PixelShaderMacros.push_back(ShaderMacro("ALPHA_TESTED", alphaTested ? "1" : "0"));
    PixelShaderMacros.push_back(ShaderMacro("WRITEFEEDBACK", writeFeedback ? "1" : "0"));

    return shaderFactory.CreateShader("app/gbufferfeedback_ps.hlsl", "main", &PixelShaderMacros, nvrhi::ShaderType::Pixel);
}

nvrhi::InputLayoutHandle GBufferFillPassFeedback::CreateInputLayout(nvrhi::IShader* vertexShader, const CreateParameters& params)
{
    std::vector<nvrhi::VertexAttributeDesc> inputDescs =
    {
        GetVertexAttributeDesc(VertexAttribute::Position, "POS", 0),
        GetVertexAttributeDesc(VertexAttribute::PrevPosition, "PREV_POS", 1),
        GetVertexAttributeDesc(VertexAttribute::TexCoord1, "TEXCOORD", 2),
        GetVertexAttributeDesc(VertexAttribute::Normal, "NORMAL", 3),
        GetVertexAttributeDesc(VertexAttribute::Tangent, "TANGENT", 4),
        GetVertexAttributeDesc(VertexAttribute::Transform, "TRANSFORM", 5),
    };
    if (params.enableMotionVectors)
    {
        inputDescs.push_back(GetVertexAttributeDesc(VertexAttribute::PrevTransform, "PREV_TRANSFORM", 5));
    }

    return m_Device->createInputLayout(inputDescs.data(), static_cast<uint32_t>(inputDescs.size()), vertexShader);
}

void GBufferFillPassFeedback::CreateViewBindings(nvrhi::BindingLayoutHandle& layout, nvrhi::BindingSetHandle& set, const CreateParameters& params)
{
    nvrhi::BindingSetDesc bindingSetDesc;
    bindingSetDesc.bindings = {
        nvrhi::BindingSetItem::ConstantBuffer(1, m_GBufferCB),
        nvrhi::BindingSetItem::ConstantBuffer(3, m_GlobalCB)
    };

    bindingSetDesc.trackLiveness = params.trackLiveness;

    nvrhi::utils::CreateBindingSetAndLayout(m_Device, nvrhi::ShaderType::All, 0, bindingSetDesc, layout, set);
}

nvrhi::GraphicsPipelineHandle GBufferFillPassFeedback::CreateGraphicsPipeline(PipelineKey key, nvrhi::IFramebuffer* sampleFramebuffer)
{
    nvrhi::GraphicsPipelineDesc pipelineDesc;
    pipelineDesc.inputLayout = m_InputLayout;
    pipelineDesc.VS = m_VertexShader;
    pipelineDesc.GS = m_GeometryShader;
    pipelineDesc.renderState.rasterState
        .setFrontCounterClockwise(key.bits.frontCounterClockwise)
        .setCullMode(key.bits.cullMode);
    pipelineDesc.renderState.blendState.disableAlphaToCoverage();
    pipelineDesc.bindingLayouts = { m_MaterialBindings->GetLayout(), m_ViewBindingLayout };

    pipelineDesc.renderState.depthStencilState
        .setDepthWriteEnable(m_EnableDepthWrite)
        .setDepthFunc(key.bits.reverseDepth
            ? nvrhi::ComparisonFunc::GreaterOrEqual
            : nvrhi::ComparisonFunc::LessOrEqual);
        
    if (m_StencilWriteMask)
    {
        pipelineDesc.renderState.depthStencilState
            .enableStencil()
            .setStencilReadMask(0)
            .setStencilWriteMask(uint8_t(m_StencilWriteMask))
            .setStencilRefValue(uint8_t(m_StencilWriteMask))
            .setFrontFaceStencil(nvrhi::DepthStencilState::StencilOpDesc().setPassOp(nvrhi::StencilOp::Replace))
            .setBackFaceStencil(nvrhi::DepthStencilState::StencilOpDesc().setPassOp(nvrhi::StencilOp::Replace));
    }

    if (key.bits.alphaTested)
    {
        pipelineDesc.renderState.rasterState.setCullNone();

        if (m_PixelShaderAlphaTested)
        {
            pipelineDesc.PS = m_PixelShaderAlphaTested;
        }
        else
        {
            pipelineDesc.PS = m_PixelShader;
            pipelineDesc.renderState.blendState.alphaToCoverageEnable = true;
        }
    }
    else
    {
        pipelineDesc.PS = m_PixelShader;
    }

    if (key.bits.writeFeedback)
    {
        if (pipelineDesc.PS == m_PixelShader)
        {
            pipelineDesc.PS = m_PixelShaderFeedback;
        }
        else if (pipelineDesc.PS == m_PixelShaderAlphaTested)
        {
            pipelineDesc.PS = m_PixelShaderFeedbackAlphaTested;
        }
    }

    return m_Device->createGraphicsPipeline(pipelineDesc, sampleFramebuffer);
}

std::shared_ptr<MaterialBindingCacheFeedback> GBufferFillPassFeedback::CreateMaterialBindingCache(CommonRenderPasses& commonPasses)
{
    std::vector<MaterialResourceBindingFeedback> materialBindings = {
        { MaterialResourceFeedback::ConstantBuffer, 0 },
        { MaterialResourceFeedback::DiffuseTexture, 0 },
        { MaterialResourceFeedback::SpecularTexture, 1 },
        { MaterialResourceFeedback::NormalTexture, 2 },
        { MaterialResourceFeedback::EmissiveTexture, 3 },
        { MaterialResourceFeedback::OcclusionTexture, 4 },
        { MaterialResourceFeedback::TransmissionTexture, 5 },
        { MaterialResourceFeedback::DiffuseTextureFeedback, 0 },
        { MaterialResourceFeedback::SpecularTextureFeedback, 1 },
        { MaterialResourceFeedback::NormalTextureFeedback, 2 },
        { MaterialResourceFeedback::EmissiveTextureFeedback, 3 },
        { MaterialResourceFeedback::OcclusionTextureFeedback, 4 },
        { MaterialResourceFeedback::TransmissionTextureFeedback, 5 },
        { MaterialResourceFeedback::DiffuseTextureMinMip, 6 },
        { MaterialResourceFeedback::SpecularTextureMinMip, 7 },
        { MaterialResourceFeedback::NormalTextureMinMip, 8 },
        { MaterialResourceFeedback::EmissiveTextureMinMip, 9 },
        { MaterialResourceFeedback::OcclusionTextureMinMip, 10 },
        { MaterialResourceFeedback::TransmissionTextureMinMip, 11 },
        { MaterialResourceFeedback::Sampler, 0 },
        { MaterialResourceFeedback::SamplerMinMip, 1 },
    };

    auto samplerDesc = commonPasses.m_AnisotropicWrapSampler->getDesc();
    samplerDesc.reductionType = nvrhi::SamplerReductionType::Maximum;
    auto samplerMinMip = m_Device->createSampler(samplerDesc);

    return std::make_shared<MaterialBindingCacheFeedback>(
        m_Device,
        m_feedbackMaps,
        nvrhi::ShaderType::Pixel,
        /* registerSpace = */ 0,
        materialBindings,
        commonPasses.m_AnisotropicWrapSampler,
        samplerMinMip);
}

ViewType::Enum GBufferFillPassFeedback::GetSupportedViewTypes() const
{
    return m_SupportedViewTypes;
}

void GBufferFillPassFeedback::SetupView(GeometryPassContext& abstractContext, nvrhi::ICommandList* commandList, const donut::engine::IView* view, const donut::engine::IView* viewPrev)
{
    auto& context = static_cast<Context&>(abstractContext);
    
    GBufferFillConstants gbufferConstants = {};
    view->FillPlanarViewConstants(gbufferConstants.view);
    viewPrev->FillPlanarViewConstants(gbufferConstants.viewPrev);
    commandList->writeBuffer(m_GBufferCB, &gbufferConstants, sizeof(gbufferConstants));

    GlobalConstants globalConstants = {};
    globalConstants.frameIndex = m_frameIndex;
    globalConstants.showUnmappedRegions = m_showUnmappedRegions;
    globalConstants.feedbackThreshold = m_feedbackThreshold;
    globalConstants.enableDebug = m_enableDebug;
    commandList->writeBuffer(m_GlobalCB, &globalConstants, sizeof(globalConstants));

    context.keyTemplate.bits.frontCounterClockwise = view->IsMirrored();
    context.keyTemplate.bits.reverseDepth = view->IsReverseDepth();
}

bool GBufferFillPassFeedback::SetupMaterial(GeometryPassContext& abstractContext, const donut::engine::Material* material, nvrhi::RasterCullMode cullMode, nvrhi::GraphicsState& state)
{
    auto& context = static_cast<Context&>(abstractContext);
    
    PipelineKey key = context.keyTemplate;
    key.bits.cullMode = cullMode;

    key.bits.writeFeedback = m_writeFeedback;

    switch (material->domain)
    {
    case MaterialDomain::Opaque:
    case MaterialDomain::AlphaBlended: // Blended and transmissive domains are for the material ID pass, shouldn't be used otherwise
    case MaterialDomain::Transmissive:
    case MaterialDomain::TransmissiveAlphaTested:
    case MaterialDomain::TransmissiveAlphaBlended:
        key.bits.alphaTested = false;
        break;
    case MaterialDomain::AlphaTested:
        key.bits.alphaTested = true;
        break;
    default:
        return false;
    }

    nvrhi::IBindingSet* materialBindingSet = m_MaterialBindings->GetMaterialBindingSet(material);

    if (!materialBindingSet)
        return false;

    nvrhi::GraphicsPipelineHandle& pipeline = m_Pipelines[key.value];

    if (!pipeline)
    {
        std::lock_guard<std::mutex> lockGuard(m_Mutex);

        if (!pipeline)
            pipeline = CreateGraphicsPipeline(key, state.framebuffer);

        if (!pipeline)
            return false;
    }

    assert(pipeline->getFramebufferInfo() == state.framebuffer->getFramebufferInfo());

    state.pipeline = pipeline;
    state.bindings = { materialBindingSet, m_ViewBindings };

    return true;
}

void GBufferFillPassFeedback::SetupInputBuffers(GeometryPassContext& context, const donut::engine::BufferGroup* buffers, nvrhi::GraphicsState& state)
{
    state.vertexBuffers = {
        { buffers->vertexBuffer, 0, buffers->getVertexBufferRange(VertexAttribute::Position).byteOffset },
        { buffers->vertexBuffer, 1, buffers->getVertexBufferRange(VertexAttribute::PrevPosition).byteOffset },
        { buffers->vertexBuffer, 2, buffers->getVertexBufferRange(VertexAttribute::TexCoord1).byteOffset },
        { buffers->vertexBuffer, 3, buffers->getVertexBufferRange(VertexAttribute::Normal).byteOffset },
        { buffers->vertexBuffer, 4, buffers->getVertexBufferRange(VertexAttribute::Tangent).byteOffset },
        { buffers->instanceBuffer, 5, 0 }
    };

    state.indexBuffer = { buffers->indexBuffer, nvrhi::Format::R32_UINT, 0 };
}

nvrhi::ShaderHandle MaterialIDPassFeedback::CreatePixelShader(donut::engine::ShaderFactory& shaderFactory, const CreateParameters& params, bool writeFeedback, bool alphaTested)
{
    std::vector<ShaderMacro> PixelShaderMacros;

    return shaderFactory.CreateShader("app/material_id_ps", "main", &PixelShaderMacros, nvrhi::ShaderType::Pixel);
}
