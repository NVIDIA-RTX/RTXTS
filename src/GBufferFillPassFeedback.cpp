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

#include <donut/render/DrawStrategy.h>
#include <donut/engine/FramebufferFactory.h>
#include <donut/engine/ShaderFactory.h>
#include <donut/engine/ShadowMap.h>
#include <donut/engine/SceneTypes.h>
#include <donut/engine/CommonRenderPasses.h>
#include <donut/core/log.h>
#include <nvrhi/utils.h>
#include <utility>

#if DONUT_WITH_STATIC_SHADERS
#if DONUT_WITH_DX11
#include "compiled_shaders/passes/cubemap_gs.dxbc.h"
#include "compiled_shaders/passes/gbuffer_ps.dxbc.h"
#include "compiled_shaders/passes/gbuffer_vs_input_assembler.dxbc.h"
#include "compiled_shaders/passes/gbuffer_vs_buffer_loads.dxbc.h"
#include "compiled_shaders/passes/material_id_ps.dxbc.h"
#endif
#if DONUT_WITH_DX12
#include "compiled_shaders/passes/cubemap_gs.dxil.h"
#include "compiled_shaders/passes/gbuffer_ps.dxil.h"
#include "compiled_shaders/passes/gbuffer_vs_input_assembler.dxil.h"
#include "compiled_shaders/passes/gbuffer_vs_buffer_loads.dxil.h"
#include "compiled_shaders/passes/material_id_ps.dxil.h"
#endif
#if DONUT_WITH_VULKAN
#include "compiled_shaders/passes/cubemap_gs.spirv.h"
#include "compiled_shaders/passes/gbuffer_ps.spirv.h"
#include "compiled_shaders/passes/gbuffer_vs_input_assembler.spirv.h"
#include "compiled_shaders/passes/gbuffer_vs_buffer_loads.spirv.h"
#include "compiled_shaders/passes/material_id_ps.spirv.h"
#endif
#endif

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
    m_IsDX11 = m_Device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D11;
}

void GBufferFillPassFeedback::Init(ShaderFactory& shaderFactory, const CreateParameters& params)
{
    m_EnableMotionVectors = params.enableMotionVectors;
    m_UseInputAssembler = params.useInputAssembler;

    m_SupportedViewTypes = ViewType::PLANAR;
    if (params.enableSinglePassCubemap)
        m_SupportedViewTypes = ViewType::Enum(m_SupportedViewTypes | ViewType::CUBEMAP);

    m_VertexShader = CreateVertexShader(shaderFactory, params);
    m_InputLayout = CreateInputLayout(m_VertexShader, params);
    m_GeometryShader = CreateGeometryShader(shaderFactory, params);
    m_PixelShader = CreatePixelShader(shaderFactory, params, false, false);
    m_PixelShaderAlphaTested = CreatePixelShader(shaderFactory, params, false, true);
    m_PixelShaderFeedback = CreatePixelShader(shaderFactory, params, true, false);
    m_PixelShaderFeedbackAlphaTested = CreatePixelShader(shaderFactory, params, true, true);

    m_MaterialBindings = CreateMaterialBindingCache(*m_CommonPasses);

    m_GBufferCB = m_Device->createBuffer(nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(GBufferFillConstants),
        "GBufferFillConstants", params.numConstantBufferVersions));
    m_GlobalCB = m_Device->createBuffer(nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(GlobalConstants),
        "GlobalConstants", params.numConstantBufferVersions));

    CreateViewBindings(m_ViewBindingLayout, m_ViewBindings, params);

    m_EnableDepthWrite = params.enableDepthWrite;
    m_StencilWriteMask = params.stencilWriteMask;

    m_InputBindingLayout = CreateInputBindingLayout();
}

void GBufferFillPassFeedback::ResetBindingCache()
{
    m_MaterialBindings->Clear();
    m_InputBindingSets.clear();
}

nvrhi::ShaderHandle GBufferFillPassFeedback::CreateVertexShader(ShaderFactory& shaderFactory, const CreateParameters& params)
{
    char const* sourceFileName = "app/gbufferfeedback_vs.hlsl";

    std::vector<ShaderMacro> VertexShaderMacros;
    VertexShaderMacros.push_back(ShaderMacro("MOTION_VECTORS", params.enableMotionVectors ? "1" : "0"));

    if (params.useInputAssembler)
    {
        return shaderFactory.CreateAutoShader(sourceFileName, "input_assembler",
            DONUT_MAKE_PLATFORM_SHADER(g_gbuffer_vs_input_assembler), &VertexShaderMacros, nvrhi::ShaderType::Vertex);
    }
    else
    {
        return shaderFactory.CreateAutoShader(sourceFileName, "buffer_loads",
            DONUT_MAKE_PLATFORM_SHADER(g_gbuffer_vs_buffer_loads), &VertexShaderMacros, nvrhi::ShaderType::Vertex);
    }
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

        auto desc = nvrhi::ShaderDesc()
            .setShaderType(nvrhi::ShaderType::Geometry)
            .setFastGSFlags(nvrhi::FastGeometryShaderFlags(
                nvrhi::FastGeometryShaderFlags::ForceFastGS |
                nvrhi::FastGeometryShaderFlags::UseViewportMask |
                nvrhi::FastGeometryShaderFlags::OffsetTargetIndexByViewportIndex))
            .setCoordinateSwizzling(CubemapView::GetCubemapCoordinateSwizzle());

        return shaderFactory.CreateAutoShader("donut/passes/cubemap_gs.hlsl", "main", DONUT_MAKE_PLATFORM_SHADER(g_cubemap_gs), nullptr, desc);
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

    return shaderFactory.CreateAutoShader("app/gbufferfeedback_ps.hlsl", "main", DONUT_MAKE_PLATFORM_SHADER(g_gbuffer_ps), &PixelShaderMacros, nvrhi::ShaderType::Pixel);
}

nvrhi::InputLayoutHandle GBufferFillPassFeedback::CreateInputLayout(nvrhi::IShader* vertexShader, const CreateParameters& params)
{
    if (params.useInputAssembler)
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

    return nullptr;
}

void GBufferFillPassFeedback::CreateViewBindings(nvrhi::BindingLayoutHandle& layout, nvrhi::BindingSetHandle& set, const CreateParameters& params)
{
    auto bindingLayoutDesc = nvrhi::BindingLayoutDesc()
        .setVisibility(nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel)
        .setRegisterSpace(m_IsDX11 ? 0 : GBUFFER_SPACE_VIEW)
        .setRegisterSpaceIsDescriptorSet(!m_IsDX11)
        .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(GBUFFER_BINDING_VIEW_CONSTANTS))
        .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(GBUFFER_BINDING_GLOBAL_CONSTANTS))
        .addItem(nvrhi::BindingLayoutItem::Sampler(GBUFFER_BINDING_MATERIAL_SAMPLER))
        .addItem(nvrhi::BindingLayoutItem::Sampler(GBUFFER_BINDING_MATERIAL_SAMPLER_MINMIP));

    layout = m_Device->createBindingLayout(bindingLayoutDesc);

    auto samplerDesc = m_CommonPasses->m_AnisotropicWrapSampler->getDesc();
    samplerDesc.reductionType = nvrhi::SamplerReductionType::Maximum;
    m_samplerMinMip = m_Device->createSampler(samplerDesc);

    auto bindingSetDesc = nvrhi::BindingSetDesc()
        .setTrackLiveness(params.trackLiveness)
        .addItem(nvrhi::BindingSetItem::ConstantBuffer(GBUFFER_BINDING_VIEW_CONSTANTS, m_GBufferCB))
        .addItem(nvrhi::BindingSetItem::ConstantBuffer(GBUFFER_BINDING_GLOBAL_CONSTANTS, m_GlobalCB))
        .addItem(nvrhi::BindingSetItem::Sampler(GBUFFER_BINDING_MATERIAL_SAMPLER,
            m_CommonPasses->m_AnisotropicWrapSampler))
        .addItem(nvrhi::BindingSetItem::Sampler(GBUFFER_BINDING_MATERIAL_SAMPLER_MINMIP,
            m_samplerMinMip));

    set = m_Device->createBindingSet(bindingSetDesc, layout);
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
    if (!m_UseInputAssembler)
        pipelineDesc.bindingLayouts.push_back(m_InputBindingLayout);

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
        { MaterialResourceFeedback::ConstantBuffer,              GBUFFER_BINDING_MATERIAL_CONSTANTS },
        { MaterialResourceFeedback::ConstantBufferFeedback,      GBUFFER_BINDING_FEEDBACK_CONSTANTS },
        { MaterialResourceFeedback::DiffuseTexture,              GBUFFER_BINDING_MATERIAL_DIFFUSE_TEXTURE },
        { MaterialResourceFeedback::SpecularTexture,             GBUFFER_BINDING_MATERIAL_SPECULAR_TEXTURE },
        { MaterialResourceFeedback::NormalTexture,               GBUFFER_BINDING_MATERIAL_NORMAL_TEXTURE },
        { MaterialResourceFeedback::EmissiveTexture,             GBUFFER_BINDING_MATERIAL_EMISSIVE_TEXTURE },
        { MaterialResourceFeedback::OcclusionTexture,            GBUFFER_BINDING_MATERIAL_OCCLUSION_TEXTURE },
        { MaterialResourceFeedback::TransmissionTexture,         GBUFFER_BINDING_MATERIAL_TRANSMISSION_TEXTURE },
        { MaterialResourceFeedback::DiffuseTextureFeedback,      GBUFFER_BINDING_MATERIAL_DIFFUSE_FEEDBACKTEXTURE },
        { MaterialResourceFeedback::SpecularTextureFeedback,     GBUFFER_BINDING_MATERIAL_SPECULAR_FEEDBACKTEXTURE },
        { MaterialResourceFeedback::NormalTextureFeedback,       GBUFFER_BINDING_MATERIAL_NORMAL_FEEDBACKTEXTURE },
        { MaterialResourceFeedback::EmissiveTextureFeedback,     GBUFFER_BINDING_MATERIAL_EMISSIVE_FEEDBACKTEXTURE },
        { MaterialResourceFeedback::OcclusionTextureFeedback,    GBUFFER_BINDING_MATERIAL_OCCLUSION_FEEDBACKTEXTURE },
        { MaterialResourceFeedback::TransmissionTextureFeedback, GBUFFER_BINDING_MATERIAL_TRANSMISSION_FEEDBACKTEXTURE },
        { MaterialResourceFeedback::DiffuseTextureMinMip,        GBUFFER_BINDING_MATERIAL_DIFFUSE_MINMIPTEXTURE },
        { MaterialResourceFeedback::SpecularTextureMinMip,       GBUFFER_BINDING_MATERIAL_SPECULAR_MINMIPTEXTURE },
        { MaterialResourceFeedback::NormalTextureMinMip,         GBUFFER_BINDING_MATERIAL_NORMAL_MINMIPTEXTURE },
        { MaterialResourceFeedback::EmissiveTextureMinMip,       GBUFFER_BINDING_MATERIAL_EMISSIVE_MINMIPTEXTURE },
        { MaterialResourceFeedback::OcclusionTextureMinMip,      GBUFFER_BINDING_MATERIAL_OCCLUSION_MINMIPTEXTURE },
        { MaterialResourceFeedback::TransmissionTextureMinMip,   GBUFFER_BINDING_MATERIAL_TRANSMISSION_MINMIPTEXTURE },
    };

    return std::make_shared<MaterialBindingCacheFeedback>(
        m_Device,
        m_feedbackMaps,
        nvrhi::ShaderType::Pixel,
        /* registerSpace = */ m_IsDX11 ? 0 : GBUFFER_SPACE_MATERIAL,
        /* registerSpaceIsDescriptorSet = */ !m_IsDX11,
        materialBindings,
        commonPasses.m_AnisotropicWrapSampler,
        m_samplerMinMip);
}

ViewType::Enum GBufferFillPassFeedback::GetSupportedViewTypes() const
{
    return m_SupportedViewTypes;
}

void GBufferFillPassFeedback::SetupView(GeometryPassContext& abstractContext, nvrhi::ICommandList* commandList, const engine::IView* view, const engine::IView* viewPrev)
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

bool GBufferFillPassFeedback::SetupMaterial(GeometryPassContext& abstractContext, const engine::Material* material, nvrhi::RasterCullMode cullMode, nvrhi::GraphicsState& state)
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

    nvrhi::BufferHandle materialConstantsFeedback = m_feedbackMaps->m_materialConstantsFeedback[material];

    nvrhi::IBindingSet* materialBindingSet = m_MaterialBindings->GetMaterialBindingSet(material, materialConstantsFeedback);

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

    if (!m_UseInputAssembler)
        state.bindings.push_back(context.inputBindingSet);

    return true;
}

void GBufferFillPassFeedback::SetupInputBuffers(GeometryPassContext& abstractContext, const engine::BufferGroup* buffers, nvrhi::GraphicsState& state)
{
    auto& context = static_cast<Context&>(abstractContext);

    state.indexBuffer = { buffers->indexBuffer, nvrhi::Format::R32_UINT, 0 };

    if (m_UseInputAssembler)
    {
        state.vertexBuffers = {
            { buffers->vertexBuffer, 0, buffers->getVertexBufferRange(VertexAttribute::Position).byteOffset },
            { buffers->vertexBuffer, 1, buffers->getVertexBufferRange(VertexAttribute::PrevPosition).byteOffset },
            { buffers->vertexBuffer, 2, buffers->getVertexBufferRange(VertexAttribute::TexCoord1).byteOffset },
            { buffers->vertexBuffer, 3, buffers->getVertexBufferRange(VertexAttribute::Normal).byteOffset },
            { buffers->vertexBuffer, 4, buffers->getVertexBufferRange(VertexAttribute::Tangent).byteOffset },
            { buffers->instanceBuffer, 5, 0 }
        };
    }
    else
    {
        context.inputBindingSet = GetOrCreateInputBindingSet(buffers);
        context.positionOffset = uint32_t(buffers->getVertexBufferRange(VertexAttribute::Position).byteOffset);
        context.prevPositionOffset = uint32_t(buffers->getVertexBufferRange(VertexAttribute::PrevPosition).byteOffset);
        context.texCoordOffset = uint32_t(buffers->getVertexBufferRange(VertexAttribute::TexCoord1).byteOffset);
        context.normalOffset = uint32_t(buffers->getVertexBufferRange(VertexAttribute::Normal).byteOffset);
        context.tangentOffset = uint32_t(buffers->getVertexBufferRange(VertexAttribute::Tangent).byteOffset);
    }
}

nvrhi::BindingLayoutHandle GBufferFillPassFeedback::CreateInputBindingLayout()
{
    if (m_UseInputAssembler)
        return nullptr;

    auto bindingLayoutDesc = nvrhi::BindingLayoutDesc()
        .setVisibility(nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel)
        .setRegisterSpace(m_IsDX11 ? 0 : GBUFFER_SPACE_INPUT)
        .setRegisterSpaceIsDescriptorSet(!m_IsDX11)
        .addItem(m_IsDX11
            ? nvrhi::BindingLayoutItem::RawBuffer_SRV(GBUFFER_BINDING_INSTANCE_BUFFER)
            : nvrhi::BindingLayoutItem::StructuredBuffer_SRV(GBUFFER_BINDING_INSTANCE_BUFFER))
        .addItem(nvrhi::BindingLayoutItem::RawBuffer_SRV(GBUFFER_BINDING_VERTEX_BUFFER))
        .addItem(nvrhi::BindingLayoutItem::PushConstants(GBUFFER_BINDING_PUSH_CONSTANTS, sizeof(GBufferPushConstants)));

    return m_Device->createBindingLayout(bindingLayoutDesc);
}

nvrhi::BindingSetHandle GBufferFillPassFeedback::CreateInputBindingSet(const BufferGroup* bufferGroup)
{
    auto bindingSetDesc = nvrhi::BindingSetDesc()
        .addItem(m_IsDX11
            ? nvrhi::BindingSetItem::RawBuffer_SRV(GBUFFER_BINDING_INSTANCE_BUFFER, bufferGroup->instanceBuffer)
            : nvrhi::BindingSetItem::StructuredBuffer_SRV(GBUFFER_BINDING_INSTANCE_BUFFER, bufferGroup->instanceBuffer))
        .addItem(nvrhi::BindingSetItem::RawBuffer_SRV(GBUFFER_BINDING_VERTEX_BUFFER, bufferGroup->vertexBuffer))
        .addItem(nvrhi::BindingSetItem::PushConstants(GBUFFER_BINDING_PUSH_CONSTANTS, sizeof(GBufferPushConstants)));

    return m_Device->createBindingSet(bindingSetDesc, m_InputBindingLayout);
}

nvrhi::BindingSetHandle GBufferFillPassFeedback::GetOrCreateInputBindingSet(const BufferGroup* bufferGroup)
{
    auto it = m_InputBindingSets.find(bufferGroup);
    if (it == m_InputBindingSets.end())
    {
        auto bindingSet = CreateInputBindingSet(bufferGroup);
        m_InputBindingSets[bufferGroup] = bindingSet;
        return bindingSet;
    }

    return it->second;
}

void GBufferFillPassFeedback::SetPushConstants(
    donut::render::GeometryPassContext& abstractContext,
    nvrhi::ICommandList* commandList,
    nvrhi::GraphicsState& state,
    nvrhi::DrawArguments& args)
{
    if (m_UseInputAssembler)
        return;

    auto& context = static_cast<Context&>(abstractContext);

    GBufferPushConstants constants;
    constants.startInstanceLocation = args.startInstanceLocation;
    constants.startVertexLocation = args.startVertexLocation;
    constants.positionOffset = context.positionOffset;
    constants.prevPositionOffset = context.prevPositionOffset;
    constants.texCoordOffset = context.texCoordOffset;
    constants.normalOffset = context.normalOffset;
    constants.tangentOffset = context.tangentOffset;

    commandList->setPushConstants(&constants, sizeof(constants));

    args.startInstanceLocation = 0;
    args.startVertexLocation = 0;
}

void MaterialIDPassFeedback::Init(
    engine::ShaderFactory& shaderFactory,
    const CreateParameters& params)
{
    CreateParameters paramsCopy = params;
    // The material ID pass relies on the push constants filled by the buffer load path (firstInstance)
    paramsCopy.useInputAssembler = false;
    // The material ID pass doesn't support generating motion vectors
    paramsCopy.enableMotionVectors = false;

    GBufferFillPassFeedback::Init(shaderFactory, paramsCopy);
}

nvrhi::ShaderHandle MaterialIDPassFeedback::CreatePixelShader(engine::ShaderFactory& shaderFactory, const CreateParameters& params, bool writeFeedback, bool alphaTested)
{
    std::vector<ShaderMacro> PixelShaderMacros;
    PixelShaderMacros.push_back(ShaderMacro("ALPHA_TESTED", alphaTested ? "1" : "0"));

    return shaderFactory.CreateAutoShader("donut/passes/material_id_ps.hlsl", "main",
        DONUT_MAKE_PLATFORM_SHADER(g_material_id_ps), &PixelShaderMacros, nvrhi::ShaderType::Pixel);
}
