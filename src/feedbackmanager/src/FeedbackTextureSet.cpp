#include "FeedbackTextureSet.h"
#include "FeedbackTexture.h"
#include "FeedbackManagerInternal.h"

namespace nvfeedback
{
    FeedbackTextureSetImpl::FeedbackTextureSetImpl(
        FeedbackManagerImpl* feedbackManager,
        nvrhi::IDevice* device,
        uint32_t numReadbacks)
        : m_device(device)
        , m_feedbackManager(feedbackManager)
        , m_refCount(1)
        , m_primaryTextureIndex(0) // Default to the first texture as primary
    {
        // Empty set
    }

    FeedbackTextureSetImpl::~FeedbackTextureSetImpl()
    {
        for (auto& texture : m_textures)
        {
            if (texture)
            {
                texture->RemoveFromTextureSet(this);
            }
        }
        m_textures.clear();
    }

    void FeedbackTextureSetImpl::SetPrimaryTextureIndex(uint32_t index)
    {
        if (index >= m_textures.size())
        {
            return;
        }

        m_primaryTextureIndex = index;

        UpdateTextures();
    }

    FeedbackTexture* FeedbackTextureSetImpl::GetTexture(uint32_t index)
    {
        if (index >= m_textures.size())
        {
            return nullptr;
        }
        return m_textures[index];
    }

    void FeedbackTextureSetImpl::UpdateTextures() const
    {
        for (uint32_t i = 0; i < m_textures.size(); ++i)
        {
            m_textures[i]->UpdateTextureSets();
        }
    }
    
    bool FeedbackTextureSetImpl::AddTexture(FeedbackTexture* texture)
    {
        if (!texture)
        {
            return false;
        }
        
        // Add the texture to the set
        FeedbackTextureImpl* textureImpl = static_cast<FeedbackTextureImpl*>(texture);
        m_textures.push_back(textureImpl);
        textureImpl->AddToTextureSet(this);
        UpdateTextures();
        return true;
    }
    
    bool FeedbackTextureSetImpl::RemoveTexture(FeedbackTexture* texture)
    {
        if (!texture)
        {
            return false;
        }
        
        FeedbackTextureImpl* textureImpl = static_cast<FeedbackTextureImpl*>(texture);
        auto it = std::find(m_textures.begin(), m_textures.end(), textureImpl);
        if (it == m_textures.end())
        {
            return false;
        }
        m_textures.erase(it);
        textureImpl->RemoveFromTextureSet(this);
        UpdateTextures();
        return true;
    }
}
