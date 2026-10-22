// Ported from UE 5.8 Engine/Plugins/Experimental/MeshModelingToolsetExp/Source/MeshModelingToolsExp/Private/
// BakeMeshAttributeMapsTool.cpp (the two-input rule: Targets[0] the bake target, Targets[1] the detail mesh; the
// TangentSpaceNormal map; OnShutdown(Accept) writing the map as a texture asset), adapted: no live preview and no
// Accept - one click bakes; the texture goes through the project's texture import (TextureImporter, Intent =
// NormalMap -> BC5) instead of FTexture2DBuilder; and the map is bound on the target at once through a material
// (UE leaves the assignment to the user). See MeshBakeNormalTool.hpp.

#include "MeshBakeNormalTool.hpp"
#include "ModelingToolTarget.hpp"

#include <Editor/Core/Commands/SceneCommands.hpp>
#include <Editor/Core/MaterialAssetUtils.hpp>
#include <Editor/Core/Selection/SelectionManager.hpp>
#include <Editor/Import/TextureDnD.hpp>
#include <Editor/Import/TextureImporter.hpp>

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/MaterialData.hpp>
#include <Engine/Assets/MaterialFormat.hpp>
#include <Engine/Assets/Mesh/SurfaceMaterialAsset.hpp>
#include <Engine/Assets/TextureSourceAsset.hpp>
#include <Engine/Core/Formats/TextureIntent.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/Core/ShaderCompiler/DShader/DShaderParser.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/Geometry/DynamicMeshRenderConversion.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Material/MaterialService.hpp>
#include <Engine/Runtime/Services/Texture/TextureService.hpp>

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <stb_image/stb_image_write.h>

#include <spdlog/fmt/fmt.h>

#include <array>
#include <cctype>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace Desert::Editor::Core
{
    namespace
    {
        namespace fs  = std::filesystem;
        namespace Fmt = ::Desert::Core::Formats;

        struct BakeInput
        {
            Common::UUID          Id;
            ECS::Entity           Entity;
            std::string           Name;
            Geometry::BakeSurface Surface;
        };

        Common::ResultStr<BakeInput> ReadInput( ::Desert::Core::Scene& scene, const Common::UUID id,
                                                const char* role, const bool target )
        {
            using Out = BakeInput;
            auto ref  = scene.FindEntityByID( id );
            if ( !ref )
                return Common::MakeFormattedError<Out>( "{} entity {} is not in the scene", role,
                                                        static_cast<uint64_t>( id ) );
            const ECS::Entity e = ref->get();
            if ( !e.HasComponent<ECS::StaticMeshComponent>() || !e.HasComponent<ECS::TransformComponent>() )
                return Common::MakeFormattedError<Out>( "{} entity {} has no static mesh or no transform", role,
                                                        static_cast<uint64_t>( id ) );
            const std::string name = e.HasComponent<ECS::TagComponent>() ? e.GetComponent<ECS::TagComponent>().Tag
                                                                         : std::string( "Mesh" );
            auto              mesh = GetToolTargetMesh( e.GetComponent<ECS::StaticMeshComponent>() );
            if ( !mesh.IsSuccess() || !mesh.GetValue().Mesh )
                return Common::MakeFormattedError<Out>( "{} '{}': {}", role, name,
                                                        mesh.IsSuccess() ? "no mesh" : mesh.GetError() );
            auto render = Geometry::ToRenderMesh( *mesh.GetValue().Mesh );
            if ( !render.IsSuccess() )
                return Common::MakeFormattedError<Out>( "{} '{}': {}", role, name, render.GetError() );
            auto surface = Geometry::BakeSurfaceFromRender( render.GetValue(), glm::dmat4( e.GetWorldTransform() ),
                                                            /*needTangentFrame=*/target );
            if ( !surface.IsSuccess() )
                return Common::MakeFormattedError<Out>( "{} '{}': {}", role, name, surface.GetError() );
            return Common::MakeSuccess( Out{ id, e, name, surface.ExtractValue() } );
        }

        std::string FileStem( const std::string& name )
        {
            std::string out;
            for ( const char c : name )
                out += ( std::isalnum( static_cast<unsigned char>( c ) ) || c == '_' || c == '-' ) ? c : '_';
            return out.empty() ? std::string( "Mesh" ) : out;
        }

        // The template's slot for a map: the one texture property whose authored intent is @p intent (the same
        // word the material import binds a source map by, MaterialImportContract.cpp ReadImportTemplate) -
        // NormalMap for the normal map, PackedMasks for the ORM map the baked occlusion goes into.
        Common::ResultStr<std::string> TemplateSlot( const Assets::AssetManager& assets,
                                                     const Common::AssetHandle   shader,
                                                     const Fmt::TextureIntent intent, const char* intentName )
        {
            const Assets::AssetMetadata* meta = assets.FindMetadataByHandle( shader );
            if ( meta == nullptr )
                return Common::MakeFormattedError<std::string>( "the material's template {} is not a loaded asset",
                                                                static_cast<uint64_t>( shader ) );
            const auto source = Common::Utils::FileSystem::ReadFileContent( meta->Filepath );
            if ( !source.IsSuccess() )
                return Common::MakeFormattedError<std::string>( "template '{}': {}", meta->Filepath.generic_string(),
                                                                source.GetError() );
            const auto parsed = ::Desert::Core::Preprocess::DShaderParser::Parse( source.GetValue() );
            if ( !parsed )
                return Common::MakeFormattedError<std::string>( "template '{}': {}", meta->Filepath.generic_string(),
                                                                parsed.GetError() );
            std::vector<std::string> slots;
            for ( const auto& param : parsed.GetValue().Meta.Params )
                if ( param.IsTexture && param.SlotIntent == intent )
                    slots.push_back( param.Name );
            if ( slots.size() != 1 )
                return Common::MakeFormattedError<std::string>(
                     "template '{}' declares {} texture properties with Intent({}), not one",
                     meta->Filepath.generic_string(), slots.size(), intentName );
            return Common::MakeSuccess( slots.front() );
        }

        // The image, its texture asset (Intent = NormalMap on first import; a rebake re-takes the source and
        // keeps the asset's identity), registered with the texture service; a rebake evicts the built texture
        // so the next draw reads the new map.
        struct BakedTexture
        {
            Common::AssetHandle        Handle;
            Common::Content::AssetGuid Guid;
            fs::path                   Asset;
        };

        Common::ResultStr<BakedTexture> WriteTexture( Assets::AssetManager& assets, const int width,
                                                      const int height, const std::vector<uint8_t>& rgba,
                                                      const std::string& name, const Fmt::TextureIntent intent )
        {
            using Out           = BakedTexture;
            const fs::path  dir = Common::Constants::Path::TEXTUREDIR_PATH / "Baked";
            std::error_code ec;
            fs::create_directories( dir, ec );
            if ( ec )
                return Common::MakeFormattedError<Out>( "'{}' was not created: {}", dir.generic_string(),
                                                        ec.message() );
            const fs::path png = dir / ( name + ".png" );
            if ( stbi_write_png( png.string().c_str(), width, height, 4, rgba.data(), width * 4 ) == 0 )
                return Common::MakeFormattedError<Out>( "'{}' was not written", png.generic_string() );

            auto asset = TextureImporter::ImportSourceAsset( png, intent );
            if ( !asset.IsSuccess() )
                return Common::MakeFormattedError<Out>( "'{}': {}", png.generic_string(), asset.GetError() );
            const auto key = Assets::ReadTextureAssetKey( asset.GetValue() );
            if ( !key.IsSuccess() )
                return Common::MakeFormattedError<Out>( "'{}': {}", asset.GetValue().generic_string(),
                                                        key.GetError() );

            Common::AssetHandle handle = TextureDnD::ResolveExisting( assets, png.string() );
            if ( !handle.IsNull() )
                Runtime::ResourceRegistry::GetTextureService()->EvictBuilt( handle );
            else
                handle = TextureDnD::ResolveOrImport( assets, png.string() );
            if ( handle.IsNull() )
                return Common::MakeFormattedError<Out>( "'{}' was not registered as a texture",
                                                        asset.GetValue().generic_string() );
            return Common::MakeSuccess( Out{ handle, key.GetValue().Guid, asset.GetValue() } );
        }

        // The material that binds the map: an instance of the target's slot-0 material, or a new material on the
        // Default Surface template. A file already at the name that binds THIS texture is reused (a rebake keeps
        // the texture's identity); any other file there is never overwritten - the next free name is taken.
        Common::ResultStr<std::pair<Common::AssetHandle, fs::path>>
        WriteMaterial( Assets::AssetManager& assets, const Common::AssetHandle slot0, const BakedTexture& texture,
                       const BakedTexture* orm, const std::string& stem )
        {
            using Out = std::pair<Common::AssetHandle, fs::path>;
            Assets::MaterialData data;
            Common::AssetHandle  shader;
            bool                 instance = false;
            if ( !slot0.IsNull() )
            {
                const auto parent = assets.FindByHandle<Assets::SurfaceMaterialAsset>( slot0 );
                if ( !parent || parent->Data().Guid().IsNull() )
                    return Common::MakeFormattedError<Out>(
                         "the target's slot 0 names material {}, which is not a loaded, saved material asset",
                         static_cast<uint64_t>( slot0 ) );
                data.SetParent( parent->Data().Guid() );
                shader   = parent->GetShaderHandle();
                instance = true;
            }
            else
            {
                if ( const auto stated = MaterialAssetUtils::StateDefaultSurface( data, assets ); !stated )
                    return Common::MakeError<Out>( stated.GetError() );
                shader = Assets::MaterialData::HandleOf( data.ShaderGuid() );
            }
            const auto slot = TemplateSlot( assets, shader, Fmt::TextureIntent::NormalMap, "NormalMap" );
            if ( !slot.IsSuccess() )
                return Common::MakeError<Out>( slot.GetError() );
            data.SetTexture(
                 slot.GetValue(), texture.Guid,
                 fs::relative( texture.Asset, Common::Constants::Path::ASSETS_PATH ).generic_string() );
            // The baked occlusion enters the template's ORM map (R = occlusion, G and B white so roughness and
            // metallic keep their factors). A parent that already binds an ORM map is refused: the baked one would
            // replace its roughness and metallic.
            std::string ormSlot;
            if ( orm != nullptr )
            {
                const auto packed = TemplateSlot( assets, shader, Fmt::TextureIntent::PackedMasks, "PackedMasks" );
                if ( !packed.IsSuccess() )
                    return Common::MakeError<Out>( packed.GetError() );
                ormSlot = packed.GetValue();
                if ( instance )
                    if ( const auto parent = assets.FindByHandle<Assets::SurfaceMaterialAsset>( slot0 );
                         parent && parent->Data().GetTexture( ormSlot ) != 0 )
                        return Common::MakeFormattedError<Out>(
                             "the target's material already binds an ORM map in '{}' - the baked occlusion would "
                             "replace its roughness and metallic; bake without Ambient Occlusion or clear that "
                             "map",
                             ormSlot );
                data.SetTexture(
                     ormSlot, orm->Guid,
                     fs::relative( orm->Asset, Common::Constants::Path::ASSETS_PATH ).generic_string() );
            }

            const std::string ext( Common::Constants::Extensions::MATERIAL_EXTENSION );
            const fs::path    dir = Common::Constants::Path::MATERIAL_PATH;
            std::error_code   ec;
            fs::create_directories( dir, ec );
            const std::string base = stem + "_Baked";
            for ( int n = 0;; ++n )
            {
                const fs::path path = dir / ( n == 0 ? base + ext : base + "_" + std::to_string( n ) + ext );
                if ( fs::exists( path, ec ) )
                {
                    auto existing = assets.FindByPath<Assets::SurfaceMaterialAsset>( path.generic_string() );
                    if ( !existing )
                        existing = assets.CreateAsset<Assets::SurfaceMaterialAsset>( path.generic_string() );
                    if ( existing &&
                         existing->Data().GetTexture( slot.GetValue() ) ==
                              static_cast<uint64_t>( Common::Content::HandleForGuid( texture.Guid ) ) &&
                         ( orm == nullptr
                                ? true
                                : existing->Data().GetTexture( ormSlot ) ==
                                       static_cast<uint64_t>( Common::Content::HandleForGuid( orm->Guid ) ) ) &&
                         ( existing->Data().ParentText() != nullptr ) == instance )
                        return Common::MakeSuccess( Out{ existing->GetMetadata().Handle, path } );
                    continue;
                }
                if ( const auto written = Assets::WriteMaterialFile( path, data ); !written )
                    return Common::MakeFormattedError<Out>( "'{}' was not written: {}", path.generic_string(),
                                                            written.GetError() );
                auto asset = assets.CreateAsset<Assets::SurfaceMaterialAsset>( path.generic_string() );
                if ( !asset )
                    return Common::MakeFormattedError<Out>( "'{}' did not load", path.generic_string() );
                // An instance never builds a runtime material of its own (MaterialsPanelComponent.cpp).
                if ( instance )
                    Runtime::ResourceRegistry::GetMaterialService()->RegisterAsset( asset );
                else
                    Runtime::ResourceRegistry::GetMaterialService()->Register( asset );
                return Common::MakeSuccess( Out{ asset->GetMetadata().Handle, path } );
            }
        }
    } // namespace

    BakeNormalMapArgs& BakeNormalMapSettings()
    {
        static BakeNormalMapArgs s_Args;
        return s_Args;
    }

    Common::ResultStr<BakeNormalMapOutcome>
    ApplyBakeNormalMap( ::Desert::Core::Scene& scene, Assets::AssetManager& assets, const BakeNormalMapArgs& args )
    {
        using Out             = BakeNormalMapOutcome;
        const auto& selection = SelectionManager::GetSelection();
        if ( selection.size() != 2 )
            return Common::MakeFormattedError<Out>(
                 "Bake Normal Map: select exactly two entities - the low-poly target first, then the high-poly "
                 "detail - not {}",
                 selection.size() );
        auto target = ReadInput( scene, selection[0], "target", true );
        if ( !target.IsSuccess() )
            return Common::MakeFormattedError<Out>( "Bake Normal Map: {}", target.GetError() );
        auto detail = ReadInput( scene, selection[1], "detail", false );
        if ( !detail.IsSuccess() )
            return Common::MakeFormattedError<Out>( "Bake Normal Map: {}", detail.GetError() );

        const auto map =
             Geometry::BakeMeshMaps( target.GetValue().Surface, detail.GetValue().Surface, args.Settings );
        if ( !map.IsSuccess() )
            return Common::MakeFormattedError<Out>( "Bake Normal Map: '{}' from '{}': {}", target.GetValue().Name,
                                                    detail.GetValue().Name, map.GetError() );

        const Geometry::MeshMapBakeResult& maps = map.GetValue();
        const std::string                  stem = FileStem( target.GetValue().Name );
        const auto texture = WriteTexture( assets, maps.Width, maps.Height, maps.Rgba, stem + "_Normal",
                                           Fmt::TextureIntent::NormalMap );
        if ( !texture.IsSuccess() )
            return Common::MakeFormattedError<Out>( "Bake Normal Map: {}", texture.GetError() );
        // UE writes each map as its own texture: AO and Curvature are one-channel masks (BC4). The occlusion is
        // also packed into an ORM map (R = AO, G = B = 1) the material binds.
        std::optional<BakedTexture> occlusion, curvature, orm;
        if ( args.Settings.Occlusion )
        {
            auto ao = WriteTexture( assets, maps.Width, maps.Height, maps.OcclusionRgba, stem + "_AO",
                                    Fmt::TextureIntent::Mask );
            if ( !ao.IsSuccess() )
                return Common::MakeFormattedError<Out>( "Bake Normal Map: {}", ao.GetError() );
            occlusion = ao.ExtractValue();
            std::vector<uint8_t> packed( maps.OcclusionRgba.size() );
            for ( size_t i = 0; i < packed.size(); i += 4 )
            {
                packed[i]     = maps.OcclusionRgba[i];
                packed[i + 1] = 255;
                packed[i + 2] = 255;
                packed[i + 3] = 255;
            }
            auto packedOrm = WriteTexture( assets, maps.Width, maps.Height, packed, stem + "_ORM",
                                           Fmt::TextureIntent::PackedMasks );
            if ( !packedOrm.IsSuccess() )
                return Common::MakeFormattedError<Out>( "Bake Normal Map: {}", packedOrm.GetError() );
            orm = packedOrm.ExtractValue();
        }
        if ( args.Settings.Curvature )
        {
            auto c = WriteTexture( assets, maps.Width, maps.Height, maps.CurvatureRgba, stem + "_Curvature",
                                   Fmt::TextureIntent::Mask );
            if ( !c.IsSuccess() )
                return Common::MakeFormattedError<Out>( "Bake Normal Map: {}", c.GetError() );
            curvature = c.ExtractValue();
        }

        const auto& slots    = target.GetValue().Entity.GetComponent<ECS::StaticMeshComponent>().MaterialSlots;
        const auto  material = WriteMaterial( assets, slots.empty() ? Common::AssetHandle{} : slots.front(),
                                              texture.GetValue(), orm ? &*orm : nullptr, stem );
        if ( !material.IsSuccess() )
            return Common::MakeFormattedError<Out>( "Bake Normal Map: {}", material.GetError() );

        const Common::UUID        id     = target.GetValue().Id;
        const Common::AssetHandle handle = material.GetValue().first;
        Commands::MutateEntityUndoable( id,
                                        [&]
                                        {
                                            auto live = scene.FindEntityByID( id );
                                            if ( !live )
                                                return;
                                            auto& smc = live->get().GetComponent<ECS::StaticMeshComponent>();
                                            if ( smc.MaterialSlots.empty() )
                                                smc.MaterialSlots.push_back( handle );
                                            for ( auto& slot : smc.MaterialSlots )
                                                slot = handle;
                                            smc.RuntimeMaterialInstances.clear();
                                            smc.RuntimeSlots.reset();
                                            smc.SeenMaterialsVersion = 0;
                                        } );

        Out out;
        out.TextureAsset  = texture.GetValue().Asset;
        out.Material      = material.GetValue().second;
        out.CoveredTexels = map.GetValue().CoveredTexels;
        out.HitTexels     = map.GetValue().HitTexels;
        if ( occlusion )
            out.OcclusionAsset = occlusion->Asset;
        if ( curvature )
            out.CurvatureAsset = curvature->Asset;
        LOG_INFO( "[Modeling] Bake Normal Map: '{}' from '{}' at {}x{}, {} of {} covered texels hit the detail "
                  "within {} cm, {} gutter texels -> '{}', material '{}'",
                  target.GetValue().Name, detail.GetValue().Name, map.GetValue().Width, map.GetValue().Height,
                  out.HitTexels, out.CoveredTexels, args.Settings.ProjectionDistance, map.GetValue().GutterTexels,
                  out.TextureAsset.generic_string(), out.Material.generic_string() );
        if ( occlusion || curvature )
            LOG_INFO( "[Modeling] Bake: ambient occlusion '{}' ({} rays), curvature '{}'",
                      out.OcclusionAsset.generic_string(), args.Settings.OcclusionRays,
                      out.CurvatureAsset.generic_string() );
        return Common::MakeSuccess( std::move( out ) );
    }
} // namespace Desert::Editor::Core
