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
//
// ONE PER SCENE (UE: FDistanceFieldSceneData lives in FScene): Core::Scene owns it (Scene::GetDistanceFields)
// and every view of the scene - the viewports and the reflection captures' face views - shares the one atlas,
// the one record and indirection buffers and the one streaming. Its device objects are rows of the scene's
// ledger instance, so the pool is the scene's budget entry, counted once, never a view's. Only the global
// distance field's clipmaps, its object grid, its object buffer and the near field stay per view
// (GlobalDistanceField).
//
// ONCE PER FRAME PER SCENE: the first view of a frame to call BeginViewFrame (a new engine frame number)
// decides the scene's resident set for the whole frame - it streams toward the distances EVERY view of the
// previous frame looked its fields up at (the nearest wins), acquires the coarse mip of every field of the
// scene's primitives, writes the atlas and uploads the records. Only there is anything allocated or evicted.
// Every view of the frame, that one included, then only looks fields up (Lookup: the record index, and its
// distance for the next frame's streaming) - never allocates, evicts or uploads, so no graph of the frame can
// read a slot or a record another view of the same frame changed. A field a view meets that the set lacks
// (it appeared mid-frame) is not drawn into the field this frame and waits: the next frame start acquires it
// first, so its coarse mip is guaranteed then (or that frame start fails loudly when the pool cannot hold it).

#include <Common/Core/ResultStr.hpp>
#include <Engine/Assets/MeshDistanceField.hpp>
#include <Engine/Graphic/ResourceLedger.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Desert::Graphic
{
    class Image3D;
}

namespace Desert::ShaderResources
{
    class StorageBuffer;
}

namespace Desert::Graphic::DistanceField
{
    // The bindings of the asset records and the indirection table in every pass that reads mesh fields
    // (Shaders/Common/MeshDistanceField.glslh, the global field's composite).
    inline constexpr uint32_t kMeshDistanceFieldAssetBinding       = 1;
    inline constexpr uint32_t kMeshDistanceFieldIndirectionBinding = 2;

    // First-fit ranges over [0, End()): what the indirection table and the asset records are carved from. Pure, so
    // the allocation is testable without a device.
    class RangeAllocator
    {
    public:
        // The first free range of @p count, or a new one at the end.
        uint32_t Allocate( uint32_t count );
        void                                  Free( uint32_t offset, uint32_t count );
        [[nodiscard]] uint32_t                End() const
        {
            return m_End;
        }

    private:
        std::map<uint32_t, uint32_t> m_Free; // offset -> count, coalesced
        uint32_t                     m_End = 0;
    };

    // Single brick slots of the atlas, below its capacity (UE FDistanceFieldBlockAllocator, one brick per block):
    // a mip's bricks need not be contiguous - the indirection table names every brick's slot - so the pool never
    // fragments and whether a mip fits is a count. Contiguous ranges did fragment: a near field's mip could not be
    // placed even after every farther field's finer mips were dropped for it, and those came back the next frame
    // only to be dropped again - the streaming never settled. Pure, so it is testable without a device.
    class BrickAllocator
    {
    public:
        // Forgets every slot; @p capacity slots are free.
        void Reset( uint32_t capacity );
        // @p count slots, or nullopt (nothing taken) when fewer are free.
        [[nodiscard]] std::optional<std::vector<uint32_t>> Allocate( uint32_t count );
        void                                               Free( const std::vector<uint32_t>& slots );
        [[nodiscard]] uint32_t                             Available() const
        {
            return m_Capacity - m_Next + static_cast<uint32_t>( m_Free.size() );
        }
        [[nodiscard]] uint32_t Used() const
        {
            return m_Capacity - Available();
        }
        // One past the highest slot ever handed out since Reset: what the atlas must hold (freed slots are
        // reused before new ones, so it is the most bricks ever resident at once).
        [[nodiscard]] uint32_t HighWater() const
        {
            return m_Next;
        }

    private:
        uint32_t              m_Capacity = 0;
        uint32_t              m_Next     = 0; // slots at and above it were never handed out
        std::vector<uint32_t> m_Free;
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
        // @p scene is the owning Core::Scene's ledger instance (Scene::GetLedgerInstance).
        explicit DistanceFieldSceneData( ResourceOwnerInstance scene );
        ~DistanceFieldSceneData();
        DistanceFieldSceneData( const DistanceFieldSceneData& )            = delete;
        DistanceFieldSceneData& operator=( const DistanceFieldSceneData& ) = delete;

        // Every view calls this first, with the engine's absolute frame number, its pool request (Scalability
        // GlobalIllumination.DistanceFieldPoolMiB) and the fields of every distance-field-bearing primitive of
        // the scene. A new number starts the scene's frame and decides its resident set (above): the pool becomes
        // the largest any view asked for during the previous frame (a different size drops every resident field
        // and re-creates the atlas: GetPoolGeneration moves), fields nothing holds leave, the finer mips stream,
        // the waiting fields and then @p fields get their coarse mip, the atlas and this frame's copies of the
        // records are written. The same number again (a later view) only records its pool request and returns
        // the frame start's result. An error names what the pool could not hold.
        [[nodiscard]] Common::BoolResultStr BeginViewFrame( uint64_t frame, uint32_t poolMiB,
                                                            const std::vector<std::shared_ptr<const Assets::MeshDistanceFieldVolume>>& fields );

        // Bumped whenever the pool was re-created: a view rewrites everything it composited from the old atlas.
        [[nodiscard]] uint64_t GetPoolGeneration() const
        {
            return m_PoolGeneration;
        }

        // This frame's copies of the records and the table (valid after BeginViewFrame).
        [[nodiscard]] const std::shared_ptr<ShaderResources::StorageBuffer>& GetAssetBuffer() const
        {
            return m_AssetBuffer;
        }
        [[nodiscard]] const std::shared_ptr<ShaderResources::StorageBuffer>& GetIndirectionBuffer() const
        {
            return m_IndirectionBuffer;
        }

        // The asset record index of @p field in this frame's resident set, and the distance in centimetres from
        // the view's camera to an object drawn with it - the largest per-axis gap to its box (the nearest of every
        // view's objects this frame counts; the next frame's streaming reads it). Allocates nothing: a field the
        // set lacks returns nullopt and waits for the next frame start (BeginViewFrame); so does a field without a
        // single mip (a mesh with no triangles), which never gets a record.
        [[nodiscard]] std::optional<uint32_t>
        Lookup( const std::shared_ptr<const Assets::MeshDistanceFieldVolume>& field, float distance );

        // How many mips of @p field are resident (0 = not resident): part of an object's identity in the global
        // field, so a mip arriving or leaving recomposites the object.
        [[nodiscard]] uint32_t ResidentMips( const Assets::MeshDistanceFieldVolume* field ) const;

        [[nodiscard]] const std::vector<glm::vec4>& GetAssetData() const
        {
            return m_AssetData;
        }
        [[nodiscard]] const std::vector<uint32_t>& GetIndirection() const
        {
            return m_Indirection;
        }
        // Bumped whenever GetAssetData or GetIndirection changes (a mip in or out, a pool change): a reader that
        // keeps a device copy uploads again only then.
        [[nodiscard]] uint64_t GetRevision() const
        {
            return m_Revision;
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
            std::vector<uint32_t> Bricks; // atlas slot of each brick of the mip, in the mip's brick order
            uint32_t              IndirectionOffset = 0;
            uint32_t              IndirectionCount  = 0;
        };
        struct Resident
        {
            std::weak_ptr<const Assets::MeshDistanceFieldVolume>   Field;
            uint32_t                                               AssetIndex = 0;
            uint32_t                                               NumMips    = 0; // resident, coarsest first
            std::array<ResidentMip, Assets::kDistanceFieldNumMips> Mips{};
            float                                                  Distance = 0.0f; // this frame's nearest use
            bool                                                   Used     = false;
        };

        // Makes the next finer mip of @p resident resident (slots, table, record row, queued bricks); false when
        // the pool has no room for it.
        [[nodiscard]] bool AddMip( Resident& resident, const Assets::MeshDistanceFieldVolume& field );
        // Drops the finest resident mip of @p resident.
        void DropMip( Resident& resident );
        void Release( Resident& resident );
        // Makes room for @p bricks by dropping finer mips of fields farther than @p distance, farthest first -
        // only when that is enough: false, and nothing dropped, when even every such mip would not make the
        // room (a request that cannot be met must not take mips the farther fields will only want back).
        bool                                MakeRoom( float distance, uint32_t bricks, const Resident* except );
        [[nodiscard]] Common::BoolResultStr EnsureAtlas();

        // The pool, in MiB of R8 bricks. A different size drops every resident field and re-creates the atlas:
        // true when that happened.
        [[nodiscard]] bool SetPoolMiB( uint32_t poolMiB );
        // Starts a frame's acquisition: forgets the distances of the last one.
        void BeginFrame();
        // Streams the finer mips toward the acquired distances, inside the pool and the per-frame upload
        // budget.
        void Stream();
        // Returns every field nothing holds any more to the free lists.
        void ReleaseUnused();
        // Writes the bricks Acquire and Stream queued into the atlas (created on the first call after a pool
        // change).
        [[nodiscard]] Common::BoolResultStr FlushUploads();
        // One line when the streaming settles (no field waits for a finer mip) after it moved.
        void LogStreaming();
        // The frame start of BeginViewFrame (the resident set, the atlas, the records).
        [[nodiscard]] Common::BoolResultStr DecideFrame( bool hadFrame, uint32_t pool, const std::vector<std::shared_ptr<const Assets::MeshDistanceFieldVolume>>& fields );
        // The record index of @p field, making it resident (coarsest mip) on first use, at @p distance. An error
        // when not even the coarse mip fits the pool. Frame start only.
        [[nodiscard]] Common::ResultStr<std::optional<uint32_t>>
        Acquire( const std::shared_ptr<const Assets::MeshDistanceFieldVolume>& field, float distance );

        std::unordered_map<const Assets::MeshDistanceFieldVolume*, Resident> m_Resident;
        BrickAllocator                                                     m_BrickSlots;
        RangeAllocator                                                     m_IndirectionSlots;
        RangeAllocator                                                     m_AssetSlots;
        std::vector<glm::vec4>                                             m_AssetData;
        std::vector<uint32_t>                                              m_Indirection;
        uint64_t                                                             m_Revision = 0;

        uint32_t                    m_PoolMiB        = 0;
        uint32_t                    m_CapacityBricks = 0; // the pool's ceiling; the atlas grows toward it
        std::shared_ptr<Image3D> m_Atlas;
        uint32_t                    m_AtlasSlices = 0; // the atlas's depth in brick slices
        uint32_t                    m_UploadedThisFrame = 0;
        DistanceFieldStreamingStats m_Stats;
        // Bricks waiting for the atlas: (slot, the field that holds the bytes, mip, brick within the mip).
        struct PendingBrick
        {
            uint32_t                                             Slot = 0;
            std::shared_ptr<const Assets::MeshDistanceFieldVolume> Field;
            uint32_t                                             Mip   = 0;
            uint32_t                                             Brick = 0;
        };
        std::vector<PendingBrick> m_Pending;

        ResourceOwnerInstance m_LedgerInstance; // the owning Core::Scene's
        uint64_t              m_Frame            = 0; // the engine frame BeginViewFrame last started
        bool                  m_HasFrame         = false;
        uint32_t              m_RequestedPoolMiB = 0; // the largest pool a view asked for this frame
        uint64_t              m_PoolGeneration   = 0;
        bool                  m_StreamingMoved   = false;
        // Per-frame buffers (persistent = false): each frame in flight has its own copy.
        std::shared_ptr<ShaderResources::StorageBuffer> m_AssetBuffer;
        std::shared_ptr<ShaderResources::StorageBuffer> m_IndirectionBuffer;
        // What the frame start decided: its error, returned to every later view of the frame.
        std::optional<std::string> m_FrameError;
        // Fields a view met that the frame's set lacked: acquired first at the next frame start.
        std::vector<std::shared_ptr<const Assets::MeshDistanceFieldVolume>> m_Waiting;
    };
} // namespace Desert::Graphic::DistanceField
