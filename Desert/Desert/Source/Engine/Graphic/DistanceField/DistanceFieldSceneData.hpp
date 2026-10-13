#pragma once

// THE GPU SCENE'S MESH DISTANCE FIELDS (UE FDistanceFieldSceneData, DistanceFieldObjectManagement.cpp /
// DistanceFieldStreaming.cpp): every resident mesh field's bricks in one R8 brick atlas (a volume texture 64 x 64
// bricks wide that grows in depth), its indirection tables rewritten to atlas brick slots in one table, and one
// asset record per field - the three buffers Shaders/Common/MeshDistanceField.glslh reads.
//
// Residency follows use: a field becomes resident the first time an object draws with it (Acquire) and leaves
// when nothing holds the field any more (ReleaseUnused, from the weak pointer each entry keeps) - its brick slots,
// table range and asset record go back to the free lists. UE streams the finer mips in and out by distance; here
// every mip of a resident field is resident (see the REMAINDER of GLOBAL-SDF for the streaming step).

#include <Common/Core/ResultStr.hpp>
#include <Engine/Assets/MeshDistanceField.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace Desert::Graphic
{
    class Image3D;
}

namespace Desert::Graphic::DistanceField
{
    // First-fit ranges over [0, End()): what the brick slots, the indirection table and the asset records are
    // carved from. Pure, so the allocation is testable without a device.
    class RangeAllocator
    {
    public:
        // The first free range of @p count, or a new one at the end.
        uint32_t               Allocate( uint32_t count );
        void                   Free( uint32_t offset, uint32_t count );
        [[nodiscard]] uint32_t End() const
        {
            return m_End;
        }

    private:
        std::map<uint32_t, uint32_t> m_Free; // offset -> count, coalesced
        uint32_t                     m_End = 0;
    };

    class DistanceFieldSceneData
    {
    public:
        DistanceFieldSceneData();
        ~DistanceFieldSceneData();
        DistanceFieldSceneData( const DistanceFieldSceneData& )            = delete;
        DistanceFieldSceneData& operator=( const DistanceFieldSceneData& ) = delete;

        // The asset record index of @p field, making it resident on first use. A field without a single mip
        // (a mesh with no triangles) has no record: nullopt, nothing to sample.
        [[nodiscard]] Common::ResultStr<std::optional<uint32_t>>
        Acquire( const std::shared_ptr<const Assets::MeshDistanceFieldData>& field );

        // Returns every field nothing holds any more to the free lists.
        void ReleaseUnused();

        // Writes the bricks Acquire queued into the atlas, growing (re-creating and refilling) it first when the
        // slots outgrew it. Called once per frame before any pass samples the atlas.
        [[nodiscard]] Common::BoolResultStr FlushUploads();

        [[nodiscard]] const std::vector<glm::vec4>& GetAssetData() const
        {
            return m_AssetData;
        }
        [[nodiscard]] const std::vector<uint32_t>& GetIndirection() const
        {
            return m_Indirection;
        }
        [[nodiscard]] const std::shared_ptr<Image3D>& GetBrickAtlas() const
        {
            return m_Atlas;
        }
        [[nodiscard]] uint32_t ResidentFieldCount() const
        {
            return static_cast<uint32_t>( m_Resident.size() );
        }
        [[nodiscard]] uint32_t ResidentBrickSlots() const
        {
            return m_BrickSlots.End();
        }

    private:
        struct Resident
        {
            std::weak_ptr<const Assets::MeshDistanceFieldData>  Field;
            uint32_t                                            AssetIndex = 0;
            std::array<uint32_t, Assets::kDistanceFieldNumMips> BrickOffset{};
            std::array<uint32_t, Assets::kDistanceFieldNumMips> IndirectionOffset{};
        };

        void                                Release( const Resident& resident );
        [[nodiscard]] Common::BoolResultStr EnsureAtlasCapacity();
        void QueueBricks( const Assets::MeshDistanceFieldData& field, const Resident& resident );

        std::unordered_map<const Assets::MeshDistanceFieldData*, Resident> m_Resident;
        RangeAllocator                                                     m_BrickSlots;
        RangeAllocator                                                     m_IndirectionSlots;
        RangeAllocator                                                     m_AssetSlots;
        std::vector<glm::vec4>                                             m_AssetData;
        std::vector<uint32_t>                                              m_Indirection;

        std::shared_ptr<Image3D> m_Atlas;
        uint32_t                 m_AtlasDepthInBricks = 0;
        // Bricks waiting for the atlas: (slot, the field that holds the bytes, mip, brick within the mip).
        struct PendingBrick
        {
            uint32_t                                             Slot = 0;
            std::shared_ptr<const Assets::MeshDistanceFieldData> Field;
            uint32_t                                             Mip   = 0;
            uint32_t                                             Brick = 0;
        };
        std::vector<PendingBrick> m_Pending;
    };
} // namespace Desert::Graphic::DistanceField
