// The two pure rules of Engine/Core/PlayerStart.hpp, in their own translation unit so a test compiles them
// without the scene, the asset manager and the renderer behind SpawnDefaultPawn.
#include "PlayerStart.hpp"

#include <vector>

namespace Desert::Core
{
    namespace
    {
        std::string NameList( std::span<const PlayerStartCandidate> starts,
                              const std::vector<std::size_t>&       picked )
        {
            std::string out;
            for ( const std::size_t i : picked )
            {
                if ( !out.empty() )
                    out += ", ";
                out += "'" + starts[i].Name + "'";
                if ( !starts[i].Tag.empty() )
                    out += " (tag '" + starts[i].Tag + "')";
            }
            return out;
        }

    } // namespace

    Common::ResultStr<std::size_t> ChoosePlayerStart( std::span<const PlayerStartCandidate> starts,
                                                      std::string_view                      requestedTag )
    {
        std::vector<std::size_t> all( starts.size() );
        for ( std::size_t i = 0; i < starts.size(); ++i )
            all[i] = i;

        if ( starts.empty() )
            return Common::MakeError<std::size_t>( "the level has no PlayerStart - place one (Add Component -> Player Start) "
                                      "where the pawn should appear; Play does not fall back to the origin" );

        if ( !requestedTag.empty() )
        {
            std::vector<std::size_t> tagged;
            for ( std::size_t i = 0; i < starts.size(); ++i )
                if ( starts[i].Tag == requestedTag )
                    tagged.push_back( i );
            if ( tagged.size() == 1 )
                return Common::MakeSuccess<std::size_t>( tagged.front() );
            if ( tagged.empty() )
                return Common::MakeError<std::size_t>( "no PlayerStart carries the tag '" + std::string( requestedTag ) +
                                          "'; the level has " + NameList( starts, all ) );
            return Common::MakeError<std::size_t>( std::to_string( tagged.size() ) + " PlayerStarts carry the tag '" +
                                      std::string( requestedTag ) + "': " + NameList( starts, tagged ) +
                                      " - a tag must name one start" );
        }

        if ( starts.size() == 1 )
            return Common::MakeSuccess<std::size_t>( std::size_t{ 0 } );

        std::vector<std::size_t> untagged;
        for ( std::size_t i = 0; i < starts.size(); ++i )
            if ( starts[i].Tag.empty() )
                untagged.push_back( i );
        if ( untagged.size() == 1 )
            return Common::MakeSuccess<std::size_t>( untagged.front() );
        if ( untagged.empty() )
            return Common::MakeError<std::size_t>(
                 "every PlayerStart is tagged and none was asked for: " + NameList( starts, all ) +
                 " - leave one untagged as the default start, or ask for a tag" );
        return Common::MakeError<std::size_t>( std::to_string( untagged.size() ) +
                                  " PlayerStarts have no tag: " + NameList( starts, untagged ) +
                                  " - either could be the start; tag all but one (or remove the extra)" );
    }

    Common::ResultStr<ViewTargetChoice> ChooseViewTarget( const ViewTargetInputs& in )
    {
        if ( in.PawnHasCamera )
            return Common::MakeSuccess( ViewTargetChoice{ ViewTargetKind::PawnCamera } );
        if ( in.AutoActivateCameras.size() > 1 )
        {
            std::string names;
            for ( const auto& n : in.AutoActivateCameras )
                names += ( names.empty() ? "'" : ", '" ) + n + "'";
            return Common::MakeError<ViewTargetChoice>(
                 "level '" + in.Level + "': " + std::to_string( in.AutoActivateCameras.size() ) +
                 " cameras have Auto Activate for Player (" + names +
                 ") - Play would have to pick one by entity order; clear it on all but one" );
        }
        if ( in.AutoActivateCameras.size() == 1 )
            return Common::MakeSuccess( ViewTargetChoice{ ViewTargetKind::AutoActivateCamera, 0 } );
        if ( in.PlayFromHere )
            return Common::MakeSuccess( ViewTargetChoice{ ViewTargetKind::EditorCamera } );
        return Common::MakeError<ViewTargetChoice>(
             "level '" + in.Level + "' has no view for the player: " +
             ( in.PawnSpawned ? "the pawn has no CameraComponent, and " : "it spawns no pawn, and " ) +
             "no camera has Auto Activate for Player. Give the pawn a camera, tick the flag on one scene "
             "camera, or use Play from Here to look through the editor camera" );
    }

} // namespace Desert::Core
