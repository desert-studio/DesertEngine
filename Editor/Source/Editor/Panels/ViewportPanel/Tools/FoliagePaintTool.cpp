#include "FoliagePaintTool.hpp"

#include <Editor/Core/Selection/FoliagePaint.hpp>
#include <Editor/Core/CommandHistory.hpp>
#include <Editor/Core/ToastManager.hpp>
#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/DragPayloads.hpp>
#include <Editor/Import/MeshDnD.hpp>
#include <Editor/Panels/Collections/CollectionFoliageTypes.hpp>

#include <Engine/Assets/Mesh/MeshAsset.hpp>
#include <Engine/Assets/RegistryDiscovery.hpp>
#include <Engine/Runtime/Services/AssetServiceRegistration.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/ECS/LandscapeRootOf.hpp>
#include <Engine/World/Landscape/LandscapeLayout.hpp>
#include <Engine/Assets/Serialization/LandscapeLayerInfo.hpp>
#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <ImGui/imgui.h>

#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <unordered_set>
#include <vector>

namespace Desert::Editor::Tools
{
    namespace ImGui = ::ImGui; // engine headers introduce a Desert::ImGui that would otherwise shadow ::ImGui

    namespace
    {
        // A toggle-style button that looks pressed when `active` (for the Paint/Erase tool tabs).
        bool ToolTabButton( const char* label, bool active, float width )
        {
            if ( active )
                ImGui::PushStyleColor( ImGuiCol_Button, ImGui::GetStyleColorVec4( ImGuiCol_ButtonActive ) );
            const bool pressed = ImGui::Button( label, ImVec2( width, 0.0f ) );
            if ( active )
                ImGui::PopStyleColor();
            return pressed;
        }

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

        // The cooked mesh a source path names, as a `.defoliage` names it; cooks the source on first use.
        Common::ResultStr<Assets::AssetGuidRef> MeshRefOf( Assets::AssetManager& manager,
                                                           const std::string&    meshSourcePath )
        {
            const auto handle = MeshDnD::ResolveOrImport( manager, meshSourcePath );
            if ( !handle )
                return Common::MakeFormattedError<Assets::AssetGuidRef>(
                     "mesh '{}' did not cook to a static mesh (a skinned source or an import failure)",
                     meshSourcePath );
            const auto mesh = manager.FindByHandle<Assets::MeshAsset>( handle );
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

    void FoliagePaintTool::CreateTypeFromMesh( ::Desert::Core::Scene& scene, Assets::AssetManager& manager,
                                               const std::string& meshSourcePath )
    {
        const auto mesh = MeshRefOf( manager, meshSourcePath );
        if ( !mesh )
        {
            LOG_ERROR( "[Foliage] {}", mesh.GetError() );
            return;
        }
        Assets::Serialization::FoliageTypeData wanted;
        wanted.Mesh     = mesh.GetValue();
        const auto file = Assets::Serialization::FindOrCreateFoliageTypeFile(
             Common::Constants::Path::FOLIAGE_TYPE_PATH, wanted,
             std::filesystem::path( meshSourcePath ).stem().string() );
        if ( !file )
        {
            LOG_ERROR( "[Foliage] {}", file.GetError() );
            return;
        }
        if ( auto type = OpenTypeFile( manager, file.GetValue().Path.string() ) )
            AddField( scene, manager, type );
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
        for ( const auto& file : types.GetValue().Types )
        {
            auto type = OpenTypeFile( manager, file.Path.string() );
            if ( !type )
                return Common::MakeFormattedError<bool>( "foliage type '{}' of collection '{}' did not load",
                                                         file.Path.string(), manifestPath );
            AddField( scene, manager, type );
        }
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

        ImGui::TextDisabled( "%s", type->GetMetadata().Filepath.filename().string().c_str() );
        ImGui::SetNextItemWidth( -1 );
        ImGui::SliderFloat( "##Density", &f.Density, 1.0f, 1000.0f, "Density:  %.0f / 1000x1000 cm",
                            ImGuiSliderFlags_Logarithmic );
        track();
        ImGui::SetNextItemWidth( -1 );
        ImGui::DragFloatRange2( "##Scale", &f.ScaleX.Min, &f.ScaleX.Max, 0.01f, 0.02f, 10.0f, "Scale %.2f",
                                "%.2f" );
        track();
        ImGui::SetNextItemWidth( -1 );
        ImGui::DragFloatRange2( "##ZOffset", &f.ZOffset.Min, &f.ZOffset.Max, 0.5f, -500.0f, 500.0f, "Z Off %.0f",
                                "%.0f cm" );
        track();
        ImGui::SetNextItemWidth( -1 );
        ImGui::SliderFloat( "##Pitch", &f.RandomPitchAngle, 0.0f, 90.0f, "Random Pitch:  %.0f deg" );
        track();
        ImGui::SetNextItemWidth( -1 );
        ImGui::DragFloatRange2( "##Slope", &f.GroundSlopeAngle.Min, &f.GroundSlopeAngle.Max, 0.5f, 0.0f, 90.0f,
                                "Slope %.0f", "%.0f deg" );
        track();
        ImGui::SetNextItemWidth( -1 );
        ImGui::DragFloatRange2( "##Height", &f.Height.Min, &f.Height.Max, 10.0f, -262144.0f, 262144.0f,
                                "Height %.0f", "%.0f cm" );
        track();
        // UE CullDistance: fade from Min, gone from Max; Max 0 = never culled.
        ImGui::SetNextItemWidth( -1 );
        ImGui::DragFloatRange2( "##CullDistance", &f.CullDistance.Min, &f.CullDistance.Max, 50.0f, 0.0f,
                                1000000.0f, "Cull %.0f", "%.0f cm" );
        track();

        // UE LandscapeLayers: a `.delayerinfo` dropped here restricts the type to ground painted with it.
        ImGui::TextDisabled( "LANDSCAPE LAYERS%s", f.LandscapeLayers.empty() ? " (any)" : "" );
        for ( size_t i = 0; i < f.LandscapeLayers.size(); ++i )
        {
            ImGui::PushID( static_cast<int>( i ) );
            if ( ImGui::SmallButton( ICON_MDI_CLOSE ) )
            {
                f.LandscapeLayers.erase( f.LandscapeLayers.begin() + static_cast<std::ptrdiff_t>( i ) );
                commit = true;
                ImGui::PopID();
                break;
            }
            ImGui::SameLine();
            ImGui::TextUnformatted( f.LandscapeLayers[i].Path.c_str() );
            ImGui::PopID();
        }
        ImGui::Button( ICON_MDI_PLUS " Drop a .delayerinfo", ImVec2( -1, 0 ) );
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
        if ( !f.LandscapeLayers.empty() )
        {
            ImGui::SetNextItemWidth( -1 );
            ImGui::SliderFloat( "##MinLayerWeight", &f.MinimumLayerWeight, 0.0f, 1.0f,
                                "Minimum Layer Weight:  %.2f" );
            track();
        }
        commit = ImGui::Checkbox( "Align to Normal", &f.AlignToNormal ) || commit;
        ImGui::SameLine();
        commit    = ImGui::Checkbox( "Random Yaw", &f.RandomYaw ) || commit;
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

        // The tile whose frame covers world (x, z), for the layer weight under an existing instance.
        std::optional<TileSample> TileAt( ::Desert::Core::Scene& scene, float x, float z )
        {
            for ( const auto& entity : scene.GetAllEntities() )
            {
                if ( !entity.HasComponent<ECS::LandscapeTileComponent>() )
                    continue;
                auto sample = TileOf( scene, entity.GetComponent<ECS::UUIDComponent>().UUID );
                if ( !sample )
                    continue;
                const float spanX = sample->Frame.SpacingCm * static_cast<float>( sample->Tile->SamplesX() - 1u );
                const float spanZ = sample->Frame.SpacingCm * static_cast<float>( sample->Tile->SamplesZ() - 1u );
                if ( x >= sample->Frame.OriginX && x <= sample->Frame.OriginX + spanX &&
                     z >= sample->Frame.OriginZ && z <= sample->Frame.OriginZ + spanZ )
                    return sample;
            }
            return std::nullopt;
        }

        /// One press of the foliage brush, undone and redone as a whole (UE: one FScopedTransaction per
        /// stroke, "Foliage Paint" / "Foliage Erase").
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
                }
                return all;
            }

            ::Desert::Core::Scene*          m_Scene;
            std::vector<FoliageStrokeField> m_Fields;
            std::string                     m_Label;
        };
    } // namespace

    void FoliagePaintTool::Update( ::Desert::Core::Scene& scene, const Assets::AssetManager* assetManager,
                                   const Common::Math::Ray& ray, bool pressing )
    {
        if ( !pressing || !Core::FoliagePaint::HasActive() || !assetManager )
        {
            EndStroke( scene );
            return;
        }
        if ( !m_Stroke )
        {
            m_Stroke.emplace( Core::FoliagePaint::NextStrokeSeed(), Core::FoliagePaint::Erase() );
            m_Refused.clear();
        }
        auto& manager = const_cast<Assets::AssetManager&>( *assetManager );

        ::Desert::Core::RaycastHit centre;
        if ( !scene.Raycast( ray, centre ) )
            return; // UE: no surface under the cursor, no application this tick

        FoliageBrushDab dab;
        dab.Center            = centre.Point;
        dab.Normal            = centre.Normal;
        dab.Radius            = Core::FoliagePaint::BrushRadius();
        dab.PaintDensity      = Core::FoliagePaint::PaintDensity();
        dab.Filter.Landscape  = Core::FoliagePaint::FilterLandscape();
        dab.Filter.StaticMesh = Core::FoliagePaint::FilterStaticMesh();

        for ( const auto& uuid : Core::FoliagePaint::ActiveTypes() )
        {
            auto ref = scene.FindEntityByID( uuid );
            if ( !ref || !ref->get().HasComponent<ECS::FoliageComponent>() ||
                 !ref->get().HasComponent<ECS::InstancedStaticMeshComponent>() )
                continue;
            auto& e   = ref->get();
            auto& ism = e.GetComponent<ECS::InstancedStaticMeshComponent>();
            m_Stroke->Touch( uuid, ism.InstanceTransforms );

            if ( m_Stroke->Erase() )
            {
                FoliageBrushErase( ism.InstanceTransforms, dab.Center, dab.Radius );
                continue;
            }

            const auto type = ResolveType( manager, e.GetComponent<ECS::FoliageComponent>().FoliageType );
            if ( !type )
                continue;
            const auto& data   = type->GetData();
            const auto  layers = LayerNamesOf( data );
            if ( !layers )
            {
                if ( m_Refused.insert( uuid ).second )
                    ToastManager::Push( "Foliage '" + type->GetDisplayName() + "': " + layers.GetError(),
                                        ToastLevel::Error, 6.0f );
                continue;
            }
            const std::vector<std::string>& names = layers.GetValue();

            FoliageBrushWorld world;
            // UE FFoliagePaintingGeometryFilter: a surface the brush may not paint on is traced THROUGH, so a
            // landscape-only brush reaches the ground under a mesh instead of losing the spot.
            world.Trace = [&]( const glm::vec3& start, const glm::vec3& end,
                               const FoliageSurfaceFilter& filter ) -> std::optional<FoliageTraceHit>
            {
                // UE FFoliagePaintingGeometryFilter: a surface the brush may not paint on is traced THROUGH, so
                // a landscape-only brush reaches the ground under a mesh instead of losing the spot.
                const auto accept = [&]( const Common::UUID& id )
                {
                    const bool landscape = TileOf( scene, id ).has_value();
                    return filter.Allows( landscape ? FoliageSurface::Landscape : FoliageSurface::StaticMesh );
                };
                const glm::vec3 d   = end - start;
                const float     len = glm::length( d );
                if ( len <= 0.0f )
                    return std::nullopt;
                ::Desert::Core::RaycastHit hit;
                if ( !scene.Raycast( Common::Math::Ray( start, d / len ), hit, accept ) || hit.Distance > len )
                    return std::nullopt;
                FoliageTraceHit out;
                out.Point  = hit.Point;
                out.Normal = hit.Normal;
                if ( const auto tile = TileOf( scene, hit.Entity ) )
                {
                    out.Surface = FoliageSurface::Landscape;
                    if ( !names.empty() )
                        out.LayerWeight =
                             MaxLayerWeight( *tile->Tile, tile->Frame, names, hit.Point.x, hit.Point.z );
                }
                else
                    out.Surface = FoliageSurface::StaticMesh;
                return out;
            };
            world.LayerWeightAt = [&]( const glm::vec3& p ) -> std::optional<float>
            {
                const auto tile = TileAt( scene, p.x, p.z );
                if ( !tile )
                    return std::nullopt;
                return MaxLayerWeight( *tile->Tile, tile->Frame, names, p.x, p.z );
            };

            auto added = FoliageBrushAdd( data, dab, ism.InstanceTransforms, m_Stroke->Random(), world );
            ism.InstanceTransforms.insert( ism.InstanceTransforms.end(), added.begin(), added.end() );
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
             } );
        if ( !fields.empty() )
            CommandHistory::Get().PushCommand( std::make_unique<FoliageStrokeCommand>(
                 scene, std::move( fields ), m_Stroke->Erase() ? "Foliage Erase" : "Foliage Paint" ) );
        m_Stroke.reset();
    }

    void FoliagePaintTool::DrawPanel( ::Desert::Core::Scene& scene, const Assets::AssetManager* assetManager,
                                      const glm::vec2& viewportPos )
    {
        ImGui::SetNextWindowPos( ImVec2( viewportPos.x + 12.0f, viewportPos.y + 58.0f ), ImGuiCond_Always );
        ImGui::SetNextWindowSize( ImVec2( 300.0f, 0.0f ), ImGuiCond_Always );
        ImGui::SetNextWindowBgAlpha( 0.93f );
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
        if ( !ImGui::Begin( "Foliage##FoliagePanel", nullptr, flags ) )
        {
            ImGui::End();
            return;
        }

        // ----- Tool: Paint / Erase --------------------------------------------------------------------
        bool&       erase = Core::FoliagePaint::Erase();
        const float halfW = ( ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x ) * 0.5f;
        if ( ToolTabButton( ICON_MDI_BRUSH " Paint", !erase, halfW ) )
            erase = false;
        ImGui::SameLine();
        if ( ToolTabButton( ICON_MDI_ERASER " Erase", erase, halfW ) )
            erase = true;

        // ----- Brush ----------------------------------------------------------------------------------
        ImGui::Dummy( ImVec2( 0, 2 ) );
        ImGui::TextDisabled( "BRUSH" );
        ImGui::SetNextItemWidth( -1 );
        ImGui::SliderFloat( "##BrushSize", &Core::FoliagePaint::BrushRadius(), 50.0f, 6000.0f, "Size:  %.0f cm" );
        ImGui::SetNextItemWidth( -1 );
        ImGui::SliderFloat( "##PaintDensity", &Core::FoliagePaint::PaintDensity(), 0.0f, 1.0f, "Density:  %.2f" );
        ImGui::TextDisabled( "FILTERS" );
        ImGui::Checkbox( "Landscape", &Core::FoliagePaint::FilterLandscape() );
        ImGui::SameLine();
        ImGui::Checkbox( "Static Meshes", &Core::FoliagePaint::FilterStaticMesh() );

        // ----- Foliage types --------------------------------------------------------------------------
        ImGui::Dummy( ImVec2( 0, 2 ) );
        ImGui::TextDisabled( "FOLIAGE TYPES" );

        ImGui::Button( ICON_MDI_PLUS " Add Foliage Type (mesh, collection, .defoliage)", ImVec2( -1, 0 ) );
        if ( assetManager && ImGui::BeginDragDropTarget() )
        {
            auto& manager = const_cast<Assets::AssetManager&>( *assetManager );
            if ( const ImGuiPayload* p = ImGui::AcceptDragDropPayload( ::Desert::Editor::DragPayloads::MeshAsset ) )
                CreateTypeFromMesh( scene, manager, std::string( static_cast<const char*>( p->Data ) ) );
            if ( const ImGuiPayload* p =
                      ImGui::AcceptDragDropPayload( ::Desert::Editor::DragPayloads::Collection ) )
                if ( auto added =
                          AddCollection( scene, manager, std::string( static_cast<const char*>( p->Data ) ) );
                     !added )
                    LOG_ERROR( "[Foliage] {}", added.GetError() );
            if ( const ImGuiPayload* p =
                      ImGui::AcceptDragDropPayload( ::Desert::Editor::DragPayloads::AssetFile ) )
            {
                const std::string path( static_cast<const char*>( p->Data ) );
                if ( std::filesystem::path( path ).extension() == Assets::Serialization::kFoliageTypeExtension )
                    if ( auto type = OpenTypeFile( manager, path ) )
                        AddField( scene, manager, type );
            }
            ImGui::EndDragDropTarget();
        }

        // Collect foliage-type entities once.
        std::vector<ECS::Entity> types;
        for ( const auto& entity : scene.GetAllEntities() )
            if ( entity.HasComponent<ECS::FoliageComponent>() &&
                 entity.HasComponent<ECS::InstancedStaticMeshComponent>() )
                types.push_back( entity );

        if ( types.empty() )
        {
            ImGui::TextDisabled( "Drag a mesh, a collection or a .defoliage here." );
        }
        else
        {
            if ( ImGui::SmallButton( "All" ) )
                for ( auto& t : types )
                    if ( !Core::FoliagePaint::IsActive( t.GetComponent<ECS::UUIDComponent>().UUID ) )
                        Core::FoliagePaint::ToggleActive( t.GetComponent<ECS::UUIDComponent>().UUID );
            ImGui::SameLine();
            if ( ImGui::SmallButton( "None" ) )
                Core::FoliagePaint::ClearActive();
            ImGui::SameLine();
            ImGui::TextDisabled( "(check = paint)" );

            const auto editing = Core::FoliagePaint::EditingType();
            ImGui::BeginChild( "##foltypes", ImVec2( -1, 132 ), true );
            int id = 0;
            for ( auto& t : types )
            {
                const auto  uuid = t.GetComponent<ECS::UUIDComponent>().UUID;
                const auto& ism  = t.GetComponent<ECS::InstancedStaticMeshComponent>();
                std::string name = t.HasComponent<ECS::TagComponent>()
                                       ? t.GetComponent<ECS::TagComponent>().Tag
                                       : ( "Foliage " + std::to_string( id ) );

                ImGui::PushID( id++ );
                bool active = Core::FoliagePaint::IsActive( uuid );
                if ( ImGui::Checkbox( "##chk", &active ) )
                    Core::FoliagePaint::ToggleActive( uuid );
                ImGui::SameLine();
                const std::string label = name + "   (" + std::to_string( ism.InstanceTransforms.size() ) + ")";
                // `optional == value` and not `has_value() && *opt == value`: the standard comparison is
                // false for an empty optional by definition, so the two are the same question with one
                // fewer dereference to keep in step with its guard.
                const bool editingThis = editing == uuid;
                if ( ImGui::Selectable( label.c_str(), editingThis ) )
                {
                    Core::FoliagePaint::SetEditingType( uuid );
                }
                ImGui::PopID();
            }
            ImGui::EndChild();
        }

        // ----- Selected type settings -----------------------------------------------------------------
        const auto editing = Core::FoliagePaint::EditingType();
        if ( editing )
        {
            if ( auto ref = scene.FindEntityByID( *editing );
                 ref && ref->get().HasComponent<ECS::FoliageComponent>() )
            {
                ImGui::Dummy( ImVec2( 0, 2 ) );
                ImGui::TextDisabled( "TYPE SETTINGS" );
                auto type = assetManager
                                 ? ResolveType( const_cast<Assets::AssetManager&>( *assetManager ),
                                                ref->get().GetComponent<ECS::FoliageComponent>().FoliageType )
                                 : nullptr;
                if ( type )
                    DrawTypeSettings( const_cast<Assets::AssetManager&>( *assetManager ), type );
                else
                    ImGui::TextDisabled( "No foliage type (drop a .defoliage on the entity's Details)." );

                ImGui::Dummy( ImVec2( 0, 2 ) );
                if ( ImGui::Button( ICON_MDI_DELETE " Remove Type", ImVec2( -1, 0 ) ) )
                {
                    Core::FoliagePaint::ClearEditingType();
                    auto e = ref->get();
                    if ( Core::FoliagePaint::IsActive( *editing ) )
                        Core::FoliagePaint::ToggleActive( *editing );
                    scene.DestroyEntity( e );
                }
            }
        }

        ImGui::Separator();
        ImGui::TextDisabled( "LMB drag: %s  -  check types to include", erase ? "erase" : "paint" );
        ImGui::End();
    }
} // namespace Desert::Editor::Tools
