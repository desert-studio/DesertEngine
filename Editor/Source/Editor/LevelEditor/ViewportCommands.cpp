#include "ViewportCommands.hpp"

#include <Editor/Core/EditorPreferences.hpp>
#include <Editor/Core/GizmoState.hpp>
#include <Editor/Core/Selection/AuthoringContext.hpp>
#include <Editor/Panels/ViewportPanel/CameraPilot.hpp>
#include <Editor/Panels/ViewportPanel/ViewportPanel.hpp>

#include <cstdio>
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
