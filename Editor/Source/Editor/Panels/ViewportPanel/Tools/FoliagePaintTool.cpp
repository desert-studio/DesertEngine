#include "FoliagePaintTool.hpp"

#include <Editor/Core/Selection/FoliagePaint.hpp>
#include <Editor/Core/Selection/ModelingToolTarget.hpp>
#include <Engine/Geometry/MeshBounds.hpp>
#include <Editor/Core/CommandHistory.hpp>
#include <Editor/Core/ToastManager.hpp>
#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/DragPayloads.hpp>
#include <Editor/Core/AssetOpen.hpp>
#include <Editor/Panels/Foliage/FoliagePalette.hpp>
#include <Editor/Core/ImGuiUtilities.hpp>
#include <Editor/Import/MeshDnD.hpp>
#include <Editor/Panels/Collections/CollectionFoliageTypes.hpp>

#include <Engine/Assets/Mesh/MeshAsset.hpp>
#include <Engine/Assets/RegistryDiscovery.hpp>
#include <Engine/Runtime/Services/AssetServiceRegistration.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/ECS/LandscapeRootOf.hpp>
#include <Engine/World/Landscape/LandscapeLayout.hpp>
#include <Engine/World/Foliage/FoliageCells.hpp>
#include <Engine/Assets/Serialization/LandscapeLayerInfo.hpp>
#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <ImGui/imgui.h>

#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <optional>
#include <limits>
#include <unordered_map>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace Desert::Editor::Tools
{
    namespace ImGui = ::ImGui; // engine headers introduce a Desert::ImGui that would otherwise shadow ::ImGui

    namespace
    {
        // A `.delayerinfo` named by path (absolute or relative to the assets root) as a type's layer reference:
        // its header GUID and its path under the assets root.
        Common::ResultStr<Assets::AssetGuidRef> LayerInfoRefOf( const std::string& path )
        {
            std::filesystem::path file( path );
            if ( file.extension() != ".delayerinfo" )
                return Common::MakeFormattedError<Assets::AssetGuidRef>(
                     "'{}' is not a landscape layer info (.delayerinfo)", path );
            if ( file.is_relative() )
                file = Common::Constants::Path::ASSETS_PATH / file;
            const auto text = Common::Utils::FileSystem::ReadFileContent( file );
            if ( !text )
                return Common::MakeFormattedError<Assets::AssetGuidRef>( "'{}' does not read: {}", file.string(),
                                                                         text.GetError() );
            const auto info = Assets::Serialization::ParseLandscapeLayerInfo( text.GetValue() );
            if ( !info )
                return Common::MakeFormattedError<Assets::AssetGuidRef>( "'{}': {}", file.string(),
                                                                         info.GetError() );
            std::error_code   ec;
            const std::string relative =
                 std::filesystem::absolute( file, ec )
                      .lexically_normal()
                      .lexically_relative( std::filesystem::absolute( Common::Constants::Path::ASSETS_PATH, ec )
                                                .lexically_normal() )
                      .generic_string();
            if ( relative.empty() || relative.starts_with( ".." ) )
                return Common::MakeFormattedError<Assets::AssetGuidRef>(
                     "'{}' is not under the assets root '{}'", file.string(),
                     Common::Constants::Path::ASSETS_PATH.string() );
            return Common::MakeSuccess( Assets::AssetGuidRef{ info.GetValue().Header->Guid, relative } );
        }

        // The reference a `.defoliage` records for a cooked static mesh: its header GUID and its path under the
        // assets root. @p meshSourcePath names the mesh in an error.
        Common::ResultStr<Assets::AssetGuidRef> MeshRefOfAsset( const Assets::Asset<Assets::MeshAsset>& mesh,
                                                                const std::string& meshSourcePath )
        {
            if ( !mesh || mesh->Guid().IsNull() )
                return Common::MakeFormattedError<Assets::AssetGuidRef>(
                     "mesh '{}' has no header GUID, so no foliage type can name it", meshSourcePath );
            // BOTH SIDES ABSOLUTE: the metadata path can be working-dir-relative while ASSETS_PATH is absolute,
            // and lexically_relative across the two answers an empty path (FO-1 wrote such a Mesh.Path).
            std::error_code   ec;
            const std::string path =
                 std::filesystem::absolute( mesh->GetMetadata().Filepath, ec )
                      .lexically_normal()
                      .lexically_relative( std::filesystem::absolute( Common::Constants::Path::ASSETS_PATH, ec )
                                                .lexically_normal() )
                      .generic_string();
            if ( path.empty() || path.starts_with( ".." ) )
                return Common::MakeFormattedError<Assets::AssetGuidRef>(
                     "mesh '{}' cooked to '{}', which is not under the assets root '{}'", meshSourcePath,
                     mesh->GetMetadata().Filepath.string(), Common::Constants::Path::ASSETS_PATH.string() );
            return Common::MakeSuccess(
                 Assets::AssetGuidRef{ Common::Content::AssetGuidToText( mesh->Guid() ), path } );
        }

        // The cooked mesh a source path names, as a `.defoliage` names it; cooks the source on first use.
        Common::ResultStr<Assets::AssetGuidRef> MeshRefOf( Assets::AssetManager& manager,
                                                           const std::string&    meshSourcePath )
        {
            const auto handle = MeshDnD::ResolveOrImport( manager, meshSourcePath );
            if ( !handle )
                return Common::MakeFormattedError<Assets::AssetGuidRef>(
                     "mesh '{}' did not cook to a static mesh (a skinned source or an import failure)",
                     meshSourcePath );
            return MeshRefOfAsset( manager.FindByHandle<Assets::MeshAsset>( handle ), meshSourcePath );
        }
    } // namespace

    Assets::Asset<Assets::FoliageTypeAsset> FoliagePaintTool::ResolveType( Assets::AssetManager&      manager,
                                                                           const Assets::AssetHandle& handle )
    {
        if ( !handle )
            return nullptr;
        auto type = manager.FindByHandle<Assets::FoliageTypeAsset>( handle );
        if ( type && !type->IsReadyForUse() )
        {
            if ( const auto loaded = Assets::LoadThroughLoader( manager, type ); !loaded )
            {
                static std::unordered_set<uint64_t> s_Reported;
                if ( s_Reported.insert( static_cast<uint64_t>( handle ) ).second )
                    LOG_ERROR( "[Foliage] Foliage type '{}' could not be loaded: {}",
                               type->GetMetadata().Filepath.string(), loaded.GetError() );
                return nullptr;
            }
        }
        return type;
    }

    Assets::Asset<Assets::FoliageTypeAsset> FoliagePaintTool::OpenTypeFile( Assets::AssetManager& manager,
                                                                            const std::string&    path )
    {
        const std::filesystem::path named( path );
        const std::filesystem::path full =
             named.is_absolute() ? named : ( Common::Constants::Path::ASSETS_PATH / named ).lexically_normal();
        auto type = manager.FindByPath<Assets::FoliageTypeAsset>( full );
        if ( !type )
            type = manager.CreateAsset<Assets::FoliageTypeAsset>( Assets::AssetPriority::Medium, full,
                                                                  /*loadAfterCreate=*/false );
        if ( !type )
            return nullptr;
        if ( const auto loaded = Assets::LoadThroughLoader( manager, type ); !loaded )
        {
            LOG_ERROR( "[Foliage] Foliage type '{}' could not be loaded: {}", full.string(), loaded.GetError() );
            return nullptr;
        }
        return type;
    }

    void FoliagePaintTool::AddField( ::Desert::Core::Scene& scene, Assets::AssetManager& manager,
                                     const Assets::Asset<Assets::FoliageTypeAsset>& type )
    {
        const Assets::AssetHandle meshHandle = type->GetMeshHandle();
        if ( meshHandle )
        {
            // The ISM draws the type's mesh, so the mesh must be known to the mesh service the way a scene
            // load makes it known (ComponentRegistry's FromGuid).
            if ( auto mesh = manager.FindByHandle<Assets::MeshAsset>( meshHandle ) )
                Runtime::EnsureMeshRegistered( mesh, manager );
            else if ( !Runtime::DiscoverMesh( meshHandle ) )
                LOG_ERROR( "[Foliage] Foliage type '{}' names mesh {} ('{}') that this project has not scanned",
                           type->GetMetadata().Filepath.string(), type->GetData().Mesh.Guid,
                           type->GetData().Mesh.Path );
        }

        for ( auto& listed : scene.GetAllEntities() )
            if ( listed.HasComponent<ECS::FoliageComponent>() &&
                 listed.GetComponent<ECS::FoliageComponent>().FoliageType == type->GetMetadata().Handle )
            {
                const auto uuid = listed.GetComponent<ECS::UUIDComponent>().UUID;
                Core::FoliagePaint::SetActiveOnly( uuid );
                Core::FoliagePaint::SetEditingType( uuid );
                return;
            }

        auto& e = scene.CreateNewEntity( "Foliage_" + type->GetDisplayName() );
        e.AddComponent<ECS::FoliageComponent>().FoliageType            = type->GetMetadata().Handle;
        e.AddComponent<ECS::InstancedStaticMeshComponent>().MeshHandle = meshHandle;
        const auto newUuid = e.GetComponent<ECS::UUIDComponent>().UUID;
        Core::FoliagePaint::SetActiveOnly( newUuid );
        Core::FoliagePaint::SetEditingType( newUuid );
    }

    Common::BoolResultStr FoliagePaintTool::AddMeshRef( ::Desert::Core::Scene&      scene,
                                                        Assets::AssetManager&       manager,
                                                        const Assets::AssetGuidRef& mesh, const std::string& stem )
    {
        Assets::Serialization::FoliageTypeData wanted;
        wanted.Mesh     = mesh;
        const auto file = Assets::Serialization::FindOrCreateFoliageTypeFile(
             Common::Constants::Path::FOLIAGE_TYPE_PATH, wanted, stem );
        if ( !file )
            return Common::MakeFormattedError<bool>( "{}", file.GetError() );
        return AddTypeFile( scene, manager, file.GetValue().Path.string() );
    }

    Common::BoolResultStr FoliagePaintTool::AddMeshFile( ::Desert::Core::Scene& scene,
                                                         Assets::AssetManager&  manager,
                                                         const std::string&     meshSourcePath )
    {
        const auto mesh = MeshRefOf( manager, meshSourcePath );
        if ( !mesh )
            return Common::MakeFormattedError<bool>( "{}", mesh.GetError() );
        return AddMeshRef( scene, manager, mesh.GetValue(),
                           std::filesystem::path( meshSourcePath ).stem().string() );
    }

    Common::BoolResultStr FoliagePaintTool::AddCollection( ::Desert::Core::Scene& scene,
                                                           Assets::AssetManager&  manager,
                                                           const std::string&     manifestPath )
    {
        const auto text = Common::Utils::FileSystem::ReadFileContent( manifestPath );
        if ( !text )
            return Common::MakeFormattedError<bool>( "{}", text.GetError() );
        auto manifest = ReadCollectionManifest( text.GetValue() );
        if ( !manifest )
            return Common::MakeFormattedError<bool>( "'{}': {}", manifestPath, manifest.GetError() );
        CollectionManifest m     = manifest.GetValue();
        const auto         types = ResolveCollectionFoliageTypes(
             m, Common::Constants::Path::FOLIAGE_TYPE_PATH, Common::Constants::Path::ASSETS_PATH,
             [&manager]( const CollectionManifestItem& item ) { return MeshRefOf( manager, item.Mesh ); } );
        if ( !types )
            return Common::MakeFormattedError<bool>( "collection '{}': {}", manifestPath, types.GetError() );
        if ( types.GetValue().ManifestChanged )
            if ( auto saved = SaveCollectionManifest( manifestPath, m ); !saved )
                return saved;
        // A collection is also a palette PRESET (Foliage::SavePalettePreset): all of its types end up checked,
        // and only they, as UE checks every type of a folder dropped on the palette.
        std::vector<Common::UUID> listed;
        for ( const auto& file : types.GetValue().Types )
        {
            auto type = OpenTypeFile( manager, file.Path.string() );
            if ( !type )
                return Common::MakeFormattedError<bool>( "foliage type '{}' of collection '{}' did not load",
                                                         file.Path.string(), manifestPath );
            AddField( scene, manager, type );
            if ( const auto editing = Core::FoliagePaint::EditingType() )
                listed.push_back( *editing );
        }
        Core::FoliagePaint::ClearActive();
        for ( const auto& uuid : listed )
            if ( !Core::FoliagePaint::IsActive( uuid ) )
                Core::FoliagePaint::ToggleActive( uuid );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr FoliagePaintTool::AddTypeFile( ::Desert::Core::Scene& scene,
                                                         Assets::AssetManager& manager, const std::string& path )
    {
        const auto type = OpenTypeFile( manager, path );
        if ( !type )
            return Common::MakeFormattedError<bool>( "foliage type '{}' does not load (the log says why)", path );
        AddField( scene, manager, type );
        return BOOLSUCCESS;
    }

    void FoliagePaintTool::DrawTypeSettings( Assets::AssetManager&                          manager,
                                             const Assets::Asset<Assets::FoliageTypeAsset>& type )
    {
        // The widgets edit a COPY that lives across frames while a drag is in flight; the file is written
        // once when an edit ends, then read back, so the asset stays the one source of the numbers.
        static Assets::AssetHandle                    s_EditHandle;
        static Assets::Serialization::FoliageTypeData s_Edit;
        static bool                                   s_Editing = false;
        if ( !s_Editing || s_EditHandle != type->GetMetadata().Handle )
        {
            s_EditHandle = type->GetMetadata().Handle;
            s_Edit       = type->GetData();
        }
        auto&      f      = s_Edit;
        bool       commit = false;
        bool       active = false;
        const auto track  = [&]()
        {
            active = active || ImGui::IsItemActive();
            commit = commit || ImGui::IsItemDeactivatedAfterEdit();
        };

        // UE's FoliageType Details, section for section (FO-UI1): Painting, Placement, Instance Settings, Wind.
        // One row per field in the Details grid's look (ImGuiUtilities property rows).
        using Row      = Utils::ImGuiUtilities;
        const auto row = [&]( const char* label, const char* tip, const std::function<void()>& widget )
        {
            Row::BeginPropertyRow( label, tip );
            ImGui::SetNextItemWidth( -1 );
            widget();
            track();
            Row::EndPropertyRow();
        };
        ImGui::TextDisabled( "%s", type->GetMetadata().Filepath.filename().string().c_str() );
        Row::ResetPropertyRows();
        if ( Row::SectionHeader( "Painting" ) )
        {
            row( "Density", "Instances per 1000x1000 cm of brushed area (UE Density)",
                 [&] {
                     ImGui::SliderFloat( "##Density", &f.Density, 1.0f, 1000.0f, "%.0f",
                                         ImGuiSliderFlags_Logarithmic );
                 } );
            row( "Scale", "Uniform scale range drawn per instance (UE ScaleX, Uniform)",
                 [&] {
                     ImGui::DragFloatRange2( "##Scale", &f.ScaleX.Min, &f.ScaleX.Max, 0.01f, 0.02f, 10.0f, "%.2f",
                                             "%.2f" );
                 } );
            row( "Z Offset", "Offset along the placement up axis, cm",
                 [&]
                 {
                     ImGui::DragFloatRange2( "##ZOffset", &f.ZOffset.Min, &f.ZOffset.Max, 0.5f, -500.0f, 500.0f,
                                             "%.0f", "%.0f cm" );
                 } );
        }
        if ( Row::SectionHeader( "Placement" ) )
        {
            Row::BeginPropertyRow( "Align to Normal", "Tilt instances to the surface normal" );
            commit = ImGui::Checkbox( "##AlignToNormal", &f.AlignToNormal ) || commit;
            Row::EndPropertyRow();
            Row::BeginPropertyRow( "Random Yaw", "Random rotation about the up axis" );
            commit = ImGui::Checkbox( "##RandomYaw", &f.RandomYaw ) || commit;
            Row::EndPropertyRow();
            row( "Random Pitch", "Random tilt off the up axis, degrees",
                 [&] { ImGui::SliderFloat( "##Pitch", &f.RandomPitchAngle, 0.0f, 90.0f, "%.0f deg" ); } );
            row( "Ground Slope", "Paint only where the slope lies in this range, degrees",
                 [&]
                 {
                     ImGui::DragFloatRange2( "##Slope", &f.GroundSlopeAngle.Min, &f.GroundSlopeAngle.Max, 0.5f,
                                             0.0f, 90.0f, "%.0f", "%.0f deg" );
                 } );
            row( "Height", "Paint only where the world height (Y) lies in this range, cm",
                 [&]
                 {
                     ImGui::DragFloatRange2( "##Height", &f.Height.Min, &f.Height.Max, 10.0f, -262144.0f,
                                             262144.0f, "%.0f", "%.0f cm" );
                 } );

            // UE LandscapeLayers: a `.delayerinfo` dropped here restricts the type to ground painted with it.
            for ( size_t i = 0; i < f.LandscapeLayers.size(); ++i )
            {
                ImGui::PushID( static_cast<int>( i ) );
                Row::BeginPropertyRow( i == 0 ? "Landscape Layers" : "" );
                const bool removed = ImGui::SmallButton( ICON_MDI_CLOSE );
                ImGui::SameLine();
                ImGui::TextUnformatted( f.LandscapeLayers[i].Path.c_str() );
                Row::EndPropertyRow();
                ImGui::PopID();
                if ( removed )
                {
                    f.LandscapeLayers.erase( f.LandscapeLayers.begin() + static_cast<std::ptrdiff_t>( i ) );
                    commit = true;
                    break;
                }
            }
            Row::BeginPropertyRow( f.LandscapeLayers.empty() ? "Landscape Layers" : "",
                                   "Drop a .delayerinfo: the type paints only on ground painted with it" );
            ImGui::Button( f.LandscapeLayers.empty() ? "Any layer - drop a .delayerinfo"
                                                     : ICON_MDI_PLUS " Add a layer",
                           ImVec2( -1, 0 ) );
            if ( ImGui::BeginDragDropTarget() )
            {
                if ( const ImGuiPayload* p =
                          ImGui::AcceptDragDropPayload( ::Desert::Editor::DragPayloads::AssetFile ) )
                {
                    if ( auto ref = LayerInfoRefOf( std::string( static_cast<const char*>( p->Data ) ) ); !ref )
                    {
                        LOG_ERROR( "[Foliage] {}", ref.GetError() );
                    }
                    else if ( std::ranges::none_of( f.LandscapeLayers, [&]( const Assets::AssetGuidRef& r )
                                                    { return r.Guid == ref.GetValue().Guid; } ) )
                    {
                        f.LandscapeLayers.push_back( ref.GetValue() );
                        commit = true;
                    }
                }
                ImGui::EndDragDropTarget();
            }
            Row::EndPropertyRow();
            if ( !f.LandscapeLayers.empty() )
                row( "Min Layer Weight", "The weight a listed layer must reach (UE MinimumLayerWeight)", [&]
                     { ImGui::SliderFloat( "##MinLayerWeight", &f.MinimumLayerWeight, 0.0f, 1.0f, "%.2f" ); } );
        }
        if ( Row::SectionHeader( "Instance Settings" ) )
        {
            // UE CullDistance: fade from Min, gone from Max; Max 0 = never culled.
            row( "Cull Distance", "Fade out from Min, none drawn from Max, cm; Max 0 = never culled",
                 [&]
                 {
                     ImGui::DragFloatRange2( "##CullDistance", &f.CullDistance.Min, &f.CullDistance.Max, 50.0f,
                                             0.0f, 1000000.0f, "%.0f", "%.0f cm" );
                 } );
        }
        // Wind (FO-7; UE SimpleGrassWind): the tip's largest sway, its rate, the height it is reached at, and
        // the direction the wind blows towards. Strength 0 = still.
        if ( Row::SectionHeader( "Wind" ) )
        {
            row( "Strength", "The tip's largest sway, cm; 0 = still", [&]
                 { ImGui::DragFloat( "##WindStrength", &f.Wind.Strength, 1.0f, 0.0f, 10000.0f, "%.0f cm" ); } );
            row( "Speed", "Sway rate, Hz",
                 [&] { ImGui::DragFloat( "##WindSpeed", &f.Wind.Speed, 0.01f, 0.0f, 20.0f, "%.2f Hz" ); } );
            row( "Full Sway Height", "The height the full sway is reached at, cm",
                 [&] { ImGui::DragFloat( "##WindHeight", &f.Wind.Height, 1.0f, 1.0f, 100000.0f, "%.0f cm" ); } );
            row( "Direction", "The direction the wind blows towards, degrees",
                 [&] {
                     ImGui::DragFloat( "##WindDirection", &f.Wind.DirectionDegrees, 1.0f, -360.0f, 360.0f,
                                       "%.0f deg" );
                 } );
        }
        s_Editing = active;

        if ( commit && !( f == type->GetData() ) )
        {
            if ( const auto saved = Assets::FoliageTypeAsset::Save( type->GetMetadata().Filepath, f ); !saved )
            {
                LOG_ERROR( "[Foliage] {}", saved.GetError() );
                s_Edit = type->GetData();
                return;
            }
            (void)type->Unload();
            if ( const auto loaded = Assets::LoadThroughLoader( manager, type ); !loaded )
                LOG_ERROR( "[Foliage] Foliage type '{}' could not be read back: {}",
                           type->GetMetadata().Filepath.string(), loaded.GetError() );
        }
    }

    namespace
    {
        // The largest weight, 0..1, of @p layers on @p tile at world (x, z): bilinear between the four samples
        // (UE ULandscapeComponent::GetLayerWeightAtLocation). A layer the tile holds no plane for weighs 0.
        float MaxLayerWeight( const World::Landscape::LandscapeTileData& tile,
                              const World::Landscape::LandscapeFrame&    frame,
                              const std::vector<std::string>& layers, float x, float z )
        {
            const float lastX = static_cast<float>( tile.SamplesX() - 1u );
            const float lastZ = static_cast<float>( tile.SamplesZ() - 1u );
            const float fx    = glm::clamp( ( x - frame.OriginX ) / frame.SpacingCm, 0.0f, lastX );
            const float fz    = glm::clamp( ( z - frame.OriginZ ) / frame.SpacingCm, 0.0f, lastZ );
            const auto  x0    = static_cast<uint32_t>( fx );
            const auto  z0    = static_cast<uint32_t>( fz );
            const auto  x1    = std::min( x0 + 1u, tile.SamplesX() - 1u );
            const auto  z1    = std::min( z0 + 1u, tile.SamplesZ() - 1u );
            const float tx = fx - static_cast<float>( x0 ), tz = fz - static_cast<float>( z0 );
            float       best = 0.0f;
            for ( const auto& name : layers )
            {
                const auto layer = tile.FindWeightLayer( name );
                if ( !layer )
                    continue;
                const auto w = [&]( uint32_t sx, uint32_t sz )
                { return static_cast<float>( tile.Weight( *layer, sx, sz ) ) / 255.0f; };
                const float v = glm::mix( glm::mix( w( x0, z0 ), w( x1, z0 ), tx ),
                                          glm::mix( w( x0, z1 ), w( x1, z1 ), tx ), tz );
                best          = std::max( best, v );
            }
            return best;
        }

        // The weight-plane names of the type's layer infos. A layer info still loading or unusable is an
        // error naming it: painting without it would place on ground the type excludes.
        Common::ResultStr<std::vector<std::string>>
        LayerNamesOf( const Assets::Serialization::FoliageTypeData& type )
        {
            std::vector<std::string> names;
            auto*                    service = Runtime::ResourceRegistry::GetLandscapeLayerInfoService();
            for ( const auto& ref : type.LandscapeLayers )
            {
                const auto guid = Common::Content::AssetGuidFromText( ref.Guid );
                if ( !guid )
                    return Common::MakeFormattedError<std::vector<std::string>>(
                         "landscape layer '{}' names an unreadable GUID '{}'", ref.Path, ref.Guid );
                const auto  handle = Common::Content::HandleForGuid( guid.GetValue() );
                const auto* info   = service ? service->Get( handle ) : nullptr;
                if ( !info )
                    return Common::MakeFormattedError<std::vector<std::string>>(
                         "landscape layer '{}' is {}", ref.Path,
                         !service ? std::string( "unreadable: no layer info service" )
                         : service->StateOf( handle ) == Runtime::LandscapeLayerInfoService::State::Pending
                              ? std::string( "still loading" )
                              : "unusable: " + service->ErrorOf( handle ) );
                names.push_back( info->LayerName );
            }
            return Common::MakeSuccess( std::move( names ) );
        }

        // The landscape tile entity @p entity names, with its frame; nullopt when it is not a drawn tile.
        struct TileSample
        {
            const World::Landscape::LandscapeTileData* Tile = nullptr;
            World::Landscape::LandscapeFrame           Frame;
        };
        std::optional<TileSample> TileOf( ::Desert::Core::Scene& scene, const Common::UUID& entity )
        {
            const auto ref = scene.FindEntityByID( entity );
            if ( !ref || !ref->get().HasComponent<ECS::LandscapeTileComponent>() )
                return std::nullopt;
            const auto& tile = ref->get().GetComponent<ECS::LandscapeTileComponent>();
            if ( !tile.Heights )
                return std::nullopt;
            const auto root = scene.FindEntityByID( tile.Landscape );
            if ( !root || !root->get().HasComponent<ECS::LandscapeComponent>() )
                return std::nullopt;
            return TileSample{ &*tile.Heights,
                               World::Landscape::LandscapeTileFrame( ECS::LandscapeRootOf( root->get() ),
                                                                     tile.TileX, tile.TileZ ) };
        }

        // UE ULandscapeInfo::XYtoComponentMap: the drawn tiles, gathered once per dab and found by entity or by
        // the tile coordinate a world (x, z) falls in. Walking every entity per question cost 43 ms for the
        // 828 instances under a second dab on Terrain_Grass (FO-3b).
        class LandscapeTileIndex
        {
        public:
            explicit LandscapeTileIndex( ::Desert::Core::Scene& scene )
            {
                for ( const auto& entity : scene.GetAllEntities() )
                {
                    if ( !entity.HasComponent<ECS::LandscapeTileComponent>() )
                        continue;
                    const Common::UUID id     = entity.GetComponent<ECS::UUIDComponent>().UUID;
                    const auto         sample = TileOf( scene, id );
                    if ( !sample )
                        continue;
                    const auto& tile = entity.GetComponent<ECS::LandscapeTileComponent>();
                    m_ByEntity.emplace( id, *sample );
                    LandscapeTiles& landscape = LandscapeOf( scene, tile.Landscape );
                    landscape.ByCoord.emplace( Key( tile.TileX, tile.TileZ ), *sample );
                }
            }

            [[nodiscard]] const TileSample* OfEntity( const Common::UUID& entity ) const
            {
                const auto it = m_ByEntity.find( entity );
                return it == m_ByEntity.end() ? nullptr : &it->second;
            }

            // The tile whose rectangle holds (x, z), both edges inclusive; a point on a shared edge is also
            // looked for in the tile below it, so the last tile's far edge is found too.
            [[nodiscard]] const TileSample* At( float x, float z ) const
            {
                for ( const auto& landscape : m_Landscapes )
                {
                    const auto tx =
                         static_cast<int32_t>( std::floor( ( x - landscape.OriginX ) / landscape.ExtentCm ) );
                    const auto tz =
                         static_cast<int32_t>( std::floor( ( z - landscape.OriginZ ) / landscape.ExtentCm ) );
                    for ( const int32_t dz : { 0, -1 } )
                        for ( const int32_t dx : { 0, -1 } )
                        {
                            const auto it = landscape.ByCoord.find( Key( tx + dx, tz + dz ) );
                            if ( it != landscape.ByCoord.end() && Holds( it->second, x, z ) )
                                return &it->second;
                        }
                }
                return nullptr;
            }

        private:
            struct LandscapeTiles
            {
                Common::UUID                            Root;
                float                                   OriginX  = 0.0f;
                float                                   OriginZ  = 0.0f;
                float                                   ExtentCm = 1.0f; // one tile's side
                std::unordered_map<int64_t, TileSample> ByCoord;
            };

            static int64_t Key( int32_t tileX, int32_t tileZ )
            {
                return ( static_cast<int64_t>( tileX ) << 32 ) ^
                       static_cast<int64_t>( static_cast<uint32_t>( tileZ ) );
            }

            static bool Holds( const TileSample& sample, float x, float z )
            {
                const float spanX = sample.Frame.SpacingCm * static_cast<float>( sample.Tile->SamplesX() - 1u );
                const float spanZ = sample.Frame.SpacingCm * static_cast<float>( sample.Tile->SamplesZ() - 1u );
                return x >= sample.Frame.OriginX && x <= sample.Frame.OriginX + spanX &&
                       z >= sample.Frame.OriginZ && z <= sample.Frame.OriginZ + spanZ;
            }

            // TileOf has already checked that the root exists and is a landscape.
            LandscapeTiles& LandscapeOf( ::Desert::Core::Scene& scene, const Common::UUID& root )
            {
                for ( auto& landscape : m_Landscapes )
                    if ( landscape.Root == root )
                        return landscape;
                const auto layout = ECS::LandscapeRootOf( scene.FindEntityByID( root )->get() );
                auto&      added  = m_Landscapes.emplace_back();
                added.Root        = root;
                added.OriginX     = layout.Origin.x;
                added.OriginZ     = layout.Origin.z;
                added.ExtentCm    = static_cast<float>( layout.QuadsPerTile ) * layout.SpacingCm;
                return added;
            }

            std::unordered_map<Common::UUID, TileSample> m_ByEntity;
            std::vector<LandscapeTiles>                  m_Landscapes;
        };

        /// One press of a foliage tool, undone and redone as a whole - instances and selection (UE: one
        /// FScopedTransaction per stroke, "Foliage Paint" / "Foliage Remove" / ...).
        class FoliageStrokeCommand final : public ICommand
        {
        public:
            FoliageStrokeCommand( ::Desert::Core::Scene& scene, std::vector<FoliageStrokeField> fields,
                                  std::string label )
                 : m_Scene( &scene ), m_Fields( std::move( fields ) ), m_Label( std::move( label ) )
            {
            }

            bool Undo() override
            {
                return Write( true );
            }
            bool Redo() override
            {
                return Write( false );
            }
            std::string GetLabel() const override
            {
                return m_Label;
            }

        private:
            bool Write( bool before )
            {
                bool all = true;
                for ( const auto& field : m_Fields )
                {
                    const auto ref = m_Scene->FindEntityByID( field.Entity );
                    if ( !ref || !ref->get().HasComponent<ECS::InstancedStaticMeshComponent>() )
                    {
                        ToastManager::Push( "Foliage undo: the painted field is gone", ToastLevel::Error, 6.0f );
                        all = false;
                        continue;
                    }
                    ref->get().GetComponent<ECS::InstancedStaticMeshComponent>().InstanceTransforms =
                         before ? field.Before : field.After;
                    Core::FoliagePaint::Selection()[field.Entity] =
                         before ? field.SelectedBefore : field.SelectedAfter;
                }
                return all;
            }

            ::Desert::Core::Scene*          m_Scene;
            std::vector<FoliageStrokeField> m_Fields;
            std::string                     m_Label;
        };
    } // namespace

    namespace
    {
        // FO-6: the grid foliage is filed by - the world's first grid, the one the planner places every
        // composite by (WorldPartitionPlan::UnusedGrids) - or none for a world that is not partitioned, whose
        // foliage stays one field per type (UE: one IFA per level).
        std::optional<double> FoliageCellSize( ::Desert::Core::Scene& scene )
        {
            const auto& partition = scene.GetWorldPartition();
            if ( !partition.has_value() || partition->Grids.empty() )
                return std::nullopt;
            return static_cast<double>( partition->Grids[0].CellSize );
        }

        bool IsFoliageField( const ECS::Entity& entity )
        {
            return entity.HasComponent<ECS::FoliageComponent>() &&
                   entity.HasComponent<ECS::InstancedStaticMeshComponent>();
        }

        // Every field painting @p type, in scene order (UE: the FFoliageInfo of the type in every IFA).
        std::vector<ECS::Entity> FieldsOfType( ::Desert::Core::Scene& scene, const Assets::AssetHandle& type )
        {
            std::vector<ECS::Entity> fields;
            for ( const auto& entity : scene.GetAllEntities() )
                if ( IsFoliageField( entity ) && entity.GetComponent<ECS::FoliageComponent>().FoliageType == type )
                    fields.push_back( entity );
            return fields;
        }

        // The cell a field is filed under: the cell its entity stands in (World::Foliage::FoliageCellAnchor puts
        // a cell field at its cell's centre). A field made before the world was partitioned stands where it was
        // made; RepartitionFoliage moves its instances out and leaves it the field of that one cell.
        World::Foliage::CellCoord CellOfField( const ECS::Entity& field, double cellSize )
        {
            const auto& at = field.GetComponent<ECS::TransformComponent>().Translation;
            return ::Desert::Core::Rules::CellOf( at.x, at.z, cellSize );
        }

        struct BrushDisc
        {
            float X      = 0.0f;
            float Z      = 0.0f;
            float Radius = 0.0f;
        };

        // The fields of one palette row's type an edit works on, merged (World::Foliage::GatherFoliage).
        struct CellGather
        {
            ECS::Entity                            Lead; ///< the palette row: what a new cell field copies
            std::vector<Common::UUID>              Fields;
            std::vector<World::Foliage::CellCoord> Cells;
            World::Foliage::FoliageGathered        Data;
        };

        // @p disc: only the fields whose cell it reaches (every field when the world is not partitioned);
        // none: every field of the type. Each gathered field is touched in @p stroke before anything changes.
        Common::ResultStr<CellGather> GatherCells( ::Desert::Core::Scene& scene, const Common::UUID& row,
                                                   const std::optional<BrushDisc>& disc, FoliageStroke& stroke )
        {
            const auto ref = scene.FindEntityByID( row );
            if ( !ref || !IsFoliageField( ref->get() ) )
                return Common::MakeFormattedError<CellGather>( "foliage: palette row {} is not a foliage field",
                                                               static_cast<uint64_t>( row ) );
            CellGather gather;
            gather.Lead                                            = ref->get();
            const auto                                    cellSize = FoliageCellSize( scene );
            std::vector<World::Foliage::FoliageCellField> fields;
            for ( const auto& field :
                  FieldsOfType( scene, gather.Lead.GetComponent<ECS::FoliageComponent>().FoliageType ) )
            {
                World::Foliage::CellCoord cell;
                if ( cellSize.has_value() )
                {
                    cell = CellOfField( field, *cellSize );
                    if ( disc.has_value() && !World::Foliage::FoliageCellTouchesDisc( cell, *cellSize, disc->X,
                                                                                      disc->Z, disc->Radius ) )
                        continue;
                }
                const auto  uuid      = field.GetComponent<ECS::UUIDComponent>().UUID;
                const auto& instances = field.GetComponent<ECS::InstancedStaticMeshComponent>().InstanceTransforms;
                const auto& selected  = Core::FoliagePaint::Selection()[uuid];
                stroke.Touch( uuid, instances, selected );
                gather.Fields.push_back( uuid );
                gather.Cells.push_back( cell );
                fields.push_back( World::Foliage::FoliageCellField{ cell, instances, selected } );
            }
            auto merged = World::Foliage::GatherFoliage( fields );
            if ( !merged )
                return Common::MakeFormattedError<CellGather>( "{}", merged.GetError() );
            gather.Data = merged.ExtractValue();
            return Common::MakeSuccess( std::move( gather ) );
        }

        // Writes @p gather back, every instance into the field of its cell (World::Foliage::ScatterFoliage); a
        // cell with no field gets a copy of the row's field standing in it, touched in @p stroke while still empty
        // so an undo empties it. Not partitioned: everything goes back into the first gathered field.
        Common::BoolResultStr ScatterCells( ::Desert::Core::Scene& scene, CellGather& gather,
                                            FoliageStroke& stroke )
        {
            const auto cellSize = FoliageCellSize( scene );
            // Two fields filed under one cell (a type replaced into another, a world whose grid changed) meet in
            // the first; the others come back empty. Not partitioned, every field is "the one cell".
            std::vector<World::Foliage::CellCoord>  unique;
            std::vector<std::optional<std::size_t>> slotOf( gather.Fields.size() );
            for ( std::size_t i = 0; i < gather.Fields.size(); ++i )
            {
                const auto& cell = gather.Cells[i];
                if ( std::find( unique.begin(), unique.end(), cell ) != unique.end() ||
                     ( !cellSize.has_value() && !unique.empty() ) )
                    continue;
                slotOf[i] = unique.size();
                unique.push_back( cell );
            }
            std::vector<World::Foliage::FoliageCellField> out;
            if ( !cellSize.has_value() )
            {
                out.resize( unique.size() );
                if ( !out.empty() )
                {
                    out[0].Instances = std::move( gather.Data.Instances );
                    out[0].Selected  = std::move( gather.Data.Selected );
                }
            }
            else
            {
                auto scattered = World::Foliage::ScatterFoliage( gather.Data, unique, *cellSize );
                if ( !scattered )
                    return Common::MakeFormattedError<bool>( "{}", scattered.GetError() );
                out = scattered.ExtractValue();
            }
            for ( std::size_t i = unique.size(); i < out.size(); ++i )
                slotOf.emplace_back( i );

            if ( out.size() > unique.size() )
            {
                // Read before creating: a new entity may move the registry's storage under a reference.
                const auto type    = gather.Lead.GetComponent<ECS::FoliageComponent>().FoliageType;
                const auto lead    = gather.Lead.GetComponent<ECS::InstancedStaticMeshComponent>();
                const auto tag     = gather.Lead.GetComponent<ECS::TagComponent>().Tag;
                const bool visible = !gather.Lead.HasComponent<ECS::VisibilityComponent>() ||
                                     gather.Lead.GetComponent<ECS::VisibilityComponent>().Visible;
                for ( std::size_t i = unique.size(); i < out.size(); ++i )
                {
                    const auto& cell  = out[i].Cell;
                    auto&       field = scene.CreateNewEntity( tag + "_" + std::to_string( cell.X ) + "_" +
                                                               std::to_string( cell.Z ) );
                    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) cellSize is set: only a scatter adds
                    // fields
                    field.GetComponent<ECS::TransformComponent>().Translation =
                         World::Foliage::FoliageCellAnchor( cell, *cellSize );
                    field.AddComponent<ECS::FoliageComponent>().FoliageType = type;
                    auto& ism         = field.AddComponent<ECS::InstancedStaticMeshComponent>();
                    ism.MeshHandle    = lead.MeshHandle;
                    ism.MaterialSlots = lead.MaterialSlots;
                    ism.CastShadows   = lead.CastShadows;
                    if ( !visible )
                        scene.SetVisibleRecursive( field, false );
                    const auto uuid = field.GetComponent<ECS::UUIDComponent>().UUID;
                    stroke.Touch( uuid, {}, {} );
                    gather.Fields.push_back( uuid );
                }
            }

            for ( std::size_t i = 0; i < gather.Fields.size(); ++i )
            {
                const auto ref = scene.FindEntityByID( gather.Fields[i] );
                if ( !ref || !IsFoliageField( ref->get() ) )
                    return Common::MakeFormattedError<bool>(
                         "foliage: field {} vanished while its cell was written",
                         static_cast<uint64_t>( gather.Fields[i] ) );
                auto& instances = ref->get().GetComponent<ECS::InstancedStaticMeshComponent>().InstanceTransforms;
                auto& selected  = Core::FoliagePaint::Selection()[gather.Fields[i]];
                if ( !slotOf[i].has_value() )
                {
                    instances.clear();
                    selected.clear();
                    continue;
                }
                instances = std::move( out[*slotOf[i]].Instances );
                selected  = std::move( out[*slotOf[i]].Selected );
            }

            return BOOLSUCCESS;
        }

        // Every instance of every field of @p row's type, as a selection per field.
        void SelectEveryInstance( ::Desert::Core::Scene& scene, const ECS::Entity& row,
                                  std::unordered_map<Common::UUID, FoliageSelection>& wanted )
        {
            for ( const auto& field :
                  FieldsOfType( scene, row.GetComponent<ECS::FoliageComponent>().FoliageType ) )
            {
                FoliageSelection all(
                     field.GetComponent<ECS::InstancedStaticMeshComponent>().InstanceTransforms.size() );
                for ( uint32_t i = 0; i < all.size(); ++i )
                    all[i] = i;
                wanted[field.GetComponent<ECS::UUIDComponent>().UUID] = std::move( all );
            }
        }

        std::vector<FoliageStrokeField> FinishStroke( ::Desert::Core::Scene& scene, const FoliageStroke& stroke )
        {
            return stroke.Finish(
                 [&]( const Common::UUID& uuid ) -> const std::vector<glm::mat4>*
                 {
                     const auto ref = scene.FindEntityByID( uuid );
                     if ( !ref || !ref->get().HasComponent<ECS::InstancedStaticMeshComponent>() )
                         return nullptr;
                     return &ref->get().GetComponent<ECS::InstancedStaticMeshComponent>().InstanceTransforms;
                 },
                 []( const Common::UUID& uuid ) { return Core::FoliagePaint::SelectionOf( uuid ); } );
        }

        // Every field of every type in @p rows (palette rows) gathered whole and filed back by cell.
        Common::BoolResultStr RepartitionRows( ::Desert::Core::Scene& scene, const std::vector<Common::UUID>& rows,
                                               FoliageStroke& stroke )
        {
            for ( const auto& row : rows )
            {
                auto gathered = GatherCells( scene, row, std::nullopt, stroke );
                if ( !gathered )
                    return Common::MakeFormattedError<bool>( "{}", gathered.GetError() );
                CellGather gather = gathered.ExtractValue();
                if ( auto filed = ScatterCells( scene, gather, stroke ); !filed )
                    return filed;
            }
            return BOOLSUCCESS;
        }
    } // namespace

    std::vector<glm::mat4> FoliagePaintTool::RowInstances( ::Desert::Core::Scene& scene, const Common::UUID& row,
                                                           const std::optional<glm::vec3>& around, float radius )
    {
        std::vector<glm::mat4> all;
        const auto             ref = scene.FindEntityByID( row );
        if ( !ref || !IsFoliageField( ref->get() ) )
            return all;
        const auto cellSize = FoliageCellSize( scene );
        for ( const auto& field :
              FieldsOfType( scene, ref->get().GetComponent<ECS::FoliageComponent>().FoliageType ) )
        {
            if ( around.has_value() && cellSize.has_value() &&
                 !World::Foliage::FoliageCellTouchesDisc( CellOfField( field, *cellSize ), *cellSize, around->x,
                                                          around->z, radius ) )
                continue;
            const auto& instances = field.GetComponent<ECS::InstancedStaticMeshComponent>().InstanceTransforms;
            all.insert( all.end(), instances.begin(), instances.end() );
        }
        return all;
    }

    Common::BoolResultStr FoliagePaintTool::RepartitionFoliage( ::Desert::Core::Scene& scene )
    {
        std::vector<Common::UUID> rows;
        for ( const auto& field : PaletteFields( scene ) )
            rows.push_back( field.GetComponent<ECS::UUIDComponent>().UUID );
        FoliageStroke stroke( 0u );
        if ( auto filed = RepartitionRows( scene, rows, stroke ); !filed )
            return filed;
        auto fields = FinishStroke( scene, stroke );
        if ( !fields.empty() )
            CommandHistory::Get().PushCommand(
                 std::make_unique<FoliageStrokeCommand>( scene, std::move( fields ), "Foliage Repartition" ) );
        return BOOLSUCCESS;
    }

    void FoliagePaintTool::Update( ::Desert::Core::Scene& scene, const Assets::AssetManager* assetManager,
                                   const Common::Math::Ray& ray, bool pressing, bool shift )
    {
        if ( !pressing || !Core::FoliagePaint::HasActive() || !assetManager )
        {
            EndStroke( scene );
            return;
        }
        if ( !m_Stroke )
        {
            m_Stroke.emplace( Core::FoliagePaint::NextStrokeSeed() );
            m_StrokeTool = Core::FoliagePaint::Tool();
            m_Applied    = false;
            m_Refused.clear();
        }
        // UE: Single places and Select picks once per click, not once per tick of a held button.
        if ( m_Applied )
            return;
        auto& manager = const_cast<Assets::AssetManager&>( *assetManager );

        if ( m_StrokeTool == Core::FoliageTool::Select )
        {
            PickAlongRay( scene, ray, shift );
            m_Applied = true;
            return;
        }

        ::Desert::Core::RaycastHit centre;
        if ( !scene.Raycast( ray, centre ) )
            return; // UE: no surface under the cursor, no application this tick

        if ( m_StrokeTool == Core::FoliageTool::Fill )
        {
            // UE's Fill acts on the actor clicked, once per click.
            m_Applied = true;
            if ( auto filled = FillEntity( scene, manager, centre.Entity ); !filled )
                ToastManager::Push( filled.GetError(), ToastLevel::Warning, 6.0f );
            return;
        }

        FoliageBrushDab dab;
        dab.Center            = centre.Point;
        dab.Normal            = centre.Normal;
        dab.Radius            = Core::FoliagePaint::BrushRadius();
        dab.PaintDensity      = Core::FoliagePaint::PaintDensity();
        dab.Filter            = Core::FoliagePaint::SurfaceFilter();
        const LandscapeTileIndex tiles( scene );
        // One landscape set for every ray of the dab: gathering it per ray was 81 of the dab's 98 ms (FO-3b).
        const auto landscapeSet = scene.GatherRaycastLandscape();

        for ( const auto& uuid : Core::FoliagePaint::ActiveTypes() )
        {
            // FO-6: the brush works on every field of the row's type whose cell its disc reaches, merged into one
            // buffer (UE: the FFoliageInfo of that type in each partition IFA the brush overlaps); what the dab
            // leaves is filed back by cell, a cell without a field getting one.
            auto gathered =
                 GatherCells( scene, uuid, BrushDisc{ dab.Center.x, dab.Center.z, dab.Radius }, *m_Stroke );
            if ( !gathered )
            {
                if ( m_Refused.insert( uuid ).second )
                    ToastManager::Push( gathered.GetError(), ToastLevel::Error, 6.0f );
                continue;
            }
            CellGather  gather    = gathered.ExtractValue();
            auto&       instances = gather.Data.Instances;
            auto&       selected  = gather.Data.Selected;
            const auto& lead      = gather.Lead;
            [&]()
            {
                if ( m_StrokeTool == Core::FoliageTool::Remove )
                {
                    FoliageBrushRemove( instances, dab.Center, dab.Radius, &selected );
                    return;
                }
                if ( m_StrokeTool == Core::FoliageTool::Lasso )
                {
                    FoliageSelectInSphere( instances, dab.Center, dab.Radius, !shift, selected );
                    return;
                }

                const auto type = ResolveType( manager, lead.GetComponent<ECS::FoliageComponent>().FoliageType );
                if ( !type )
                    return;
                const auto& data   = type->GetData();
                const auto  layers = LayerNamesOf( data );
                if ( !layers )
                {
                    if ( m_Refused.insert( uuid ).second )
                        ToastManager::Push( "Foliage '" + type->GetDisplayName() + "': " + layers.GetError(),
                                            ToastLevel::Error, 6.0f );
                    return;
                }
                const std::vector<std::string>& names = layers.GetValue();

                // The trace's own split (FO-3b): the scene raycast against the layer-weight sample at the hit.
                using Clock           = std::chrono::steady_clock;
                double     raycastMs  = 0.0;
                double     hitLayerMs = 0.0;
                const auto msSince    = []( Clock::time_point at )
                { return std::chrono::duration<double, std::milli>( Clock::now() - at ).count(); };

                FoliageBrushWorld world;
                world.Trace = [&]( const glm::vec3& start, const glm::vec3& end,
                                   const FoliageSurfaceFilter& filter ) -> std::optional<FoliageTraceHit>
                {
                    // UE FFoliagePaintingGeometryFilter: a surface the brush may not paint on is traced THROUGH,
                    // so a landscape-only brush reaches the ground under a mesh instead of losing the spot.
                    const auto accept = [&]( const Common::UUID& id )
                    {
                        const bool landscape = tiles.OfEntity( id ) != nullptr;
                        return filter.Allows( landscape ? FoliageSurface::Landscape : FoliageSurface::StaticMesh );
                    };
                    const glm::vec3 d   = end - start;
                    const float     len = glm::length( d );
                    if ( len <= 0.0f )
                        return std::nullopt;
                    ::Desert::Core::RaycastHit hit;
                    const auto                 rayAt = Clock::now();
                    const bool                 found =
                         scene.Raycast( Common::Math::Ray( start, d / len ), hit, accept, landscapeSet );
                    raycastMs += msSince( rayAt );
                    if ( !found || hit.Distance > len )
                        return std::nullopt;
                    const auto      layerAt = Clock::now();
                    FoliageTraceHit out;
                    out.Point  = hit.Point;
                    out.Normal = hit.Normal;
                    if ( const auto* tile = tiles.OfEntity( hit.Entity ) )
                    {
                        out.Surface = FoliageSurface::Landscape;
                        if ( !names.empty() )
                            out.LayerWeight =
                                 MaxLayerWeight( *tile->Tile, tile->Frame, names, hit.Point.x, hit.Point.z );
                        if ( filter.LayerFiltered )
                            out.BrushLayerWeight =
                                 MaxLayerWeight( *tile->Tile, tile->Frame, Core::FoliagePaint::BrushLayers(),
                                                 hit.Point.x, hit.Point.z );
                    }
                    else
                        out.Surface = FoliageSurface::StaticMesh;
                    hitLayerMs += msSince( layerAt );
                    return out;
                };
                world.LayerWeightAt = [&]( const glm::vec3& p ) -> std::optional<float>
                {
                    const auto* tile = tiles.At( p.x, p.z );
                    if ( !tile )
                        return std::nullopt;
                    return MaxLayerWeight( *tile->Tile, tile->Frame, names, p.x, p.z );
                };

                if ( m_StrokeTool == Core::FoliageTool::Single )
                {
                    if ( const auto placed = FoliageBrushSingle( data, dab, m_Stroke->Random(), world ) )
                        instances.push_back( *placed );
                    return;
                }
                if ( m_StrokeTool == Core::FoliageTool::Reapply )
                {
                    const auto r =
                         FoliageBrushReapply( data, Core::FoliagePaint::Reapply(), dab, instances,
                                              m_Stroke->Readjusted( uuid ), m_Stroke->Random(), world, &selected );
                    LOG_INFO( "[Foliage] reapply '{}': {} rebuilt, {} removed, {} without ground; field now {}",
                              type->GetDisplayName(), r.Updated, r.Removed, r.Skipped, instances.size() );
                    return;
                }

                FoliageBrushStats stats;
                const auto        dabAt = Clock::now();
                auto added = FoliageBrushAdd( data, dab, instances, m_Stroke->Random(), world, &stats );
                instances.insert( instances.end(), added.begin(), added.end() );
                LOG_INFO(
                     "[Foliage] dab '{}': {} placed ({} candidates, {} hits, {} passed; field now {}) in {:.1f} "
                     "ms: existing-layer {:.1f}, generate {:.1f}, trace {:.1f} (scene raycast {:.1f}, hit layer "
                     "weight {:.1f}), filters {:.1f}, place {:.1f}",
                     type->GetDisplayName(), stats.Placed, stats.Candidates, stats.Hits, stats.Passed,
                     instances.size(), msSince( dabAt ), stats.ExistingLayerMs, stats.GenerateMs, stats.TraceMs,
                     raycastMs, hitLayerMs, stats.FilterMs, stats.PlaceMs );
            }();
            if ( auto filed = ScatterCells( scene, gather, *m_Stroke ); !filed )
                ToastManager::Push( filed.GetError(), ToastLevel::Error, 6.0f );
        }
    }

    void FoliagePaintTool::EndStroke( ::Desert::Core::Scene& scene )
    {
        if ( !m_Stroke )
            return;
        auto fields = m_Stroke->Finish(
             [&]( const Common::UUID& uuid ) -> const std::vector<glm::mat4>*
             {
                 const auto ref = scene.FindEntityByID( uuid );
                 if ( !ref || !ref->get().HasComponent<ECS::InstancedStaticMeshComponent>() )
                     return nullptr;
                 return &ref->get().GetComponent<ECS::InstancedStaticMeshComponent>().InstanceTransforms;
             },
             []( const Common::UUID& uuid ) { return Core::FoliagePaint::SelectionOf( uuid ); } );
        if ( !fields.empty() )
            CommandHistory::Get().PushCommand( std::make_unique<FoliageStrokeCommand>(
                 scene, std::move( fields ), std::string( "Foliage " ) + Core::FoliageToolName( m_StrokeTool ) ) );
        m_Stroke.reset();
    }

    void FoliagePaintTool::PickAlongRay( ::Desert::Core::Scene& scene, const Common::Math::Ray& ray, bool shift )
    {
        // UE SelectInstanceAtLocation over every checked type: the instance the ray enters first wins; a plain
        // click replaces the selection of the checked types, Shift adds to it.
        std::optional<std::pair<Common::UUID, uint32_t>> best;
        float                                            bestDistance = std::numeric_limits<float>::max();
        std::vector<ECS::Entity>                         candidates;
        for ( const auto& row : Core::FoliagePaint::ActiveTypes() )
            if ( const auto ref = scene.FindEntityByID( row ); ref && IsFoliageField( ref->get() ) )
                for ( const auto& field :
                      FieldsOfType( scene, ref->get().GetComponent<ECS::FoliageComponent>().FoliageType ) )
                    candidates.push_back( field );
        for ( const auto& candidate : candidates )
        {
            const auto uuid = candidate.GetComponent<ECS::UUIDComponent>().UUID;
            auto       ref  = scene.FindEntityByID( uuid );
            if ( !ref || !ref->get().HasComponent<ECS::InstancedStaticMeshComponent>() )
                continue;
            const auto& instances =
                 ref->get().GetComponent<ECS::InstancedStaticMeshComponent>().InstanceTransforms;
            m_Stroke->Touch( uuid, instances, Core::FoliagePaint::SelectionOf( uuid ) );
            if ( !shift )
                Core::FoliagePaint::Selection()[uuid].clear();
            // The instance's mesh box (Geometry::LocalBounds, the culler's extent), not a stand-in sphere.
            const auto  meshHandle = ref->get().GetComponent<ECS::InstancedStaticMeshComponent>().MeshHandle;
            const auto* mesh       = Runtime::ResourceRegistry::GetMeshService()->GetAsset( meshHandle );
            if ( !mesh )
            {
                if ( m_Refused.insert( uuid ).second )
                    ToastManager::Push(
                         "Foliage select: the field's mesh is not loaded, its instances cannot be picked",
                         ToastLevel::Error, 6.0f );
                continue;
            }
            const Common::Math::AABB box = Geometry::LocalBounds( mesh->GetSubmeshes() );
            const auto picked = FoliagePickInstance( instances, ray.Origin, ray.Direction, box.Min, box.Max );
            if ( picked && picked->Distance < bestDistance )
            {
                best         = std::make_pair( uuid, picked->Index );
                bestDistance = picked->Distance;
            }
        }
        if ( !best )
            return;
        auto& selected = Core::FoliagePaint::Selection()[best->first];
        if ( std::find( selected.begin(), selected.end(), best->second ) == selected.end() )
        {
            selected.push_back( best->second );
            std::sort( selected.begin(), selected.end() );
        }
    }

    Common::BoolResultStr FoliagePaintTool::EditSelection(
         ::Desert::Core::Scene& scene, const std::string& label,
         const std::function<void( std::vector<glm::mat4>&, FoliageSelection& )>& edit )
    {
        if ( Core::FoliagePaint::SelectedCount() == 0 )
            return Common::MakeError( "foliage " + label + ": no instance is selected" );
        FoliageStroke             stroke( 0u );
        std::vector<Common::UUID> edited;
        for ( auto& [uuid, selected] : Core::FoliagePaint::Selection() )
        {
            auto ref = scene.FindEntityByID( uuid );
            if ( selected.empty() || !ref || !ref->get().HasComponent<ECS::InstancedStaticMeshComponent>() )
                continue;
            auto& instances = ref->get().GetComponent<ECS::InstancedStaticMeshComponent>().InstanceTransforms;
            stroke.Touch( uuid, instances, selected );
            edit( instances, selected );
            edited.push_back( uuid );
        }
        // FO-6: an instance moved over a cell edge goes to the neighbour cell's field (UE
        // FoliagePartitioningUtils::Update after a transform). Every edited field's type is filed again.
        std::vector<Common::UUID>        rows;
        std::unordered_set<Common::UUID> types;
        for ( const auto& uuid : edited )
            if ( const auto ref = scene.FindEntityByID( uuid );
                 ref && IsFoliageField( ref->get() ) &&
                 types.insert( static_cast<const Common::UUID&>(
                                    ref->get().GetComponent<ECS::FoliageComponent>().FoliageType ) )
                      .second )
                rows.push_back( uuid );
        if ( auto filed = RepartitionRows( scene, rows, stroke ); !filed )
            return filed;
        auto fields = stroke.Finish(
             [&]( const Common::UUID& uuid ) -> const std::vector<glm::mat4>*
             {
                 const auto ref = scene.FindEntityByID( uuid );
                 if ( !ref || !ref->get().HasComponent<ECS::InstancedStaticMeshComponent>() )
                     return nullptr;
                 return &ref->get().GetComponent<ECS::InstancedStaticMeshComponent>().InstanceTransforms;
             },
             []( const Common::UUID& uuid ) { return Core::FoliagePaint::SelectionOf( uuid ); } );
        if ( !fields.empty() )
            CommandHistory::Get().PushCommand(
                 std::make_unique<FoliageStrokeCommand>( scene, std::move( fields ), "Foliage " + label ) );
        return BOOLSUCCESS;
    }

    Common::BoolResultStr
    FoliagePaintTool::ReplaceSelection( ::Desert::Core::Scene& scene, const std::string& label,
                                        const std::unordered_map<Common::UUID, FoliageSelection>& wanted )
    {
        FoliageStroke stroke( 0u );
        size_t        total = 0;
        for ( const auto& [uuid, selection] : wanted )
        {
            auto ref = scene.FindEntityByID( uuid );
            if ( !ref || !ref->get().HasComponent<ECS::InstancedStaticMeshComponent>() )
                continue;
            auto& selected = Core::FoliagePaint::Selection()[uuid];
            stroke.Touch( uuid, ref->get().GetComponent<ECS::InstancedStaticMeshComponent>().InstanceTransforms,
                          selected );
            selected = selection;
            total += selection.size();
        }
        if ( total == 0 )
            return Common::MakeError( "foliage " + label + ": there is no instance to select" );
        auto fields = stroke.Finish(
             [&]( const Common::UUID& uuid ) -> const std::vector<glm::mat4>*
             {
                 const auto ref = scene.FindEntityByID( uuid );
                 if ( !ref || !ref->get().HasComponent<ECS::InstancedStaticMeshComponent>() )
                     return nullptr;
                 return &ref->get().GetComponent<ECS::InstancedStaticMeshComponent>().InstanceTransforms;
             },
             []( const Common::UUID& uuid ) { return Core::FoliagePaint::SelectionOf( uuid ); } );
        if ( !fields.empty() )
            CommandHistory::Get().PushCommand(
                 std::make_unique<FoliageStrokeCommand>( scene, std::move( fields ), "Foliage " + label ) );
        return BOOLSUCCESS;
    }

    void FoliagePaintTool::DrawSelection( ::Desert::Core::Scene& scene, const glm::mat4& viewProjection,
                                          const glm::vec2& viewportPos, const glm::vec2& viewportSize )
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        for ( const auto& [uuid, selected] : Core::FoliagePaint::Selection() )
        {
            auto ref = scene.FindEntityByID( uuid );
            if ( selected.empty() || !ref || !ref->get().HasComponent<ECS::InstancedStaticMeshComponent>() )
                continue;
            const auto& instances =
                 ref->get().GetComponent<ECS::InstancedStaticMeshComponent>().InstanceTransforms;
            for ( const uint32_t i : selected )
            {
                if ( i >= instances.size() )
                    continue;
                const glm::vec4 clip = viewProjection * glm::vec4( glm::vec3( instances[i][3] ), 1.0f );
                if ( clip.w <= 0.0f )
                    continue; // behind the camera
                const float  x = ( clip.x / clip.w * 0.5f + 0.5f ) * viewportSize.x;
                const float  y = ( 0.5f - clip.y / clip.w * 0.5f ) * viewportSize.y;
                const ImVec2 at( viewportPos.x + x, viewportPos.y + y );
                dl->AddCircle( at, 6.0f, IM_COL32( 0, 0, 0, 200 ), 12, 3.5f );
                dl->AddCircle( at, 6.0f, IM_COL32( 255, 170, 30, 255 ), 12, 2.0f );
            }
        }
    }

    Common::BoolResultStr FoliagePaintTool::FillEntity( ::Desert::Core::Scene& scene,
                                                        Assets::AssetManager& manager, const Common::UUID& entity )
    {
        const auto target = scene.FindEntityByID( entity );
        if ( !target || !target->get().HasComponent<ECS::StaticMeshComponent>() )
            return Common::MakeError( "foliage fill: the entity has no static mesh to fill" );
        const auto mesh = GetToolTargetMesh( target->get().GetComponent<ECS::StaticMeshComponent>() );
        if ( !mesh )
            return Common::MakeError( "foliage fill: " + mesh.GetError() );
        if ( !Core::FoliagePaint::HasActive() )
            return Common::MakeError( "foliage fill: no foliage type is checked in the palette" );

        // The mesh's triangles in the world (UE FFoliagePaintBucketTriangle over LocalToWorld).
        const glm::mat4                  toWorld = target->get().GetWorldTransform();
        const Geometry::DynamicMesh3&    dm      = *mesh.GetValue().Mesh;
        std::vector<FoliageFillTriangle> triangles;
        triangles.reserve( static_cast<size_t>( dm.TriangleCount() ) );
        const auto world = [&]( int v )
        { return glm::vec3( toWorld * glm::vec4( glm::vec3( dm.GetVertex( v ) ), 1.0f ) ); };
        for ( const int t : dm.TriangleIndicesItr() )
        {
            const auto tri = dm.GetTriangle( t );
            triangles.push_back( { world( tri.A ), world( tri.B ), world( tri.C ), FoliageSurface::StaticMesh } );
        }

        FoliageSurfaceFilter filter;
        filter.Landscape  = Core::FoliagePaint::FilterLandscape();
        filter.StaticMesh = Core::FoliagePaint::FilterStaticMesh();
        FoliageStroke stroke( Core::FoliagePaint::NextStrokeSeed() );
        size_t        placed = 0;
        for ( const auto& uuid : Core::FoliagePaint::ActiveTypes() )
        {
            auto ref = scene.FindEntityByID( uuid );
            if ( !ref || !ref->get().HasComponent<ECS::FoliageComponent>() ||
                 !ref->get().HasComponent<ECS::InstancedStaticMeshComponent>() )
                continue;
            const auto type = ResolveType( manager, ref->get().GetComponent<ECS::FoliageComponent>().FoliageType );
            if ( !type )
                continue;
            auto& instances = ref->get().GetComponent<ECS::InstancedStaticMeshComponent>().InstanceTransforms;
            stroke.Touch( uuid, instances, Core::FoliagePaint::SelectionOf( uuid ) );
            const auto added = FoliageFill( type->GetData(), triangles, Core::FoliagePaint::PaintDensity(), filter,
                                            stroke.Random() );
            instances.insert( instances.end(), added.begin(), added.end() );
            placed += added.size();
            LOG_INFO( "[Foliage] fill '{}': {} placed on {} triangles; field now {}", type->GetDisplayName(),
                      added.size(), triangles.size(), instances.size() );
        }
        if ( auto filed = RepartitionRows( scene, Core::FoliagePaint::ActiveTypes(), stroke ); !filed )
            return filed;
        auto fields = stroke.Finish(
             [&]( const Common::UUID& uuid ) -> const std::vector<glm::mat4>*
             {
                 const auto ref = scene.FindEntityByID( uuid );
                 if ( !ref || !ref->get().HasComponent<ECS::InstancedStaticMeshComponent>() )
                     return nullptr;
                 return &ref->get().GetComponent<ECS::InstancedStaticMeshComponent>().InstanceTransforms;
             },
             []( const Common::UUID& uuid ) { return Core::FoliagePaint::SelectionOf( uuid ); } );
        if ( !fields.empty() )
            CommandHistory::Get().PushCommand(
                 std::make_unique<FoliageStrokeCommand>( scene, std::move( fields ), "Foliage Fill" ) );
        if ( placed == 0 )
            return Common::MakeFormattedError(
                 "foliage fill: nothing placed on {} triangles (density, slope, height "
                 "or the static-mesh filter refused all of them)",
                 triangles.size() );
        return BOOLSUCCESS;
    }

    Common::BoolResultStr FoliagePaintTool::RemoveSelected( ::Desert::Core::Scene& scene )
    {
        return EditSelection( scene, "Remove Selected",
                              []( std::vector<glm::mat4>& instances, FoliageSelection& selected )
                              { FoliageRemoveSelected( instances, selected ); } );
    }

    Common::BoolResultStr FoliagePaintTool::MoveSelected( ::Desert::Core::Scene& scene, const glm::vec3& offset )
    {
        return EditSelection( scene, "Move Selected",
                              [&]( std::vector<glm::mat4>& instances, FoliageSelection& selected )
                              { FoliageMoveSelected( instances, selected, offset ); } );
    }

    Common::BoolResultStr FoliagePaintTool::SelectNone( ::Desert::Core::Scene& scene )
    {
        return EditSelection( scene, "Select None",
                              []( std::vector<glm::mat4>&, FoliageSelection& selected ) { selected.clear(); } );
    }

    // ----- the palette's actions (FO-UI1) --------------------------------------------------------------------

    std::vector<ECS::Entity> FoliagePaintTool::PaletteFields( ::Desert::Core::Scene& scene )
    {
        // One row per TYPE (UE's palette lists types, not the IFAs holding them): the first field of each type in
        // scene order stands for every field of it — a partitioned world has one per cell (FO-6).
        std::vector<ECS::Entity>         fields;
        std::unordered_set<Common::UUID> listed; // a type, by its handle's id
        for ( const auto& entity : scene.GetAllEntities() )
            if ( IsFoliageField( entity ) && listed
                                                  .insert( static_cast<const Common::UUID&>(
                                                       entity.GetComponent<ECS::FoliageComponent>().FoliageType ) )
                                                  .second )
                fields.push_back( entity );
        return fields;
    }

    namespace
    {
        // The palette field @p uuid names, or an error naming the action.
        Common::ResultStr<ECS::Entity> FieldOf( ::Desert::Core::Scene& scene, const Common::UUID& uuid,
                                                const char* action )
        {
            const auto ref = scene.FindEntityByID( uuid );
            if ( !ref || !ref->get().HasComponent<ECS::FoliageComponent>() ||
                 !ref->get().HasComponent<ECS::InstancedStaticMeshComponent>() )
                return Common::MakeFormattedError<ECS::Entity>(
                     "foliage {}: {} is not a foliage type in the palette", action,
                     static_cast<uint64_t>( uuid ) );
            return Common::MakeSuccess( ref->get() );
        }

        std::string TypePathOf( const ECS::Entity& field, const Assets::AssetManager& manager )
        {
            const auto handle = field.GetComponent<ECS::FoliageComponent>().FoliageType;
            const auto type   = manager.FindByHandle<Assets::FoliageTypeAsset>( handle );
            return type ? type->GetMetadata().Filepath.string() : std::string();
        }
    } // namespace

    Common::BoolResultStr FoliagePaintTool::AddFromEntity( ::Desert::Core::Scene& scene,
                                                           Assets::AssetManager&  manager,
                                                           const Common::UUID&    entity )
    {
        const auto ref = scene.FindEntityByID( entity );
        if ( !ref )
            return Common::MakeError( "foliage from selection: the selected entity is not in the scene" );
        const auto& e = ref->get();
        if ( e.HasComponent<ECS::FoliageComponent>() )
        {
            const std::string path = TypePathOf( e, manager );
            if ( path.empty() )
                return Common::MakeError(
                     "foliage from selection: the selected foliage field names no loaded type" );
            return AddTypeFile( scene, manager, path );
        }
        Assets::AssetHandle mesh;
        if ( e.HasComponent<ECS::StaticMeshComponent>() )
            mesh = e.GetComponent<ECS::StaticMeshComponent>().MeshHandle;
        else if ( e.HasComponent<ECS::InstancedStaticMeshComponent>() )
            mesh = e.GetComponent<ECS::InstancedStaticMeshComponent>().MeshHandle;
        if ( !mesh )
            return Common::MakeError(
                 "foliage from selection: the selected entity draws no mesh asset (a primitive or no mesh)" );
        const auto asset = manager.FindByHandle<Assets::MeshAsset>( mesh );
        if ( !asset )
            return Common::MakeFormattedError<bool>(
                 "foliage from selection: mesh {} is not known to the asset manager",
                 static_cast<uint64_t>( mesh ) );
        const auto ref2 = MeshRefOfAsset( asset, asset->GetMetadata().Filepath.string() );
        if ( !ref2 )
            return Common::MakeFormattedError<bool>( "foliage from selection: {}", ref2.GetError() );
        return AddMeshRef( scene, manager, ref2.GetValue(), asset->GetMetadata().Filepath.stem().string() );
    }

    Common::BoolResultStr FoliagePaintTool::SelectAllInstances( ::Desert::Core::Scene& scene )
    {
        if ( !Core::FoliagePaint::HasActive() )
            return Common::MakeError( "foliage select all: no foliage type is checked in the palette" );
        std::unordered_map<Common::UUID, FoliageSelection> wanted;
        for ( const auto& uuid : Core::FoliagePaint::ActiveTypes() )
            if ( const auto row = FieldOf( scene, uuid, "select all" ) )
                SelectEveryInstance( scene, row.GetValue(), wanted );
        return ReplaceSelection( scene, "Select All", wanted );
    }

    Common::BoolResultStr FoliagePaintTool::SelectTypeInstances( ::Desert::Core::Scene& scene,
                                                                 const Common::UUID&    field )
    {
        const auto e = FieldOf( scene, field, "select instances" );
        if ( !e )
            return Common::MakeFormattedError<bool>( "{}", e.GetError() );
        std::unordered_map<Common::UUID, FoliageSelection> wanted;
        SelectEveryInstance( scene, e.GetValue(), wanted );
        return ReplaceSelection( scene, "Select Instances", wanted );
    }

    Common::BoolResultStr FoliagePaintTool::RemoveType( ::Desert::Core::Scene& scene, const Common::UUID& field )
    {
        auto e = FieldOf( scene, field, "remove type" );
        if ( !e )
            return Common::MakeFormattedError<bool>( "{}", e.GetError() );
        if ( Core::FoliagePaint::EditingType() == field )
            Core::FoliagePaint::ClearEditingType();
        if ( Core::FoliagePaint::IsActive( field ) )
            Core::FoliagePaint::ToggleActive( field );
        for ( auto& entity :
              FieldsOfType( scene, e.GetValue().GetComponent<ECS::FoliageComponent>().FoliageType ) )
        {
            Core::FoliagePaint::Selection().erase( entity.GetComponent<ECS::UUIDComponent>().UUID );
            scene.DestroyEntity( entity );
        }
        return BOOLSUCCESS;
    }

    Common::BoolResultStr FoliagePaintTool::ReplaceType( ::Desert::Core::Scene& scene,
                                                         Assets::AssetManager& manager, const Common::UUID& field,
                                                         const std::string& typePath )
    {
        auto e = FieldOf( scene, field, "replace type" );
        if ( !e )
            return Common::MakeFormattedError<bool>( "{}", e.GetError() );
        const auto type = OpenTypeFile( manager, typePath );
        if ( !type )
            return Common::MakeFormattedError<bool>( "foliage replace type: '{}' does not load (the log says why)",
                                                     typePath );
        auto& source = e.GetValue();
        if ( source.GetComponent<ECS::FoliageComponent>().FoliageType == type->GetMetadata().Handle )
            return Common::MakeFormattedError<bool>( "foliage replace type: the field already paints '{}'",
                                                     typePath );
        if ( const auto mesh = type->GetMeshHandle() )
            if ( auto asset = manager.FindByHandle<Assets::MeshAsset>( mesh ) )
                Runtime::EnsureMeshRegistered( asset, manager );

        // UE ReplaceFoliageTypeObject: every field of the old type paints the new one; where a field of the new
        // type already holds the cell, the instances merge into it (UE merges into the existing FoliageInfo) and
        // the emptied field goes.
        const auto newType   = type->GetMetadata().Handle;
        const auto survivors = FieldsOfType( scene, newType );
        for ( auto& moved : FieldsOfType( scene, source.GetComponent<ECS::FoliageComponent>().FoliageType ) )
        {
            moved.GetComponent<ECS::FoliageComponent>().FoliageType            = newType;
            moved.GetComponent<ECS::InstancedStaticMeshComponent>().MeshHandle = type->GetMeshHandle();
            moved.GetComponent<ECS::TagComponent>().Tag = "Foliage_" + type->GetDisplayName();
        }
        FoliageStroke stroke( 0u );
        const auto    row = survivors.empty() ? field : survivors.front().GetComponent<ECS::UUIDComponent>().UUID;
        if ( auto filed = RepartitionRows( scene, { row }, stroke ); !filed )
            return filed;
        auto fields = FieldsOfType( scene, newType );
        for ( std::size_t i = 1; i < fields.size(); ++i )
            if ( fields[i].GetComponent<ECS::InstancedStaticMeshComponent>().InstanceTransforms.empty() )
            {
                const auto uuid = fields[i].GetComponent<ECS::UUIDComponent>().UUID;
                if ( Core::FoliagePaint::IsActive( uuid ) )
                    Core::FoliagePaint::ToggleActive( uuid );
                Core::FoliagePaint::Selection().erase( uuid );
                scene.DestroyEntity( fields[i] );
            }
        Core::FoliagePaint::SetEditingType( fields.front().GetComponent<ECS::UUIDComponent>().UUID );
        return BOOLSUCCESS;
    }

    Common::BoolResultStr FoliagePaintTool::SaveTypeCopy( ::Desert::Core::Scene& scene,
                                                          Assets::AssetManager&  manager,
                                                          const Common::UUID&    field )
    {
        auto e = FieldOf( scene, field, "save as asset" );
        if ( !e )
            return Common::MakeFormattedError<bool>( "{}", e.GetError() );
        const auto type = ResolveType( manager, e.GetValue().GetComponent<ECS::FoliageComponent>().FoliageType );
        if ( !type )
            return Common::MakeError( "foliage save as asset: the field's type does not load (the log says why)" );
        const auto copy = Foliage::SaveFoliageTypeCopy( type->GetMetadata().Filepath, type->GetData() );
        if ( !copy )
            return Common::MakeFormattedError<bool>( "foliage save as asset: {}", copy.GetError() );
        return ReplaceType( scene, manager, field, copy.GetValue().string() );
    }

    Common::BoolResultStr FoliagePaintTool::ShowTypeInBrowser( ::Desert::Core::Scene& scene,
                                                               const Common::UUID&    field )
    {
        const auto e = FieldOf( scene, field, "show in browser" );
        if ( !e )
            return Common::MakeFormattedError<bool>( "{}", e.GetError() );
        const auto handle = e.GetValue().GetComponent<ECS::FoliageComponent>().FoliageType;
        if ( !handle )
            return Common::MakeError( "foliage show in browser: the field names no type" );
        Core::AssetFieldRequests::Request( handle, Core::AssetFieldAction::ShowInBrowser );
        return BOOLSUCCESS;
    }

    Common::BoolResultStr FoliagePaintTool::ToggleTypeVisible( ::Desert::Core::Scene& scene,
                                                               const Common::UUID&    field )
    {
        auto e = FieldOf( scene, field, "visibility" );
        if ( !e )
            return Common::MakeFormattedError<bool>( "{}", e.GetError() );
        auto&      entity  = e.GetValue();
        const bool visible = !entity.HasComponent<ECS::VisibilityComponent>() ||
                             entity.GetComponent<ECS::VisibilityComponent>().Visible;
        for ( auto& each : FieldsOfType( scene, entity.GetComponent<ECS::FoliageComponent>().FoliageType ) )
            scene.SetVisibleRecursive( each, !visible );
        return BOOLSUCCESS;
    }

    Common::BoolResultStr FoliagePaintTool::SavePreset( ::Desert::Core::Scene& scene,
                                                        Assets::AssetManager& manager, const std::string& name )
    {
        std::vector<Foliage::PalettePresetEntry> entries;
        for ( const auto& field : PaletteFields( scene ) )
        {
            const auto type = ResolveType( manager, field.GetComponent<ECS::FoliageComponent>().FoliageType );
            if ( !type || !type->GetData().Header )
                return Common::MakeFormattedError<bool>( "foliage preset '{}': the type of '{}' does not load",
                                                         name, field.GetComponent<ECS::TagComponent>().Tag );
            std::error_code   ec;
            const std::string typePath =
                 std::filesystem::absolute( type->GetMetadata().Filepath, ec )
                      .lexically_normal()
                      .lexically_relative( std::filesystem::absolute( Common::Constants::Path::ASSETS_PATH, ec )
                                                .lexically_normal() )
                      .generic_string();
            entries.push_back(
                 { type->GetDisplayName(),
                   ( Common::Constants::Path::ASSETS_PATH / type->GetData().Mesh.Path ).generic_string(),
                   Assets::AssetGuidRef{ type->GetData().Header->Guid, typePath } } );
        }
        const auto saved = Foliage::SavePalettePreset( Common::Constants::Path::COLLECTIONS_PATH, name, entries );
        if ( !saved )
            return Common::MakeFormattedError<bool>( "{}", saved.GetError() );
        ToastManager::Push( "Foliage preset saved: " + saved.GetValue().generic_string(), ToastLevel::Info, 4.0f );
        return BOOLSUCCESS;
    }
} // namespace Desert::Editor::Tools
