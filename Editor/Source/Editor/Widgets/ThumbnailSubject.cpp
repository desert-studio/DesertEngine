#include "ThumbnailSubject.hpp"

#include <Editor/Import/CookPaths.hpp>
#include <Editor/Import/ImportedMeshAsset.hpp>
#include <Editor/Import/MeshMaterial.hpp>

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/AsyncAssetLoader.hpp>
#include <Engine/Assets/Mesh/MeshAsset.hpp>
#include <Engine/Assets/Mesh/StaticMeshAsset.hpp>
#include <Engine/Assets/Mesh/SurfaceMaterialAsset.hpp>
#include <Engine/Assets/Serialization/ImportRecord.hpp>
#include <Engine/Core/Formats/ShaderProgramMeta.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/AssetServiceRegistration.hpp>

#include <filesystem>
#include <unordered_map>
#include <utility>

namespace Desert::Editor::ThumbnailSubject
{
    Common::ResultStr<Preview> PreviewRouteFor( const Assets::SurfaceMaterialAsset& asset )
    {
        // ── CAN THE PREVIEW'S DRAW PATH EXECUTE THIS MATERIAL AT ALL? ─────────────────────────────────
        //
        // The domain is the material's, through its shader. Without the shader service there is no domain
        // to read, and answering "Sphere" anyway is what a missing service used to mean here — a default
        // dressed as a decision. It is refused instead, and the sweep prints the reason once.
        auto* shaders = Runtime::ResourceRegistry::GetShaderService();
        if ( shaders == nullptr )
            return Common::MakeFormattedError<Preview>( "there is no shader service, so no domain to ask" );

        const std::string shaderName = asset.GetShaderName();
        const auto        shader     = shaders->GetByName( shaderName );
        if ( !shader )
        {
            return Common::MakeFormattedError<Preview>(
                 "its shader '{}' is not registered, so the domain that decides how to photograph it "
                 "cannot be read",
                 shaderName );
        }

        const Core::Formats::ShaderDomain domain = shader->GetProgramMeta().Domain;

        // The domain alone decides. A masked material is NOT flattened onto a card: it goes on the ball
        // like any other surface and the mesh path's alpha discard cuts it (StaticMeshPBR.shader).
        if ( const auto how = PreviewForMaterial( domain, asset.Data().PreviewMesh.has_value() ) )
            return Common::MakeSuccess( *how );

        // Skybox, Terrain, PostProcess, Unspecified. NAMED RATHER THAN DROPPED: until now these reached
        // the mesh path, were refused there by MeshRenderer at LOG_ERROR one frame after the queue had
        // committed, and the empty frame was still written to disk and filed as the picture of the
        // material. A black square the freshness rule then calls correct for ever.
        return Common::MakeFormattedError<Preview>(
             "its shader '{}' declares Domain {}, and no thumbnail producer draws that domain — the mesh "
             "path executes only {} and the dome only {}. Photographing it would write an empty frame and "
             "file it as the picture of this material",
             shaderName, Core::Formats::ShaderDomainName( domain ),
             Core::Formats::ShaderDomainName( Core::Formats::kMeshPathDomain ),
             Core::Formats::ShaderDomainName( Core::Formats::kVolumePathDomain ) );
    }

    namespace
    {
        // THE READS IN FLIGHT, BY PATH. The request handle is the keep-alive (AsyncAssetLoader.hpp), so
        // holding it here is what keeps the shell alive until the bytes arrive; erasing the entry in the
        // delegate is the ordinary end of a request whose result arrived.
        std::unordered_map<std::string, Assets::LoadRequest>& MaterialReadsInFlight()
        {
            static std::unordered_map<std::string, Assets::LoadRequest> reads;
            return reads;
        }

        // Everything after the bytes are in memory: route, register, answer. Shared by the resident case
        // (answered in the caller's frame) and the arrival (answered from the loader's Pump).
        Common::ResultStr<Material>
        ResolveLoadedMaterial( Assets::AssetManager&                              manager,
                               const Assets::Asset<Assets::SurfaceMaterialAsset>& asset,
                               const std::string&                                 assetPath )
        {
            auto route = PreviewRouteFor( *asset );
            if ( !route )
                return Common::MakeFormattedError<Material>( "'{}': {}", assetPath, route.GetError() );

            // THE PREVIEW MESH IS RESOLVED HERE, where the manager is: named by GUID, located by its path, and
            // the record at that path must state the same GUID — a moved or replaced source is refused by
            // name rather than photographed as whatever now sits there.
            Common::AssetHandle previewMesh{ static_cast<uint64_t>( 0 ) };
            if ( route.GetValue() == Preview::Mesh )
            {
                const auto& ref    = *asset->Data().PreviewMesh;
                const auto  stated = Assets::Serialization::ReadImportRecordGuid( ref.Path );
                if ( !stated )
                    return Common::MakeFormattedError<Material>( "'{}': its PreviewMesh '{}': {}", assetPath,
                                                                 ref.Path, stated.GetError() );
                if ( Common::Content::AssetGuidToText( stated.GetValue() ) != ref.Guid )
                    return Common::MakeFormattedError<Material>(
                         "'{}': its PreviewMesh names GUID {} at '{}', and the record there states {}", assetPath,
                         ref.Guid, ref.Path, Common::Content::AssetGuidToText( stated.GetValue() ) );
                const auto mesh = ResolveMesh( manager, ref.Path );
                if ( !mesh )
                    return Common::MakeFormattedError<Material>( "'{}': its PreviewMesh: {}", assetPath,
                                                                 mesh.GetError() );
                previewMesh = mesh.GetValue().Handle;
            }

            // Was `if ( !GetMaterialService()->Get( h ) ) Register( a )`. `Get` BUILDS the runtime material on a
            // miss, so the question and the answer were the same call — and the sweep asks it about every
            // material in the project. The registration is a map write now; the build happens when the capture
            // shades with it, which is one frame later and only for the materials actually photographed.
            Runtime::EnsureMaterialRegistered( asset );

            // THE SERVICE MAY HOLD ITS OWN SHELL of this material — one it discovered from the registry row
            // before the browser asked — and registration keeps that one. The capture shades through the
            // service, so an unread service shell was parsed inside the capture's frame (M_HDR_Chrome,
            // CB_Red, … on Starter). Waited for here on a worker (AwaitOne: not an in-frame load), which is
            // free when the service's asset is the one just read.
            (void)Runtime::AwaitAssetClosure( asset->GetMetadata().Handle,
                                              Common::Content::ContentKind::Material );

            Material out;
            out.Handle = asset->GetMetadata().Handle;
            out.How         = route.GetValue();
            out.PreviewMesh = previewMesh;
            return Common::MakeSuccess( out );
        }
    } // namespace

    Common::ResultStr<std::optional<Material>> ResolveMaterial( Assets::AssetManager&    manager,
                                                                const std::string&       assetPath,
                                                                const OnMaterialArrived& onArrived )
    {
        using Answer = std::optional<Material>;
        if ( !onArrived )
            return Common::MakeFormattedError<Answer>( "'{}': asked with no arrival delegate, so a pending "
                                                       "answer would be a thumbnail nobody ever hears about",
                                                       assetPath );

        // Mirrors the component deserializer's create-if-missing logic, which is what a cold start needs:
        // the preloader registers every `.demat` under MATERIAL_PATH, but a material an artist has just
        // dropped in — or one that lives outside that root — is not in the manager yet. Created UNLOADED:
        // the read is the loader's, below.
        auto asset = manager.FindByPath<Assets::SurfaceMaterialAsset>( assetPath );
        if ( !asset )
            asset = manager.CreateAsset<Assets::SurfaceMaterialAsset>( assetPath, false );
        if ( !asset )
        {
            return Common::MakeFormattedError<Answer>( "'{}' is not a material the asset manager will accept",
                                                       assetPath );
        }

        // PARSED BEFORE IT IS ASKED ANYTHING, ON BOTH ROUTES, AND THAT MOVE IS THE WHOLE DEFECT. The Load
        // used to sit inside the create branch above, so a material the PRELOADER had already registered
        // never got one — and the boot scanner of the time registered every `.demat` under MATERIAL_PATH
        // with `loadAfterCreate=false`, i.e. as an unparsed shell; on-demand shells are created the same way. A
        // shell states no ShaderName, `SurfaceMaterialAsset::GetShaderName()` answers "StaticMeshPBR", and every
        // question below was then answered about a material that does not exist.
        //
        // MEASURED, because this is what it cost: on a clean start of this repository the sweep resolved
        // 52 cloud materials as Surface-domain, queued them as mesh draws, and the capture — running
        // seconds later, against an asset something else had parsed in the meantime — reached
        // MeshRenderer with the REAL shader. Three domain refusals in the log (Volume, Skybox, Terrain),
        // and an empty frame written to disk as each material's picture. The check was not missing; it was
        // reading a default.
        //
        // AL1-5c: parsed, but no longer HERE. The Load that stood on this line ran inside the frame for
        // every shell the sweep reached (16 `.demat`, ~5 ms, one second after the window appeared on
        // Starter). The shell is handed to AsyncAssetLoader instead and the question is asked again when
        // the bytes arrive — still never of a shell.
        if ( asset->IsReadyForUse() )
        {
            auto resolved = ResolveLoadedMaterial( manager, asset, assetPath );
            if ( !resolved )
                return Common::MakeError<Answer>( resolved.GetError() );
            return Common::MakeSuccess( Answer( resolved.GetValue() ) );
        }

        auto& reads = MaterialReadsInFlight();
        if ( reads.contains( assetPath ) )
            return Common::MakeSuccess( Answer() );

        // clang-tidy 18 blames the closures' implicit copies (std::string, std::function, weak_ptr), which
        // the loader's std::function needs; the same finding EditorLayer's palette entries carry.
        // NOLINTBEGIN(bugprone-exception-escape)
        Assets::LoadRequest request = Assets::AsyncAssetLoader::Get().Request(
             asset,
             [assetPath, onArrived,
              weakManager = manager.weak_from_this()]( const Assets::Asset<Assets::AssetBase>& loaded,
                                                       Assets::LoadOutcome outcome, const std::string& error )
             {
                 // The map's handle is released first: the result has arrived, and the resolution below may
                 // itself ask for another read.
                 MaterialReadsInFlight().erase( assetPath );
                 if ( outcome != Assets::LoadOutcome::Loaded )
                 {
                     onArrived( assetPath, Common::MakeFormattedError<Material>( "'{}' could not be read: {}",
                                                                                 assetPath, error ) );
                     return;
                 }
                 // THE SHADER IS NAMED THROUGH THE MANAGER, which a worker may not touch: the read leaves
                 // the shader name empty and ResolveDependencies fills it here, back on the main thread.
                 // Skipping it answered "shader '' is not registered" for 41 materials on Starter.
                 const auto owner = weakManager.lock();
                 if ( !owner )
                 {
                     onArrived( assetPath, Common::MakeFormattedError<Material>(
                                                "'{}' arrived after its asset manager closed", assetPath ) );
                     return;
                 }
                 loaded->ResolveDependencies( *owner );
                 onArrived(
                      assetPath,
                      ResolveLoadedMaterial(
                           *owner, std::static_pointer_cast<Assets::SurfaceMaterialAsset>( loaded ), assetPath ) );
             },
             [assetPath]() { MaterialReadsInFlight().erase( assetPath ); } );
        // NOLINTEND(bugprone-exception-escape)
        if ( !request.IsValid() )
            return Common::MakeFormattedError<Answer>( "'{}': the async loader refused the read", assetPath );
        reads.emplace( assetPath, std::move( request ) );
        return Common::MakeSuccess( Answer() );
    }

    Common::ResultStr<Mesh> ResolveMesh( Assets::AssetManager& manager, const std::string& sourcePath )
    {
        // A PURE PATH COMPUTATION, hoisted above every filesystem question: CookPaths::MeshAsset is
        // an extension swap, no stat. The gate below is the filesystem/DDC question and it stays where
        // it is.
        const std::string cooked = CookPaths::MeshAsset( sourcePath ).generic_string();

        // AF4h: an import never writes `cooked` to disk any more (the source envelope lives in the DDC,
        // keyed by `sourcePath`'s bytes), so `exists(cooked)` alone answered "not cooked" for every
        // freshly imported mesh forever, degrading it to the type icon. StaticMeshCookAvailable covers
        // both a hand-authored `.stmesh` (exists(cooked) is true, identity path) and an import (a fresh
        // DDC envelope under `sourcePath`).
        if ( !StaticMeshCookAvailable( cooked, sourcePath ) )
        {
            return Common::MakeFormattedError<Mesh>(
                 "'{}' has not been cooked, so there is no '{}' to photograph. Meshes load only from the "
                 "cooked form; the browser shows the type icon until the import produces one",
                 sourcePath, cooked );
        }

        auto asset = manager.FindByPath<Assets::MeshAsset>( cooked );
        if ( !asset )
        {
            asset = manager.CreateAsset<Assets::StaticMeshAsset>( cooked, false );
            if ( !asset )
                return Common::MakeFormattedError<Mesh>( "'{}' could not be created as a static mesh", cooked );
        }

        // REGISTER AND BUILD, ON BOTH ROUTES. The registration used to live inside the `if` above, so a
        // cooked mesh the manager ALREADY held — which was every mesh while a boot scanner created them — reached
        // the line below having never been offered to the mesh service at all.
        //
        // AND THE REFUSAL NAMES THE CONDITION THAT HELD. What stood here was one message for three
        // different facts, and the words it chose were the rarest one's: a mesh nothing had registered
        // produced "built no drawable geometry (a skinned mesh's static buffer is empty by design)", which
        // sends the next reader to look at rigs. This header's own doc block already promised three
        // distinct refusals; it is the code that had two.
        // NOT READ HERE (AL1-5c). EnsureMeshDrawable builds through MeshService::LoadNow, which parses a
        // cold mesh on this thread: 726 ms for base_basic_shaded.stmesh, inside a frame, a second after the
        // window appeared. A cold mesh is registered and asked for instead — MeshService answers pending and
        // hands the read to AsyncAssetLoader — and the sweep's next pass, which re-finds every asset that
        // still has no picture, meets it resident.
        if ( !asset->IsReadyForUse() )
        {
            Runtime::RequestMeshRead( asset, manager );
            return Common::MakeFormattedError<Mesh>( "'{}' is being read on a worker; its picture follows on a "
                                                     "later pass",
                                                     cooked );
        }
        const auto readiness = Runtime::EnsureMeshDrawable( asset, manager );
        if ( readiness != Runtime::MeshReadiness::Drawable )
        {
            return Common::MakeFormattedError<Mesh>(
                 "{}, so a capture would photograph empty sky and file it as the asset",
                 Runtime::ExplainMeshReadiness( readiness, cooked ) );
        }

        Mesh out;
        out.Handle     = asset->GetMetadata().Handle;
        out.CookedPath = cooked;
        out.Material   = MeshMaterial::ResolveSidecar( manager, sourcePath );
        return Common::MakeSuccess( out );
    }
} // namespace Desert::Editor::ThumbnailSubject
