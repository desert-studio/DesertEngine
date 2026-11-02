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
    // THE PER-DRAW PUSH VALUES OF ONE MESH OR TERRAIN DRAW, BY VALUE (UE FMeshDrawCommand: the command is
    // immutable and carries what differs per draw; the shared material is never written to record it). Built in
    // the node's setup next to the draw (MeshDrawCommand::Push); MeshDrawList::Record lays them, by name through
    // the cell's reconciled MaterialLayout (MaterialBinder::ResolvePush, the same placement
    // Material::WritePushField uses), over a COPY of the material's push block, and that copy is the draw's push
    // block. Two passes recording the same material on two threads therefore read one unchanged material.
    //
    // A field the cell does not declare is skipped (BoneOffset on a static cell), exactly as WritePushField
    // skips it, unless the write is Required (Material::SetPrimitiveIndex / SetSkinnedBoneOffset verified it):
    // then a missing field is the draw's error. A value whose size is not the field's is always an error.
    // TerrainRenderer::RecordDraws builds its push blocks the same way (row, clip-from-world, slope) and hands
    // them to Renderer::DrawProcedural / DrawProceduralIndirect.
    class DrawPushFields
    {
    public:
        static constexpr uint32_t kMaxFieldBytes = sizeof( glm::mat4 );

        // RDG-PARALLEL-7 (RDG-REC-4; UE lays a shader's parameters out once per shader, FShaderParameterMap): the
        // closed set of per-draw fields the setters below write. A draw places them through its layout's
        // Placement, built once per layout, instead of finding each field by name.
        enum class FieldId : uint8_t
        {
            Transform,
            PrimitiveIndex,
            MaterialIndex,
            PrimitiveFlags,
            WindA,
            WindB,
            BoneOffset,
            ShadowSlope,
            SkinCacheVertexCount,
            SkinCacheAddressLo,
            SkinCacheAddressHi,
            Count
        };
        static constexpr size_t kFieldCount = static_cast<size_t>( FieldId::Count );
        // The push field each FieldId names in a cell's MaterialLayout.
        static constexpr std::array<std::string_view, kFieldCount> kFieldNames = {
             "Transform", "PrimitiveIndex", "MaterialIndex", "PrimitiveFlags",       "WindA",
             "WindB",     "BoneOffset",     "ShadowSlope",   "SkinCacheVertexCount", "SkinCacheAddressLo",
             "SkinCacheAddressHi" };

        struct Field
        {
            std::string_view                      Name; // a literal: the command outlives no name it holds
            uint32_t                              Size     = 0;
            bool                                  Required = false;
            std::array<std::byte, kMaxFieldBytes> Value{};
            FieldId                               Id = FieldId::Count;
        };

        // Where every FieldId lands in one MaterialLayout (its push field, found by name once): what
        // MaterialBinder::ResolvePush would answer for each, with the layout's size kept so a draw's size check
        // is the same.
        struct Placement
        {
            struct Slot
            {
                bool     Present = false;
                uint32_t Offset  = 0;
                uint32_t Size    = 0;
            };
            std::array<Slot, kFieldCount> Slots{};
        };
        [[nodiscard]] static Placement Place( const Core::Formats::MaterialLayout& layout )
        {
            Placement placement;
            for ( size_t id = 0; id < kFieldCount; ++id )
            {
                const Core::Formats::MaterialLayoutPushField* field = layout.FindPush( kFieldNames[id] );
                if ( field != nullptr )
                    placement.Slots[id] = Placement::Slot{ .Present = true, .Offset = field->Offset,
                                                           .Size = field->Size };
            }
            return placement;
        }

        // Material::SetPushMatrix.
        DrawPushFields& Transform( const glm::mat4& matrix )
        {
            return Write( FieldId::Transform, matrix, false );
        }
        // Material::SetPrimitiveIndex (required: every cell that draws a motion row declares it).
        DrawPushFields& PrimitiveIndex( const uint32_t row )
        {
            return Write( FieldId::PrimitiveIndex, row, true );
        }
        // Material::SetMaterialIndex.
        DrawPushFields& MaterialIndex( const uint32_t row )
        {
            return Write( FieldId::MaterialIndex, row, false );
        }
        // Material::SetPrimitiveFlags.
        DrawPushFields& PrimitiveFlags( const uint32_t flags )
        {
            return Write( FieldId::PrimitiveFlags, flags, false );
        }
        // Material::SetInstancedWind.
        DrawPushFields& Wind( const InstanceWindPush& wind )
        {
            Write( FieldId::WindA, wind.A, false );
            return Write( FieldId::WindB, wind.B, false );
        }
        // Material::SetSkinnedBoneOffset (required) and the shadow / silhouette cells' SetBoneOffset (not).
        DrawPushFields& BoneOffset( const uint32_t firstBone, const bool required )
        {
            return Write( FieldId::BoneOffset, firstBone, required );
        }
        // Material::SetShadowSlopeBias (required: a light-view caster cell that is handed a slope declares it).
        DrawPushFields& ShadowSlope( const float slopeBias, const float slopeClamp )
        {
            return Write( FieldId::ShadowSlope, glm::vec2( slopeBias, slopeClamp ), true );
        }
        // MaterialShadowSkinned / MaterialSilhouetteSkinned::SetSkinCache.
        DrawPushFields& SkinCache( const uint64_t address, const uint32_t vertexCount )
        {
            Write( FieldId::SkinCacheVertexCount, vertexCount, false );
            Write( FieldId::SkinCacheAddressLo, static_cast<uint32_t>( address & 0xffffffffu ), false );
            return Write( FieldId::SkinCacheAddressHi, static_cast<uint32_t>( address >> 32u ), false );
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
                if ( Common::BoolResultStr laid = Lay( push, field, slot ); !laid )
                    return laid;
            }
            return Common::MakeSuccess( true );
        }
        // The same, through @p placement: Place of the layout the draw records with (no lookup by name).
        [[nodiscard]] Common::BoolResultStr WriteInto( std::span<std::byte> push, const Placement& placement ) const
        {
            for ( const Field& field : m_Fields )
            {
                const Placement::Slot&   placed = placement.Slots[static_cast<size_t>( field.Id )];
                MaterialBinder::PushSlot slot;
                if ( placed.Present )
                {
                    slot.Offset = placed.Offset;
                    slot.Status = MaterialBinder::PushWrite::SizeMismatch;
                    if ( placed.Size == field.Size )
                        slot.Status = MaterialBinder::PushWrite::Written;
                }
                if ( Common::BoolResultStr laid = Lay( push, field, slot ); !laid )
                    return laid;
            }
            return Common::MakeSuccess( true );
        }

    private:
        // One field at @p slot (MaterialBinder::ResolvePush's answer for it).
        [[nodiscard]] static Common::BoolResultStr Lay( std::span<std::byte> push, const Field& field,
                                                        const MaterialBinder::PushSlot& slot )
        {
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
            return Common::MakeSuccess( true );
        }

        template <typename T>
        DrawPushFields& Write( const FieldId id, const T& value, const bool required )
        {
            static_assert( sizeof( T ) <= kMaxFieldBytes );
            Field field{ .Name     = kFieldNames[static_cast<size_t>( id )],
                         .Size     = static_cast<uint32_t>( sizeof( T ) ),
                         .Required = required,
                         .Id       = id };
            std::memcpy( field.Value.data(), &value, sizeof( T ) );
            // A later write of the same field replaces the earlier one (the order the setters ran in).
            for ( Field& held : m_Fields )
                if ( held.Id == id )
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
