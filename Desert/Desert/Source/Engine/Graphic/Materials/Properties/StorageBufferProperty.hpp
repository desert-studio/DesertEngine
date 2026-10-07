#pragma once

#include <Engine/Graphic/Materials/Properties/MaterialProperty.hpp>

#include <Engine/ShaderResources/StorageBuffer.hpp>

namespace Desert::Graphic
{
    class StorageBufferProperty : public MaterialProperty
    {
    public:
        StorageBufferProperty( const std::shared_ptr<ShaderResources::StorageBuffer>& buffer ) : m_Buffer( buffer )
        {
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
            const auto wrote = m_Buffer->SetData( data, size );
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
            m_Buffer  = buffer;
            m_Written = true;
            NoteWritten();
        }

        // Whether anything ever filled this buffer (SetRawData / SetBuffer). The reflection-created buffer of an
        // unwritten property holds no copy to bind, so a draw would be refused at record time; the material's route
        // fill leaves it out instead (MaterialExecutor::GetRouteFill) and the pass's setup refuses the block.
        [[nodiscard]] bool IsWritten() const noexcept
        {
            return m_Written;
        }

        const auto& GetStorageBuffer() const
        {
            return m_Buffer;
        }

    private:
        std::shared_ptr<ShaderResources::StorageBuffer> m_Buffer;
        bool                                            m_Written = false;
    };
} // namespace Desert::Graphic