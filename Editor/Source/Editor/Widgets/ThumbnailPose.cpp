#include "ThumbnailPose.hpp"

#include <Common/Core/Logger.hpp>

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/AsyncAssetLoader.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/Mesh/AnimationAsset.hpp>
#include <Engine/Assets/Mesh/MeshAsset.hpp>
#include <Engine/Assets/Mesh/SkinnedMeshAsset.hpp>
#include <Engine/Geometry/Mesh.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/AssetServiceRegistration.hpp>

#include <filesystem>
#include <unordered_map>

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

    namespace
    {
        // THE CLIP READS IN FLIGHT, BY PATH: the request handle is the keep-alive (AsyncAssetLoader.hpp); the
        // delegate drops it and the next ask (the tile's next frame, the splash's next tick) meets it resident.
        std::unordered_map<std::string, Assets::LoadRequest>& ClipReadsInFlight()
        {
            static std::unordered_map<std::string, Assets::LoadRequest> reads;
            return reads;
        }
        // A clip whose read FAILED, with the reason: answered as a refusal from then on, never re-read per frame.
        std::unordered_map<std::string, std::string>& ClipReadsFailed()
        {
            static std::unordered_map<std::string, std::string> failed;
            return failed;
        }

        // The clip at @p clipPath, read — or nullptr with @p pending set while a worker reads it.
        Common::ResultStr<std::shared_ptr<Assets::AnimationAsset>> ReadClip( Assets::AssetManager& manager,
                                                                             const std::string&    clipPath,
                                                                             bool&                 pending )
        {
            using ClipPtr = std::shared_ptr<Assets::AnimationAsset>;
            pending       = false;
            auto clip     = manager.FindByPath<Assets::AnimationAsset>( clipPath );
            if ( !clip )
                clip = manager.CreateAsset<Assets::AnimationAsset>( clipPath, false );
            if ( !clip )
                return Common::MakeFormattedError<ClipPtr>( "'{}' could not be created as an animation clip",
                                                            clipPath );
            if ( clip->IsReadyForUse() )
                return Common::MakeSuccess( clip );
            if ( const auto failed = ClipReadsFailed().find( clipPath ); failed != ClipReadsFailed().end() )
                return Common::MakeFormattedError<ClipPtr>( "the clip '{}' could not be read: {}", clipPath,
                                                            failed->second );
            pending     = true;
            auto& reads = ClipReadsInFlight();
            if ( reads.contains( clipPath ) )
                return Common::MakeSuccess( ClipPtr() );
            // NOLINTBEGIN(bugprone-exception-escape)
            reads.emplace( clipPath, Assets::AsyncAssetLoader::Get().Request(
                                          clip,
                                          [clipPath]( const Assets::Asset<Assets::AssetBase>&,
                                                      Assets::LoadOutcome outcome, const std::string& error )
                                          {
                                              ClipReadsInFlight().erase( clipPath );
                                              if ( outcome != Assets::LoadOutcome::Loaded )
                                                  ClipReadsFailed()[clipPath] = error;
                                          },
                                          [clipPath]() { ClipReadsInFlight().erase( clipPath ); } ) );
            // NOLINTEND(bugprone-exception-escape)
            return Common::MakeSuccess( ClipPtr() );
        }
    } // namespace

    Common::ResultStr<ThumbnailSubject::Mesh> ResolvePoseSubject( Assets::AssetManager& manager,
                                                                  const std::string&    subjectPath )
    {
        using Common::Content::ContentKind;
        using ThumbnailSubject::Mesh;
        const std::string extension = std::filesystem::path( subjectPath ).extension().string();
        if ( extension == ".skmesh" )
            return ResolveSkinnedMesh( manager, subjectPath );

        ContentKind kind = ContentKind::Skeleton;
        if ( extension == ".anim" )
            kind = ContentKind::Animation;
        else if ( extension != ".skeleton" )
            return Common::MakeFormattedError<Mesh>( "'{}' is not a skeletal mesh, a skeleton or an animation: "
                                                     "no pose to photograph",
                                                     subjectPath );

        const std::string_view kindName = Common::Content::KindName( kind );
        const auto             row      = Assets::ContentRegistry::RowOfPath( kind, subjectPath );
        if ( !row )
            return Common::MakeFormattedError<Mesh>( "{} '{}' has no row in the content registry, so its rig is "
                                                     "unknown and no preview mesh can be chosen",
                                                     kindName, subjectPath );
        if ( row->RigSignature == 0 )
            return Common::MakeFormattedError<Mesh>( "{} '{}' states no rig (signature 0): no mesh stands on it",
                                                     kindName, subjectPath );
        const auto preview = Assets::ContentRegistry::PreviewMeshRow( row->RigSignature );
        if ( !preview )
            return Common::MakeFormattedError<Mesh>( "{} '{}' has no preview mesh: no skeletal mesh in the content "
                                                     "registry stands on its rig {:016x}",
                                                     kindName, subjectPath, row->RigSignature );

        auto mesh = ResolveSkinnedMesh( manager, preview->Path.generic_string() );
        if ( !mesh )
            return Common::MakeFormattedError<Mesh>( "{} '{}': its preview mesh {}", kindName, subjectPath,
                                                     mesh.GetError() );
        Mesh out       = mesh.GetValue();
        out.CookedPath = subjectPath; // filed and judged under the subject, never under its preview mesh
        if ( kind == ContentKind::Animation )
        {
            bool       pending = false;
            const auto clip    = ReadClip( manager, subjectPath, pending );
            if ( !clip )
                return Common::MakeError<Mesh>( clip.GetError() );
            out.Clip    = clip.GetValue();
            out.Pending = out.Pending || pending;
        }
        return Common::MakeSuccess( out );
    }
} // namespace Desert::Editor::ThumbnailPose
