#include "ClipSkeletonMatch.hpp"

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

        return Common::MakeFormattedError<size_t>(
             "no clip named '{}' is registered ({} clip(s) known, {} of them play on skeleton '{}').", clipName,
             clips.size(), SelectClipsForMesh( clips, mesh ).size(), mesh.Skeleton.Name );
    }
} // namespace Desert::Animation
