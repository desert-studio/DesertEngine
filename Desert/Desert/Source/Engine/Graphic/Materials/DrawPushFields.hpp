#pragma once

#include <Engine/Graphic/InstanceWind.hpp>
#include <Engine/Graphic/Materials/MaterialBinder.hpp>

#include <Common/Core/ResultStr.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

namespace Desert::Graphic
{
    // THE PER-DRAW PUSH VALUES OF ONE MESH DRAW, BY VALUE (UE FMeshDrawCommand: the command is immutable and
    // carries what differs per draw; the shared material is never written to record it). Built in the node's
    // setup next to the draw (MeshDrawCommand::Push); MeshDrawList::Record lays them, by name through the cell's
    // reconciled MaterialLayout (MaterialBinder::ResolvePush, the same placement Material::WritePushField uses),
    // over a COPY of the material's push block, and that copy is the draw's push block. Two passes recording the
    // same material on two threads therefore read one unchanged material.
    //
    // A field the cell does not declare is skipped (BoneOffset on a static cell), exactly as WritePushField
    // skips it, unless the write is Required (Material::SetPrimitiveIndex / SetSkinnedBoneOffset verified it):
    // then a missing field is the draw's error. A value whose size is not the field's is always an error.
    class DrawPushFields
    {
    public:
        static constexpr uint32_t kMaxFieldBytes = sizeof( glm::mat4 );

        struct Field
        {
            std::string_view                      Name; // a literal: the command outlives no name it holds
            uint32_t                              Size     = 0;
            bool                                  Required = false;
            std::array<std::byte, kMaxFieldBytes> Value{};
        };

        // Material::SetPushMatrix.
        DrawPushFields& Transform( const glm::mat4& matrix )
        {
            return Write( "Transform", matrix, false );
        }
        // Material::SetPrimitiveIndex (required: every cell that draws a motion row declares it).
        DrawPushFields& PrimitiveIndex( const uint32_t row )
        {
            return Write( "PrimitiveIndex", row, true );
        }
        // Material::SetMaterialIndex.
        DrawPushFields& MaterialIndex( const uint32_t row )
        {
            return Write( "MaterialIndex", row, false );
        }
        // Material::SetPrimitiveFlags.
        DrawPushFields& PrimitiveFlags( const uint32_t flags )
        {
            return Write( "PrimitiveFlags", flags, false );
        }
        // Material::SetInstancedWind.
        DrawPushFields& Wind( const InstanceWindPush& wind )
        {
            Write( "WindA", wind.A, false );
            return Write( "WindB", wind.B, false );
        }
        // Material::SetSkinnedBoneOffset (required) and the shadow / silhouette cells' SetBoneOffset (not).
        DrawPushFields& BoneOffset( const uint32_t firstBone, const bool required )
        {
            return Write( "BoneOffset", firstBone, required );
        }
        // MaterialShadowSkinned / MaterialSilhouetteSkinned::SetSkinCache.
        DrawPushFields& SkinCache( const uint64_t address, const uint32_t vertexCount )
        {
            Write( "SkinCacheVertexCount", vertexCount, false );
            Write( "SkinCacheAddressLo", static_cast<uint32_t>( address & 0xffffffffu ), false );
            return Write( "SkinCacheAddressHi", static_cast<uint32_t>( address >> 32u ), false );
        }

        [[nodiscard]] bool IsEmpty() const
        {
            return m_Fields.empty();
        }

        // Writes every field into @p push (the copy of the material's push block) at its layout offset.
        [[nodiscard]] Common::BoolResultStr WriteInto( std::span<std::byte>                 push,
                                                       const Core::Formats::MaterialLayout& layout ) const
        {
            for ( const Field& field : m_Fields )
            {
                const MaterialBinder::PushSlot slot =
                     MaterialBinder::ResolvePush( layout, field.Name, field.Size );
                switch ( slot.Status )
                {
                    case MaterialBinder::PushWrite::Written:
                        if ( static_cast<size_t>( slot.Offset ) + field.Size > push.size() )
                            return Common::MakeFormattedError(
                                 "push field '{}' at byte {} (+{}) is past the {}-byte push block", field.Name,
                                 slot.Offset, field.Size, push.size() );
                        std::memcpy( push.data() + slot.Offset, field.Value.data(), field.Size );
                        break;
                    case MaterialBinder::PushWrite::SizeMismatch:
                        return Common::MakeFormattedError( "push field '{}' is not {} bytes in the cell's layout",
                                                           field.Name, field.Size );
                    case MaterialBinder::PushWrite::Absent:
                        if ( field.Required )
                            return Common::MakeFormattedError( "the cell has no push field '{}'", field.Name );
                        break;
                }
            }
            return Common::MakeSuccess( true );
        }

    private:
        template <typename T>
        DrawPushFields& Write( const std::string_view name, const T& value, const bool required )
        {
            static_assert( sizeof( T ) <= kMaxFieldBytes );
            Field field{ .Name = name, .Size = static_cast<uint32_t>( sizeof( T ) ), .Required = required };
            std::memcpy( field.Value.data(), &value, sizeof( T ) );
            // A later write of the same field replaces the earlier one (the order the setters ran in).
            for ( Field& held : m_Fields )
                if ( held.Name == name )
                {
                    held = field;
                    return *this;
                }
            m_Fields.push_back( field );
            return *this;
        }

        std::vector<Field> m_Fields;
    };
} // namespace Desert::Graphic
