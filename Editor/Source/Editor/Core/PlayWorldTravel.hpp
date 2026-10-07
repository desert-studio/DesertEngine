#pragma once

// LEVEL TRAVEL IN PLAY-IN-EDITOR (UE: a PIE world is a DUPLICATE of the editor world,
// UEditorEngine::CreatePIEWorldByDuplication; a travel in PIE loads the new map into the PIE world context with
// the UEDPIE_ prefix through UEngine::TickWorldTravel -> LoadMap, and the editor's level is never touched;
// EndPlayMap destroys the PIE world and what remains is the level being authored).
//
// Ours plays IN the document's scene and keeps the authored level as the Play snapshot, so the same contract
// reads: a travel replaces the PLAYED world, never the snapshot; Stop restores the snapshot whatever map Play
// ended on; neither touches the authored document -- not its file (SceneFiles' open path) and not its dirty
// star (the CommandHistory revision: the undo stack of the world being left is dropped, which moves no revision).
//
// This object owns that state and those decisions; the host (PlaySession) supplies the two world operations:
// load a map into the played scene, and restore the snapshot into it.

#include <Engine/Core/LevelTravel.hpp>
#include "Editor/Core/CommandHistory.hpp"

#include <Common/Core/ResultStr.hpp>

#include <functional>
#include <string>
#include <utility>

namespace Desert::Editor
{
    class PlayWorldTravel
    {
    public:
        // Tear the played world down, load the map at the resolved path, begin Play in it.
        using LoadMap = std::function<Common::BoolResultStr( const std::string& path )>;
        // Rebuild the authored level from its snapshot.
        using RestoreAuthored = std::function<Common::BoolResultStr( const std::string& snapshot )>;

        explicit PlayWorldTravel( ::Desert::Core::LevelTravel& travel = ::Desert::Core::LevelTravel::Get() ) : m_Travel( travel )
        {
        }

        // Play began on the authored level serialized as @p authoredSnapshot.
        void Begin( std::string authoredSnapshot )
        {
            m_Snapshot = std::move( authoredSnapshot );
            m_CurrentMap.clear();
        }

        [[nodiscard]] bool Active() const
        {
            return !m_Snapshot.empty();
        }
        [[nodiscard]] const std::string& AuthoredSnapshot() const
        {
            return m_Snapshot;
        }
        // The map the played world travelled to; empty while it is still the authored level.
        [[nodiscard]] const std::string& CurrentMap() const
        {
            return m_CurrentMap;
        }

        // THE EDITOR'S FRAME BOUNDARY FOR Core::OpenLevel. In Play the queued travel is loaded into the played
        // world; outside Play there is no game world, and the level open is the one being authored -- the
        // request is refused with its target rather than left queued or applied to the document.
        // Returns false when nothing was pending; the refusal or the load's own error otherwise.
        [[nodiscard]] Common::BoolResultStr Tick( const LoadMap& load )
        {
            if ( !Active() )
            {
                return m_Travel.TickTravel(
                     []( const std::string& path )
                     {
                         return Common::MakeFormattedError<bool>(
                              "OpenLevel('{}') needs a game world -- press Play; the level being authored is "
                              "not replaced",
                              path );
                     } );
            }
            return m_Travel.TickTravel(
                 [this, &load]( const std::string& path ) -> Common::BoolResultStr
                 {
                     // The undo stack recorded in the world being left targets entities about to be destroyed.
                     // Clear() drops the stacks and keeps the revision: the authored document stays as clean
                     // or as dirty as it was when Play began.
                     CommandHistory::Get().Clear();
                     auto loaded = load( path );
                     // Even a failed load has torn the old played world down: whatever is there now is not
                     // the authored level, and Stop restores the snapshot either way.
                     m_CurrentMap = path;
                     return loaded;
                 } );
        }

        // Stop (UEditorEngine::EndPlayMap): a travel queued by the ending session dies with it, and the
        // authored level comes back from the snapshot whichever map was played last.
        [[nodiscard]] Common::BoolResultStr End( const RestoreAuthored& restore )
        {
            m_Travel.Cancel();
            if ( !Active() )
                return Common::MakeSuccess( false );
            const std::string snapshot = std::exchange( m_Snapshot, {} );
            m_CurrentMap.clear();
            return restore( snapshot );
        }

        // The document Play was bound to closed: nothing to restore into.
        void Discard()
        {
            m_Travel.Cancel();
            m_Snapshot.clear();
            m_CurrentMap.clear();
        }

    private:
        ::Desert::Core::LevelTravel& m_Travel;
        std::string        m_Snapshot;   // the authored level, serialized when Play began
        std::string        m_CurrentMap; // resolved path of the last travel; empty = the authored level
    };
} // namespace Desert::Editor
