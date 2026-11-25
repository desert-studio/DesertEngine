#pragma once

#include <Water/Public/WaterWaveTable.hpp>

#include <entt/entt.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace Desert::ECS
{
    /**
     * @brief The world's wave sources: which waves each of its water bodies is drawn with and floated on.
     *
     * Reference: UE 5 Water keeps one state per body per world and both the renderer and the queries read it. Here
     * it is one record per world, so the two cannot choose differently: a lane per distinct (wave set, WaveOrigin)
     * - a body past Water::kMaxWaveLanes is flat for everybody, a set that cannot be read is flat for everybody -
     * and the lane's state. The FFT ocean is a lane's state only once a renderer of this world reported it can
     * draw it.
     *
     * ONE WORLD, ONE OCEAN: a world's bodies name at most one FFT wave set. Its owner is the body of the lowest
     * UUID (the persistent id, so the answer survives a reload, a paste and an undo); a body naming another FFT
     * set is refused - an error naming both, once per change - and is no water for anybody. While the lowest
     * body's set is still being read, the other FFT bodies wait (Pending) rather than be handed the ocean.
     *
     * Lives in the registry's context as a shared_ptr, made with the scene by the module's scene state
     * (ModuleHooks::SceneStates). The module's Prepares hook refreshes it once a frame, serially and before any
     * system: every reader of the frame then sees the same record and only reads.
     */
    class WaterWaveSources final
    {
    public:
        WaterWaveSources();

        /// Brings the record up to the registry. Main thread, no reader running.
        void Refresh( entt::registry& registry );

        /// A scene was read into the world: its refusals are said again, and the reads of its bodies' sets start.
        void SceneLoaded( const entt::registry& registry );

        /// The world's entities are gone (Scene::Clear): another ocean, another verdict.
        void Reset();

        /// Never null.
        [[nodiscard]] const std::shared_ptr<const Water::WaterWaveTable>& Table() const noexcept
        {
            return m_Table;
        }

        /// The body's lane in Table(), -1 for a flat one.
        [[nodiscard]] int32_t LaneOf( entt::entity entity ) const noexcept;

        /// Whether the body is no water: a second ocean.
        [[nodiscard]] bool IsRefused( entt::entity entity ) const noexcept;

        /// 0 for a world that refuses nobody; another refused set, another value (the shore field's key).
        [[nodiscard]] uint64_t RefusedKey() const noexcept
        {
            return m_RefusedKey;
        }

    private:
        enum class Choice : uint8_t
        {
            Flat, // names no set, or one that cannot be read
            Wait, // its set, or the ocean's owner, is not read yet
            Gerstner,
            Ocean,
            Refused
        };
        // One body: what the rule is a function of.
        struct Row
        {
            entt::entity Entity = entt::null;
            uint64_t     Waves  = 0; // the set's handle, 0 for none
            uint64_t     UUID   = 0;

            [[nodiscard]] bool operator==( const Row& ) const = default;
        };
        // And the rule's answer, with the frame's lane.
        struct Answer
        {
            entt::entity Entity = entt::null;
            Choice       Is     = Choice::Flat;
            int32_t      Lane   = -1;
        };

        // The body's row as the registry states it now; whether every row of m_Rows still is (its revision stands).
        [[nodiscard]] static Row    RowOf( const entt::registry& registry, entt::entity entity );
        [[nodiscard]] bool          RowsStand( const entt::registry& registry ) const;
        void                        StateWinds( entt::registry& registry );
        void                        Decide( const entt::registry& registry );
        void                        BuildLanes( const entt::registry& registry );
        [[nodiscard]] const Answer* Find( entt::entity entity ) const noexcept;

        std::vector<Row>    m_Rows;         // by entity: what m_Answers was decided for
        std::vector<Answer> m_Answers;      // by entity
        uint64_t            m_Revision = 0; // WaterBodyRevision and WaterWavesService::ShapeSerial of the decision
        uint64_t            m_ShapeSerial = 0;
        uint64_t            m_RefusedKey  = 0;
        // The refusals already said, and the body they were said against.
        std::vector<entt::entity> m_Said;
        entt::entity              m_SaidOwner   = entt::null;
        bool                      m_SaidTooMany = false;

        std::vector<Assets::AssetHandle>             m_WindStated; // storage reused
        std::vector<Water::WaterWaveLane>            m_Lanes;      // this refresh's lanes, storage reused
        std::shared_ptr<Water::WaterFFTSurface>      m_Surface;
        std::shared_ptr<const Water::WaterWaveTable> m_Table;
    };
} // namespace Desert::ECS
