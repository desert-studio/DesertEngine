#pragma once

#include <Common/Core/ResultStr.hpp>

#include <Engine/Graphic/Shader.hpp>
#include "BaseBuffer.hpp"

namespace Desert::ShaderResources
{
    class UniformBuffer : public BaseBuffer
    {
    public:
        // Inline on purpose: this was the type's ONLY out-of-line symbol, and its .cpp pulls in
        // RendererAPI and VulkanUniformBuffer for Create(). A test that wants a device-free buffer to
        // drive UniformBufferProperty had to link that whole chain for a member-wise copy. Create() stays
        // where it is — the backend choice genuinely belongs to the .cpp.
        // The ledger row — see Engine/Graphic/ResourceLedger.hpp. ONE UniformBuffer object is one VkBuffer
        // per (view x frame in flight) that wrote or bound it on the Vulkan backend (ViewCopiedBlock.hpp), so
        // this row stands for a number of device allocations that grows with the open views.
        explicit UniformBuffer( const ShaderLayout::UniformBuffer& uniform )
             : m_UniformModel( uniform ),
               m_Accounting( Graphic::ResourceOwnership::Take( Graphic::ResourceKind::UniformBuffer ) )
        {
        }
        virtual ~UniformBuffer() = default;

        void ClaimOwnership( const Graphic::ResourceOwner owner,
                             const Common::AssetHandle    asset = Common::AssetHandle{} )
        {
            m_Accounting.Claim( owner, asset );
        }

        virtual uint32_t GetBinding() const override final
        {
            return m_UniformModel.BindingPoint;
        }

        virtual uint32_t GetSize() const override final
        {
            return m_UniformModel.Size;
        }

        virtual const std::vector<ShaderLayout::ShaderFieldLayout>& GetFields() const override final
        {
            return m_UniformModel.Fields;
        }

        // The block name from shader reflection ("CameraUB", "MaterialUB", ...). Exists so a refusal can
        // say WHICH buffer refused: "a uniform buffer rejected a write" is not actionable, and the
        // defect this guards against was found by staring at a frame rather than at a message.
        const std::string& GetName() const
        {
            return m_UniformModel.Name;
        }

        // The field route's bookkeeping on the ACTIVE view's copy for the current frame (ViewCopiedBlock): the
        // newest field version it holds, and the note that it now holds everything up to `version`.
        [[nodiscard]] virtual uint64_t ActiveAppliedVersion() const          = 0;
        virtual void                   NoteActiveApplied( uint64_t version ) = 0;

    protected:
        ShaderLayout::UniformBuffer m_UniformModel;

    private:
        static std::shared_ptr<UniformBuffer> Create( const ShaderLayout::UniformBuffer& uniform );

        Graphic::ResourceOwnership m_Accounting;

        friend class ShaderResourcesManager;
    };

} // namespace Desert::ShaderResources