#pragma once

// The one route by which the world's WorldPartition block is edited in the editor: the World Partition panel
// (conversion, HLOD per grid, Runtime grids) and the control channel (`set world WorldPartition...`) both
// commit through EditWorldPartition, so every edit is one history entry and is refused by the same rules.

#include <Editor/Core/CommandHistory.hpp>

#include <Common/Core/ResultStr.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>
#include <Engine/Core/Serialize/WorldPartitionHLODRules.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <memory>
#include <optional>
#include <string>

namespace Desert::Editor
{
    // One history entry for "the world's partition changed": the block before and the block after.
    //
    // WEAK, not shared. A history entry that owned the scene would keep a closed world alive for as
    // long as the undo stack remembered it; ICommand's contract is that an entry whose target is
    // gone returns false and is discarded, and a weak pointer is how that is DETECTED rather than
    // assumed. The scene is addressed as a whole object because the partition is a property of the
    // world itself and has no id of its own. One class for every edit of the block (the conversion, a
    // grid's HLOD layer): what differs between them is only the label the history shows.
    class WorldPartitionEditCommand final : public ICommand
    {
    public:
        WorldPartitionEditCommand( std::string label, std::weak_ptr<::Desert::Core::Scene> scene,
                                   std::optional<::Desert::Core::WorldPartitionSerialized> before,
                                   ::Desert::Core::WorldPartitionSerialized                after )
             : m_Label( std::move( label ) ), m_Scene( std::move( scene ) ), m_Before( std::move( before ) ),
               m_After( std::move( after ) )
        {
        }

        [[nodiscard]] std::string GetLabel() const override
        {
            return m_Label;
        }

        bool Undo() override
        {
            return Apply( m_Before );
        }

        bool Redo() override
        {
            return Apply( m_After );
        }

    private:
        bool Apply( const std::optional<::Desert::Core::WorldPartitionSerialized>& partition )
        {
            const std::shared_ptr<::Desert::Core::Scene> scene = m_Scene.lock();
            if ( !scene )
                return false;
            scene->SetWorldPartition( partition );
            return true;
        }

        std::string                                             m_Label;
        std::weak_ptr<::Desert::Core::Scene>                    m_Scene;
        std::optional<::Desert::Core::WorldPartitionSerialized> m_Before;
        ::Desert::Core::WorldPartitionSerialized                m_After;
    };

    // THE GRIDS A WORLD MAY STATE - what Rules::PlanWorldPartition (step 4a) refuses as unusable, refused at the
    // edit instead of after it: at least one grid, every grid named once, a positive finite cell size; plus a
    // positive finite loading range (a range of 0 would make a grid that never streams anything in).
    [[nodiscard]] inline Common::BoolResultStr
    ValidateRuntimeGrids( const ::Desert::Core::WorldPartitionSerialized& partition )
    {
        if ( partition.Grids.empty() )
            return Common::MakeError<bool>( std::string( "a partitioned world needs at least one runtime grid" ) );
        for ( std::size_t g = 0; g < partition.Grids.size(); ++g )
        {
            const ::Desert::Core::WorldPartitionGridSerialized& grid = partition.Grids[g];
            if ( grid.Name.empty() )
                return Common::MakeError<bool>( std::format( "grid {} has no name", g ) );
            if ( std::any_of( partition.Grids.begin(), partition.Grids.begin() + static_cast<std::ptrdiff_t>( g ),
                              [&]( const ::Desert::Core::WorldPartitionGridSerialized& other )
                              { return other.Name == grid.Name; } ) )
                return Common::MakeError<bool>( std::format( "two grids are named '{}'", grid.Name ) );
            if ( !( grid.CellSize > 0.0f ) || !std::isfinite( grid.CellSize ) )
                return Common::MakeError<bool>( std::format( "grid '{}': the cell size must be positive, not {}",
                                                             grid.Name, grid.CellSize ) );
            if ( !( grid.LoadingRange > 0.0f ) || !std::isfinite( grid.LoadingRange ) )
                return Common::MakeError<bool>( std::format(
                     "grid '{}': the loading range must be positive, not {}", grid.Name, grid.LoadingRange ) );
        }
        return Common::MakeSuccess( true );
    }

    // One edit of the block: refused whole (nothing written, nothing recorded) or applied and recorded as ONE
    // history entry. An edit that changes nothing records nothing and answers false.
    [[nodiscard]] inline Common::BoolResultStr
    EditWorldPartition( const std::shared_ptr<::Desert::Core::Scene>& scene, std::string label,
                        ::Desert::Core::WorldPartitionSerialized after )
    {
        if ( !scene )
            return Common::MakeError<bool>( std::string( "no scene is open" ) );
        std::optional<::Desert::Core::WorldPartitionSerialized> before = scene->GetWorldPartition();
        if ( before == after )
            return Common::MakeSuccess( false );
        if ( auto grids = ValidateRuntimeGrids( after ); !grids )
            return grids;
        if ( auto layers = ::Desert::Core::Rules::ValidateHLODLayers( after ); !layers )
            return layers;
        scene->SetWorldPartition( after );
        CommandHistory::Get().PushCommand( std::make_unique<WorldPartitionEditCommand>(
             std::move( label ), scene, std::move( before ), std::move( after ) ) );
        return Common::MakeSuccess( true );
    }
} // namespace Desert::Editor
