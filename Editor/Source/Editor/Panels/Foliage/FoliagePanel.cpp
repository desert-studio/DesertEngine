// The Foliage panel (FO-UI1), after UE 5.8 Engine/Plugins/Editor/FoliageEdit (SFoliageEdit.cpp: the tool bar
// and the Brush Options / Filters details; SFoliagePalette.cpp: the palette's tile and tree views, its search box,
// its "+ Foliage" picker and the per-type context menu). Not ported line by line: UE builds these from Slate
// widgets and UObject details customisations (FFoliageTypePaintingCustomization); here the same sections are
// ImGui rows in the editor's UE5 Details look (ImGuiUtilities property rows and section headers). What each
// control DOES is a FoliagePaintTool action, shared with the palette commands in EditorLayer, so every widget
// below is reachable without a mouse (FoliagePalette suite, census).

#include <Editor/Panels/ViewportPanel/Tools/FoliagePaintTool.hpp>

#include <Editor/Core/DragPayloads.hpp>
#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/ImGuiUtilities.hpp>
#include <Editor/Core/Selection/FoliagePaint.hpp>
#include <Editor/Core/Selection/SelectionManager.hpp>
#include <Editor/Core/ToastManager.hpp>
#include <Editor/Panels/Foliage/FoliagePalette.hpp>
#include <Editor/Widgets/ThumbnailCache.hpp>
#include <Editor/Widgets/ThumbnailFreshness.hpp>
#include <Editor/Widgets/ThumbnailService.hpp>
#include <Editor/Widgets/UIHelper/ImGuiUI.hpp>
#include <Engine/Assets/Mesh/MeshAsset.hpp>
#include <Engine/ECS/EntityVisibility.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <ImGui/imgui.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Editor::Tools
{
    namespace ImGui = ::ImGui; // engine headers introduce a Desert::ImGui that would otherwise shadow ::ImGui
    using Row       = Utils::ImGuiUtilities;

    namespace
    {
        void Report( const Common::BoolResultStr& done )
        {
            if ( !done )
                ToastManager::Push( done.GetError(), ToastLevel::Warning, 5.0f );
        }

        // A tool-bar button that looks pressed when `active` (UE's toggled tool button).
        bool ToolButton( const char* label, bool active, float width )
        {
            if ( active )
                ImGui::PushStyleColor( ImGuiCol_Button, ImGui::GetStyleColorVec4( ImGuiCol_ButtonActive ) );
            const bool pressed = ImGui::Button( label, ImVec2( width, 0.0f ) );
            if ( active )
                ImGui::PopStyleColor();
            return pressed;
        }

        // The mesh's LOD 0 triangles (what one instance submits at full detail).
        uint64_t TrianglesOf( const Assets::AssetManager* manager, const Assets::AssetHandle& mesh )
        {
            if ( !manager || !mesh )
                return 0;
            const auto asset = manager->FindByHandle<Assets::MeshAsset>( mesh );
            if ( !asset || !asset->IsReadyForUse() )
                return 0;
            uint64_t triangles = 0;
            for ( const auto& submesh : asset->GetSubmeshes() )
                triangles += submesh.IndexCount / 3u;
            return triangles;
        }

        // The landscape weight-plane names the scene's tiles hold, sorted, each once (the brush layer filter's
        // choices).
        std::vector<std::string> LandscapeLayerNames( ::Desert::Core::Scene& scene )
        {
            std::set<std::string> names;
            for ( const auto& entity : scene.GetAllEntities() )
            {
                if ( !entity.HasComponent<ECS::LandscapeTileComponent>() )
                    continue;
                const auto& tile = entity.GetComponent<ECS::LandscapeTileComponent>();
                if ( tile.Heights )
                    for ( const auto& layer : tile.Heights->WeightLayers() )
                        names.insert( layer.Name );
            }
            return { names.begin(), names.end() };
        }

        // What the "+ Foliage" picker offers: every `.defoliage` and every static-mesh source under the assets
        // root. Scanned when the picker opens, not per frame.
        struct PickerEntries
        {
            std::vector<std::filesystem::path> Types;
            std::vector<std::filesystem::path> Meshes;
            std::vector<std::filesystem::path> Prefabs; ///< FO-8: a Prefab type is found or made
        };
        PickerEntries ScanPicker()
        {
            PickerEntries out;
            // Through the one content enumeration (loose files and a mounted .dpak alike).
            for ( const std::filesystem::path& file :
                  Common::Utils::FileSystem::ListFilesRecursive( Common::Constants::Path::ASSETS_PATH ) )
            {
                const std::string ext = file.extension().string();
                if ( ext == Assets::Serialization::kFoliageTypeExtension )
                    out.Types.push_back( file );
                else if ( ext == ".fbx" || ext == ".obj" || ext == ".gltf" || ext == ".glb" )
                    out.Meshes.push_back( file );
                else if ( ext == ".deprefab" )
                    out.Prefabs.push_back( file );
            }
            std::ranges::sort( out.Types );
            std::ranges::sort( out.Meshes );
            std::ranges::sort( out.Prefabs );
            return out;
        }

        // A thumbnail of the type's mesh for the palette's grid, through the one freshness rule; null while
        // none is rendered (the tile then shows a glyph).
        const void* ThumbnailOf( const Assets::AssetManager* manager, UI::UIHelper* uiHelper,
                                 const Assets::AssetHandle& mesh )
        {
            static ThumbnailCache s_Thumbnails;
            if ( !manager || !uiHelper || !mesh )
                return nullptr;
            const auto asset = manager->FindByHandle<Assets::MeshAsset>( mesh );
            if ( !asset )
                return nullptr;
            const std::string source = asset->GetMetadata().Filepath.generic_string();
            const std::string png    = ThumbnailService::Get().RequestMesh( mesh, source );
            if ( png.empty() )
                return nullptr;
            if ( ThumbnailFreshness::Judge( ThumbnailFreshness::Observe( png, source ) ) !=
                 ThumbnailFreshness::Verdict::Show )
            {
                s_Thumbnails.Invalidate( png );
                return nullptr;
            }
            const auto image = s_Thumbnails.Get( png );
            return image ? uiHelper->GetTextureID( image ) : nullptr;
        }

        // A count in at most five characters for the narrow cost column: 950, 12.4K, 99.0M.
        std::string Compact( uint64_t n )
        {
            char text[16];
            if ( n < 1000u )
                std::snprintf( text, sizeof( text ), "%llu", static_cast<unsigned long long>( n ) );
            else if ( n < 1000000u )
                std::snprintf( text, sizeof( text ), "%.1fK", static_cast<double>( n ) / 1e3 );
            else if ( n < 1000000000u )
                std::snprintf( text, sizeof( text ), "%.1fM", static_cast<double>( n ) / 1e6 );
            else
                std::snprintf( text, sizeof( text ), "%.1fG", static_cast<double>( n ) / 1e9 );
            return text;
        }

        std::string Thousands( uint64_t n )
        {
            std::string digits = std::to_string( n );
            for ( int at = static_cast<int>( digits.size() ) - 3; at > 0; at -= 3 )
                digits.insert( static_cast<size_t>( at ), "," );
            return digits;
        }

        // The row's label: the entity's tag without the "Foliage_" prefix AddField gives it.
        std::string RowName( const ECS::Entity& field )
        {
            std::string name = field.GetComponent<ECS::TagComponent>().Tag;
            if ( name.starts_with( "Foliage_" ) )
                name.erase( 0, 8 );
            return name;
        }
    } // namespace

    void FoliagePaintTool::DrawFootprint( ::Desert::Core::Scene& scene, const Assets::AssetManager* assetManager,
                                          const glm::mat4& viewProjection, const glm::vec2& viewportPos,
                                          const glm::vec2& viewportSize )
    {
        const auto hover = Core::FoliagePaint::HoverPoint();
        if ( !hover || !assetManager || Core::FoliagePaint::Tool() != Core::FoliageTool::Paint )
            return;
        const float radius   = Core::FoliagePaint::BrushRadius();
        float       expected = 0.0f;
        for ( const auto& uuid : Core::FoliagePaint::ActiveTypes() )
        {
            const auto ref = scene.FindEntityByID( uuid );
            if ( !ref || !ref->get().HasComponent<ECS::FoliageComponent>() )
                continue;
            const auto type = ResolveType( const_cast<Assets::AssetManager&>( *assetManager ),
                                           ref->get().GetComponent<ECS::FoliageComponent>().FoliageType );
            if ( !type )
                continue;
            expected += Foliage::PreviewFoliageFootprint( type->GetData().Density, radius,
                                                          Core::FoliagePaint::PaintDensity(),
                                                          RowInstances( scene, uuid, *hover, radius ), *hover )
                             .Expected;
        }

        const auto project = [&]( const glm::vec3& p, ImVec2& out )
        {
            const glm::vec4 clip = viewProjection * glm::vec4( p, 1.0f );
            if ( clip.w <= 0.0f )
                return false;
            out = ImVec2( viewportPos.x + ( clip.x / clip.w * 0.5f + 0.5f ) * viewportSize.x,
                          viewportPos.y + ( 0.5f - clip.y / clip.w * 0.5f ) * viewportSize.y );
            return true;
        };
        ImDrawList*         dl        = ImGui::GetWindowDrawList();
        constexpr int       kSegments = 64;
        std::vector<ImVec2> ring;
        ring.reserve( kSegments );
        for ( int i = 0; i < kSegments; ++i )
        {
            const float a = 6.2831853f * static_cast<float>( i ) / static_cast<float>( kSegments );
            ImVec2      at;
            if ( project( *hover + glm::vec3( std::cos( a ) * radius, 0.0f, std::sin( a ) * radius ), at ) )
                ring.push_back( at );
        }
        if ( ring.size() > 2 )
        {
            dl->AddPolyline( ring.data(), static_cast<int>( ring.size() ), IM_COL32( 0, 0, 0, 160 ),
                             ImDrawFlags_Closed, 3.5f );
            dl->AddPolyline( ring.data(), static_cast<int>( ring.size() ), IM_COL32( 120, 220, 110, 255 ),
                             ImDrawFlags_Closed, 1.8f );
        }
        ImVec2 centre;
        if ( project( *hover, centre ) )
        {
            char text[64];
            std::snprintf( text, sizeof( text ), "~%.0f per dab", expected );
            const ImVec2 size = ImGui::CalcTextSize( text );
            const ImVec2 at( centre.x - size.x * 0.5f, centre.y - size.y * 0.5f );
            dl->AddRectFilled( ImVec2( at.x - 5, at.y - 3 ), ImVec2( at.x + size.x + 5, at.y + size.y + 3 ),
                               IM_COL32( 20, 20, 20, 200 ), 3.0f );
            dl->AddText( at, IM_COL32( 230, 240, 225, 255 ), text );
        }
    }

    void FoliagePaintTool::DrawPanel( ::Desert::Core::Scene& scene, const Assets::AssetManager* assetManager,
                                      const glm::vec2& viewportPos, UI::UIHelper* uiHelper )
    {
        // Floats at the viewport's left edge while Foliage mode is on (UE's mode toolkit beside the level): as
        // tall as its content, down to the bottom of the editor window at most, scrolling beyond that.
        const ImGuiViewport* main = ImGui::GetMainViewport();
        const float          top  = viewportPos.y + 58.0f;
        ImGui::SetNextWindowPos( ImVec2( viewportPos.x + 12.0f, top ), ImGuiCond_Always );
        ImGui::SetNextWindowSize( ImVec2( 380.0f, 0.0f ), ImGuiCond_Always );
        ImGui::SetNextWindowSizeConstraints(
             ImVec2( 380.0f, 120.0f ),
             ImVec2( 380.0f, std::max( 120.0f, main->WorkPos.y + main->WorkSize.y - top - 8.0f ) ) );
        ImGui::SetNextWindowBgAlpha( 0.96f );
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                       ImGuiWindowFlags_NoDocking;
        if ( !ImGui::Begin( "Foliage##FoliagePanel", nullptr, flags ) )
        {
            ImGui::End();
            return;
        }
        auto* manager = const_cast<Assets::AssetManager*>( assetManager );

        // ----- Tool bar (UE SFoliageEdit's toolbar, in its order) ------------------------------------------
        {
            Core::FoliageTool& tool = Core::FoliagePaint::Tool();
            const float        third =
                 ( ImGui::GetContentRegionAvail().x - 2.0f * ImGui::GetStyle().ItemSpacing.x ) / 3.0f;
            if ( ToolButton( ICON_MDI_CURSOR_DEFAULT_CLICK " Select", tool == Core::FoliageTool::Select, third ) )
                tool = Core::FoliageTool::Select;
            ImGui::SameLine();
            if ( ToolButton( ICON_MDI_SELECT_ALL " All", false, third ) )
                Report( SelectAllInstances( scene ) );
            ImGui::SameLine();
            if ( ToolButton( ICON_MDI_SELECT_OFF " Deselect", false, third ) )
                Report( SelectNone( scene ) );
            if ( ToolButton( ICON_MDI_LASSO " Lasso", tool == Core::FoliageTool::Lasso, third ) )
                tool = Core::FoliageTool::Lasso;
            ImGui::SameLine();
            if ( ToolButton( ICON_MDI_BRUSH " Paint", tool == Core::FoliageTool::Paint, third ) )
                tool = Core::FoliageTool::Paint;
            ImGui::SameLine();
            if ( ToolButton( ICON_MDI_REFRESH " Reapply", tool == Core::FoliageTool::Reapply, third ) )
                tool = Core::FoliageTool::Reapply;
            if ( ToolButton( ICON_MDI_SPROUT " Single", tool == Core::FoliageTool::Single, third ) )
                tool = Core::FoliageTool::Single;
            ImGui::SameLine();
            if ( ToolButton( ICON_MDI_FORMAT_COLOR_FILL " Fill", tool == Core::FoliageTool::Fill, third ) )
                tool = Core::FoliageTool::Fill;
            ImGui::SameLine();
            if ( ToolButton( ICON_MDI_ERASER " Remove", tool == Core::FoliageTool::Remove, third ) )
                tool = Core::FoliageTool::Remove;
        }
        const Core::FoliageTool tool = Core::FoliagePaint::Tool();
        Row::ResetPropertyRows();

        // ----- Brush Options -------------------------------------------------------------------------------
        if ( Row::SectionHeader( "Brush Options" ) )
        {
            Row::BeginPropertyRow( "Brush Size", "The brush radius, cm" );
            ImGui::SetNextItemWidth( -1 );
            ImGui::SliderFloat( "##BrushSize", &Core::FoliagePaint::BrushRadius(), 50.0f, 6000.0f, "%.0f cm" );
            Row::EndPropertyRow();
            Row::BeginPropertyRow( "Paint Density",
                                   "Share of each type's Density one dab tops up to (UE PaintDensity)" );
            ImGui::SetNextItemWidth( -1 );
            ImGui::SliderFloat( "##PaintDensity", &Core::FoliagePaint::PaintDensity(), 0.0f, 1.0f, "%.2f" );
            Row::EndPropertyRow();

            // FO-UI1 footprint preview: what one dab adds at the cursor, over the checked types.
            Row::BeginPropertyRow( "Footprint",
                                   "Instances one dab would add at the cursor (an upper bound: slope, "
                                   "height and layer rules can only lower it)" );
            if ( const auto hover = Core::FoliagePaint::HoverPoint(); hover && manager )
            {
                Foliage::FoliageFootprint sum;
                for ( const auto& uuid : Core::FoliagePaint::ActiveTypes() )
                {
                    const auto ref = scene.FindEntityByID( uuid );
                    if ( !ref || !ref->get().HasComponent<ECS::FoliageComponent>() )
                        continue;
                    const auto type =
                         ResolveType( *manager, ref->get().GetComponent<ECS::FoliageComponent>().FoliageType );
                    if ( !type )
                        continue;
                    const auto one = Foliage::PreviewFoliageFootprint(
                         type->GetData().Density, Core::FoliagePaint::BrushRadius(),
                         Core::FoliagePaint::PaintDensity(),
                         RowInstances( scene, uuid, *hover, Core::FoliagePaint::BrushRadius() ), *hover );
                    sum.Desired += one.Desired;
                    sum.Existing += one.Existing;
                    sum.Expected += one.Expected;
                }
                ImGui::Text( "~%.0f new per dab", sum.Expected );
                Row::Tooltip( ( "The checked types want " +
                                std::to_string( static_cast<long>( sum.Desired + 0.5f ) ) +
                                " under the brush and " + std::to_string( sum.Existing ) + " are there" )
                                   .c_str() );
            }
            else
                ImGui::TextDisabled( "hover the ground" );
            Row::EndPropertyRow();
        }

        // ----- Filters (UE bFilterLandscape / bFilterStaticMesh; + the brush's landscape layer) -------------
        if ( Row::SectionHeader( "Filters", false ) )
        {
            Row::BeginPropertyRow( "Landscape" );
            ImGui::Checkbox( "##FilterLandscape", &Core::FoliagePaint::FilterLandscape() );
            Row::EndPropertyRow();
            Row::BeginPropertyRow( "Static Meshes" );
            ImGui::Checkbox( "##FilterStaticMesh", &Core::FoliagePaint::FilterStaticMesh() );
            Row::EndPropertyRow();

            auto&             layers = Core::FoliagePaint::BrushLayers();
            const std::string shown  = layers.empty()       ? std::string( "Any layer" )
                                       : layers.size() == 1 ? layers.front()
                                                            : std::to_string( layers.size() ) + " layers";
            Row::BeginPropertyRow( "Landscape Layer", "The brush places on landscape only where these layers are "
                                                      "painted, for every checked type" );
            ImGui::SetNextItemWidth( -1 );
            if ( ImGui::BeginCombo( "##BrushLayers", shown.c_str() ) )
            {
                for ( const auto& name : LandscapeLayerNames( scene ) )
                {
                    const auto it     = std::ranges::find( layers, name );
                    bool       picked = it != layers.end();
                    if ( ImGui::Checkbox( name.c_str(), &picked ) )
                    {
                        if ( picked )
                            layers.push_back( name );
                        else
                            layers.erase( it );
                    }
                }
                ImGui::EndCombo();
            }
            Row::EndPropertyRow();
            if ( !layers.empty() )
            {
                Row::BeginPropertyRow( "Layer Min Weight" );
                ImGui::SetNextItemWidth( -1 );
                ImGui::SliderFloat( "##BrushLayerMin", &Core::FoliagePaint::BrushLayerMinWeight(), 0.0f, 1.0f,
                                    "%.2f" );
                Row::EndPropertyRow();
            }
        }

        // ----- Selection and Reapply: shown with the tools they belong to -----------------------------------
        if ( tool == Core::FoliageTool::Select || tool == Core::FoliageTool::Lasso ||
             Core::FoliagePaint::SelectedCount() > 0 )
        {
            const std::string detail = std::to_string( Core::FoliagePaint::SelectedCount() ) + " instances";
            if ( Row::SectionHeader( "Selection", true, detail.c_str() ) )
            {
                const float half = ( ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x ) * 0.5f;
                if ( ImGui::Button( ICON_MDI_DELETE " Delete", ImVec2( half, 0 ) ) )
                    Report( RemoveSelected( scene ) );
                ImGui::SameLine();
                ImGui::SetNextItemWidth( half );
                ImGui::DragFloat3( "##MoveOffset", &Core::FoliagePaint::MoveOffset().x, 1.0f, -100000.0f,
                                   100000.0f, "%.0f" );
                if ( ImGui::Button( ICON_MDI_ARROW_ALL " Move by offset (cm)", ImVec2( -1, 0 ) ) )
                    Report( MoveSelected( scene, Core::FoliagePaint::MoveOffset() ) );
            }
        }
        if ( tool == Core::FoliageTool::Reapply && Row::SectionHeader( "Reapply" ) )
        {
            auto&       r       = Core::FoliagePaint::Reapply();
            bool*       bits[]  = { &r.Scale,       &r.ZOffset, &r.AlignToNormal,   &r.RandomYaw, &r.RandomPitch,
                                    &r.GroundSlope, &r.Height,  &r.LandscapeLayers, &r.Density };
            const char* names[] = { "Scale", "Z Offset", "Align",  "Yaw",    "Pitch",
                                    "Slope", "Height",   "Layers", "Density" };
            for ( size_t i = 0; i < std::size( bits ); ++i )
            {
                if ( i % 3 != 0 )
                    ImGui::SameLine( static_cast<float>( i % 3 ) * ImGui::GetContentRegionAvail().x / 3.0f );
                ImGui::Checkbox( names[i], bits[i] );
            }
        }

        // ----- Palette ------------------------------------------------------------------------------------
        std::vector<ECS::Entity> fields = PaletteFields( scene );
        const std::string        count  = std::to_string( fields.size() ) + " types";
        if ( Row::SectionHeader( "Palette", true, count.c_str() ) )
        {
            static PickerEntries s_Picker;
            if ( ImGui::Button( ICON_MDI_PLUS " Foliage" ) )
            {
                s_Picker = ScanPicker();
                ImGui::OpenPopup( "##FoliagePicker" );
            }
            if ( ImGui::BeginPopup( "##FoliagePicker" ) )
            {
                ImGui::TextDisabled( "FOLIAGE TYPES" );
                for ( const auto& path : s_Picker.Types )
                    if ( ImGui::Selectable( ( ICON_MDI_GRASS " " + path.stem().string() ).c_str() ) && manager )
                        Report( AddTypeFile( scene, *manager, path.string() ) );
                ImGui::Separator();
                ImGui::TextDisabled( "STATIC MESHES (a type is found or made)" );
                for ( const auto& path : s_Picker.Meshes )
                    if ( ImGui::Selectable( ( ICON_MDI_CUBE_OUTLINE " " + path.stem().string() ).c_str() ) &&
                         manager )
                        Report( AddMeshFile( scene, *manager, path.generic_string() ) );
                ImGui::Separator();
                ImGui::TextDisabled( "PREFABS (a Prefab type is found or made)" );
                for ( const auto& path : s_Picker.Prefabs )
                    if ( ImGui::Selectable( ( ICON_MDI_PACKAGE_VARIANT " " + path.stem().string() ).c_str() ) &&
                         manager )
                        Report( AddPrefabFile( scene, *manager, path.generic_string() ) );
                ImGui::EndPopup();
            }
            ImGui::SameLine();
            if ( ImGui::Button( ICON_MDI_CURSOR_DEFAULT_CLICK " From Selection" ) && manager )
            {
                if ( const auto& selected = Core::SelectionManager::GetSelected() )
                    Report( AddFromEntity( scene, *manager, *selected ) );
                else
                    Report( Common::MakeError( "foliage from selection: no entity is selected" ) );
            }
            ImGui::SameLine();
            bool& grid = Core::FoliagePaint::GridView();
            if ( ImGui::Button( grid ? ICON_MDI_VIEW_LIST : ICON_MDI_VIEW_GRID ) )
                grid = !grid;
            Row::Tooltip( grid ? "List view" : "Grid view" );

            static char s_Search[128] = {};
            std::snprintf( s_Search, sizeof( s_Search ), "%s", Core::FoliagePaint::Search().c_str() );
            ImGui::SetNextItemWidth( -1 );
            if ( ImGui::InputTextWithHint( "##FoliageSearch", ICON_MDI_MAGNIFY " Search foliage types", s_Search,
                                           sizeof( s_Search ) ) )
                Core::FoliagePaint::Search() = s_Search;

            // The drop zone is the whole list, as in UE: a Static Mesh, a `.defoliage` or a collection.
            const float rowHeight  = ImGui::GetFrameHeightWithSpacing();
            const float listHeight = grid ? 196.0f
                                          : std::clamp( rowHeight * static_cast<float>( fields.size() ) + 12.0f,
                                                        rowHeight * 2.0f, 156.0f );
            ImGui::BeginChild( "##FoliagePaletteList", ImVec2( -1, listHeight ), true );
            if ( fields.empty() )
                ImGui::TextDisabled( "Drop a Static Mesh, a .defoliage or a collection here,\nor use + Foliage." );

            const auto editing = Core::FoliagePaint::EditingType();
            const auto rowMenu = [&]( const Common::UUID& uuid )
            {
                if ( !ImGui::BeginPopupContextItem( "##FoliageRowMenu" ) )
                    return;
                if ( ImGui::BeginMenu( ICON_MDI_SWAP_HORIZONTAL " Replace" ) )
                {
                    if ( ImGui::IsWindowAppearing() )
                        s_Picker = ScanPicker();
                    for ( const auto& path : s_Picker.Types )
                        if ( ImGui::MenuItem( path.stem().string().c_str() ) && manager )
                            Report( ReplaceType( scene, *manager, uuid, path.string() ) );
                    ImGui::EndMenu();
                }
                if ( ImGui::MenuItem( ICON_MDI_SELECT_ALL " Select all instances" ) )
                    Report( SelectTypeInstances( scene, uuid ) );
                if ( ImGui::MenuItem( ICON_MDI_CONTENT_SAVE " Save as asset" ) && manager )
                    Report( SaveTypeCopy( scene, *manager, uuid ) );
                if ( ImGui::MenuItem( ICON_MDI_FOLDER_SEARCH " Show in Content Browser" ) )
                    Report( ShowTypeInBrowser( scene, uuid ) );
                ImGui::Separator();
                if ( ImGui::MenuItem( ICON_MDI_DELETE " Remove" ) )
                    Report( RemoveType( scene, uuid ) );
                ImGui::EndPopup();
            };

            if ( !grid && !fields.empty() &&
                 ImGui::BeginTable( "##FoliageTypes", 5,
                                    ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                         ImGuiTableFlags_SizingFixedFit ) )
            {
                ImGui::TableSetupColumn( "##chk", ImGuiTableColumnFlags_WidthFixed, 22.0f );
                ImGui::TableSetupColumn( "##eye", ImGuiTableColumnFlags_WidthFixed, 22.0f );
                ImGui::TableSetupColumn( "Type", ImGuiTableColumnFlags_WidthStretch );
                ImGui::TableSetupColumn( "Count", ImGuiTableColumnFlags_WidthFixed, 52.0f );
                ImGui::TableSetupColumn( "Cost", ImGuiTableColumnFlags_WidthFixed, 74.0f );
                for ( auto& field : fields )
                {
                    const std::string name = RowName( field );
                    if ( !Foliage::PaletteNameMatches( name, Core::FoliagePaint::Search() ) )
                        continue;
                    const auto  uuid   = field.GetComponent<ECS::UUIDComponent>().UUID;
                    const auto& ism    = field.GetComponent<ECS::InstancedStaticMeshComponent>();
                    const bool  hidden = !field.HasComponent<ECS::VisibilityComponent>()
                                              ? false
                                              : !field.GetComponent<ECS::VisibilityComponent>().Visible;
                    const auto  type =
                         manager ? ResolveType( *manager, field.GetComponent<ECS::FoliageComponent>().FoliageType )
                                  : nullptr;
                    // FO-6: the row stands for the type's field in every cell.
                    const auto instances = RowInstances( scene, uuid );
                    const auto cost      = Foliage::MeasureFoliageTypeCost(
                         instances,
                         type ? type->GetData().CullDistance : Assets::Serialization::FoliageFloatInterval{},
                         Core::FoliagePaint::ViewPosition(), TrianglesOf( assetManager, ism.MeshHandle ), hidden );

                    ImGui::PushID( static_cast<int>( static_cast<uint64_t>( uuid ) ) );
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    bool active = Core::FoliagePaint::IsActive( uuid );
                    if ( ImGui::Checkbox( "##active", &active ) )
                        Core::FoliagePaint::ToggleActive( uuid );
                    Row::Tooltip( "Paint with this type" );
                    ImGui::TableNextColumn();
                    if ( ImGui::SmallButton( hidden ? ICON_MDI_EYE_OFF : ICON_MDI_EYE ) )
                        Report( ToggleTypeVisible( scene, uuid ) );
                    Row::Tooltip( hidden ? "Hidden - show the type" : "Hide the type (nothing is removed)" );
                    ImGui::TableNextColumn();
                    if ( ImGui::Selectable( name.c_str(), editing == uuid, ImGuiSelectableFlags_SpanAllColumns ) )
                        Core::FoliagePaint::SetEditingType( uuid );
                    rowMenu( uuid );
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted( Thousands( cost.Instances ).c_str() );
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled( "%s tri", Compact( cost.Triangles ).c_str() );
                    Row::Tooltip( ( Thousands( cost.InCullRange ) + " of " + Thousands( cost.Instances ) +
                                    " instances within the cull distance of the camera, " +
                                    Thousands( cost.TrianglesPerInstance ) + " triangles each" )
                                       .c_str() );
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            else if ( grid )
            {
                const float tile = 76.0f;
                const int   columns =
                     std::max( 1, static_cast<int>( ImGui::GetContentRegionAvail().x / ( tile + 6.0f ) ) );
                int shown = 0;
                for ( auto& field : fields )
                {
                    const std::string name = RowName( field );
                    if ( !Foliage::PaletteNameMatches( name, Core::FoliagePaint::Search() ) )
                        continue;
                    const auto  uuid   = field.GetComponent<ECS::UUIDComponent>().UUID;
                    const auto& ism    = field.GetComponent<ECS::InstancedStaticMeshComponent>();
                    const bool  active = Core::FoliagePaint::IsActive( uuid );
                    if ( shown++ % columns != 0 )
                        ImGui::SameLine();
                    ImGui::PushID( static_cast<int>( static_cast<uint64_t>( uuid ) ) );
                    ImGui::BeginGroup();
                    const ImVec2 at = ImGui::GetCursorScreenPos();
                    // Click = paint with it (UE: the tile's check); the name below edits it.
                    if ( ImGui::InvisibleButton( "##tile", ImVec2( tile, tile ) ) )
                        Core::FoliagePaint::ToggleActive( uuid );
                    rowMenu( uuid );
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    dl->AddRectFilled( at, ImVec2( at.x + tile, at.y + tile ), IM_COL32( 36, 36, 36, 255 ), 3.0f );
                    if ( const void* tex = ThumbnailOf( assetManager, uiHelper, ism.MeshHandle ) )
                        dl->AddImageRounded( reinterpret_cast<ImTextureID>( const_cast<void*>( tex ) ),
                                             ImVec2( at.x + 2, at.y + 2 ),
                                             ImVec2( at.x + tile - 2, at.y + tile - 2 ), ImVec2( 0, 0 ),
                                             ImVec2( 1, 1 ), IM_COL32_WHITE, 3.0f );
                    else
                    {
                        const ImVec2 glyph = ImGui::CalcTextSize( ICON_MDI_GRASS );
                        dl->AddText( ImVec2( at.x + ( tile - glyph.x ) * 0.5f, at.y + ( tile - glyph.y ) * 0.5f ),
                                     IM_COL32( 140, 170, 120, 255 ), ICON_MDI_GRASS );
                    }
                    dl->AddRect( at, ImVec2( at.x + tile, at.y + tile ),
                                 active ? IM_COL32( 60, 150, 255, 255 ) : IM_COL32( 20, 20, 20, 255 ), 3.0f, 0,
                                 active ? 2.0f : 1.0f );
                    const std::string countText = Thousands( RowInstances( scene, uuid ).size() );
                    dl->AddText( ImVec2( at.x + 4, at.y + tile - ImGui::GetTextLineHeight() - 2 ),
                                 IM_COL32( 230, 230, 230, 255 ), countText.c_str() );
                    ImGui::SetNextItemWidth( tile );
                    if ( ImGui::Selectable( name.c_str(), Core::FoliagePaint::EditingType() == uuid, 0,
                                            ImVec2( tile, 0 ) ) )
                        Core::FoliagePaint::SetEditingType( uuid );
                    ImGui::EndGroup();
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();
            if ( manager && ImGui::BeginDragDropTarget() )
            {
                if ( const ImGuiPayload* p = ImGui::AcceptDragDropPayload( DragPayloads::MeshAsset ) )
                    Report( AddMeshFile( scene, *manager, std::string( static_cast<const char*>( p->Data ) ) ) );
                if ( const ImGuiPayload* p = ImGui::AcceptDragDropPayload( DragPayloads::PrefabFile ) )
                    Report( AddPrefabFile( scene, *manager, std::string( static_cast<const char*>( p->Data ) ) ) );
                if ( const ImGuiPayload* p = ImGui::AcceptDragDropPayload( DragPayloads::Collection ) )
                    Report( AddCollection( scene, *manager, std::string( static_cast<const char*>( p->Data ) ) ) );
                if ( const ImGuiPayload* p = ImGui::AcceptDragDropPayload( DragPayloads::AssetFile ) )
                {
                    const std::string path( static_cast<const char*>( p->Data ) );
                    if ( std::filesystem::path( path ).extension() ==
                         Assets::Serialization::kFoliageTypeExtension )
                        Report( AddTypeFile( scene, *manager, path ) );
                }
                ImGui::EndDragDropTarget();
            }

            // Presets: the palette saved as a collection; a collection dropped (or its palette command) applies
            // it.
            static char s_Preset[64] = {};
            std::snprintf( s_Preset, sizeof( s_Preset ), "%s", Core::FoliagePaint::PresetName().c_str() );
            ImGui::SetNextItemWidth( ImGui::GetContentRegionAvail().x * 0.55f );
            if ( ImGui::InputText( "##PresetName", s_Preset, sizeof( s_Preset ) ) )
                Core::FoliagePaint::PresetName() = s_Preset;
            ImGui::SameLine();
            if ( ImGui::Button( ICON_MDI_CONTENT_SAVE_ALL " Save Preset", ImVec2( -1, 0 ) ) && manager )
                Report( SavePreset( scene, *manager, Core::FoliagePaint::PresetName() ) );
        }

        // ----- Details of the type being edited (UE: the palette's details panel) ---------------------------
        if ( const auto editing = Core::FoliagePaint::EditingType(); editing && manager )
        {
            if ( auto ref = scene.FindEntityByID( *editing );
                 ref && ref->get().HasComponent<ECS::FoliageComponent>() )
            {
                auto&             field  = ref->get();
                const std::string header = "Details: " + RowName( field );
                if ( Row::SectionHeader( header.c_str() ) )
                {
                    if ( auto type =
                              ResolveType( *manager, field.GetComponent<ECS::FoliageComponent>().FoliageType ) )
                        DrawTypeSettings( *manager, type );
                    else
                        ImGui::TextDisabled( "The field's type does not load (the log says why)." );
                    // The field's own switch (the ISM's), beside the type's numbers: UE's CastShadow row.
                    Row::BeginPropertyRow( "Cast Shadows", "This field's instances in the shadow cascades" );
                    ImGui::Checkbox( "##CastShadows",
                                     &field.GetComponent<ECS::InstancedStaticMeshComponent>().CastShadows );
                    Row::EndPropertyRow();
                }
            }
        }

        static constexpr const char* kHints[] = {
             "LMB drag: paint",  "LMB click: place one", "LMB click: select one",        "LMB drag: select",
             "LMB drag: remove", "LMB drag: reapply",    "LMB click: fill a static mesh" };
        ImGui::TextDisabled( "%s  -  on the checked types", kHints[static_cast<size_t>( tool )] );
        ImGui::End();
    }
} // namespace Desert::Editor::Tools
