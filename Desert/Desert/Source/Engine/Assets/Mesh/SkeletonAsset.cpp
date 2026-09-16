#include "SkeletonAsset.hpp"
#include <Engine/Assets/Serialization/Skeleton.hpp>

#include <Common/Utilities/FileSystem.hpp>
#include <Common/Core/Serialization/GlmReflection.hpp>

#include <rflcpp/rfl.hpp>
#include <rflcpp/rfl/json.hpp>

namespace Desert::Assets
{
    SkeletonAsset::SkeletonAsset( const AssetPriority priority, const Common::Filepath& filepath )
         : AssetBase( priority, filepath, GetTypeID() )
    {
    }

    Common::BoolResultStr SkeletonAsset::Load()
    {
        const auto raw = Common::Utils::FileSystem::ReadFileContent( m_Metadata.Filepath );
        if ( !raw )
            return Common::MakeError( raw.GetError() );

        const auto dataReflected = rfl::json::read<Serialization::SkeletonAssetData>( raw.GetValue() );
        if ( !dataReflected.has_value() )
        {
            return Common::MakeError( dataReflected.error().what() );
        }

        auto data = dataReflected.value();

        m_Skeleton = std::make_unique<Animation::Skeleton>( std::move( data.Bones ) );
        // Taken from the bones that were just read, never from `data.Signature`: the file's own field is
        // what a cook WROTE, and this is what the rig in memory IS. A mesh is matched against the second.
        m_Signature = m_Skeleton->GetSignature();

        return BOOLSUCCESS;
    }

    Common::BoolResultStr SkeletonAsset::Unload()
    {
        // WAS `return BOOLSUCCESS` — WITHOUT A SEMICOLON. It compiled only because `#define BOOLSUCCESS
        // Common::MakeSuccess( true );` carries one inside the macro, and it is the clearest evidence in
        // the set that these thirteen bodies were written with no caller and never read again: every other
        // one of them writes the semicolon.
        //
        // It also leaked the one thing this class owns. `m_Skeleton` is a `unique_ptr<Animation::Skeleton>`
        // holding the whole bone hierarchy, and nothing released it.
        m_Skeleton.reset();
        // `m_Signature` is deliberately NOT cleared — it is this rig's identity rather than its payload,
        // and clearing it is the defect GetSignature's comment records. The suite
        // `AssetEviction.AnUnloadedAssetStopsAnsweringWithItsPayload` states this carve-out next to the
        // fields that DO go, and `AssetEviction.ASkinnedMeshRebindsItsRigAfterASweepHasReleasedBoth`
        // asserts it over a rig that was really loaded — so the next reader has to decide, not infer.
        return BOOLSUCCESS;
    }
} // namespace Desert::Assets
