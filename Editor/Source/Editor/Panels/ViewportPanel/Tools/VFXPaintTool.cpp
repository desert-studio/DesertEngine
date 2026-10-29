#include "VFXPaintTool.hpp"

#include <Editor/Core/CommandHistory.hpp>
#include <Editor/Core/CommandPalette.hpp>
#include <Editor/Core/Selection/SelectionManager.hpp>
#include <Editor/Core/Selection/ViewportMode.hpp>
#include <Editor/Core/ToastManager.hpp>
#include <Editor/Panels/ViewportPanel/Tools/FoliageBrush.hpp>
#include <Engine/Assets/Serialization/FoliageType.hpp>
#include <Engine/ECS/Components.hpp>
#include <Common/Core/Logger.hpp>

#include <ImGui/imgui.h>

#include <algorithm>
#include <format>
#include <memory>
#include <utility>

namespace Desert::Editor::Tools
{
    namespace
    {
        /// The set named @p name of @p vfx, made empty when absent.
        ECS::VFXPointSet& SetOf( ECS::VFXComponentData& vfx, const std::string& name )
        {
            for ( ECS::VFXPointSet& s : vfx.PointSets )
                if ( s.Name == name )
                    return s;
            vfx.PointSets.push_back( ECS::VFXPointSet{ name, {} } );
            return vfx.PointSets.back();
        }

        /// One press of the Painted Set brush, undone and redone as a whole (UE: one FScopedTransaction).
        class VFXPointSetStrokeCommand final : public ICommand
        {
        public:
            VFXPointSetStrokeCommand( ::Desert::Core::Scene& scene, Common::UUID entity, std::string set,
                                      std::vector<glm::vec4> before, std::vector<glm::vec4> after, bool hadSet )
                 : m_Scene( &scene ), m_Entity( entity ), m_Set( std::move( set ) ), m_Before( std::move( before ) ),
                   m_After( std::move( after ) ), m_HadSet( hadSet )
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
                return std::format( "VFX Paint {}", m_Set );
            }

        private:
            bool Write( bool before )
            {
                const auto ref = m_Scene->FindEntityByID( m_Entity );
                if ( !ref || !ref->get().HasComponent<ECS::VFXComponent>() )
                {
                    ToastManager::Push( "VFX Paint undo: the painted VFX entity is gone", ToastLevel::Error, 6.0f );
                    return false;
                }
                auto& data = ref->get().GetComponent<ECS::VFXComponent>().Data;
                if ( before && !m_HadSet )
                    std::erase_if( data.PointSets, [&]( const ECS::VFXPointSet& s ) { return s.Name == m_Set; } );
                else
                    SetOf( data, m_Set ).Points = before ? m_Before : m_After;
                return true;
            }

            ::Desert::Core::Scene* m_Scene;
            Common::UUID           m_Entity;
            std::string            m_Set;
            std::vector<glm::vec4> m_Before;
            std::vector<glm::vec4> m_After;
            bool                   m_HadSet;
        };
    } // namespace

    VFXPaintTool::Settings& VFXPaintTool::Brush()
    {
        static Settings settings;
        return settings;
    }

    Common::ResultStr<Common::UUID> VFXPaintTool::Target( ::Desert::Core::Scene& scene )
    {
        const auto& selected = Core::SelectionManager::GetSelected();
        if ( !selected.has_value() )
            return Common::MakeError<Common::UUID>( "VFX Paint: select the VFX entity to paint a point set for" );
        const auto ref = scene.FindEntityByID( *selected );
        if ( !ref || !ref->get().HasComponent<ECS::VFXComponent>() )
            return Common::MakeError<Common::UUID>( "VFX Paint: the selected entity has no VFX component" );
        return Common::MakeSuccess( *selected );
    }

    void VFXPaintTool::Update( ::Desert::Core::Scene& scene, const Common::Math::Ray& ray, const bool pressing )
    {
        if ( !pressing )
        {
            if ( !m_Stroke )
                return;
            Stroke stroke = std::move( *m_Stroke );
            m_Stroke.reset();
            const auto ref = scene.FindEntityByID( stroke.Entity );
            if ( !ref || !ref->get().HasComponent<ECS::VFXComponent>() )
                return;
            const auto* set = ref->get().GetComponent<ECS::VFXComponent>().Data.FindPointSet( Brush().Set );
            if ( set == nullptr || set->Points == stroke.Before )
                return; // the stroke placed nothing: no undo step
            LOG_INFO( "[VFX Paint] stroke on entity {}: {} dab(s), set '{}' {} -> {} points",
                      static_cast<uint64_t>( stroke.Entity ), stroke.Dabs, Brush().Set, stroke.Before.size(),
                      set->Points.size() );
            CommandHistory::Get().PushCommand( std::make_unique<VFXPointSetStrokeCommand>(
                 scene, stroke.Entity, Brush().Set, std::move( stroke.Before ), set->Points, stroke.HadSet ) );
            return;
        }

        if ( !m_Stroke )
        {
            const auto target = Target( scene );
            if ( !target.IsSuccess() )
            {
                ToastManager::Push( target.GetError(), ToastLevel::Warning, 4.0f );
                m_Stroke = Stroke{}; // the press is consumed; nothing is painted until release
                m_Stroke->Entity = Common::UUID( 0 );
                return;
            }
            Stroke stroke;
            stroke.Entity    = target.GetValue();
            const auto& data = scene.FindEntityByID( stroke.Entity )->get().GetComponent<ECS::VFXComponent>().Data;
            if ( const auto* set = data.FindPointSet( Brush().Set ) )
            {
                stroke.Before = set->Points;
                stroke.HadSet = true;
            }
            stroke.Seed = 0x5646583135ull + ++m_Strokes; // one stream per stroke: the stroke replays the same
            m_Stroke    = std::move( stroke );
        }
        if ( static_cast<uint64_t>( m_Stroke->Entity ) == 0 )
            return;
        const auto ref = scene.FindEntityByID( m_Stroke->Entity );
        if ( !ref || !ref->get().HasComponent<ECS::VFXComponent>() )
            return;

        ::Desert::Core::RaycastHit hit;
        if ( !scene.Raycast( ray, hit ) )
            return;

        // The Foliage brush's dab (UE AddInstancesForBrush): candidates traced through the brush sphere onto
        // the surfaces, topped up only to the density the disc should hold, so painting over a spot again adds
        // nothing. The set's points are its "instances".
        auto&           entity  = ref->get();
        const glm::mat4 toWorld = entity.GetWorldTransform();
        const glm::mat4 toLocal = glm::inverse( toWorld );
        auto&           set     = SetOf( entity.GetComponent<ECS::VFXComponent>().Data, Brush().Set );

        std::vector<glm::mat4> existing;
        existing.reserve( set.Points.size() );
        for ( const glm::vec4& p : set.Points )
        {
            glm::mat4 m( 1.0f );
            m[3] = toWorld * glm::vec4( glm::vec3( p ), 1.0f );
            existing.push_back( m );
        }
        Assets::Serialization::FoliageTypeData type;
        type.Density = Brush().Density * 100.0f; // per 1000 x 1000 cm (UE FoliageType Density)
        FoliageBrushDab dab;
        dab.Center = hit.Point;
        dab.Normal = hit.Normal;
        dab.Radius = Brush().Radius;
        FoliageBrushWorld world;
        world.Trace = [&scene]( const glm::vec3& start, const glm::vec3& end,
                                const FoliageSurfaceFilter& ) -> std::optional<FoliageTraceHit>
        {
            const glm::vec3 d   = end - start;
            const float     len = glm::length( d );
            ::Desert::Core::RaycastHit h;
            if ( len <= 0.0f || !scene.Raycast( Common::Math::Ray( start, d / len ), h ) || h.Distance > len )
                return std::nullopt;
            FoliageTraceHit out;
            out.Point   = h.Point;
            out.Normal  = h.Normal;
            out.Surface = FoliageSurface::StaticMesh;
            return out;
        };
        world.LayerWeightAt = []( const glm::vec3& ) -> std::optional<float> { return std::nullopt; };
        FoliageRandom rng( m_Stroke->Seed + m_Stroke->Dabs++ );
        for ( const glm::mat4& placed : FoliageBrushAdd( type, dab, existing, rng, world ) )
            set.Points.push_back( glm::vec4( glm::vec3( toLocal * placed[3] ), 1.0f ) );
    }

    void VFXPaintTool::DrawPanel( ::Desert::Core::Scene& scene, const glm::vec2& viewportPos )
    {
        ImGui::SetNextWindowPos( ImVec2( viewportPos.x + 12.0f, viewportPos.y + 48.0f ), ImGuiCond_Always );
        ImGui::SetNextWindowBgAlpha( 0.9f );
        if ( ImGui::Begin( "VFX Paint##mode", nullptr,
                           ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing ) )
        {
            ImGui::TextDisabled( "VFX PAINT  ·  Painted Set" );
            ImGui::Separator();
            const auto target = Target( scene );
            if ( !target.IsSuccess() )
                ImGui::TextColored( ImVec4( 0.9f, 0.6f, 0.3f, 1.0f ), "%s", target.GetError().c_str() );
            else
            {
                const auto& entity = scene.FindEntityByID( target.GetValue() )->get();
                const auto* set    = entity.GetComponent<ECS::VFXComponent>().Data.FindPointSet( Brush().Set );
                ImGui::Text( "Component  %s", entity.GetComponent<ECS::TagComponent>().Tag.c_str() );
                ImGui::Text( "Point set  %s  ·  %zu points", Brush().Set.c_str(), set ? set->Points.size() : 0u );
            }
            ImGui::SetNextItemWidth( 160.0f );
            ImGui::DragFloat( "Radius", &Brush().Radius, 1.0f, 1.0f, 2000.0f, "%.0f cm" );
            ImGui::SetNextItemWidth( 160.0f );
            ImGui::DragFloat( "Density", &Brush().Density, 0.5f, 0.1f, 1000.0f, "%.1f / m²" );
        }
        ImGui::End();
    }

    void VFXPaintTool::DrawPoints( ::Desert::Core::Scene& scene, const glm::mat4& viewProjection,
                                   const glm::vec2& viewportPos, const glm::vec2& viewportSize )
    {
        const auto target = Target( scene );
        if ( !target.IsSuccess() )
            return;
        const auto&     entity  = scene.FindEntityByID( target.GetValue() )->get();
        const glm::mat4 toWorld = entity.GetWorldTransform();
        ImDrawList*     dl      = ImGui::GetWindowDrawList();
        for ( const ECS::VFXPointSet& set : entity.GetComponent<ECS::VFXComponent>().Data.PointSets )
            for ( const glm::vec4& p : set.Points )
            {
                const glm::vec4 clip = viewProjection * ( toWorld * glm::vec4( glm::vec3( p ), 1.0f ) );
                if ( clip.w <= 0.0f )
                    continue;
                const ImVec2 at( viewportPos.x + ( clip.x / clip.w * 0.5f + 0.5f ) * viewportSize.x,
                                 viewportPos.y + ( 0.5f - clip.y / clip.w * 0.5f ) * viewportSize.y );
                dl->AddCircleFilled( at, 3.0f, IM_COL32( 0, 0, 0, 200 ), 8 );
                dl->AddCircleFilled( at, 2.0f, IM_COL32( 110, 160, 230, 255 ), 8 );
            }
    }

    void VFXPaintTool::AppendCommands( std::vector<PaletteCommand>& commands )
    {
        commands.push_back( { "VFX", "VFX Paint mode", []
                              {
                                  Core::ViewportMode::Set( Core::EditorMode::VFXPaint );
                                  return PaletteCommandDone();
                              } } );
        commands.push_back( { "VFX", "VFX Paint: density high", []
                              {
                                  Brush().Density = kDensityHigh;
                                  return PaletteCommandDone();
                              } } );
        commands.push_back( { "VFX", "VFX Paint: density low", []
                              {
                                  Brush().Density = kDensityLow;
                                  return PaletteCommandDone();
                              } } );
        for ( const int radius : { 50, 100, 300 } )
            commands.push_back( { "VFX", std::format( "VFX Paint: radius {} cm", radius ), [radius]
                                  {
                                      Brush().Radius = static_cast<float>( radius );
                                      return PaletteCommandDone();
                                  } } );
    }
} // namespace Desert::Editor::Tools
