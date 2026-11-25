#pragma once

#include <Water/Public/WaterShoreField.hpp>

#include <Common/Core/UUID.hpp>

#include <entt/entt.hpp>

#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace Desert::ECS
{
    /**
     * @brief The one owner of a world's shore field (Water::WaterShoreField): baked once for the registry and read
     * by the renderer (WaterZoneECSSystem) and by the physics' water query (WaterPhysicsFeature) alike.
     *
     * The bake is a job over copies of the tiles' samples; the previous field stays current while it runs. It is
     * keyed by what it reads: the tiles and their roots, the height edits (LandscapeDirtyConsumer::Water), the
     * bodies' footprints and levels and the landscape materials' mean displacement.
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

        /// The same, baking the world's first field on the calling thread: a body must not fall through the first
        /// seconds of water because its floor is still being read.
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
        };
        // A root's mean displacement, resolved again only when its material's revision moved.
        struct RootOffset
        {
            uint64_t Material     = 0;
            uint32_t Invalidation = 0;
            uint32_t Values       = 0;
            uint64_t Content      = 0;
            float    OffsetCm     = 0.0f;
        };

        static Common::ResultStr<Field> Bake( const Input& input );

        void Adopt( Common::ResultStr<Field> baked );
        // Fills m_Bodies and m_WorldKey; takes the tiles' water edits unless a bake is under way. False: no body.
        bool  ReadWorld( entt::registry& registry );
        Input CopyInput( entt::registry& registry );
        float RootOffsetOf( entt::registry& registry, const Common::UUID& root );

        std::mutex                               m_Mutex;
        Field                                    m_Field;
        std::future<Common::ResultStr<Field>>    m_Job;
        uint64_t                                 m_Key      = 0; // what the newest bake was started for
        uint64_t                                 m_WorldKey = 0; // what the world is now
        uint64_t                                 m_Edits    = 0; // height edits seen
        std::vector<Water::WaterBodyLevel>       m_Bodies;
        std::unordered_map<uint64_t, RootOffset> m_Roots;
    };
} // namespace Desert::ECS
