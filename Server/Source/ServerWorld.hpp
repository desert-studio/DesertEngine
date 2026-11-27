#pragma once

// THE SERVER'S WORLD (NET-ARCH §4.1, §6): one cooked World Partition world streamed headless - the same index,
// plan and residency executor the client's WorldStreamer runs, with a ResidencyWorld that holds a cell's records
// as data instead of making drawable entities of them. No scene, no renderer, no device.

#include "ServerCellCollision.hpp"

#include <Engine/Core/Serialize/CookedWorld.hpp>
#include <Engine/Core/Serialize/WorldCellLoader.hpp>
#include <Engine/Core/Serialize/WorldPartitionResidencyExecutor.hpp>
#include <Engine/Gameplay/TeleportSpot.hpp>

#include <Net/PawnTeleport.hpp>

#include <Common/Core/ResultStr.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Desert::Server
{
    // A cell unit resident on this server: its residency unit, the index's name for it and its record count.
    struct ResidentCell
    {
        std::size_t Unit = 0;
        std::string Name;
        std::size_t Records = 0;
    };

    // Also the simulation's view of the streamed world (Net::IWorldResidency): whether the collision at a point a
    // pawn is to be placed at is resident yet, asked of the executor's state, never copied (NET-TELEPORT-ENCROACH).
    class ServerWorld final : public Core::Rules::ResidencyWorld, public Net::IWorldResidency
    {
    public:
        // Starts streaming @p world: its always-loaded units are resident, no cell is yet (the first Step starts
        // loading what the sources want). @p settings: the project's World Partition settings. @p physics: the
        // server's collision world - every record the server holds that collides on the client (a static collider,
        // a landscape tile) is a body in it while it is held (ServerCellCollision), so the pawns sweep the cells
        // the client draws. @p search: the game placer's search (the one main.cpp gives Gameplay::PawnTeleporter) -
        // ResidencyAt counts a cell by the column it reaches. An index whose cell states Bounds but no Height is
        // refused, naming the cell.
        [[nodiscard]] static Common::ResultStr<std::unique_ptr<ServerWorld>>
        Begin( Core::CookedWorldStart world, const Core::Rules::ResidencySettings& settings,
               Physics::PhysicsWorld& physics, const Gameplay::TeleportSearch& search );

        // One server step: collect the reads that finished, step the residency from @p sources, apply it.
        [[nodiscard]] Common::ResultStr<Core::Rules::ResidencyTick>
        Step( std::span<const Core::Rules::StreamingSource> sources, double nowSeconds );

        // The cell units Activated now (the residency union of the sources of the last Step), in unit order.
        [[nodiscard]] std::vector<ResidentCell> ResidentCells() const;
        // Records of the activated cell units this server holds (Activate / Destroy).
        [[nodiscard]] std::size_t HeldCellRecords() const
        {
            return static_cast<std::size_t>( std::count( m_Held.begin(), m_Held.end(), true ) );
        }
        [[nodiscard]] std::size_t AlwaysLoadedUnits() const
        {
            return m_Executor->Plan().AlwaysLoaded.size();
        }
        [[nodiscard]] std::size_t LoadsInFlight() const
        {
            return m_Loader->InFlight();
        }
        [[nodiscard]] const ServerCellCollision& Collision() const
        {
            return *m_Collision;
        }
        [[nodiscard]] const Core::CookedWorldStart& World() const
        {
            return m_World;
        }

        // IWorldResidency: the cell units whose loose bounds hold (X, Z) of @p position and whose height span,
        // grown by the search's column, reaches its Y - none is NoCell (nothing will ever load there; the note
        // says whether no cell holds (X, Z) or the cells there do not reach Y), all Activated is Resident (their
        // bodies are in the physics world), else Loading. Every outcome carries its reason in the note.
        [[nodiscard]] Net::PointResidency ResidencyAt( const Net::NetVec3& position ) const override;

        // ResidencyWorld: a cell's records are held as the loader read them (the server's record set), not made
        // into entities - the scene-side server (NET-1: PlayerControllers, replicated entities) builds on this.
        [[nodiscard]] Common::BoolResultStr Activate( std::size_t unit, std::size_t firstMember,
                                                      std::span<const std::size_t> records ) override;
        void                                Destroy( std::span<const std::size_t> records ) override;
        void                                StartLoad( std::size_t unit, std::uint64_t ticket ) override;
        void                                CancelLoad( std::size_t unit, std::uint64_t ticket ) override;
        void                                Unload( std::size_t unit ) override;
        [[nodiscard]] std::vector<Core::Rules::LoadOutcome> TakeLoadOutcomes() override;

    private:
        ServerWorld( Core::CookedWorldStart world, const Gameplay::TeleportSearch& search );

        Core::CookedWorldStart                        m_World;
        Gameplay::TeleportSearch                      m_Search; // the placer's search (Begin's @p search)
        std::optional<Core::Rules::ResidencyExecutor> m_Executor;
        std::unique_ptr<Core::WorldCellLoader>        m_Loader;
        std::vector<bool>                             m_Held; // per world record: held by an activated unit
        std::unique_ptr<ServerCellCollision>          m_Collision;
    };
} // namespace Desert::Server
