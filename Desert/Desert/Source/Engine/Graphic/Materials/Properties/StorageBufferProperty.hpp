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

        void Apply( MaterialBackend* backend ) override
        {
            if ( IsDirty() )
            {
                backend->ApplyStorageBuffer( this );
                MarkClean();
            }
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

            MarkDirty(); // every slot owes itself this write
        }

        // Replace the reflection-created buffer with an externally-owned one (the particle simulation's
        // compute-written buffer, the procedural sky's parameter rows). Marks dirty so the next Apply
        // rebinds the new VkBuffer.
        void SetBuffer( const std::shared_ptr<ShaderResources::StorageBuffer>& buffer )
        {
            m_Buffer     = buffer;
            MarkDirty(); // every slot owes itself this write
        }

        const auto& GetStorageBuffer() const
        {
            return m_Buffer;
        }

    private:
        std::shared_ptr<ShaderResources::StorageBuffer> m_Buffer;
    };
} // namespace Desert::Graphic