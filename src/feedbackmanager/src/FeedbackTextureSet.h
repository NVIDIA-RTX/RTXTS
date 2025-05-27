#pragma once

#include "FeedbackTexture.h"
#include <nvrhi/nvrhi.h>
#include <memory>
#include <vector>
#include <atomic>

namespace nvfeedback
{
    class FeedbackManagerImpl;

    class FeedbackTextureSetImpl : public FeedbackTextureSet
    {
    public:
        unsigned long AddRef() override
        {
            return ++m_refCount;
        }

        unsigned long Release() override
        {
            unsigned long result = --m_refCount;
            if (result == 0) {
                delete this;
            }
            return result;
        }

        // New constructor for creating an empty set
        FeedbackTextureSetImpl(
            FeedbackManagerImpl* feedbackManager,
            nvrhi::IDevice* device,
            uint32_t numReadbacks);
        ~FeedbackTextureSetImpl();

        uint32_t GetNumTextures() const override { return (uint32_t)m_textures.size(); }
        
        void SetPrimaryTextureIndex(uint32_t index) override;
        uint32_t GetPrimaryTextureIndex() const override { return m_primaryTextureIndex; }

        FeedbackTexture* GetTexture(uint32_t index) override;
        FeedbackTexture* GetPrimaryTexture() const override { return m_textures[m_primaryTextureIndex]; }
        
        bool AddTexture(FeedbackTexture* texture) override;
        bool RemoveTexture(FeedbackTexture* texture) override;

    private:
        nvrhi::IDevice* m_device;
        FeedbackManagerImpl* m_feedbackManager;
        std::vector<FeedbackTextureImpl*> m_textures;
        uint32_t m_primaryTextureIndex;

        std::atomic<unsigned long> m_refCount;
        
        // Method to update the follower textures vector
        void UpdateTextures() const;
        
        void UpdateTextureState();
    };
}
