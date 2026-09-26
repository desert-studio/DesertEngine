#pragma once

#include <Engine/Core/EngineContext.hpp>

#include <array>
#include <cstring>

#include <Engine/ShaderResources/ShaderReflectionTypes.hpp>
#include <Engine/Graphic/Materials/Properties/PropertyVersion.hpp>
#include <Common/Core/Memory/Buffer.hpp>

namespace Desert::Graphic
{
    class FieldProperty
    {
    public:
        FieldProperty( const ShaderResources::ShaderLayout::ShaderFieldLayout& field ) : m_Field( field )
        {
            // Born CLEAN, and that is load-bearing rather than tidiness. A field nobody has written has
            // nothing to say — but while these were born dirty, every uniform block that merely DECLARED
            // fields reported having some. Material::Bind's generic flush therefore wrote them, which
            // claimed the block as field-filled, and the engine's own whole-block write of CameraUB,
            // TimeUB and DirectionLightsUB was refused by name a moment later. Every shader-graph mesh
            // vanished from the frame while every suite stayed green, because one of them asserted the
            // defect: "fields are born dirty — if they were not, this guard would be guarding nothing".
            //
            // Born-dirty bought nothing anyway: the local data is zeroed below, so flushing an unwritten
            // field pushed zeroes. Real values arrive through ApplyDefaults and SetParamRaw, and both
            // WRITE the field, which is what makes it dirty.
            m_LocalData.Allocate( field.Size );
            // Common::Memory::Buffer::Allocate is a bare `new std::byte[]`, so without this the shadow
            // copy of a field nobody has written yet is whatever the heap last held. The fill-kind
            // refusal is what stops those bytes reaching the GPU; this is the second line — if some
            // route we have not thought of ever flushes an unwritten field, the result is a
            // deterministic black that a test can pin, not a frame that differs between runs.
            m_LocalData.ZeroInitialize();
        }

        [[nodiscard]] bool IsArray() const
        {
            return m_Field.ArraySize > 1U;
        }

        [[nodiscard]] uint32_t GetArraySize() const
        {
            return m_Field.ArraySize;
        }

        template <typename T>
        T GetValue() const
        {
            static_assert( std::is_standard_layout_v<T>, "T must be standard layout" );
            T value;
            memcpy( &value, m_LocalData.Data, sizeof( T ) );
            return value;
        }

        template <typename T>
        std::vector<T> GetArray( uint32_t count ) const
        {
            static_assert( std::is_standard_layout_v<T>, "T must be standard layout" );
            std::vector<T> result( count );
            memcpy( result.data(), m_LocalData.Data, sizeof( T ) * count );
            return result;
        }

        // The version of this field's latest write; PropertyVersion::kNeverWritten until it has one. The
        // owning buffer's copies each remember the newest version they applied, so "dirty" is a question
        // about a (view x frame) copy, not about the field (UniformBufferProperty::UpdateFields).
        [[nodiscard]] uint64_t GetVersion() const noexcept
        {
            return m_Version;
        }

        const ShaderResources::ShaderLayout::ShaderFieldLayout& GetFieldInfo() const
        {
            return m_Field;
        }

        const Common::Memory::Buffer& GetLocalData() const
        {
            return m_LocalData;
        }

    private:
        // WRITING A FIELD IS THE UNIFORM BUFFER'S BUSINESS, not the caller's. Reaching a FieldProperty
        // and writing it directly leaves the buffer that owns it believing nothing has happened, so a
        // whole-block SetRawData is still accepted afterwards and erases the value — and the reverse,
        // a buffer the engine fills whole, gets its uninitialised shadow copies flushed over live
        // camera matrices (ShaderResources::BufferFillKind.hpp). UniformBufferProperty::WriteField is
        // the one way in, and it claims the fill route in the same step.
        //
        // Private + friend rather than a comment asking people not to: the previous arrangement was a
        // comment, and the flush that emptied a frame was written by someone who had read it.
        friend class UniformBufferProperty;

        bool SetRawBytes( const void* data, size_t size )
        {
            if ( size > m_Field.Size )
                return false;
            memcpy( m_LocalData.Data, data, size );
            m_Version = PropertyVersion::Next();
            return true;
        }

        ShaderResources::ShaderLayout::ShaderFieldLayout m_Field;
        Common::Memory::Buffer                           m_LocalData;
        uint64_t                                         m_Version = PropertyVersion::kNeverWritten;
    };
} // namespace Desert::Graphic