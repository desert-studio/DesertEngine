#include "ThumbnailSubject.hpp"

#include <Editor/Import/CookPaths.hpp>
#include <Editor/Import/MeshMaterial.hpp>

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/Mesh/MeshAsset.hpp>
#include <Engine/Assets/Mesh/StaticMeshAsset.hpp>
#include <Engine/Assets/Mesh/SurfaceMaterialAsset.hpp>
#include <Engine/Core/Formats/ShaderProgramMeta.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/AssetServiceRegistration.hpp>

#include <filesystem>

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

        const std::string shaderName = asset.Data().EffectiveShaderName();
        const auto        shader     = shaders->GetByName( shaderName );
        if ( !shader )
        {
            return Common::MakeFormattedError<Preview>(
                 "its shader '{}' is not registered, so the domain that decides how to photograph it "
                 "cannot be read",
                 shaderName );
        }

        const Core::Formats::ShaderDomain domain = shader->GetProgramMeta().Domain;

        // A cutout material garbles on a sphere: the atlas wraps and the picture becomes one of the ball.
        // THE ONE STATEMENT OF THAT RULE — it used to be copied into the browser tile, the Details slot
        // and the static-mesh row, three files deciding one thing.
        const bool cutout = asset.Data().GetFloat( "AlphaCutoff" ) > 0.0f;
        if ( const auto how = PreviewForDomain( domain, cutout ) )
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

    Common::ResultStr<Material> ResolveMaterial( Assets::AssetManager& manager, const std::string& assetPath )
    {
        // Mirrors the component deserializer's create-if-missing logic, which is what a cold start needs:
        // the preloader registers every `.demat` under MATERIAL_PATH, but a material an artist has just
        // dropped in — or one that lives outside that root — is not in the manager yet.
        auto asset = manager.FindByPath<Assets::SurfaceMaterialAsset>( assetPath );
        if ( !asset )
            asset = manager.CreateAsset<Assets::SurfaceMaterialAsset>( Assets::AssetPriority::High, assetPath );
        if ( !asset )
        {
            return Common::MakeFormattedError<Material>( "'{}' is not a material the asset manager will accept",
                                                         assetPath );
        }

        // PARSED BEFORE IT IS ASKED ANYTHING, ON BOTH ROUTES, AND THAT MOVE IS THE WHOLE DEFECT. The Load
        // used to sit inside the create branch above, so a material the PRELOADER had already registered
        // never got one — and `AssetPreloader::PreloadCookedAssetsAndMaterials` registers every `.demat`
        // under MATERIAL_PATH with `loadAfterCreate=false`, i.e. as an unparsed shell. A shell states no
        // ShaderName, `MaterialData::EffectiveShaderName()` answers "StaticMeshPBR", and every question
        // below was then answered about a material that does not exist.
        //
        // MEASURED, because this is what it cost: on a clean start of this repository the sweep resolved
        // 52 cloud materials as Surface-domain, queued them as mesh draws, and the capture — running
        // seconds later, against an asset something else had parsed in the meantime — reached
        // MeshRenderer with the REAL shader. Three domain refusals in the log (Volume, Skybox, Terrain),
        // and an empty frame written to disk as each material's picture. The check was not missing; it was
        // reading a default.
        if ( !asset->IsReadyForUse() )
            asset->Load();

        auto route = PreviewRouteFor( *asset );
        if ( !route )
            return Common::MakeFormattedError<Material>( "'{}': {}", assetPath, route.GetError() );

        // Was `if ( !GetMaterialService()->Get( h ) ) Register( a )`. `Get` BUILDS the runtime material on a
        // miss, so the question and the answer were the same call — and the sweep asks it about every
        // material in the project. The registration is a map write now; the build happens when the capture
        // shades with it, which is one frame later and only for the materials actually photographed.
        Runtime::EnsureMaterialRegistered( asset );

        Material out;
        out.Handle = asset->GetMetadata().Handle;
        out.How    = route.GetValue();
        return Common::MakeSuccess( out );
    }

    Common::ResultStr<Mesh> ResolveMesh( Assets::AssetManager& manager, const std::string& sourcePath )
    {
        // A PURE PATH COMPUTATION, hoisted above every filesystem question: CookPaths::CookedMesh is
        // fs::relative and a string replace, no stat. The `exists` check below is the filesystem question
        // and it stays where it is.
        const std::string cooked = CookPaths::CookedMesh( sourcePath, ".stmesh" ).generic_string();

        std::error_code ec;
        if ( !std::filesystem::exists( cooked, ec ) )
        {
            return Common::MakeFormattedError<Mesh>(
                 "'{}' has not been cooked, so there is no '{}' to photograph. Meshes load only from the "
                 "cooked form; the browser shows the type icon until the import produces one",
                 sourcePath, cooked );
        }

        auto asset = manager.FindByPath<Assets::MeshAsset>( cooked );
        if ( !asset )
        {
            asset = manager.CreateAsset<Assets::StaticMeshAsset>( Assets::AssetPriority::High, cooked );
            if ( !asset )
                return Common::MakeFormattedError<Mesh>( "'{}' could not be created as a static mesh", cooked );
        }

        // REGISTER AND BUILD, ON BOTH ROUTES. The registration used to live inside the `if` above, so a
        // cooked mesh the manager ALREADY held — which is every mesh, once AssetPreloader has run — reached
        // the line below having never been offered to the mesh service at all.
        //
        // AND THE REFUSAL NAMES THE CONDITION THAT HELD. What stood here was one message for three
        // different facts, and the words it chose were the rarest one's: a mesh nothing had registered
        // produced "built no drawable geometry (a skinned mesh's static buffer is empty by design)", which
        // sends the next reader to look at rigs. This header's own doc block already promised three
        // distinct refusals; it is the code that had two.
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
