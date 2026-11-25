#pragma once

// THE SERVER'S COLLISION OF ITS CELLS (UE dedicated server: the same cooked collision as the client - the
// streamed level's static bodies and landscape heightfield collision components). ServerWorld hands every record
// it activates here; a record that collides on the client collides here, built from the same blocks:
//   - Collider + RigidBody (static): ECS::PhysicsECSSystem's body, at the record's world pose;
//   - LandscapeTile: ECS::LandscapeCollision's heightfield (World/Landscape/LandscapeCollisionShape), framed by
//     the tile's Landscape root record (LandscapeTileFrame).
// The pawns move through this world (Gameplay::PhysicsPawnMoveWorld). Bodies leave with their records (Destroy).

#include <Engine/Assets/Prefab/PrefabData.hpp>
#include <Engine/World/Landscape/LandscapeLayout.hpp>
#include <PhysicsCore/PhysicsWorld.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace Desert::Server
{
    class ServerCellCollision
    {
    public:
        explicit ServerCellCollision( Physics::PhysicsWorld& physics ) : m_Physics( physics )
        {
        }
        ~ServerCellCollision();
        ServerCellCollision( const ServerCellCollision& )            = delete;
        ServerCellCollision& operator=( const ServerCellCollision& ) = delete;

        // The collision record @p record (a world record index) carries, if any. @p unitRecords: the records
        // read with it (its unit), where its parents are. A landscape tile whose root is not yet held waits for it.
        void Add( std::size_t record, const Assets::EntityData& data, std::span<const Assets::EntityData> unitRecords );
        void Remove( std::size_t record );

        [[nodiscard]] std::size_t Bodies() const
        {
            return m_Bodies.size();
        }
        [[nodiscard]] std::size_t Heightfields() const
        {
            return m_Heightfields;
        }

    private:
        struct PendingTile
        {
            std::size_t Record = 0;
            std::string Root;
            int32_t     TileX = 0, TileZ = 0;
            std::string HeightFile;
        };
        void AddTile( const PendingTile& tile, const World::Landscape::LandscapeRoot& root );

        Physics::PhysicsWorld&                                        m_Physics;
        std::unordered_map<std::size_t, Physics::BodyHandle>          m_Bodies; // world record -> body
        std::unordered_map<std::size_t, bool>                         m_IsHeightfield;
        std::unordered_map<std::string, World::Landscape::LandscapeRoot> m_Roots; // root record id -> frame
        std::unordered_map<std::size_t, std::string>                  m_RootOfRecord;
        std::vector<PendingTile>                                      m_Pending;
        std::vector<float>                                            m_HeightsCm;
        std::size_t                                                   m_Heightfields = 0;
    };
} // namespace Desert::Server
