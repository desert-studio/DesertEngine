#include "FoliagePaintTool.hpp"

#include <Editor/Core/Selection/FoliagePaint.hpp>
#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/DragPayloads.hpp>
#include <Editor/Import/MeshDnD.hpp>

#include <ImGui/imgui.h>

#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <random>
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
    } // namespace

    void FoliagePaintTool::CreateType( ::Desert::Core::Scene& scene, const Assets::AssetManager* assetManager,
                                       const std::string& meshSourcePath )
    {
        if ( !assetManager )
            return;
        const auto handle =
             MeshDnD::ResolveOrImport( const_cast<Assets::AssetManager&>( *assetManager ), meshSourcePath );
        if ( !handle ) // skinned source / cook failure
            return;

        auto& e = scene.CreateNewEntity(
             std::string( "Foliage_" ) + std::filesystem::path( meshSourcePath ).stem().string() );
        e.AddComponent<ECS::FoliageComponent>();
        e.AddComponent<ECS::InstancedStaticMeshComponent>().MeshHandle = handle;
        const auto newUuid = e.GetComponent<ECS::UUIDComponent>().UUID;
        Core::FoliagePaint::SetActiveOnly( newUuid );
        Core::FoliagePaint::SetEditingType( newUuid );
    }

    void FoliagePaintTool::Paint( ::Desert::Core::Scene& scene, const Common::Math::Ray& ray )
    {
        if ( !Core::FoliagePaint::HasActive() )
            return;

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

            auto& fol = e.GetComponent<ECS::FoliageComponent>();
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

            // Per-type slope filter (uses the brush-centre surface).
            if ( slopeDeg < fol.SlopeMinDeg || slopeDeg > fol.SlopeMaxDeg )
                continue;

            const int count = glm::max(
                 1, static_cast<int>( std::round( fol.Density * Core::FoliagePaint::PaintDensity() ) ) );
            for ( int i = 0; i < count; ++i )
            {
                const float ang = u01( rng ) * 6.2831853f;
                const float rr  = radius * std::sqrt( u01( rng ) ); // uniform disk
                glm::vec3   pos = hitPos + glm::vec3( std::cos( ang ) * rr, 0.0f, std::sin( ang ) * rr );
                pos.y += glm::mix( fol.ZOffsetMin, fol.ZOffsetMax, u01( rng ) );
                const float scl = glm::mix( fol.ScaleMin, fol.ScaleMax, u01( rng ) );

                glm::mat4 m = glm::translate( glm::mat4( 1.0f ), pos );
                if ( fol.AlignToNormal )
                    m *= glm::mat4_cast( AlignUpToNormal( hitN ) );
                if ( fol.RandomYaw )
                    m = glm::rotate( m, u01( rng ) * 6.2831853f, glm::vec3( 0.0f, 1.0f, 0.0f ) );
                if ( fol.MaxPitchDeg > 0.01f )
                {
                    const float     pitch = glm::radians( fol.MaxPitchDeg ) * u01( rng );
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

        ImGui::Button( ICON_MDI_PLUS " Add Foliage Type  (drop a mesh)", ImVec2( -1, 0 ) );
        if ( ImGui::BeginDragDropTarget() )
        {
            if ( const ImGuiPayload* p = ImGui::AcceptDragDropPayload( ::Desert::Editor::DragPayloads::MeshAsset ) )
                CreateType( scene, assetManager, std::string( static_cast<const char*>( p->Data ) ) );
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
            ImGui::TextDisabled( "Drag a mesh here or from a Collection." );
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
                auto& f = ref->get().GetComponent<ECS::FoliageComponent>();
                ImGui::Dummy( ImVec2( 0, 2 ) );
                ImGui::TextDisabled( "TYPE SETTINGS" );

                ImGui::SetNextItemWidth( -1 );
                ImGui::SliderFloat( "##Density", &f.Density, 1.0f, 80.0f, "Density:  %.0f / dab" );

                ImGui::SetNextItemWidth( ImGui::GetContentRegionAvail().x * 0.5f - 4.0f );
                ImGui::DragFloatRange2( "##Scale", &f.ScaleMin, &f.ScaleMax, 0.01f, 0.02f, 10.0f, "Scale %.2f",
                                        "%.2f" );
                ImGui::SetNextItemWidth( -1 );
                ImGui::DragFloatRange2( "##ZOffset", &f.ZOffsetMin, &f.ZOffsetMax, 0.01f, -2.0f, 2.0f,
                                        "Z Off %.2f", "%.2f" );
                ImGui::SetNextItemWidth( -1 );
                ImGui::SliderFloat( "##Pitch", &f.MaxPitchDeg, 0.0f, 45.0f, "Random Tilt:  %.0f deg" );
                ImGui::SetNextItemWidth( -1 );
                ImGui::DragFloatRange2( "##Slope", &f.SlopeMinDeg, &f.SlopeMaxDeg, 0.5f, 0.0f, 90.0f, "Slope %.0f",
                                        "%.0f deg" );

                ImGui::Checkbox( "Align to Normal", &f.AlignToNormal );
                ImGui::SameLine();
                ImGui::Checkbox( "Random Yaw", &f.RandomYaw );

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
