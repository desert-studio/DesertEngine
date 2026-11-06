#pragma once

#include <Engine/Graphic/InstanceWind.hpp>
#include <Engine/Graphic/Materials/MaterialBinder.hpp>
#include <Engine/Graphic/Shader.hpp>

#include <Common/Core/ResultStr.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <utility>
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
            // RENDER-JOB-CPU-3: a compacted bin's draw rows (GpuInstances.hpp; the cells of the draw-row
            // permutation declare them, every other cell skips them).
            DrawRowsLo,
            DrawRowsHi,
            Count
        };
        static constexpr size_t kFieldCount = static_cast<size_t>( FieldId::Count );
        // The push field each FieldId names in a cell's MaterialLayout.
        static constexpr std::array<std::string_view, kFieldCount> kFieldNames = {
             "Transform", "PrimitiveIndex", "MaterialIndex", "PrimitiveFlags",       "WindA",
             "WindB",     "BoneOffset",     "ShadowSlope",   "SkinCacheVertexCount", "SkinCacheAddressLo",
             "SkinCacheAddressHi", "DrawRowsLo",     "DrawRowsHi" };

        // CPU-FRAME-ARENA 3 (UE FMeshDrawCommand: the shader bindings live inline in the command, no allocation
        // per draw): each FieldId's size is its setter's type, so every field has a fixed place in the command's
        // inline value bytes. A command carries no heap container: building and dropping a frame's thousands of
        // draws allocates nothing.
        static constexpr std::array<uint32_t, kFieldCount> kFieldBytes = {
             sizeof( glm::mat4 ), sizeof( uint32_t ), sizeof( uint32_t ),  sizeof( uint32_t ), sizeof( glm::vec4 ),
             sizeof( glm::vec4 ), sizeof( uint32_t ), sizeof( glm::vec2 ), sizeof( uint32_t ), sizeof( uint32_t ),
             sizeof( uint32_t ),  sizeof( uint32_t ), sizeof( uint32_t ) };
        [[nodiscard]] static constexpr std::array<uint32_t, kFieldCount> ValueOffsets() noexcept
        {
            std::array<uint32_t, kFieldCount> offsets{};
            uint32_t                          at = 0;
            for ( size_t id = 0; id < kFieldCount; ++id )
            {
                offsets[id] = at;
                at += kFieldBytes[id];
            }
            return offsets;
        }
        static constexpr std::array<uint32_t, kFieldCount> kValueOffsets = ValueOffsets();
        static constexpr uint32_t kValueBytes = kValueOffsets[kFieldCount - 1] + kFieldBytes[kFieldCount - 1];
        static_assert( kFieldCount <= 16, "the written / required masks are 16 bits" );

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
        DrawPushFields& Transform( const glm::mat4& matrix ) noexcept
        {
            return Write<FieldId::Transform>( matrix, false );
        }
        // Material::SetPrimitiveIndex (required: every cell that draws a motion row declares it).
        DrawPushFields& PrimitiveIndex( const uint32_t row ) noexcept
        {
            return Write<FieldId::PrimitiveIndex>( row, true );
        }
        // Material::SetMaterialIndex.
        DrawPushFields& MaterialIndex( const uint32_t row ) noexcept
        {
            return Write<FieldId::MaterialIndex>( row, false );
        }
        // Material::SetPrimitiveFlags.
        DrawPushFields& PrimitiveFlags( const uint32_t flags ) noexcept
        {
            return Write<FieldId::PrimitiveFlags>( flags, false );
        }
        // Material::SetInstancedWind.
        DrawPushFields& Wind( const InstanceWindPush& wind ) noexcept
        {
            Write<FieldId::WindA>( wind.A, false );
            return Write<FieldId::WindB>( wind.B, false );
        }
        // Material::SetSkinnedBoneOffset (required) and the shadow / silhouette cells' SetBoneOffset (not).
        DrawPushFields& BoneOffset( const uint32_t firstBone, const bool required ) noexcept
        {
            return Write<FieldId::BoneOffset>( firstBone, required );
        }
        // Material::SetShadowSlopeBias (required: a light-view caster cell that is handed a slope declares it).
        DrawPushFields& ShadowSlope( const float slopeBias, const float slopeClamp ) noexcept
        {
            return Write<FieldId::ShadowSlope>( glm::vec2( slopeBias, slopeClamp ), true );
        }
        // MaterialShadowSkinned / MaterialSilhouetteSkinned::SetSkinCache.
        DrawPushFields& SkinCache( const uint64_t address, const uint32_t vertexCount ) noexcept
        {
            Write<FieldId::SkinCacheVertexCount>( vertexCount, false );
            Write<FieldId::SkinCacheAddressLo>( static_cast<uint32_t>( address & 0xffffffffu ), false );
            return Write<FieldId::SkinCacheAddressHi>( static_cast<uint32_t>( address >> 32u ), false );
        }

        // RENDER-JOB-CPU-3: the address of a compacted bin's first slot of GpuDrawBinRefs::DrawRows - the vertex
        // stage reads its draw's row at [gl_DrawIDARB] (Common/GpuInstanceDrawRow.glslh).
        DrawPushFields& DrawRows( const uint64_t address ) noexcept
        {
            Write<FieldId::DrawRowsLo>( static_cast<uint32_t>( address & 0xffffffffu ), false );
            return Write<FieldId::DrawRowsHi>( static_cast<uint32_t>( address >> 32u ), false );
        }

        [[nodiscard]] bool IsEmpty() const noexcept
        {
            return m_Written == 0u;
        }

        // Writes every field into @p push (the copy of the material's push block) at its layout offset, in
        // FieldId order (each field once: a later write of a field replaced the earlier one).
        [[nodiscard]] Common::BoolResultStr WriteInto( std::span<std::byte>                 push,
                                                       const Core::Formats::MaterialLayout& layout ) const
        {
            for ( size_t id = 0; id < kFieldCount; ++id )
            {
                if ( !IsWritten( id ) )
                    continue;
                const MaterialBinder::PushSlot slot =
                     MaterialBinder::ResolvePush( layout, kFieldNames[id], kFieldBytes[id] );
                if ( Common::BoolResultStr laid = Lay( push, id, slot ); !laid )
                    return laid;
            }
            return Common::MakeSuccess( true );
        }
        // The same, through @p placement: Place of the layout the draw records with (no lookup by name).
        [[nodiscard]] Common::BoolResultStr WriteInto( std::span<std::byte> push, const Placement& placement ) const
        {
            for ( size_t id = 0; id < kFieldCount; ++id )
            {
                if ( !IsWritten( id ) )
                    continue;
                const Placement::Slot&   placed = placement.Slots[id];
                MaterialBinder::PushSlot slot;
                if ( placed.Present )
                {
                    slot.Offset = placed.Offset;
                    slot.Status = MaterialBinder::PushWrite::SizeMismatch;
                    if ( placed.Size == kFieldBytes[id] )
                        slot.Status = MaterialBinder::PushWrite::Written;
                }
                if ( Common::BoolResultStr laid = Lay( push, id, slot ); !laid )
                    return laid;
            }
            return Common::MakeSuccess( true );
        }

        // CPU-FRAME-ARENA 4: THE PLACEMENT OF A SHADER'S LAYOUT, MADE ONCE PER SHADER CODE (UE: a shader's
        // parameter map is bound at compile, never searched per draw). Keyed by (Shader::GetSerial,
        // Shader::GetCodeGeneration) - the pair names one MaterialLayout, since the layout is replaced only by a
        // build that bumps the generation, and a serial is never reused as an address is - so a placement holds
        // across records and frames: a steady frame finds no field by name at all. One table per thread
        // (ThreadShaderPlacements), so recording threads share nothing. Bounded: past kCapacity distinct shader
        // codes a slot is reused round-robin (re-placed on its next use, the same answer). Of's answer is used
        // before the next Of on the thread. A null shader places nothing (Material::GetMaterialLayout's empty
        // layout for a material with no program).
        class ShaderPlacements
        {
        public:
            static constexpr std::size_t kCapacity = 64;

            [[nodiscard]] const Placement& Of( const Shader* shader ) noexcept
            {
                static const Placement kNone{};
                if ( shader == nullptr )
                    return kNone;
                const uint64_t serial     = shader->GetSerial();
                const uint64_t generation = shader->GetCodeGeneration();
                if ( m_Last < m_Count && m_Entries[m_Last].Serial == serial &&
                     m_Entries[m_Last].Generation == generation )
                    return m_Entries[m_Last].Placed;
                for ( std::size_t e = 0; e < m_Count; ++e )
                {
                    if ( m_Entries[e].Serial == serial && m_Entries[e].Generation == generation )
                    {
                        m_Last = e;
                        return m_Entries[e].Placed;
                    }
                }
                std::size_t slot = m_Count;
                if ( m_Count < kCapacity )
                    ++m_Count;
                else
                    slot = m_NextEvict++ % kCapacity;
                m_Entries[slot] = Entry{ .Serial     = serial,
                                         .Generation = generation,
                                         .Placed     = Place( shader->GetMaterialLayout() ) };
                m_Last          = slot;
                return m_Entries[slot].Placed;
            }

        private:
            struct Entry
            {
                uint64_t  Serial     = 0; // 0: no shader (serials start at 1)
                uint64_t  Generation = 0;
                Placement Placed{};
            };
            std::array<Entry, kCapacity> m_Entries{};
            std::size_t                  m_Count     = 0;
            std::size_t                  m_Last      = 0;
            std::size_t                  m_NextEvict = 0;
        };
        [[nodiscard]] static ShaderPlacements& ThreadShaderPlacements() noexcept
        {
            thread_local ShaderPlacements t_Placements;
            return t_Placements;
        }

    private:
        [[nodiscard]] bool IsWritten( const size_t id ) const noexcept
        {
            return ( m_Written & ( 1u << id ) ) != 0u;
        }

        // Field @p id at @p slot (MaterialBinder::ResolvePush's answer for it).
        [[nodiscard]] Common::BoolResultStr Lay( std::span<std::byte> push, const size_t id,
                                                 const MaterialBinder::PushSlot& slot ) const
        {
            const std::string_view name = kFieldNames[id];
            const uint32_t         size = kFieldBytes[id];
            switch ( slot.Status )
            {
                case MaterialBinder::PushWrite::Written:
                    if ( static_cast<size_t>( slot.Offset ) + size > push.size() )
                        return Common::MakeFormattedError(
                             "push field '{}' at byte {} (+{}) is past the {}-byte push block", name, slot.Offset,
                             size, push.size() );
                    std::memcpy( push.data() + slot.Offset, m_Values.data() + kValueOffsets[id], size );
                    break;
                case MaterialBinder::PushWrite::SizeMismatch:
                    return Common::MakeFormattedError( "push field '{}' is not {} bytes in the cell's layout",
                                                       name, size );
                case MaterialBinder::PushWrite::Absent:
                    if ( ( m_Required & ( 1u << id ) ) != 0u )
                        return Common::MakeFormattedError( "the cell has no push field '{}'", name );
                    break;
            }
            return Common::MakeSuccess( true );
        }

        // A later write of the same field replaces the earlier one (its value and whether it is required).
        template <FieldId Id, typename T>
        DrawPushFields& Write( const T& value, const bool required ) noexcept
        {
            constexpr size_t id = static_cast<size_t>( Id );
            static_assert( sizeof( T ) == kFieldBytes[id], "a field is written with its setter's type" );
            std::memcpy( m_Values.data() + kValueOffsets[id], &value, sizeof( T ) );
            const auto bit = static_cast<uint16_t>( 1u << id );
            m_Written      = static_cast<uint16_t>( m_Written | bit );
            if ( required )
                m_Required = static_cast<uint16_t>( m_Required | bit );
            else
                m_Required = static_cast<uint16_t>( m_Required & ~bit );
            return *this;
        }

        std::array<std::byte, kValueBytes> m_Values{};
        uint16_t                           m_Written  = 0; // bit per FieldId
        uint16_t                           m_Required = 0; // bit per FieldId: its last write was Required
    };
} // namespace Desert::Graphic
