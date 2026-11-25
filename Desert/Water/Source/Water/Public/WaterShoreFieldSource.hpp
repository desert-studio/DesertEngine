#pragma once

#include <Water/Public/WaterShoreField.hpp>

#include <Common/Core/UUID.hpp>

#include <entt/entt.hpp>

#include <atomic>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

namespace Desert::ECS
{
    struct LandscapeTileRef; // Landscape/Public/LandscapeRootOf.hpp

    /**
     * @brief The one owner of a world's shore field (Water::WaterShoreField): baked once for the registry and read
     * by the renderer (WaterZoneECSSystem) and by the physics' water query (WaterPhysicsFeature) alike.
     *
     * The bake is a job over copies of the tiles' samples; the previous field stays current while it runs. It is
     * keyed by what it reads: the tiles' heights (LandscapeTileData::HeightStamp), frames and roots, the bodies'
     * footprints and levels and the landscape materials' mean displacement.
     *
     * Lives in the registry's context as a shared_ptr, created by the module's Prepares hook; the two readers run
     * inside parallel groups, so every call is serialised by the source's own mutex.
     */
    class WaterShoreFieldSource final
    {
    public:
        using Field = std::shared_ptr<const Water::WaterShoreField>;

        /// The current field, null until the first bake is done or while the world has no water body.
        [[nodiscard]] Field Update( entt::registry& registry );

        /// The same, with the world's first bake finished before it returns (taken from the job when no worker
        /// has begun it): a body must not fall through the first seconds of water because its floor is still being
        /// read. Null: no body, or the bake was refused - said once, and baked again when the world changes.
        [[nodiscard]] Field UpdateNow( entt::registry& registry );

    private:
        struct Tile
        {
            std::vector<uint16_t>            Samples;
            uint32_t                         SamplesX = 0;
            uint32_t                         SamplesZ = 0;
            World::Landscape::LandscapeFrame Frame;
        };
        struct Input
        {
            std::vector<Tile>                  Tiles;
            std::vector<Water::WaterBodyLevel> Bodies;
            float                              SpacingCm = 0.0f;
            Field                              Base; // what the new field states its changes against
        };
        // One bake. Whoever claims it runs it: the job, or UpdateNow before a worker began.
        struct BakeJob
        {
            Input                                  World;
            std::atomic_flag                       Claimed;
            std::promise<Common::ResultStr<Field>> Promise;
            std::future<Common::ResultStr<Field>>  Done = Promise.get_future();

            void Run() noexcept;
        };
        // A root's mean displacement, resolved again only when its material's revision moved; a material that did
        // not resolve is not remembered.
        struct RootOffset
        {
            uint64_t Material     = 0;
            uint32_t Invalidation = 0;
            uint32_t Values       = 0;
            uint64_t Content      = 0;
            float    OffsetCm     = 0.0f;
            bool     Resolved     = false;
        };

        static Common::ResultStr<Field> Bake( const Input& input );

        // Adopts a finished bake, reads the world and starts the bake it asks for. False: no body.
        // @p now: a first bake is left unsubmitted, for the caller to run.
        bool  Refresh( entt::registry& registry, bool now );
        void  AdoptDone();
        // Fills m_Offsets and m_WorldKey for m_Bodies over @p tiles, reading only.
        void  ReadWorld( entt::registry& registry, std::span<const LandscapeTileRef> tiles );
        Input CopyInput( std::span<const LandscapeTileRef> tiles );
        // Fills m_Offsets for this call's tiles: each root is looked up once.
        void  ResolveRootOffsets( entt::registry& registry, std::span<const LandscapeTileRef> tiles );
        float RootOffsetOf( entt::registry& registry, const Common::UUID& root );

        std::mutex                               m_Mutex;
        Field                                    m_Field;
        std::shared_ptr<BakeJob>                 m_Job;
        uint64_t                                 m_Key      = 0; // what the newest bake was started for
        uint64_t                                 m_WorldKey = 0; // what the world is now
        std::vector<Water::WaterBodyLevel>       m_Bodies;
        std::unordered_map<uint64_t, RootOffset> m_Roots;   // by root, across calls
        std::unordered_map<uint64_t, float>      m_Offsets; // this call's
    };
} // namespace Desert::ECS
