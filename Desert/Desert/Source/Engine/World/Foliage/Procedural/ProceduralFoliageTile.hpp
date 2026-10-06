#pragma once

// Ported from UE 5.8 Engine/Source/Runtime/Foliage/{Public,Private}/ProceduralFoliageTile.{h,cpp}. Adapted: the
// instances live in one vector addressed by id and the living set is kept in insertion order (UE iterates a TSet
// of pointers, whose order is an implementation detail: "short term determinism, but not long term"); a copied
// instance keeps its rotation instead of drawing a new one; which copied instances block is decided by the
// caller (CopyInstancesToTile's blocking box), so a tile's own spread past its region never blocks a neighbour's
// plants; no editor cancel counter and no physics (ExtractDesiredInstances' traces belong to the placement step).

#include <Engine/World/Foliage/Procedural/ProceduralFoliageBroadphase.hpp>
#include <Engine/World/Foliage/Procedural/ProceduralFoliageInstance.hpp>

#include <cstdint>
#include <vector>

namespace Desert::World::Foliage::Procedural
{
    class ProceduralFoliageSpawner;

    /**
     * @brief Grows foliage over one square of TileSize: seeds, then generations that age and spread them, every
     *        newcomer competing with its neighbours for room and light (UE UProceduralFoliageTile).
     *
     * The same spawner and seed grow the same instances, in the same order, on every run.
     */
    class ProceduralFoliageTile
    {
    public:
        /// Readies an empty tile (UE InitSimulation; Simulate calls it).
        void InitSimulation( const ProceduralFoliageSpawner& spawner, int32_t randomSeed );

        /// Grows the tile: the types that do not spawn in shade, then those that do, in their own generations.
        /// @p maxNumSteps < 0 runs every type's NumSteps + 1 generations.
        void Simulate( const ProceduralFoliageSpawner& spawner, int32_t randomSeed, int32_t maxNumSteps = -1 );

        /**
         * @brief Copies this tile's instances whose lower corner (location minus the larger radius) lies in
         *        @p blocking into @p toTile, moved by @p offset (UE CopyInstancesToTile).
         *
         * An instance is owned by the region its lower corner lies in, so a plant straddling a border belongs to
         * one tile. Those in @p owned (half-open: [Min, Max)) are placed by @p toTile; the rest become blockers
         * there: a neighbouring composite places them, so they compete without being placed. @p blocking must
         * hold @p owned and only what some composite places.
         */
        void CopyInstancesToTile( ProceduralFoliageTile& toTile, const Box2& owned, const Box2& blocking,
                                  glm::vec2 offset ) const;

        /// The living instances that are not blockers, ordered by location (x, then y).
        [[nodiscard]] std::vector<ProceduralFoliageInstance> PlacedInstances() const;

        /// Every living instance, blockers included, in the order the simulation keeps them.
        [[nodiscard]] std::vector<ProceduralFoliageInstance> LivingInstances() const;

        void Empty();

    private:
        [[nodiscard]] const Assets::Serialization::FoliageProcedural& TypeOf( uint32_t typeIndex ) const;
        [[nodiscard]] InstanceRadii                                   RadiiOfId( uint32_t id ) const;

        void RunSimulation( int32_t maxNumSteps, bool onlyInShade );
        void StepSimulation();
        void AddRandomSeeds( std::vector<uint32_t>& out );
        void AgeSeeds();
        void SpreadSeeds( std::vector<uint32_t>& out );
        /// UE NewSeed: files a new instance and resolves its overlaps; its id if it survives. @p rotationFrom
        /// keeps a copied instance's rotation (nullptr draws one).
        [[nodiscard]] int64_t   NewSeed( glm::vec2 location, float scale, uint32_t typeIndex, float age,
                                         bool blocker, const ProceduralFoliageInstance* rotationFrom = nullptr );
        bool                    HandleOverlaps( uint32_t id );
        void                    MarkPendingRemoval( uint32_t id );
        void                    FlushPendingRemovals();
        void                    InstancesToArray();
        [[nodiscard]] float     RandomGaussian();
        [[nodiscard]] glm::vec2 SeedOffset( const Assets::Serialization::FoliageProcedural& type,
                                            float                                           minDistance );
        [[nodiscard]] float     SeedMinDistance( const ProceduralFoliageInstance& instance, float newAge ) const;

        const ProceduralFoliageSpawner*        m_Spawner = nullptr;
        std::vector<ProceduralFoliageInstance> m_Storage; ///< every instance ever made, by id
        std::vector<uint32_t>                  m_Living;  ///< UE InstancesSet, in insertion order
        std::vector<uint32_t>                  m_PendingRemovals;
        std::vector<ProceduralFoliageInstance> m_Array; ///< UE InstancesArray: the last pass's placed instances
        ProceduralFoliageBroadphase            m_Broadphase;
        RandomStream                           m_Stream;
        int32_t                                m_SimulationStep = 0;
        int32_t                                m_RandomSeed     = 0;
        bool                                   m_OnlyInShade    = false;
    };
} // namespace Desert::World::Foliage::Procedural
