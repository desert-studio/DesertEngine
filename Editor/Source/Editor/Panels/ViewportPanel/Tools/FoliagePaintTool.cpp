#include "FoliagePaintTool.hpp"

#include <Editor/Core/Selection/FoliagePaint.hpp>
#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/DragPayloads.hpp>
#include <Editor/Import/MeshDnD.hpp>
#include <Editor/Panels/Collections/CollectionFoliageTypes.hpp>

#include <Engine/Assets/Mesh/MeshAsset.hpp>
#include <Engine/Assets/RegistryDiscovery.hpp>
#include <Engine/Runtime/Services/AssetServiceRegistration.hpp>
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
#include <random>
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

        // Rotation that maps the up axis to a surface normal (for align-to-normal placement).
        glm::quat AlignUpToNormal( const glm::vec3& n )
        {
            const glm::vec3 up( 0.0f, 1.0f, 0.0f );
            const float     d = glm::clamp( glm::dot( up, n ), -1.0f, 1.0f );
            if ( d > 0.9999f )
                return glm::quat( 1.0f, 0.0f, 0.0f, 0.0f );
            if ( d < -0.9999f )
                return glm::angleAxis( 3.14159265f, glm::vec3( 1.0f, 0.0f, 0.0f ) );
            return glm::angleAxis( std::acos( d ), glm::normalize( glm::cross( up, n ) ) );
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
        ImGui::SliderFloat( "##Density", &f.Density, 1.0f, 80.0f, "Density:  %.0f / dab" );
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

    void FoliagePaintTool::Paint( ::Desert::Core::Scene& scene, const Assets::AssetManager* assetManager,
                                  const Common::Math::Ray& ray )
    {
        if ( !Core::FoliagePaint::HasActive() || !assetManager )
            return;
        auto& manager = const_cast<Assets::AssetManager&>( *assetManager );

        ::Desert::Core::RaycastHit hit;
        if ( !scene.Raycast( ray, hit ) )
            return;
        const glm::vec3 hitPos = hit.Point;
        const glm::vec3 hitN   = hit.Normal;

        const float radius = Core::FoliagePaint::BrushRadius();
        const bool  erase  = Core::FoliagePaint::Erase();
        // Surface slope (deg) at the brush centre, for the per-type slope filter.
        const float slopeDeg = glm::degrees( std::acos( glm::clamp( hitN.y, -1.0f, 1.0f ) ) );

        static std::mt19937                   rng{ std::random_device{}() };
        std::uniform_real_distribution<float> u01( 0.0f, 1.0f );
        std::uniform_real_distribution<float> usym( -1.0f, 1.0f );

        // Paint/erase EVERY checked type under one dab (UE5-style multi-paint).
        for ( const auto& uuid : Core::FoliagePaint::ActiveTypes() )
        {
            auto ref = scene.FindEntityByID( uuid );
            if ( !ref )
                continue;
            auto& e = ref->get();
            if ( !e.HasComponent<ECS::FoliageComponent>() ||
                 !e.HasComponent<ECS::InstancedStaticMeshComponent>() )
                continue;

            auto& ism = e.GetComponent<ECS::InstancedStaticMeshComponent>();

            if ( erase )
            {
                const float r2 = radius * radius;
                auto&       xs = ism.InstanceTransforms;
                xs.erase( std::remove_if( xs.begin(), xs.end(),
                                          [&]( const glm::mat4& m )
                                          {
                                              const glm::vec3 p  = glm::vec3( m[3] );
                                              const float     dx = p.x - hitPos.x, dz = p.z - hitPos.z;
                                              return dx * dx + dz * dz <= r2;
                                          } ),
                          xs.end() );
                continue;
            }

            const auto type = ResolveType( manager, e.GetComponent<ECS::FoliageComponent>().FoliageType );
            if ( !type )
                continue;
            const auto& fol = type->GetData();

            // Per-type slope filter (uses the brush-centre surface).
            if ( slopeDeg < fol.GroundSlopeAngle.Min || slopeDeg > fol.GroundSlopeAngle.Max )
                continue;

            const int count = glm::max(
                 1, static_cast<int>( std::round( fol.Density * Core::FoliagePaint::PaintDensity() ) ) );
            for ( int i = 0; i < count; ++i )
            {
                const float ang = u01( rng ) * 6.2831853f;
                const float rr  = radius * std::sqrt( u01( rng ) ); // uniform disk
                glm::vec3   pos = hitPos + glm::vec3( std::cos( ang ) * rr, 0.0f, std::sin( ang ) * rr );
                pos.y += glm::mix( fol.ZOffset.Min, fol.ZOffset.Max, u01( rng ) );
                const float scl = glm::mix( fol.ScaleX.Min, fol.ScaleX.Max, u01( rng ) );

                glm::mat4 m = glm::translate( glm::mat4( 1.0f ), pos );
                if ( fol.AlignToNormal )
                    m *= glm::mat4_cast( AlignUpToNormal( hitN ) );
                if ( fol.RandomYaw )
                    m = glm::rotate( m, u01( rng ) * 6.2831853f, glm::vec3( 0.0f, 1.0f, 0.0f ) );
                if ( fol.RandomPitchAngle > 0.01f )
                {
                    const float     pitch = glm::radians( fol.RandomPitchAngle ) * u01( rng );
                    const glm::vec3 axis =
                         glm::normalize( glm::vec3( usym( rng ), 0.0f, usym( rng ) ) + glm::vec3( 1e-3f, 0, 0 ) );
                    m = glm::rotate( m, pitch, axis );
                }
                m = glm::scale( m, glm::vec3( scl ) );
                ism.InstanceTransforms.push_back( m );
            }
        }
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
