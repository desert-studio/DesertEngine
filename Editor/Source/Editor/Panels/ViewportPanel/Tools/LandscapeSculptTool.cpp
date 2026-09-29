// Ported from UE 5.8 Engine/Source/Editor/LandscapeEditor/Private/LandscapeEdModePaintTools.cpp:593-1000 via
// LandscapeSculpt.hpp, adapted: FEdModeLandscape's stroke lifetime (press / per-frame apply / release) and its
// one transaction per stroke become an ImGui-driven tool and a CommandHistory entry; Slate becomes ImGui.

#include "LandscapeSculptTool.hpp"

#include <Editor/Core/CommandHistory.hpp>
#include <Editor/Core/Selection/LandscapeSculptState.hpp>
#include <Editor/Core/ToastManager.hpp>
#include <Engine/ECS/LandscapeLayerRules.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

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
                                    World::Landscape::LandscapeStrokeRecord record, std::string label,
                                    World::Landscape::LandscapeEditLayerTarget layer )
                 : m_Scene( &scene ), m_Landscape( landscape ), m_Record( std::move( record ) ),
                   m_Layer( std::move( layer ) ), m_Label( std::move( label ) )
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
                auto        written =
                     World::Landscape::WriteLandscapeHeights( t.Root, t.Lookup, m_Record.Rect, values, m_Layer );
                if ( !written.IsSuccess() )
                    ToastManager::Push( written.GetError(), ToastLevel::Error, 6.0f );
                return written.IsSuccess();
            }

            ::Desert::Core::Scene*                  m_Scene;
            Common::UUID                            m_Landscape;
            World::Landscape::LandscapeStrokeRecord m_Record;
            World::Landscape::LandscapeEditLayerTarget m_Layer;
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

        /// The ramp point pick's reach, radians from the cursor ray: about the gizmo dot's radius at a common
        /// field of view (0.02 rad is ~18 px of a 1080 px, 60-degree view).
        constexpr float kRampPickRadians = 0.02f;

        /// One placement or drag of the ramp's points, undone as a whole. UE keeps the points on the tool
        /// outside the transaction buffer; ours are undoable so a misplaced drag is one Ctrl+Z.
        class RampPointsCommand final : public ICommand
        {
        public:
            RampPointsCommand( const World::Landscape::LandscapeRampPoints& before,
                               const World::Landscape::LandscapeRampPoints& after )
                 : m_Before( before ), m_After( after )
            {
            }
            bool Undo() override
            {
                Core::LandscapeSculptState::Get().RampPoints = m_Before;
                return true;
            }
            bool Redo() override
            {
                Core::LandscapeSculptState::Get().RampPoints = m_After;
                return true;
            }
            std::string GetLabel() const override
            {
                return "Landscape Ramp points";
            }

        private:
            World::Landscape::LandscapeRampPoints m_Before;
            World::Landscape::LandscapeRampPoints m_After;
        };

        /// Records @p before -> the state's current points as one undo step; nothing when they did not change.
        void RecordRampPoints( World::Landscape::LandscapeRampPoints before )
        {
            auto after    = Core::LandscapeSculptState::Get().RampPoints;
            before.Moving = false;
            after.Moving  = false;
            if ( before == after )
                return;
            CommandHistory::Get().PushCommand( std::make_unique<RampPointsCommand>( before, after ) );
        }

        std::optional<glm::vec3> RampHit( ::Desert::Core::Scene& scene, const Common::Math::Ray& ray )
        {
            const auto landscape = ECS::FirstLandscape( scene.GetRegistry() );
            return landscape ? TraceLandscape( scene, *landscape, ray ) : std::nullopt;
        }
    } // namespace

    Common::BoolResultStr LandscapeSculptTool::Begin( ::Desert::Core::Scene& scene )
    {
        const auto landscape = ECS::FirstLandscape( scene.GetRegistry() );
        if ( !landscape )
            return Common::MakeError( "landscape sculpt: the scene has no landscape" );
        auto target = ECS::FindLandscapeEditTarget( scene.GetRegistry(), *landscape );
        if ( !target.IsSuccess() )
            return Common::MakeError( target.GetError() );
        auto rootEntity = ECS::FindLandscapeRootEntity( scene.GetRegistry(), *landscape );
        auto rules = ECS::LandscapeLayerRulesOf( scene.GetRegistry().get<ECS::LandscapeComponent>( rootEntity ),
                                                 *Runtime::ResourceRegistry::GetLandscapeLayerInfoService() );
        if ( !rules )
            return Common::MakeError( "landscape sculpt: " + rules.GetError() );
        auto layer = ECS::FindLandscapeEditLayerTarget( scene.GetRegistry(), *landscape, rules.ExtractValue(),
                                                        Core::LandscapeSculptState::Get().EditingLayer );
        if ( !layer )
            return Common::MakeError( layer.GetError() );
        m_Layer  = layer.ExtractValue();
        m_Target = target.GetValue();
        m_Stroke.emplace( m_Target->Root, m_Target->Lookup, m_Target->Bounds, *m_Layer );
        m_ToolName = Core::LandscapeToolName( Core::LandscapeSculptState::Get().Settings.Tool );
        m_Failed   = false;
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
        switch ( settings.Tool )
        {
            case Core::LandscapeTool::Sculpt:
                return m_Stroke->ApplySculpt( weights.GetValue(), settings.Brush, { invert, deltaSeconds } );
            case Core::LandscapeTool::Smooth:
                return m_Stroke->ApplySmooth( weights.GetValue(), settings.Brush, settings.Smooth );
            case Core::LandscapeTool::Flatten:
                return m_Stroke->ApplyFlatten( weights.GetValue(), settings.Brush, settings.Flatten,
                                               positions[0] );
            case Core::LandscapeTool::Noise:
                return m_Stroke->ApplyNoise( weights.GetValue(), settings.Brush, settings.Noise );
            case Core::LandscapeTool::Erase:
                return m_Stroke->ApplyErase( weights.GetValue(), settings.Brush );
            case Core::LandscapeTool::Ramp:
                return Common::MakeError( "landscape ramp: the ramp is not stroked; set its two points and run "
                                          "'Landscape: Ramp: apply'" );
            case Core::LandscapeTool::Erosion:
                return m_Stroke->ApplyErosion( weights.GetValue(), settings.Brush, settings.Erosion );
            case Core::LandscapeTool::HydroErosion:
                return m_Stroke->ApplyHydroErosion( weights.GetValue(), settings.Brush, settings.HydroErosion );
            case Core::LandscapeTool::Mirror:
            case Core::LandscapeTool::CopyPaste:
                return Common::MakeError( "landscape mirror / copy-paste: not stroked; use their rows" );
        }
        return Common::MakeError( "landscape stroke: unknown tool" );
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
                const std::string label = std::string( "Landscape " ) + m_ToolName;
                CommandHistory::Get().PushCommand( std::make_unique<LandscapeStrokeCommand>(
                     scene, m_Target->Landscape, record.GetValue(), label, *m_Layer ) );
            }
        }
        m_Stroke.reset();
        m_Target.reset();
    }

    void LandscapeSculptTool::SetRampPoint( ::Desert::Core::Scene& scene, const Common::Math::Ray& ray,
                                            bool start )
    {
        auto&      ramp  = Core::LandscapeSculptState::Get().RampPoints;
        const auto point = RampHit( scene, ray );
        if ( !point )
        {
            ToastManager::Push( "landscape ramp: no landscape under the ray", ToastLevel::Error, 6.0f );
            return;
        }
        if ( !start && ramp.NumPoints == 0 )
        {
            ToastManager::Push( "landscape ramp: set the start first", ToastLevel::Error, 6.0f );
            return;
        }
        const auto before = ramp;
        // A palette point is a click: the start begins the ramp over, the end is the second point laid or moved.
        if ( start )
            World::Landscape::LandscapeRampReset( ramp );
        else if ( ramp.NumPoints == 2 )
            ramp.SelectedPoint = 1;
        World::Landscape::LandscapeRampPress( ramp, -1, point );
        World::Landscape::LandscapeRampRelease( ramp );
        RecordRampPoints( before );
    }

    void LandscapeSculptTool::UpdateRamp( ::Desert::Core::Scene& scene, const Common::Math::Ray& mouseRay,
                                          bool hovered )
    {
        auto& ramp = Core::LandscapeSculptState::Get().RampPoints;
        if ( m_RampAtPress )
        {
            // UE: the release ends the move (InputKey's IE_Released / EndTool); the whole press is one step.
            if ( !ImGui::IsMouseDown( ImGuiMouseButton_Left ) )
            {
                World::Landscape::LandscapeRampRelease( ramp );
                RecordRampPoints( *m_RampAtPress );
                m_RampAtPress.reset();
                return;
            }
            // UE's MouseMove: only a moved cursor moves the point, so a click without a drag lays one point.
            const ImVec2 delta = ImGui::GetIO().MouseDelta;
            if ( delta.x != 0.0f || delta.y != 0.0f )
                World::Landscape::LandscapeRampMove( ramp, RampHit( scene, mouseRay ) );
            return;
        }
        if ( !hovered || !ImGui::IsMouseClicked( ImGuiMouseButton_Left ) || ImGui::IsAnyItemActive() )
            return;
        const auto    before = ramp;
        const int32_t picked = World::Landscape::PickLandscapeRampPoint( ramp, mouseRay.Origin, mouseRay.Direction,
                                                                         kRampPickRadians );
        if ( World::Landscape::LandscapeRampPress( ramp, picked,
                                                   picked >= 0 ? std::nullopt : RampHit( scene, mouseRay ) ) )
            m_RampAtPress = before;
    }

    bool LandscapeSculptTool::ServeRampRequest( ::Desert::Core::Scene& scene, const Common::Math::Ray& centreRay )
    {
        auto&                              state   = Core::LandscapeSculptState::Get();
        const Core::LandscapeStrokeRequest request = state.Request;
        switch ( request )
        {
            case Core::LandscapeStrokeRequest::RampStart:
            case Core::LandscapeStrokeRequest::RampEnd:
                state.Request = Core::LandscapeStrokeRequest::None;
                SetRampPoint( scene, centreRay, request == Core::LandscapeStrokeRequest::RampStart );
                return true;
            case Core::LandscapeStrokeRequest::RampReset:
            {
                state.Request     = Core::LandscapeStrokeRequest::None;
                const auto before = state.RampPoints;
                World::Landscape::LandscapeRampReset( state.RampPoints );
                RecordRampPoints( before );
                return true;
            }
            case Core::LandscapeStrokeRequest::RampApply:
                break;
            case Core::LandscapeStrokeRequest::MirrorPoint:
            case Core::LandscapeStrokeRequest::MirrorApply:
            case Core::LandscapeStrokeRequest::CopyCornerA:
            case Core::LandscapeStrokeRequest::CopyCornerB:
            case Core::LandscapeStrokeRequest::Copy:
            case Core::LandscapeStrokeRequest::Paste:
                state.Request = Core::LandscapeStrokeRequest::None;
                ServeComponentRequest( scene, centreRay, request );
                return true;
            case Core::LandscapeStrokeRequest::None:
            case Core::LandscapeStrokeRequest::Raise:
            case Core::LandscapeStrokeRequest::Lower:
                return false;
        }
        state.Request = Core::LandscapeStrokeRequest::None;
        // UE's CanApplyRamp: exactly two points.
        if ( state.RampPoints.NumPoints < 2 )
        {
            ToastManager::Push( "landscape ramp: set both the start and the end first", ToastLevel::Error, 6.0f );
            return true;
        }
        auto begun   = Begin( scene );
        m_ToolName   = Core::LandscapeToolName( Core::LandscapeTool::Ramp );
        auto applied = begun.IsSuccess() ? m_Stroke->ApplyRamp( state.RampPoints.Points[0],
                                                                state.RampPoints.Points[1], state.Settings.Ramp )
                                         : begun;
        if ( !applied.IsSuccess() )
            ToastManager::Push( applied.GetError(), ToastLevel::Error, 6.0f );
        End( scene );
        return true;
    }

    void LandscapeSculptTool::ServeComponentRequest( ::Desert::Core::Scene& scene, const Common::Math::Ray& ray,
                                                     Core::LandscapeStrokeRequest request )
    {
        using Request = Core::LandscapeStrokeRequest;
        auto& state   = Core::LandscapeSculptState::Get();
        if ( request == Request::MirrorApply )
        {
            auto begun = Begin( scene );
            m_ToolName = Core::LandscapeToolName( Core::LandscapeTool::Mirror );
            auto applied =
                 begun.IsSuccess() ? m_Stroke->ApplyMirror( state.MirrorPoint, state.Settings.Mirror ) : begun;
            if ( !applied.IsSuccess() )
                ToastManager::Push( applied.GetError(), ToastLevel::Error, 6.0f );
            End( scene );
            return;
        }
        if ( request == Request::Copy )
        {
            if ( !state.CopyCornerA || !state.CopyCornerB )
            {
                ToastManager::Push( "landscape copy: set both corners first", ToastLevel::Error, 6.0f );
                return;
            }
            auto begun = Begin( scene );
            auto copied =
                 begun.IsSuccess()
                      ? World::Landscape::CopyLandscapeHeights( m_Target->Root, m_Target->Lookup, m_Target->Bounds,
                                                                *state.CopyCornerA, *state.CopyCornerB )
                      : Common::MakeError<World::Landscape::LandscapeCopyBuffer>( begun.GetError() );
            if ( copied.IsSuccess() )
                state.CopyBuffer = copied.GetValue();
            else
                ToastManager::Push( copied.GetError(), ToastLevel::Error, 6.0f );
            m_Stroke.reset();
            m_Target.reset();
            return;
        }
        const auto landscape = ECS::FirstLandscape( scene.GetRegistry() );
        const auto point     = landscape ? TraceLandscape( scene, *landscape, ray ) : std::nullopt;
        if ( !point )
        {
            ToastManager::Push( "landscape: no landscape under the ray", ToastLevel::Error, 6.0f );
            return;
        }
        if ( request == Request::MirrorPoint )
            state.MirrorPoint = *point;
        else if ( request == Request::CopyCornerA )
            state.CopyCornerA = *point;
        else if ( request == Request::CopyCornerB )
            state.CopyCornerB = *point;
        else if ( request == Request::Paste )
        {
            auto begun   = Begin( scene );
            m_ToolName   = "Paste";
            auto applied = begun.IsSuccess()
                                ? m_Stroke->ApplyPaste( state.CopyBuffer, *point, state.Settings.PasteMode,
                                                        state.Settings.Brush )
                                : begun;
            if ( !applied.IsSuccess() )
                ToastManager::Push( applied.GetError(), ToastLevel::Error, 6.0f );
            End( scene );
        }
    }

    void LandscapeSculptTool::Update( ::Desert::Core::Scene& scene, const Common::Math::Ray& mouseRay,
                                      const Common::Math::Ray& centreRay, bool hovered, float deltaSeconds )
    {
        auto& state = Core::LandscapeSculptState::Get();
        if ( !m_Stroke && ServeRampRequest( scene, centreRay ) )
            return;
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

        // UE's ramp places and drags points instead of stroking; it owns the press from the click to the release.
        if ( state.Settings.Tool == Core::LandscapeTool::Ramp && !m_Stroke )
        {
            UpdateRamp( scene, mouseRay, hovered );
            return;
        }
        m_RampAtPress.reset();
        const bool pressed = hovered && ImGui::IsMouseDown( ImGuiMouseButton_Left ) && !ImGui::IsAnyItemActive();
        if ( !pressed )
        {
            if ( m_Stroke )
                End( scene );
            return;
        }
        if ( state.Settings.Tool == Core::LandscapeTool::Mirror ||
             state.Settings.Tool == Core::LandscapeTool::CopyPaste )
        {
            // A click places the mirror line (UE: the transform widget) or pastes there (UE: the gizmo's spot).
            if ( ImGui::IsMouseClicked( ImGuiMouseButton_Left ) )
                ServeComponentRequest( scene, mouseRay,
                                       state.Settings.Tool == Core::LandscapeTool::Mirror
                                            ? Core::LandscapeStrokeRequest::MirrorPoint
                                            : Core::LandscapeStrokeRequest::Paste );
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
