#include "ClipSkeletonMatch.hpp"

#include <format>
#include <iterator>

namespace Desert::Animation
{
    namespace
    {
        Common::BoolResultStr Plays( const ClipRigIdentity& clip, const MeshSkeletonIdentity& mesh )
        {
            return ClipPlaysOnMesh( clip.Skeleton, mesh.Skeleton, mesh.Compatible );
        }
    } // namespace

    std::vector<size_t> SelectClipsForMesh( const std::vector<ClipRigIdentity>& clips,
                                            const MeshSkeletonIdentity&         mesh )
    {
        std::vector<size_t> selected;
        for ( size_t i = 0; i < clips.size(); ++i )
            if ( Plays( clips[i], mesh ) )
                selected.push_back( i );
        return selected;
    }

    Common::ResultStr<size_t> FindClipForMesh( const std::vector<ClipRigIdentity>& clips,
                                               const MeshSkeletonIdentity& mesh, const std::string& clipName )
    {
        if ( clipName.empty() )
        {
            return Common::MakeError<size_t>(
                 "no clip name was asked for; an unnamed clip cannot resolve to anything." );
        }

        // Two failures, two fixes: the clip is not registered (cook / import problem), or it is and the rule
        // refused it (reference problem: the refusal names both skeletons).
        std::string refusal;
        for ( size_t i = 0; i < clips.size(); ++i )
        {
            if ( clips[i].ClipName != clipName )
                continue;
            const auto plays = Plays( clips[i], mesh );
            if ( plays )
                return Common::MakeSuccess( i );
            refusal = plays.GetError();
        }

        if ( !refusal.empty() )
            return Common::MakeFormattedError<size_t>( "clip '{}' does not play on this mesh: {}", clipName,
                                                       refusal );

        // A wrong name is fixed by the RIGHT name: the clips this rig plays are listed, so the message itself
        // says that 'Fox_Survey' was asked of a rig whose clip is called 'Survey'.
        const std::vector<size_t> playable = SelectClipsForMesh( clips, mesh );
        std::string               names;
        for ( const size_t i : playable )
            std::format_to( std::back_inserter( names ), "{}'{}'", names.empty() ? ": " : ", ",
                            clips[i].ClipName );
        return Common::MakeFormattedError<size_t>(
             "no clip named '{}' is registered ({} clip(s) known, {} of them play on skeleton '{}'{}).", clipName,
             clips.size(), playable.size(), mesh.Skeleton.Name, names );
    }
} // namespace Desert::Animation
