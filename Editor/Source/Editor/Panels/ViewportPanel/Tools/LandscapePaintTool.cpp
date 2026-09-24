// Ported from UE 5.8 Engine/Source/Editor/LandscapeEditor/Private/LandscapeEdModePaintTools.cpp:50-480
// (FLandscapeToolStrokePaint / FLandscapeToolPaint) via LandscapePaint.hpp, adapted: FEdModeLandscape's stroke
// lifetime (press / per-frame apply / release) and its one transaction per stroke become an ImGui-driven tool and
// a CommandHistory entry; the target layer is a name in the root's LandscapeComponent::Layers, not a ULayerInfo.

#include "LandscapePaintTool.hpp"

#include <Editor/Core/CommandHistory.hpp>
#include <Editor/Core/ToastManager.hpp>

#include <Engine/ECS/LandscapeRootOf.hpp>
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

        /// One press of the brush, undone and redone as a whole (UE: one FScopedTransaction per stroke).
        class LandscapePaintCommand final : public ICommand
        {
        public:
            LandscapePaintCommand( ::Desert::Core::Scene& scene, const Common::UUID& landscape,
                                   World::Landscape::LandscapePaintRecord record, std::string label )
                 : m_Scene( &scene ), m_Landscape( landscape ), m_Record( std::move( record ) ),
                   m_Label( std::move( label ) )
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
                auto target = ECS::FindLandscapeEditTarget( m_Scene->GetRegistry(), m_Landscape );
                if ( !target.IsSuccess() )
                {
                    ToastManager::Push( target.GetError(), ToastLevel::Error, 6.0f );
                    return false;
                }
                auto written =
                     World::Landscape::ApplyLandscapePaintRecord( target.GetValue().Lookup, m_Record, before );
                if ( !written.IsSuccess() )
                    ToastManager::Push( written.GetError(), ToastLevel::Error, 6.0f );
                return written.IsSuccess();
            }

            ::Desert::Core::Scene*                 m_Scene;
            Common::UUID                           m_Landscape;
            World::Landscape::LandscapePaintRecord m_Record;
            std::string                            m_Label;
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

    Common::BoolResultStr LandscapePaintTool::Begin( ::Desert::Core::Scene& scene )
    {
        auto&      registry  = scene.GetRegistry();
        const auto landscape = ECS::FirstLandscape( registry );
        if ( !landscape )
            return Common::MakeError( "landscape paint: the scene has no landscape" );
        const auto& paint = Core::LandscapeSculptState::Get().Paint;
        if ( paint.Layer.empty() )
            return Common::MakeError(
                 "landscape paint: no target layer is selected; pick one under Target Layers" );
        const auto rootEntity = ECS::FindLandscapeRootEntity( registry, *landscape );
        if ( rootEntity == entt::null )
            return Common::MakeError( "landscape paint: the landscape root is not loaded" );
        auto rules  = ECS::LandscapeLayerRulesOf( registry.get<ECS::LandscapeComponent>( rootEntity ) );
        auto target = ECS::FindLandscapeEditTarget( registry, *landscape );
        if ( !target.IsSuccess() )
            return Common::MakeError( target.GetError() );
        m_Target = target.GetValue();
        m_Stroke.emplace( m_Target->Root, m_Target->Lookup, std::move( rules ) );
        m_Failed = false;
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr LandscapePaintTool::Step( ::Desert::Core::Scene& scene, const Common::Math::Ray& ray,
                                                    bool invert )
    {
        const auto point = TraceLandscape( scene, m_Target->Landscape, ray );
        if ( !point )
            return Common::MakeSuccess( true ); // UE: no landscape under the cursor, no application this frame
        const auto&                    state     = Core::LandscapeSculptState::Get();
        const std::array<glm::vec2, 1> positions = { glm::vec2( point->x, point->z ) };
        auto weights = World::Landscape::ComputeLandscapeBrush( m_Target->Root, state.Settings.Brush, positions );
        if ( !weights.IsSuccess() )
            return Common::MakeError( weights.GetError() );
        return m_Stroke->Apply( weights.GetValue(), state.Settings.Brush, state.Paint, invert );
    }

    void LandscapePaintTool::End( ::Desert::Core::Scene& scene )
    {
        if ( m_Stroke && m_Stroke->Touched() )
        {
            auto record = m_Stroke->Finish();
            if ( !record.IsSuccess() )
                ToastManager::Push( record.GetError(), ToastLevel::Error, 6.0f );
            else
            {
                const std::string label = "Landscape Paint " + Core::LandscapeSculptState::Get().Paint.Layer;
                CommandHistory::Get().PushCommand( std::make_unique<LandscapePaintCommand>(
                     scene, m_Target->Landscape, record.GetValue(), label ) );
            }
        }
        m_Stroke.reset();
        m_Target.reset();
    }

    void LandscapePaintTool::Update( ::Desert::Core::Scene& scene, const Common::Math::Ray& mouseRay,
                                     const Common::Math::Ray& centreRay, bool hovered )
    {
        auto& state = Core::LandscapeSculptState::Get();
        // A palette Raise / Lower is one application at the viewport centre: Raise paints, Lower erases.
        if ( !m_Stroke && ( state.Request == Core::LandscapeStrokeRequest::Raise ||
                            state.Request == Core::LandscapeStrokeRequest::Lower ) )
        {
            const bool erase = state.Request == Core::LandscapeStrokeRequest::Lower;
            state.Request    = Core::LandscapeStrokeRequest::None;
            auto begun       = Begin( scene );
            auto stepped     = begun.IsSuccess() ? Step( scene, centreRay, erase ) : begun;
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
        auto stepped = Step( scene, mouseRay, ImGui::GetIO().KeyShift );
        if ( !stepped.IsSuccess() && !m_Failed )
        {
            // One toast per stroke: the step is refused every frame the brush stays over the same tile.
            ToastManager::Push( stepped.GetError(), ToastLevel::Error, 6.0f );
            m_Failed = true;
        }
    }
} // namespace Desert::Editor::Tools
