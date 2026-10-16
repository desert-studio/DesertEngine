#pragma once

// A DESTRUCTIBLE ENTITY TAKES ITS FRACTURE INTO THE DESTRUCTION WORLD, AND OUT AGAIN.
//
// Sync() adds every Transform + DestructibleComponent entity that is not simulated yet to the scene's
// Destruction::DestructionWorld at its world pose (UE: UGeometryCollectionComponent registering its physics
// proxy at BeginPlay). The fracture comes from the caller's lookup, so the system reads it from the
// FractureService and a suite hands it a fracture built in memory. The release rides EnTT's
// on_destroy<DestructibleComponent>, which fires for a destroyed entity and a removed component alike —
// PhysicsBodyLifetime.hpp's shape and reason.
//
// THE RUN-TIME SOURCES (DST-RUNTIME). A Baked entity's fracture is its `.dfrac` (the caller's lookup). On Load and
// At Impact cut the entity's own static mesh (its editable mesh, its primitive or its mesh asset — the order the
// mesh draw takes them in) on a worker (Destruction/RuntimeFracture.hpp); Sync asks again each frame and the
// entity is not simulated until its cut is ready.
//   On Load   — one level of RuntimeSites Uniform Voronoi sites, seeded by the entity's UUID: two identical
//               crates break differently, and the same crate the same way on every run.
//   At Impact — first the WHOLE (the mesh with no cut: one node, one body) so it falls and is hit like any
//               destructible. After each physics step CutOnImpact takes the strongest contact on it whose impulse
//               reaches its level-1 damage threshold and sends the mesh to be cut with a Radial level centred on
//               the hit (fracture space), seeded by the UUID and the hit point. When the cut is ready the whole is
//               replaced by it at the whole's pose and velocity, and the hit's impulse is applied to the cut as an
//               external strain falling off over Impact Radius (DestructionWorld::ApplyField) — what broke the
//               whole breaks the pieces at the hit. The swap lands one or two frames after the hit.
// The fracture the world simulates for a run-time entity is on its component (RuntimeFracture) for the draw.
//
// Refused by name, once per entity per Play: an empty Rest Collection (Baked), a fracture that cannot be read or
// cut, a run-time entity without a static mesh, more than kMaxRuntimeSites sites, a world scale other than one
// (the bake is in centimetres of the fracture's own space and is simulated as it is), and anything
// DestructionWorld::Add refuses.

#include <Engine/Assets/Common.hpp>
#include <Engine/Destruction/DestructionWorld.hpp>
#include <Engine/Destruction/RuntimeFracture.hpp>

#include <Common/Core/ResultStr.hpp>

#include <entt/entt.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace Desert::ECS
{
    class DestructibleLifetime final
    {
    public:
        /// Success with null = the fracture is still loading (asked again next Sync); an error = it never will.
        using FractureLookup = std::function<Common::ResultStr<std::shared_ptr<const Destruction::FractureData>>(
             const Assets::AssetHandle& )>;

        /// The file behind a static mesh asset, empty while the asset is not loaded (MeshService).
        using MeshFileLookup = std::function<std::filesystem::path( const Assets::AssetHandle& )>;

        explicit DestructibleLifetime( Destruction::DestructionWorld& world ) : m_World( &world )
        {
        }
        ~DestructibleLifetime();

        DestructibleLifetime( const DestructibleLifetime& )            = delete;
        DestructibleLifetime& operator=( const DestructibleLifetime& ) = delete;

        /// Listens on @p registry from now on; idempotent, moves to a new registry (callable every frame).
        void Attach( entt::registry& registry );
        /// Stops listening. Must run before the world it releases into goes.
        void Detach();

        /// Adds every destructible entity not simulated yet, and swaps in every At Impact cut that is ready.
        /// @p meshFile resolves the static mesh asset a run-time source cuts; without it such a mesh is refused.
        void Sync( entt::registry& registry, const FractureLookup& lookup, const MeshFileLookup& meshFile = {} );

        /// After a physics step: every At Impact whole a contact reached its threshold on is sent to be cut
        /// around the strongest such hit (see the header comment). Run before PublishEvents.
        void CutOnImpact( entt::registry& registry );

        /// Replaces @p registry's DestructionEventQueue with the world's events since its last ClearEvents that
        /// their entity's Notify flags ask for, named by destructible entity (UE UGeometryCollectionComponent::
        /// DispatchBreakEvent / DispatchRemovalEvent / collision dispatch). Run after the physics step that
        /// made them.
        void PublishEvents( entt::registry& registry ) const;

        /// After a physics step: every simulated destructible's SimulatedNodeWorld from its bodies (UE: the
        /// component's global matrices pulled from the physics proxy), what its pieces draw at.
        void WritePoses( entt::registry& registry ) const;

    private:
        struct RuntimeState
        {
            enum class StageKind
            {
                Cutting, // the first cut (On Load) or the whole (At Impact) is on a worker
                Whole,   // At Impact: the whole is simulated, waiting for a hit
                Struck,  // At Impact: the cut around the hit is on a worker
                Done,    // nothing more to make
            };
            StageKind                                 Stage  = StageKind::Cutting;
            Destruction::RuntimeFractureQueue::Ticket Ticket = 0;
            Destruction::RuntimeFractureRequest       Source; // the source and its GUIDs; Settings per cut
            uint64_t                                  Seed           = 0;
            float                                     TriggerImpulse = 0.0f; // kg*cm/s
            glm::vec3                                 HitLocal{ 0.0f };      // fracture space, cm
            float                                     HitImpulse = 0.0f;
        };

        void OnDestructibleDestroyed( entt::registry& registry, entt::entity entity );
        void Refuse( entt::entity entity, const std::string& reason );
        /// The source of @p entity's run-time cuts, or why it has none.
        [[nodiscard]] Common::ResultStr<Destruction::RuntimeFractureRequest>
        RuntimeSource( entt::registry& registry, entt::entity entity, const MeshFileLookup& meshFile ) const;
        /// Its world pose as a DestructibleDesc with the component's settings, or why it cannot be simulated.
        [[nodiscard]] Common::ResultStr<Destruction::DestructibleDesc> DescOf( entt::registry& registry,
                                                                               entt::entity    entity ) const;
        /// The ready At Impact cuts replace their wholes.
        void SwapStruck( entt::registry& registry );

        Destruction::DestructionWorld*                 m_World    = nullptr;
        entt::registry*                                m_Registry = nullptr;
        std::unordered_set<entt::entity>               m_Refused;
        std::unordered_map<entt::entity, RuntimeState> m_Runtime;
        Destruction::RuntimeFractureQueue              m_Cuts;
    };
} // namespace Desert::ECS
