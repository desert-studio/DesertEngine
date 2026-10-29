#pragma once

#include <Engine/Graphic/Materials/Properties/MaterialProperty.hpp>

#include <Engine/ShaderResources/StorageBuffer.hpp>

namespace Desert::Graphic
{
    class StorageBufferProperty : public MaterialProperty
    {
    public:
        StorageBufferProperty( const std::shared_ptr<ShaderResources::StorageBuffer>& buffer )
             : m_Buffer( buffer ), m_Binding( buffer ? buffer->GetBinding() : 0u )
        {
        }

        // The shader slot this property fills: the reflection-created buffer's binding, kept when SetBuffer
        // replaces the buffer. An external buffer carries the binding of the program that made it (the grass
        // generator's compute slot), which is not this shader's.
        [[nodiscard]] uint32_t GetBinding() const noexcept
        {
            return m_Binding;
        }

        // The backend is asked EVERY time: whether this view's set needs a write is the set's own record
        // (DescriptorCopyRecord: the copy it points at and the version it applied), not a flag here.
        void Apply( MaterialBackend* backend ) override
        {
            backend->ApplyStorageBuffer( this );
        }

        void SetRawData( const void* data, uint32_t size )
        {
            // THE REFUSAL TERMINATES HERE, AND THAT IS A DECISION RATHER THAN AN OMISSION. Above this
            // sit ~25 `sb->SetRawData(...)` call sites in material Update() overrides that the render
            // graph calls with no channel of their own, and there is nothing any of them could do with a
            // failure that this line does not already do: a storage block is rebuilt from its owner's
            // CPU data every frame, so a refused write is retried on the next one by construction. What
            // was genuinely missing was that NOBODY WAS TOLD — including, until Г13, this line, because
            // SetData returned void. The refusal names the buffer itself (BaseBuffer has no GetName and
            // does not need one for this), which is what turns "the lighting looked wrong for one frame"
            // into a grep.
            //
            // A LENT buffer is never written through the property: it is its owner's (the scene material table,
            // a view's ObjectMotions), shared by every material it is lent to, and a write here would overwrite
            // rows other draws of the frame name. The owner writes it (GpuScene::UpdateMaterialRow, the view's
            // upload); a material that has its own data to upload takes its own buffer back with SetBuffer.
            if ( m_IsLent )
            {
                LOG_ERROR( "[SSBO] binding {}: {} byte(s) refused: this property reads a buffer lent by its owner "
                           "(LendViewBuffer: the scene material table or a view's per-frame buffer), which only "
                           "its owner writes",
                           m_Binding, size );
                return;
            }
            const auto buffer = GetStorageBuffer();
            if ( !buffer )
            {
                LOG_ERROR( "[SSBO] binding {}: {} byte(s) were not uploaded: the property has no buffer", m_Binding,
                           size );
                return;
            }
            const auto wrote = buffer->SetData( data, size );
            if ( !wrote.IsSuccess() )
                LOG_ERROR( "[SSBO] {} byte(s) were not uploaded, the shader reads the previous frame's "
                           "contents -- {}",
                           size, wrote.GetError() );

            m_Written = true;
            NoteWritten();
        }

        // Replace the reflection-created buffer with an externally-owned one (the particle simulation's
        // compute-written buffer, the procedural sky's parameter rows). Marks dirty so the next Apply
        // rebinds the new VkBuffer.
        void SetBuffer( const std::shared_ptr<ShaderResources::StorageBuffer>& buffer )
        {
            m_Buffer = buffer;
            m_Lent.reset();
            m_IsLent  = false;
            m_Written = true;
            NoteWritten();
        }

        // Bind a VIEW's per-frame buffer (ObjectMotions, ObjectBones, WorldParameters — SceneFrameBinding) without
        // taking a share of it. The material is an asset that outlives every view drawing it; holding the view's
        // buffer here kept it (and its device memory) alive past the view's close, under the closed view's ledger
        // name (EDITOR-LOG-CLEAN). The view owns it; the next view to draw this material lends its own.
        void LendViewBuffer( const std::shared_ptr<ShaderResources::StorageBuffer>& buffer )
        {
            m_Buffer.reset();
            m_Lent    = buffer;
            m_IsLent  = true;
            m_Written = true;
            NoteWritten();
        }

        // Whether anything ever filled this buffer (SetRawData / SetBuffer). The reflection-created buffer of an
        // unwritten property holds no copy to bind, so a draw would be refused at record time; the material's
        // route fill leaves it out instead (MaterialExecutor::GetRouteFill) and the pass's setup refuses the
        // block.
        // A lent buffer whose view has closed holds nothing to bind: unwritten until the next view lends one.
        [[nodiscard]] bool IsWritten() const noexcept
        {
            return m_Written && ( m_Buffer != nullptr || !m_Lent.expired() );
        }

        [[nodiscard]] std::shared_ptr<ShaderResources::StorageBuffer> GetStorageBuffer() const
        {
            return m_Buffer ? m_Buffer : m_Lent.lock();
        }

        // The buffer this property owns — never a view's lent one, which its view accounts for.
        [[nodiscard]] const std::shared_ptr<ShaderResources::StorageBuffer>& GetOwnedStorageBuffer() const noexcept
        {
            return m_Buffer;
        }

    private:
        std::shared_ptr<ShaderResources::StorageBuffer> m_Buffer;
        std::weak_ptr<ShaderResources::StorageBuffer>   m_Lent; // LendViewBuffer: the view owns it, not this
        uint32_t                                        m_Binding = 0;
        bool                                            m_Written = false;
        bool                                            m_IsLent  = false; // LendViewBuffer until SetBuffer
    };
} // namespace Desert::Graphic