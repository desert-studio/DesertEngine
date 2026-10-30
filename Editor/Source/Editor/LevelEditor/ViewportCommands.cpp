#include "ViewportCommands.hpp"

#include <Editor/Core/EditorPreferences.hpp>
#include <Editor/Core/GizmoState.hpp>
#include <Editor/Core/Selection/AuthoringContext.hpp>
#include <Editor/Core/Selection/ViewportMode.hpp>
#include <Editor/Panels/ViewportPanel/CameraPilot.hpp>
#include <Editor/Panels/ViewportPanel/ViewportPanel.hpp>

#include <cstdio>
#include <iterator>
#include <string>

namespace Desert::Editor
{
    void AppendViewportCommands( std::vector<PaletteCommand>& commands )
    {
        // THE SNAP, AND THE PERF HUD. Both are things a person does with a single click and neither had a
        // name, so neither could be done unattended — and a gap in this dictionary is a gap in what an
        // agent can do at all, which is the claim the palette exists to make good on. Found by needing
        // them: К6 moved the snap step to one owner and then could not photograph the defect it fixed,
        // because the sequence is "set a step, do something unrelated, look" and the channel could reach
        // neither half. The three toolbar popups and the View -> Show menu were the only ways in.
        //
        // THE STEPS ARE THE TOOLBAR'S OWN LISTS, not a copy: kGridSteps and kAngleSteps are declared once
        // at the top of this file and read by DrawSnapPopup as well, so a step added there appears here
        // and the two can never offer different menus.
        //
        // Labels are ASCII on purpose. A client addresses a command by its exact label over the control
        // channel (`desertctl run Snap "Angle snap 15 deg"`), and the degree sign the toolbar button draws
        // is two UTF-8 bytes that a shell argument carries badly.
        for ( const float step : kGridSteps )
        {
            char label[48];
            if ( step >= 100.0f )
                std::snprintf( label, sizeof( label ), "Grid snap %.0f m", step / 100.0f );
            else
                std::snprintf( label, sizeof( label ), "Grid snap %.0f cm", step );
            commands.push_back( { "Snap", label, [step]
                                  {
                                      Core::GizmoState::SetTranslateSnap( step );
                                      return PaletteCommandDone();
                                  } } );
        }
        for ( const float step : kAngleSteps )
        {
            char label[48];
            std::snprintf( label, sizeof( label ), "Angle snap %.0f deg", step );
            commands.push_back( { "Snap", label, [step]
                                  {
                                      Core::GizmoState::SetRotateSnapDegrees( step );
                                      return PaletteCommandDone();
                                  } } );
        }
        commands.push_back( { "Snap", "Toggle snapping", []
                              {
                                  Core::GizmoState::SetPersistentSnap( !Core::GizmoState::PersistentSnap() );
                                  return PaletteCommandDone();
                              } } );

        // UE's Select Mode (the mode toolbar's first button): leaves Foliage / Modeling / Landscape / Fracture
        // mode the way the toolbar's Select does, so a scenario can close a mode's panel before its frame.
        commands.push_back( { "Mode", "Select mode", []
                              {
                                  Core::ViewportMode::Set( Core::EditorMode::Select );
                                  return PaletteCommandDone();
                              } } );

        // ── THE TRANSFORM TOOLS AND THE SPACE THEY WORK IN ───────────────────────────────────────────
        //
        // FOUND BY NEEDING IT, exactly as the Delete-entity entry above was. Every one of these is a
        // toolbar button and a W/E/R keystroke, and both of those are a HUMAN — so the space toggle could
        // be photographed in one of its two states and the "a locked entity draws no gizmo" claim could
        // not be photographed at all, because nothing without a mouse could put a gizmo on screen first.
        //
        // The same Core::GizmoState setters the buttons call, so these are a second SPELLING of the
        // request and never a second copy of the state.
        {
            using Gz = Core::GizmoState;

            constexpr struct
            {
                const char*   Label;
                Gz::Operation Op;
            } kTools[] = {
                 { "Select (no gizmo)", Gz::Operation::None },
                 { "Move", Gz::Operation::Translate },
                 { "Rotate", Gz::Operation::Rotate },
                 { "Scale", Gz::Operation::Scale },
            };
            for ( const auto& tool : kTools )
            {
                const auto op = tool.Op;
                commands.push_back( { "Transform", tool.Label, [op]
                                      {
                                          Gz::Set( op );
                                          return PaletteCommandDone();
                                      } } );
            }

            // Both spaces are offered by name rather than as one "toggle", because a client that cannot
            // see the button needs to be able to ASK for a state instead of flipping an unknown one.
            constexpr struct
            {
                const char* Label;
                Gz::Space   Space;
            } kSpaces[] = {
                 { "Space: World", Gz::Space::World },
                 { "Space: Local", Gz::Space::Local },
            };
            for ( const auto& choice : kSpaces )
            {
                const auto space = choice.Space;
                commands.push_back( { "Transform", choice.Label, [space]
                                      {
                                          Gz::SetSpace( space );
                                          return PaletteCommandDone();
                                      } } );
            }
        }

        // The View -> Show item, under a name. It is the cheapest action in the editor that saves the
        // preferences file while having nothing whatever to do with the gizmo, which is exactly what makes
        // it the other half of К6's scenario — and it is a dictionary entry in its own right, since
        // "turn the frame timings on" is something a person asks for by name.
        commands.push_back( { "Action", "Toggle the Perf HUD", []
                              {
                                  EditorPreferences::Get().ShowPerfHud = !EditorPreferences::Get().ShowPerfHud;
                                  EditorPreferences::Save();
                                  return PaletteCommandDone();
                              } } );

        commands.push_back( { "Camera", "Eject (stop piloting)", [] { return Editor::EjectPilot(); } } );

        // THE TWO ENDS OF К10's SCENARIO, UNDER NAMES, for the reason К6 named the snap steps and the item
        // above: a scenario whose steps can only be reached by clicking is a scenario no unattended run can
        // walk, and a claim about it is therefore unphotographable. Both are dictionary entries in their own
        // right — "show me the grid" and "switch to 2D" are things a person asks for by name, and UE's own
        // Show > Grid is searchable for the same reason.
        //
        // Note which one saves and which one does not, because that IS К10: the grid is the USER'S ANSWER
        // and persists on the click; 2D UI mode is a VIEWPORT MODE and persists nowhere at all.
        commands.push_back( { "View", "Toggle the grid", []
                              {
                                  auto& view    = EditorPreferences::Get().DebugView;
                                  view.ShowGrid = !view.ShowGrid;
                                  EditorPreferences::Save();
                                  return PaletteCommandDone();
                              } } );
        // The surface-cache card placement under a name: the frame that shows cards covering walls and kerbs has
        // to be taken over the channel.
        commands.push_back( { "View", "Toggle card placement", []
                              {
                                  auto& view             = EditorPreferences::Get().DebugView;
                                  view.ShowCardPlacement = !view.ShowCardPlacement;
                                  EditorPreferences::Save();
                                  return PaletteCommandDone();
                              } } );
        // EVERY BUFFER VIEW UNDER A NAME - UE's `viewmode <name>` / r.Lumen.Visualize from the console: a frame of a
        // debug view is the proof of the pass behind it, and a view reachable only through the toolbar combo
        // cannot be photographed over the channel (a scenario on an external project had no way to see the
        // indirect light alone). One row per DeferredDebugMode, so a new mode is a new row here; the labels of the
        // modes that were reachable before keep their exact text (committed scenarios address them).
        constexpr struct
        {
            const char*                 Label;
            Graphic::DeferredDebugMode Mode;
        } kBufferViews[] = {
            { "View mode: Albedo", Graphic::DeferredDebugMode::Albedo },
            { "View mode: World Normal", Graphic::DeferredDebugMode::Normal },
            { "View mode: Metallic", Graphic::DeferredDebugMode::Metallic },
            { "View mode: Roughness", Graphic::DeferredDebugMode::Roughness },
            { "View mode: Ambient Occlusion", Graphic::DeferredDebugMode::AO },
            { "View mode: Indirect Lighting (GI)", Graphic::DeferredDebugMode::GI },
            { "View mode: Light Complexity", Graphic::DeferredDebugMode::LightComplexity },
            { "View mode: Overdraw", Graphic::DeferredDebugMode::Overdraw },
            { "View mode: Material Complexity", Graphic::DeferredDebugMode::MaterialComplexity },
            { "View mode: Shading Model", Graphic::DeferredDebugMode::ShadingModel },
            { "View mode: Sun Shadow Receive", Graphic::DeferredDebugMode::SunShadowReceive },
            { "View mode: Velocity", Graphic::DeferredDebugMode::Velocity },
            { "View mode: Light Grid (World)", Graphic::DeferredDebugMode::LightGridWorld },
            { "View mode: Global Distance Field", Graphic::DeferredDebugMode::GlobalDistanceField },
            { "View mode: Surface Cache Atlas", Graphic::DeferredDebugMode::SurfaceCacheAtlas },
            { "View mode: Lumen Scene (Cards)", Graphic::DeferredDebugMode::SurfaceCacheScene },
            { "View mode: Lumen Scene (Lit)", Graphic::DeferredDebugMode::SurfaceCacheSceneLit },
            { "View mode: Reflections", Graphic::DeferredDebugMode::Reflections },
            { "View mode: Local Exposure", Graphic::DeferredDebugMode::LocalExposure },
        };
        static_assert( std::size( kBufferViews ) == static_cast<size_t>( Graphic::DeferredDebugMode::LocalExposure ),
                       "a DeferredDebugMode without a View mode command cannot be photographed over the channel" );
        for ( const auto& row : kBufferViews )
        {
            const Graphic::DeferredDebugMode mode = row.Mode;
            commands.push_back( { "View", row.Label, [mode]
                                  {
                                      auto& view = EditorPreferences::Get().DebugView;
                                      view.ClearViewMode();
                                      view.DeferredDebug = mode;
                                      EditorPreferences::Save();
                                      return PaletteCommandDone();
                                  } } );
        }
        // The two directional-shadow views under names, for the same reason: a cascade cache that lags a sun
        // change is proved only by a Shadow Factor / Shadow Cascades frame taken right after the change.
        constexpr struct
        {
            const char*              Label;
            Graphic::ShadowDebugMode Mode;
        } kShadowViews[] = {
             { "View mode: Shadow Factor", Graphic::ShadowDebugMode::ShadowFactor },
             { "View mode: Shadow Cascades", Graphic::ShadowDebugMode::Cascades },
        };
        static_assert( std::size( kShadowViews ) == static_cast<size_t>( Graphic::ShadowDebugMode::Cascades ),
                       "a ShadowDebugMode without a View mode command cannot be photographed over the channel" );
        for ( const auto& row : kShadowViews )
        {
            const Graphic::ShadowDebugMode mode = row.Mode;
            commands.push_back( { "View", row.Label, [mode]
                                  {
                                      auto& view = EditorPreferences::Get().DebugView;
                                      view.ClearViewMode();
                                      view.ShadowDebug = mode;
                                      EditorPreferences::Save();
                                      return PaletteCommandDone();
                                  } } );
        }
        // The virtual-geometry views under names (UE r.Nanite.Visualize): the [vg_clusters] scenario photographs
        // the clusters view over the channel.
        constexpr struct
        {
            const char*                  Label;
            Graphic::VirtualGeometryView Mode;
        } kVirtualGeometryViews[] = {
             { "View mode: Virtual Geometry Clusters", Graphic::VirtualGeometryView::Clusters },
        };
        static_assert(
             std::size( kVirtualGeometryViews ) == static_cast<size_t>( Graphic::VirtualGeometryView::Clusters ),
             "a VirtualGeometryView without a View mode command cannot be photographed over the channel" );
        for ( const auto& row : kVirtualGeometryViews )
        {
            const Graphic::VirtualGeometryView mode = row.Mode;
            commands.push_back( { "View", row.Label, [mode]
                                  {
                                      auto& view = EditorPreferences::Get().DebugView;
                                      view.ClearViewMode();
                                      view.VirtualGeometry = mode;
                                      EditorPreferences::Save();
                                      return PaletteCommandDone();
                                  } } );
        }
        // Lit and Wireframe under names (the dropdown's first two entries): the CubeGrid seam check is "no
        // interior faces", which only a wireframe frame shows, and a mode reachable only by the toolbar
        // combo could not be photographed over the channel. Same channel reset as the dropdown
        // (DebugViewState::ClearViewMode); persisted the way the dropdown persists it.
        constexpr struct
        {
            const char* Label;
            bool        Wireframe;
        } kViewModes[] = { { "View mode: Lit", false }, { "View mode: Wireframe", true } };
        for ( const auto& mode : kViewModes )
        {
            const bool wireframe = mode.Wireframe;
            commands.push_back( { "View", mode.Label, [wireframe]
                                  {
                                      auto& view = EditorPreferences::Get().DebugView;
                                      view.ClearViewMode();
                                      view.WireframeMode = wireframe;
                                      EditorPreferences::Save();
                                      return PaletteCommandDone();
                                  } } );
        }
        // THE VIEWPORT'S FOUR AUTHORING MODES (07 §14.2), and the reason they are palette entries rather
        // than only toolbar segments is Г14's rule applied to this tier: a capability reachable only by a
        // mouse click does not exist for the control channel, so no unattended run could ever photograph
        // the bone overlay or the control shapes — and an overlay whose appearance cannot be checked is
        // exactly the "built, tested and unseen" shape this project keeps paying for. They are VIEWPORT
        // MODES and persist nowhere, like 2D UI mode above and unlike the grid.
        //
        // GENERATED FROM THE MODE TABLE, not typed out: a fifth mode reaches the channel by existing.
        // This replaced one entry, "Toggle the control rig overlay", which flipped a process-wide static
        // directly — it could name no owner, so over the channel it could not be refused and could not
        // say which character it had just started posing.
        for ( const Core::AuthoringMode mode : Core::kAuthoringModes )
        {
            commands.push_back( { "View", std::string( "Viewport mode: " ) + Core::AuthoringModeName( mode ),
                                  [mode] { return Editor::ViewportPanel::RequestAuthoringMode( mode ); } } );
        }
    }
} // namespace Desert::Editor
