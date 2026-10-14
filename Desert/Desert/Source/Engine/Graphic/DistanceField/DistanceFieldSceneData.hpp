#pragma once

// THE GPU SCENE'S MESH DISTANCE FIELDS (UE FDistanceFieldSceneData, DistanceFieldObjectManagement.cpp /
// DistanceFieldStreaming.cpp): the resident mips of every mesh field in one R8 brick atlas (a volume texture 64 x
// 64 bricks wide, as deep as the pool), their indirection tables rewritten to atlas brick slots in one table, and
// one asset record per field - the three buffers Shaders/Common/MeshDistanceField.glslh reads.
//
// RESIDENCY FOLLOWS USE AND DISTANCE (UE DistanceFieldStreaming). A field becomes resident the first time an
// object draws with it (Acquire), with its COARSEST mip only - the always-loaded one; the finer mips stream in by
// the distance from the camera to the nearest object using the field (Stream) - all of them where a clipmap that
// reads them is (UE's policy, DfWantedMips) - and out again when the camera leaves; all of it inside the pool the
// Scalability parameter GlobalIllumination.DistanceFieldPoolMiB sets, nearest first, a bounded number of bricks
// uploaded per frame. A field leaves when nothing holds it any more (ReleaseUnused, from the weak pointer each
// entry keeps) - its brick slots, table ranges and asset record go back to the free lists.
//
// The record lists the resident mips COARSEST FIRST (UE's reversed mip index), so a finer mip arriving or leaving
// only changes the record's mip count and one row; Mips[] of the asset stays finest first.

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
        uint32_t Allocate( uint32_t count );
        // The same, but never past @p limit: nullopt when no free range below it holds @p count.
        [[nodiscard]] std::optional<uint32_t> Allocate( uint32_t count, uint32_t limit );
        void                                  Free( uint32_t offset, uint32_t count );
        [[nodiscard]] uint32_t                End() const
        {
            return m_End;
        }
        // Slots in use below End() (End() minus the free ranges).
        [[nodiscard]] uint32_t Used() const;

    private:
        std::map<uint32_t, uint32_t> m_Free; // offset -> count, coalesced
        uint32_t                     m_End = 0;
    };

    // What the pool holds after a frame's streaming (the log line, the view's census).
    struct DistanceFieldStreamingStats
    {
        uint32_t Fields         = 0;
        uint32_t CapacityBricks = 0;
        uint32_t UsedBricks     = 0;
        uint32_t CoarseBricks   = 0;
        uint32_t ResidentByMips[Assets::kDistanceFieldNumMips]{}; // fields with 1, 2, 3 mips resident
        uint32_t UnmetRequests  = 0; // fields wanting a finer mip than the pool or this frame's uploads gave
        uint32_t UploadedBricks = 0; // this frame
    };

    class DistanceFieldSceneData
    {
    public:
        DistanceFieldSceneData();
        ~DistanceFieldSceneData();
        DistanceFieldSceneData( const DistanceFieldSceneData& )            = delete;
        DistanceFieldSceneData& operator=( const DistanceFieldSceneData& ) = delete;

        // The pool, in MiB of R8 bricks (Scalability GlobalIllumination.DistanceFieldPoolMiB). A different size
        // drops every resident field and re-creates the atlas: true when that happened, so the caller rewrites
        // everything composited from the old one.
        [[nodiscard]] bool SetPoolMiB( uint32_t poolMiB );

        // Starts a frame: forgets the distances of the last one.
        void BeginFrame();

        // The asset record index of @p field, making it resident (coarsest mip) on first use, and the distance in
        // centimetres from the camera to an object drawn with it - the largest per-axis gap to its box, the
        // measure of the camera-centred clipmap boxes (the nearest of a frame's objects counts). A
        // field without a single mip (a mesh with no triangles) has no record: nullopt, nothing to sample. An
        // error when not even the coarse mip fits the pool.
        [[nodiscard]] Common::ResultStr<std::optional<uint32_t>>
        Acquire( const std::shared_ptr<const Assets::MeshDistanceFieldData>& field, float distance );

        // Streams the finer mips toward what this frame's distances want, inside the pool and the per-frame
        // upload budget. After every Acquire of the frame, before FlushUploads.
        void Stream();

        // How many mips of @p field are resident (0 = not resident): part of an object's identity in the global
        // field, so a mip arriving or leaving recomposites the object.
        [[nodiscard]] uint32_t ResidentMips( const Assets::MeshDistanceFieldData* field ) const;

        // Returns every field nothing holds any more to the free lists.
        void ReleaseUnused();

        // Writes the bricks Acquire and Stream queued into the atlas (created on the first call after a pool
        // change). Called once per frame before any pass samples the atlas.
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
        [[nodiscard]] const DistanceFieldStreamingStats& GetStats() const
        {
            return m_Stats;
        }

    private:
        // One resident mip, by its place in the record (0 = the coarsest).
        struct ResidentMip
        {
            uint32_t BrickOffset       = 0;
            uint32_t BrickCount        = 0;
            uint32_t IndirectionOffset = 0;
            uint32_t IndirectionCount  = 0;
        };
        struct Resident
        {
            std::weak_ptr<const Assets::MeshDistanceFieldData>     Field;
            uint32_t                                               AssetIndex = 0;
            uint32_t                                               NumMips    = 0; // resident, coarsest first
            std::array<ResidentMip, Assets::kDistanceFieldNumMips> Mips{};
            float                                                  Distance = 0.0f; // this frame's nearest use
            bool                                                   Used     = false;
        };

        // Makes the next finer mip of @p resident resident (slots, table, record row, queued bricks); false when
        // the pool has no room for it.
        [[nodiscard]] bool AddMip( Resident& resident, const Assets::MeshDistanceFieldData& field );
        // Drops the finest resident mip of @p resident.
        void DropMip( Resident& resident );
        void Release( Resident& resident );
        // Frees room for @p bricks by dropping finer mips of fields farther than @p distance, farthest first.
        bool EvictFartherThan( float distance, uint32_t bricks, const Resident* except );
        [[nodiscard]] Common::BoolResultStr EnsureAtlas();

        std::unordered_map<const Assets::MeshDistanceFieldData*, Resident> m_Resident;
        RangeAllocator                                                     m_BrickSlots;
        RangeAllocator                                                     m_IndirectionSlots;
        RangeAllocator                                                     m_AssetSlots;
        std::vector<glm::vec4>                                             m_AssetData;
        std::vector<uint32_t>                                              m_Indirection;

        uint32_t                    m_PoolMiB        = 0;
        uint32_t                    m_CapacityBricks = 0;
        std::shared_ptr<Image3D> m_Atlas;
        uint32_t                    m_UploadedThisFrame = 0;
        DistanceFieldStreamingStats m_Stats;
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
