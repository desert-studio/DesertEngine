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
// Refused by name, once per entity per Play: an empty Rest Collection, a fracture that cannot be read, a
// world scale other than one (the bake is in centimetres of the fracture's own space and is simulated as
// it is), and anything DestructionWorld::Add refuses.

#include <Engine/Assets/Common.hpp>
#include <Engine/Destruction/DestructionWorld.hpp>

#include <Common/Core/ResultStr.hpp>

#include <entt/entt.hpp>

#include <functional>
#include <memory>
#include <string>
#include <unordered_set>

namespace Desert::ECS
{
    class DestructibleLifetime final
    {
    public:
        /// Success with null = the fracture is still loading (asked again next Sync); an error = it never will.
        using FractureLookup = std::function<Common::ResultStr<std::shared_ptr<const Destruction::FractureData>>(
             const Assets::AssetHandle& )>;

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

        /// Adds every destructible entity not simulated yet.
        void Sync( entt::registry& registry, const FractureLookup& lookup );

    private:
        void OnDestructibleDestroyed( entt::registry& registry, entt::entity entity );
        void Refuse( entt::entity entity, const std::string& reason );

        Destruction::DestructionWorld*   m_World    = nullptr;
        entt::registry*                  m_Registry = nullptr;
        std::unordered_set<entt::entity> m_Refused;
    };
} // namespace Desert::ECS
