#include "ThumbnailPose.hpp"

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/Mesh/MeshAsset.hpp>
#include <Engine/Assets/Mesh/SkinnedMeshAsset.hpp>
#include <Engine/Geometry/Mesh.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/AssetServiceRegistration.hpp>

namespace Desert::Editor::ThumbnailPose
{
    Common::ResultStr<ThumbnailSubject::Mesh> ResolveSkinnedMesh( Assets::AssetManager& manager,
                                                                  const std::string&    skinnedPath )
    {
        using ThumbnailSubject::Mesh;
        auto asset = manager.FindByPath<Assets::MeshAsset>( skinnedPath );
        if ( !asset )
        {
            asset = manager.CreateAsset<Assets::SkinnedMeshAsset>( skinnedPath, false );
            if ( !asset )
                return Common::MakeFormattedError<Mesh>( "'{}' could not be created as a skinned mesh",
                                                         skinnedPath );
        }

        // Not read on this thread (AL1-5c): a cold mesh is asked for and answered Pending, exactly as the
        // static resolver does; the caller asks again on a later frame and meets it resident.
        if ( !asset->IsReadyForUse() )
        {
            Runtime::RequestMeshRead( asset, manager );
            Mesh pending;
            pending.Handle     = asset->GetMetadata().Handle;
            pending.CookedPath = skinnedPath;
            pending.Pending    = true;
            return Common::MakeSuccess( pending );
        }
        const auto readiness = Runtime::EnsureMeshDrawable( asset, manager );
        if ( readiness != Runtime::MeshReadiness::Drawable )
        {
            return Common::MakeFormattedError<Mesh>( "{}, so a pose capture would photograph empty sky",
                                                     Runtime::ExplainMeshReadiness( readiness, skinnedPath ) );
        }
        const Assets::AssetHandle handle = asset->GetMetadata().Handle;
        const auto*               built  = Runtime::ResourceRegistry::GetMeshService()->Get( handle );
        if ( built == nullptr || !built->IsSkinned() )
            return Common::MakeFormattedError<Mesh>( "'{}' did not build as a skinned mesh: there is no skeleton "
                                                     "to stand in its bind pose",
                                                     skinnedPath );

        // Its own slots' materials and textures resident before the capture, as the static route waits.
        (void)Runtime::AwaitAssetClosure( handle, Common::Content::ContentKind::SkinnedMesh );

        Mesh out;
        out.Handle     = handle;
        out.CookedPath = skinnedPath;
        return Common::MakeSuccess( out );
    }
} // namespace Desert::Editor::ThumbnailPose
