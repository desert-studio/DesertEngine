// Ported from UE 5.8 Engine/Source/Editor/LandscapeEditor/Private/LandscapeEdModePaintTools.cpp:593-1000 via
// LandscapeSculpt.hpp, adapted: FEdModeLandscape's stroke lifetime (press / per-frame apply / release) and its
// one transaction per stroke become an ImGui-driven tool and a CommandHistory entry; Slate becomes ImGui.

#include "LandscapeSculptTool.hpp"

#include <Editor/Core/CommandHistory.hpp>
#include <Editor/Core/Selection/LandscapeSculptState.hpp>
#include <Editor/Core/ToastManager.hpp>

#include <Engine/World/Landscape/LandscapeRaycast.hpp>

#include <ImGui/imgui.h>

#include <array>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Desert::Editor::Tools
{
    namespace
    {
        constexpr float kTraceDistanceCm = 1.0e7f;

        /// One stroke, undone and redone as a whole (UE: one FScopedTransaction per stroke).
        class LandscapeStrokeCommand final : public ICommand
        {
        public:
            LandscapeStrokeCommand( ::Desert::Core::Scene& scene, const Common::UUID& landscape,
                                    World::Landscape::LandscapeStrokeRecord record, std::string label )
                 : m_Scene( &scene ), m_Landscape( landscape ), m_Record( std::move( record ) ),
                   m_Label( std::move( label ) )
            {
            }

            bool Undo() override
            {
                return Write( m_Record.Before );
            }
            bool Redo() override
            {
                return Write( m_Record.After );
            }
            std::string GetLabel() const override
            {
                return m_Label;
            }

        private:
            bool Write( const std::vector<uint16_t>& values )
            {
                auto target = ECS::FindLandscapeEditTarget( m_Scene->GetRegistry(), m_Landscape );
                if ( !target.IsSuccess() )
                {
                    ToastManager::Push( target.GetError(), ToastLevel::Error, 6.0f );
                    return false;
                }
                const auto& t = target.GetValue();
                auto written  = World::Landscape::WriteLandscapeHeights( t.Root, t.Lookup, m_Record.Rect, values );
                if ( !written.IsSuccess() )
                    ToastManager::Push( written.GetError(), ToastLevel::Error, 6.0f );
                return written.IsSuccess();
            }

            ::Desert::Core::Scene*                  m_Scene;
            Common::UUID                            m_Landscape;
            World::Landscape::LandscapeStrokeRecord m_Record;
            std::string                             m_Label;
        };

        std::optional<glm::vec3> TraceLandscape( ::Desert::Core::Scene& scene, const Common::UUID& landscape,
                                                 const Common::Math::Ray& ray )
        {
            const auto drawable = ECS::DrawableLandscapeTiles( scene.GetRegistry() );
            std::vector<World::Landscape::LandscapeRayTile> tiles;
            for ( const auto& tile : drawable.Tiles )
                if ( tile.Component->Landscape == landscape )
                    tiles.push_back( { &*tile.Component->Heights, tile.Frame } );
            const auto hit =
                 World::Landscape::RaycastLandscape( tiles, ray.Origin, ray.Direction, kTraceDistanceCm );
            if ( !hit )
                return std::nullopt;
            return hit->Point;
        }
    } // namespace

    void LandscapeSculptTool::DrawPanel( const glm::vec2& viewportPos )
    {
        auto& settings = Core::LandscapeSculptState::Get().Settings;
        ImGui::SetNextWindowPos( ImVec2( viewportPos.x + 12.0f, viewportPos.y + 48.0f ), ImGuiCond_Always );
        ImGui::SetNextWindowBgAlpha( 0.9f );
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing;
        if ( !ImGui::Begin( "##LandscapeSculptPanel", nullptr, flags ) )
        {
            ImGui::End();
            return;
        }
        ImGui::TextUnformatted( "Landscape" );
        ImGui::Separator();
        ImGui::Text( "Tool %s   Radius %.0f cm   Falloff %.2f (%s)   Strength %.2f",
                     Core::LandscapeToolName( settings.Tool ), settings.Brush.RadiusCm,
                     settings.Brush.FalloffFraction, Core::LandscapeFalloffName( settings.Brush.Shape ),
                     settings.Brush.Strength );
        if ( settings.Tool == Core::LandscapeTool::Smooth )
            ImGui::Text( "Filter radius %d   Detail smooth %s   Detail scale %.2f",
                         settings.Smooth.FilterKernelRadius, settings.Smooth.DetailSmooth ? "on" : "off",
                         settings.Smooth.DetailScale );

        // Every button is a row of LandscapeToolControls(), which the palette offers too (see that header).
        const char* row = nullptr;
        int         id  = 0;
        for ( const auto& control : Core::LandscapeToolControls() )
        {
            if ( !control.Shown( settings ) )
                continue;
            if ( row == nullptr || std::string( row ) != control.Row )
            {
                row = control.Row;
                ImGui::TextUnformatted( row );
            }
            ImGui::SameLine();
            ImGui::PushID( id++ );
            const std::string text = control.Label.substr( control.Label.find( ':' ) + 2 );
            if ( ImGui::SmallButton( text.c_str() ) )
                control.Apply( settings );
            ImGui::PopID();
        }
        ImGui::TextDisabled( "LMB raises / smooths, Shift+LMB lowers" );
        ImGui::End();
    }

    Common::BoolResultStr LandscapeSculptTool::Begin( ::Desert::Core::Scene& scene )
    {
        const auto landscape = ECS::FirstLandscape( scene.GetRegistry() );
        if ( !landscape )
            return Common::MakeError( "landscape sculpt: the scene has no landscape" );
        auto target = ECS::FindLandscapeEditTarget( scene.GetRegistry(), *landscape );
        if ( !target.IsSuccess() )
            return Common::MakeError( target.GetError() );
        m_Target = target.GetValue();
        m_Stroke.emplace( m_Target->Root, m_Target->Lookup, m_Target->Bounds );
        m_Failed = false;
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr LandscapeSculptTool::Step( ::Desert::Core::Scene& scene, const Common::Math::Ray& ray,
                                                     bool invert, float deltaSeconds )
    {
        const auto point = TraceLandscape( scene, m_Target->Landscape, ray );
        if ( !point )
            return Common::MakeSuccess( true ); // UE: no landscape under the cursor, no application this frame
        const auto&                    settings  = Core::LandscapeSculptState::Get().Settings;
        const std::array<glm::vec2, 1> positions = { glm::vec2( point->x, point->z ) };
        auto weights = World::Landscape::ComputeLandscapeBrush( m_Target->Root, settings.Brush, positions );
        if ( !weights.IsSuccess() )
            return Common::MakeError( weights.GetError() );
        if ( settings.Tool == Core::LandscapeTool::Sculpt )
            return m_Stroke->ApplySculpt( weights.GetValue(), settings.Brush, { invert, deltaSeconds } );
        return m_Stroke->ApplySmooth( weights.GetValue(), settings.Brush, settings.Smooth );
    }

    void LandscapeSculptTool::End( ::Desert::Core::Scene& scene )
    {
        if ( m_Stroke && m_Stroke->Touched() )
        {
            auto record = m_Stroke->Finish();
            if ( !record.IsSuccess() )
                ToastManager::Push( record.GetError(), ToastLevel::Error, 6.0f );
            else if ( record.GetValue().Before != record.GetValue().After )
            {
                const std::string label =
                     std::string( "Landscape " ) +
                     Core::LandscapeToolName( Core::LandscapeSculptState::Get().Settings.Tool );
                CommandHistory::Get().PushCommand( std::make_unique<LandscapeStrokeCommand>(
                     scene, m_Target->Landscape, record.GetValue(), label ) );
            }
        }
        m_Stroke.reset();
        m_Target.reset();
    }

    void LandscapeSculptTool::Update( ::Desert::Core::Scene& scene, const Common::Math::Ray& mouseRay,
                                      const Common::Math::Ray& centreRay, bool hovered, float deltaSeconds )
    {
        auto& state = Core::LandscapeSculptState::Get();
        if ( state.Request != Core::LandscapeStrokeRequest::None && !m_Stroke )
        {
            const bool lower = state.Request == Core::LandscapeStrokeRequest::Lower;
            state.Request    = Core::LandscapeStrokeRequest::None;
            auto begun       = Begin( scene );
            auto stepped     = begun.IsSuccess() ? Step( scene, centreRay, lower,
                                                         World::Landscape::kLandscapeStrokeMaxDeltaSeconds )
                                                 : begun;
            if ( !stepped.IsSuccess() )
                ToastManager::Push( stepped.GetError(), ToastLevel::Error, 6.0f );
            End( scene );
            return;
        }

        const bool pressed = hovered && ImGui::IsMouseDown( ImGuiMouseButton_Left ) && !ImGui::IsAnyItemActive();
        if ( !pressed )
        {
            if ( m_Stroke )
                End( scene );
            return;
        }
        if ( !m_Stroke )
        {
            auto begun = Begin( scene );
            if ( !begun.IsSuccess() )
            {
                if ( !m_Failed )
                    ToastManager::Push( begun.GetError(), ToastLevel::Error, 6.0f );
                m_Failed = true;
                return;
            }
        }
        auto stepped = Step( scene, mouseRay, ImGui::GetIO().KeyShift, deltaSeconds );
        if ( !stepped.IsSuccess() && !m_Failed )
        {
            // One toast per stroke: the step is refused every frame the brush stays over the same tile.
            ToastManager::Push( stepped.GetError(), ToastLevel::Error, 6.0f );
            m_Failed = true;
        }
    }
} // namespace Desert::Editor::Tools
