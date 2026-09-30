// The pure rules of Engine/Core/PlayerStart.hpp and PawnBodyRules.hpp, in their own translation unit so a test
// compiles them without the scene, the asset manager and the renderer behind SpawnDefaultPawn.
#include "PawnBodyRules.hpp"
#include "PlayerStart.hpp"

#include <Engine/Assets/Prefab/PrefabOverrides.hpp>

#include <algorithm>
#include <format>
#include <span>
#include <vector>

namespace Desert::Core
{
    namespace
    {
        // An override not yet applied, with the part of its path still to walk from the records being read.
        struct PendingOverride
        {
            const Assets::PrefabOverrideData* Over;
            std::span<const Common::UUID>     Rest;
        };

        using PawnBlock = std::optional<Common::Json::Value>;

        Common::ResultStr<PawnBlock> FindPawnBlock( const std::vector<Assets::EntityData>& records,
                                                    const std::vector<PendingOverride>&    pending,
                                                    const NestedPrefabRecords&             nested,
                                                    std::vector<std::string>&              stack )
        {
            for ( const Assets::EntityData& record : records )
            {
                if ( record.PrefabPath )
                {
                    // A link, not an entity (PrefabRecordPolicy::InstantiatedLater): the nested file's body
                    // stands here, under the overrides addressed into it - its own first, the outer ones after.
                    const std::string& path = *record.PrefabPath;
                    if ( std::find( stack.begin(), stack.end(), path ) != stack.end() )
                        return Common::MakeFormattedError<PawnBlock>( "the prefab '{}' nests itself", path );
                    const auto body = nested( path );
                    if ( !body )
                        return Common::MakeFormattedError<PawnBlock>( "the nested prefab '{}': {}", path,
                                                                      body.GetError() );
                    std::vector<PendingOverride> inner;
                    if ( record.PrefabOverrides )
                        for ( const Assets::PrefabOverrideData& over : *record.PrefabOverrides )
                            inner.push_back( { &over, over.Path } );
                    for ( const PendingOverride& outer : pending )
                        if ( record.id && outer.Rest.size() > 1 && outer.Rest.front() == *record.id )
                            inner.push_back( { outer.Over, outer.Rest.subspan( 1 ) } );
                    stack.push_back( path );
                    auto found = FindPawnBlock( *body.GetValue(), inner, nested, stack );
                    stack.pop_back();
                    if ( !found || found.GetValue() )
                        return found;
                    continue;
                }
                PawnBlock block;
                if ( const auto own = record.Components.get( "CharacterController" ); own.has_value() )
                    block = own.value();
                for ( const PendingOverride& over : pending )
                {
                    if ( !record.id || over.Rest.size() != 1 || over.Rest.front() != *record.id )
                        continue;
                    const auto fields = over.Over->Components.get( "CharacterController" );
                    if ( !fields.has_value() )
                        continue;
                    block = block ? Assets::MergePayload( *block, fields.value() ) : fields.value();
                }
                if ( block )
                    return Common::MakeSuccess( std::move( block ) );
            }
            return Common::MakeSuccess( PawnBlock{} );
        }
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
            return Common::MakeError<std::size_t>(
                 "the level has no PlayerStart - place one (Add Component -> Player Start) "
                 "where the pawn should appear; Play does not fall back to the origin" );

        if ( !requestedTag.empty() )
        {
            std::vector<std::size_t> tagged;
            for ( std::size_t i = 0; i < starts.size(); ++i )
                if ( starts[i].Tag == requestedTag )
                    tagged.push_back( i );
            if ( tagged.size() == 1 )
                return Common::MakeSuccess( static_cast<std::size_t>( tagged.front() ) );
            if ( tagged.empty() )
                return Common::MakeError<std::size_t>( "no PlayerStart carries the tag '" +
                                                       std::string( requestedTag ) + "'; the level has " +
                                                       NameList( starts, all ) );
            return Common::MakeError<std::size_t>(
                 std::to_string( tagged.size() ) + " PlayerStarts carry the tag '" + std::string( requestedTag ) +
                 "': " + NameList( starts, tagged ) + " - a tag must name one start" );
        }

        if ( starts.size() == 1 )
            return Common::MakeSuccess( std::size_t{ 0 } );

        std::vector<std::size_t> untagged;
        for ( std::size_t i = 0; i < starts.size(); ++i )
            if ( starts[i].Tag.empty() )
                untagged.push_back( i );
        if ( untagged.size() == 1 )
            return Common::MakeSuccess( static_cast<std::size_t>( untagged.front() ) );
        if ( untagged.empty() )
            return Common::MakeError<std::size_t>(
                 "every PlayerStart is tagged and none was asked for: " + NameList( starts, all ) +
                 " - leave one untagged as the default start, or ask for a tag" );
        return Common::MakeError<std::size_t>(
             std::to_string( untagged.size() ) + " PlayerStarts have no tag: " + NameList( starts, untagged ) +
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

    Common::ResultStr<PlayRequest> PlayRequestFromArgs( std::span<const std::string> args )
    {
        constexpr std::string_view kFlag = "--player-start";
        PlayRequest                request;
        bool                       stated = false;
        for ( std::size_t i = 0; i < args.size(); ++i )
        {
            if ( args[i] != kFlag )
                continue;
            if ( stated )
                return Common::MakeError<PlayRequest>( "--player-start is stated twice; name one start" );
            if ( i + 1 >= args.size() || args[i + 1].empty() || args[i + 1].starts_with( "--" ) )
                return Common::MakeError<PlayRequest>(
                     "--player-start needs the tag of a PlayerStart after it (its Player Start Tag)" );
            request.PlayerStartTag = args[++i];
            stated                 = true;
        }
        return Common::MakeSuccess( std::move( request ) );
    }

    Common::ResultStr<std::optional<Common::Json::Value>>
    PawnControllerBlock( const std::vector<Assets::EntityData>& records, const NestedPrefabRecords& nested )
    {
        std::vector<std::string> stack;
        return FindPawnBlock( records, {}, nested, stack );
    }
} // namespace Desert::Core
