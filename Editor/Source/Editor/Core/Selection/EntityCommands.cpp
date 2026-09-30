#include "EntityCommands.hpp"

#include <Editor/Core/AssetOpen.hpp>
#include <Editor/Core/Commands/InstanceFold.hpp>
#include <Editor/Core/Commands/PoseEditTransaction.hpp>
#include <Editor/Core/Commands/SceneCommands.hpp>
#include <Editor/Core/ControlNudgeRequest.hpp>
#include <Editor/Core/Selection/ModelingState.hpp>
#include <Editor/Core/Selection/SelectionManager.hpp>
#include <Editor/Core/SubjectEditorRegistry.hpp>
#include <Editor/Core/SubjectOpenRequest.hpp>
#include <Editor/Panels/SceneProperties/ComponentWidgets/MaterialsPanelComponent.hpp>
#include <Editor/Panels/ViewportPanel/CameraPilot.hpp>
#include <Common/Core/Units.hpp>
#include <Engine/Animation/Animator.hpp>
#include <Engine/Animation/Rig/ControlHierarchy.hpp>
#include <Engine/Animation/Rig/ControlRigStage.hpp>
#include <Engine/Core/EditorCamera.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/EntityLock.hpp>
#include <Engine/Geometry/PrimitiveType.hpp>

#include <format>
#include <string>
#include <utility>

namespace Desert::Editor
{
    EntityCommands::EntityCommands( const std::shared_ptr<::Desert::Core::Scene>& mainScene,
                                    SubjectEditorRegistry& subjectEditors, ActiveCamera activeCamera )
        : m_MainSceneSlot( &mainScene ), m_SubjectEditors( &subjectEditors ), m_ActiveCamera( std::move( activeCamera ) )
    {
    }

    void EntityCommands::Append( std::vector<PaletteCommand>& commands )
    {
        // Entities — select any object in the open scene.
        if ( MainScene() )
        {
            for ( const auto& entity : MainScene()->GetAllEntities() )
            {
                if ( !entity.HasComponent<ECS::UUIDComponent>() )
                    continue;
                const Common::UUID uuid = entity.GetComponent<ECS::UUIDComponent>().UUID;
                std::string        name = entity.HasComponent<ECS::TagComponent>()
                                               ? entity.GetComponent<ECS::TagComponent>().Tag
                                               : std::string( "Entity" );
                commands.push_back( { "Entity", name, [uuid]
                                      {
                                          Core::SelectionManager::SetSelected( uuid );
                                          return PaletteCommandDone();
                                      } } );
                // Ctrl+click: a two-input tool (Boolean, Trim) reads A and B in selection order.
                commands.push_back( { "Entity", "Add to selection " + name, [uuid]
                                      {
                                          Core::SelectionManager::AddToSelection( uuid );
                                          return PaletteCommandDone();
                                      } } );

                // AND ITS EDITORS, because until now there was NO WAY TO OPEN ONE without a mouse. A
                // subject document — the Sequencer, the AnimGraph — is opened by a button in the Details
                // panel, and a button is the one gesture an unattended run cannot make. So every claim
                // about those windows was unphotographable, including the one this task exists to make.
                //
                // Generated from the registry rather than listed, so an editor registered tomorrow is
                // offered here the moment it exists. The two filters are the registry's own: the digest of
                // the registered type name has to BE the key it is registered under (which is what excludes
                // the asset editors, whose subject is a file and not a component), and `Exists` has to say
                // there is something on this entity to open — the same predicate the Details button asks.
                for ( const SubjectTypeKey type : m_SubjectEditors->RegisteredTypes() )
                {
                    const std::string typeName = m_SubjectEditors->TypeName( type );
                    if ( ComponentSubjectType( typeName ) != type )
                    {
                        continue;
                    }
                    const SubjectId subject = ComponentSubject( uuid, typeName );
                    if ( !m_SubjectEditors->Exists( subject ) )
                    {
                        continue;
                    }
                    commands.push_back( { "Open", "Editor: " + typeName + " on " + name, [subject]
                                          {
                                              Core::SubjectOpenRequests::Request( subject );
                                              return PaletteCommandDone();
                                          } } );
                }

                // DELETING ONE IS ALSO SOMETHING A PERSON DOES, and until now the palette could only
                // SELECT. The Outliner's context menu and the Delete key both reach
                // Commands::DeleteEntity — the same undoable command this runs — so the capability
                // was always there and only the dictionary entry was missing.
                //
                // FOUND BY NEEDING IT. Verifying "a document closes with its subject" through the control
                // channel means killing a subject through the control channel, and there was no way to
                // destroy an entity without a mouse: the channel runs these closures and nothing else. A
                // gap in the palette is a gap in what an agent can do at all, which is the one claim the
                // palette exists to make good on.
                commands.push_back( { "Entity", "Delete " + name, [uuid]
                                      {
                                          Commands::DeleteEntity( uuid );
                                          return PaletteCommandDone();
                                      } } );

                // Pilot, the Outliner's and the Details panel's third door: the one an unattended run can
                // open, so "the viewport shows what the camera sees" can be photographed at all.
                if ( entity.HasComponent<ECS::CameraComponent>() )
                {
                    commands.push_back(
                         { "Camera", "Pilot " + name, [uuid] { return Editor::PilotCameraEntity( uuid ); } } );
                }

                // LOCKING ONE IS TOO, and by the paragraph directly above it has to be here. The padlock
                // in the Outliner's gutter and the row's context menu are both a MOUSE, and the lock's
                // whole subject is what the viewport will and will not let you touch — so a channel that
                // cannot set it cannot check it either. Same recursive setter both of those call.
                {
                    // Captures the UUID and re-resolves at RUN time, exactly as Delete above does, rather
                    // than holding a Scene* and an entt handle from build time. The list is rebuilt per
                    // use, so a stale pointer is not reachable today — but "not reachable today" is a
                    // lifetime argument the next reader has to reconstruct, and a UUID lookup that simply
                    // finds nothing needs no argument at all. Asked through the shared predicate, so this
                    // label cannot disagree with the padlock the Outliner draws for the same entity.
                    const bool locked = ECS::IsLocked( MainScene()->GetRegistry(), entity.GetHandle() );
                    commands.push_back( { "Entity", ( locked ? "Unlock " : "Lock " ) + name, [this, uuid, locked]
                                          {
                                              if ( !MainScene() )
                                                  return PaletteCommandDone();
                                              if ( auto ref = MainScene()->FindEntityByID( uuid ) )
                                                  ECS::SetLockedRecursive( MainScene()->GetRegistry(),
                                                                           ref->get().GetHandle(), !locked );
                                              return PaletteCommandDone();
                                          } } );
                }

                // ── AND WHAT CAN BE OPENED *FROM* THIS ENTITY ─────────────────────────────────────────
                //
                // The other half of U7, and the half that makes a component document reachable at all
                // without a mouse. The Details panel's button is how a person opens one; this is the same
                // request under a name, which is what puts it in THE DICTIONARY — the palette, and
                // therefore the control channel, which runs these same closures.
                //
                // A DOCUMENT REACHABLE ONLY BY CLICKING A BUTTON IS MISSING FROM THAT DICTIONARY, and the
                // dictionary is this editor's one claim that "anything a person can do, an agent can do".
                // The asset documents already had their entry (the Open group below, over the registered
                // assets); a subject that is not a file had none, because there was no file to enumerate.
                // Enumerating the ENTITIES against the registered COMPONENT kinds is the same loop over
                // the other domain.
                //
                // DERIVED FROM THE REGISTRY, never a hand-written list of the two kinds that exist today:
                // a third component document appears here the moment its factory is registered, which is
                // the census this task exists to stop anybody having to refill.
                for ( const SubjectTypeKey& type : m_SubjectEditors->RegisteredTypes() )
                {
                    if ( type.Domain != SubjectDomain::EntityComponent )
                        continue;

                    const SubjectId subject{ type.Domain, type.Facet, uuid };
                    if ( !m_SubjectEditors->Exists( subject ) )
                        continue;

                    commands.push_back( { "Open", name + " \xc2\xb7 " + m_SubjectEditors->TypeName( type ),
                                          [subject]
                                          {
                                              Core::SubjectOpenRequests::Request( subject );
                                              return PaletteCommandDone();
                                          } } );
                }
            }
        }

        if ( MainScene() )
        {
            // The material pencil's request, made from the palette: macOS gives the control channel no
            // synthetic click, and this is the same AssetFieldRequests entry the pencil and the field menu use.
            if ( const auto& primary = Core::SelectionManager::GetSelected(); primary.has_value() )
            {
                const Common::UUID owner = *primary;
                commands.push_back(
                     { "Entity", "Open the selected entity's material", [this, owner]
                       {
                           auto ref = MainScene() ? MainScene()->FindEntityByID( owner ) : std::nullopt;
                           if ( !ref )
                               return Common::MakeFormattedError<bool>( "the selection is gone" );
                           ECS::Entity entity =
                                ref->get(); // HostOf takes a mutable entity; the handle copy is cheap
                           const auto host = MaterialComponentWidget::HostOf( entity );
                           if ( host.Slots == nullptr || host.Slots->empty() )
                               return Common::MakeFormattedError<bool>(
                                    "the selected entity has no material slot" );
                           Core::AssetFieldRequests::Request( host.Slots->front(), Core::AssetFieldAction::Open );
                           return PaletteCommandDone();
                       } } );
            }
        }
        // SELECT EVERY PROP THAT MATCHES THIS ONE — UE's "Select > Matching", and the step without which
        // the collapse below has no input. A five-hundred-entity selection is five hundred ctrl-clicks,
        // which is also a gesture no unattended run can make; this turns "pick one crate" into "pick every
        // crate like it". The match is the FOLD'S OWN identity rule, so a selection this builds is never a
        // selection the fold then refuses for a reason nobody can see.
        if ( MainScene() )
        {
            if ( const auto& primary = Core::SelectionManager::GetSelected(); primary.has_value() )
            {
                const Common::UUID seed = *primary;
                commands.push_back( { "Entity", "Select all with the same static mesh", [seed]
                                      {
                                          const size_t selected = Commands::SelectMatchingStaticMeshes( seed );
                                          return PaletteCommandOutcome(
                                               selected > 0,
                                               "The selected entity carries no static mesh to match." );
                                      } } );
            }
        }

        // ── THE CONTROL RIG, UNDER NAMES ─────────────────────────────────────────────────────────────
        //
        // FOUND BY NEEDING IT, exactly as the snap steps and the transform tools above were. Every gesture
        // in `ControlManipulator` is a MOUSE gesture -- enter Control mode with a checkbox, click a shape,
        // drag it -- and synthetic input is closed on this machine at both doors. So `ControlDrag`, which
        // a suite and a census both cover, had never appeared in a single frame: there was no way to put
        // it on screen without a human. Three entries close that, and none of them is a second
        // implementation: the mode goes through the authoring context's own gate, the selection through
        // `SetSelectedControl`, and the nudge through the very `ControlDrag` object the mouse grabs.
        //
        // THE PALETTE IS AN OWNER LIKE ANY OTHER. It takes the authoring context as `Kind::Panel`, so a
        // viewport or a Sequencer that wants it back takes it the same way they take it from each other,
        // and the refusals name who holds it.
        if ( MainScene() )
        {
            if ( const auto& primary = Core::SelectionManager::GetSelected(); primary.has_value() )
            {
                const Common::UUID subject = *primary;
                commands.push_back( { "Control Rig", "Author the control rig on the selection", [this, subject]
                                      {
                                          auto& host                = Core::ActiveAuthoringContext();
                                          m_PaletteAuthoring.Entity = subject;
                                          // Focus FIRST and set the mode after: Focus adopts the published context
                                          // when the entity matches, so a mode written into `mine` beforehand is
                                          // overwritten by whatever the previous holder was in.
                                          (void)host.Focus( m_PaletteAuthoringOwner, m_PaletteAuthoring );
                                          return host.SetMode( m_PaletteAuthoringOwner, m_PaletteAuthoring,
                                                               Core::AuthoringMode::Control );
                                      } } );

                // ONE ENTRY PER CONTROL, built from the rig the selection actually carries -- the same
                // shape the per-entity and per-document entries above use. A single "select control by
                // index" entry could not be offered, because `PaletteCommand::Run` takes no arguments;
                // and a client that cannot see the viewport needs to ask for a control BY NAME anyway.
                if ( const auto& found = MainScene()->FindEntityByID( subject ) )
                {
                    const ECS::Entity& entity = found->get();
                    if ( entity.HasComponent<ECS::AnimationComponent>() )
                    {
                        const auto& animation = entity.GetComponent<ECS::AnimationComponent>();
                        if ( animation.Animator && animation.Animator->GetRig() != nullptr )
                        {
                            const Animation::ControlHierarchy& hierarchy =
                                 animation.Animator->GetRig()->GetHierarchy();
                            for ( uint32_t control = 0; control < static_cast<uint32_t>( hierarchy.Size() );
                                  ++control )
                            {
                                const std::string name = hierarchy.Get( control ).Name;
                                commands.push_back( { "Control Rig", "Select control " + name,
                                                      [this, subject, control]
                                                      {
                                                          // The palette takes the context the way it does for
                                                          // "Author": after "Viewport mode: Control" the viewport
                                                          // holds it, and picking a control by name refused.
                                                          // Focus adopts the holder's mode for the same entity.
                                                          auto& host = Core::ActiveAuthoringContext();
                                                          m_PaletteAuthoring.Entity = subject;
                                                          (void)host.Focus( m_PaletteAuthoringOwner,
                                                                            m_PaletteAuthoring );
                                                          return host.SetSelectedControl( m_PaletteAuthoringOwner,
                                                                                          m_PaletteAuthoring,
                                                                                          control );
                                                      } } );
                            }
                        }
                    }
                }
            }
        }

        // THE DRAG ITSELF. Offered unconditionally, like the snap steps: the refusal a client gets when
        // no control is selected is more useful than an entry that quietly is not in the dictionary, and
        // the dictionary is rebuilt per query anyway so a conditional one would come and go.
        //
        // PIXELS, NOT WORLD UNITS, and that is not a shortcut: `ControlDrag` converts a POINTER OFFSET
        // into the control's parent space, so the honest parameter of the gesture is the one the gesture
        // takes. A "move 10 cm" entry would have to invent the projection the drag exists to do, and the
        // two would disagree at every zoom but one.
        {
            constexpr struct
            {
                const char* Label;
                float       X;
                float       Y;
            } kNudges[] = {
                 { "Nudge the selected control 20 px right", 20.0f, 0.0f },
                 { "Nudge the selected control 20 px left", -20.0f, 0.0f },
                 { "Nudge the selected control 20 px up", 0.0f, -20.0f },
                 { "Nudge the selected control 20 px down", 0.0f, 20.0f },
                 { "Nudge the selected control 80 px right", 80.0f, 0.0f },
                 { "Nudge the selected control 80 px left", -80.0f, 0.0f },
                 { "Nudge the selected control 80 px up", 0.0f, -80.0f },
                 { "Nudge the selected control 80 px down", 0.0f, 80.0f },
            };
            for ( const auto& nudge : kNudges )
            {
                // Y GROWS DOWNWARD -- ImGui's convention and therefore the viewport's
                // (ControlManipulator.hpp), which is why "up" is negative here.
                const glm::vec2 delta( nudge.X, nudge.Y );
                commands.push_back( { "Control Rig", nudge.Label,
                                      [delta] { return Core::ControlNudgeRequests::Request( delta ); } } );
            }
        }

        // EXACT ROTATION, THE GIZMO'S ARITHMETIC WITHOUT A MOUSE. A nudge is pixels through the arcball, so
        // "turn the elbow 45 degrees" has no nudge spelling; this is UE's local rotate gizmo as one gesture:
        // the pose turns about the control's own axis, drives carry it to the bone on the next evaluation,
        // and the gesture is ONE undo entry (RecordControlDrag, the same recorder the mouse drag uses).
        {
            constexpr std::array<const char*, 3> kAxes = { "X", "Y", "Z" };
            for ( int axis = 0; axis < 3; ++axis )
            {
                for ( const float degrees : { 45.0f, -45.0f, 90.0f, -90.0f } )
                {
                    const std::string label = std::format( "Rotate selected {} {:+g}", kAxes[axis], degrees );
                    commands.push_back( { "Control Rig", label, [this, axis, degrees]
                                          { return RotateSelectedControl( axis, degrees ); } } );
                }
            }
        }
        // Placement by ray: the one door to it that does not need a mouse drag, so the control channel can
        // put an object on a hill and shoot the result. The ray is the active viewport's line of sight, and
        // the surface is whatever Scene::Raycast meets first — the landscape included.
        commands.push_back( { "Entity", "Place a cube on the surface at the viewport centre", [this]
                              {
                                  ::Desert::Core::EditorCamera* camera = ActiveEditorCamera();
                                  if ( ( camera == nullptr ) || !MainScene() )
                                      return PaletteCommandOutcome( false, "no viewport camera or no scene" );
                                  const Common::Math::Ray    ray( camera->GetPosition(), camera->GetDirection() );
                                  ::Desert::Core::RaycastHit hit;
                                  if ( !MainScene()->Raycast( ray, hit ) )
                                      return PaletteCommandOutcome( false,
                                                                    "the viewport centre looks at no surface" );
                                  auto& e       = MainScene()->CreateNewEntity( "Cube" );
                                  auto& smc     = e.AddComponent<ECS::StaticMeshComponent>();
                                  smc.Primitive = Geometry::PrimitiveType::Cube;
                                  // The primitive cube is one metre, centred on its pivot: lift it by half
                                  // along the surface normal so it stands on the surface, not in it.
                                  e.GetComponent<ECS::TransformComponent>().Translation =
                                       hit.Point + hit.Normal * ( 0.5f * Common::Units::UnitsPerMetre );
                                  const auto uuid = e.GetComponent<ECS::UUIDComponent>().UUID;
                                  Core::SelectionManager::SetSelected( uuid );
                                  Commands::NotifyCreated( { uuid } );
                                  return PaletteCommandDone();
                              } } );
        // UE's Convert to Static Mesh: the selected EditMesh entity's geometry becomes a new .stmesh asset,
        // written where the modeling tools' Output settings say (Modeling panel, "Output Type").
        commands.push_back(
             { "Entity", "Convert to Static Mesh", []
               {
                   const auto& selection = Core::SelectionManager::GetSelection();
                   if ( selection.size() != 1 )
                       return Common::MakeFormattedError<bool>(
                            "select exactly one object to convert ({} selected)", selection.size() );
                   const auto& out     = Core::ModelingState::Get().Output;
                   const auto  written = Commands::ConvertToStaticMesh( selection.front(), out.Folder, out.Name );
                   if ( !written.IsSuccess() )
                       return Common::MakeError<bool>( written.GetError() );
                   LOG_INFO( "[Modeling] converted to static mesh '{}'", written.GetValue().generic_string() );
                   return Common::MakeSuccess( true );
               } } );
    }

    void EntityCommands::AppendCollapse( std::vector<PaletteCommand>& commands )
    {
        // COLLAPSE THE SELECTION INTO ONE INSTANCED DRAW. Offered ONCE, not per entity, because its
        // subject is the selection and not an entity — the same reason the snap entries below are not
        // repeated per viewport.
        //
        // WHY IT IS A COMMAND AND NOT ONLY A BUTTON. Five hundred transforms are not typed by hand, so
        // the Details panel's instance list has no author without this; and a control that exists only
        // as a mouse click cannot be photographed or checked on this machine, where synthetic input is
        // closed at the OS. Save, "+ State" and the warning-strip rows are here for the same reason.
        //
        // It returns the planner's own refusal rather than PaletteCommandDone: a fold that would have
        // destroyed a collider must say so to whoever asked, on the channel and in the toast alike.
        commands.push_back( { "Entity", "Collapse selection into Instanced Static Mesh", []
                              {
                                  const auto folded = Commands::CollapseIntoInstancedMesh(
                                       Core::SelectionManager::GetSelection() );
                                  if ( !folded.IsSuccess() )
                                      return Common::MakeError<bool>( folded.GetError() );
                                  return Common::MakeSuccess( true );
                              } } );
    }

    Common::BoolResultStr EntityCommands::RotateSelectedControl( int axis, float degrees )
    {
        const auto& host    = Core::ActiveAuthoringContext();
        const auto  control = host.SelectedControl();
        if ( !control.has_value() || !MainScene() )
        {
            return Common::MakeError<bool>( "no control is selected; run 'Select control <name>' first" );
        }
        const auto found = MainScene()->FindEntityByID( host.Entity() );
        if ( !found || !found->get().HasComponent<ECS::AnimationComponent>() )
        {
            return Common::MakeError<bool>( "the authoring context's entity has no animation component" );
        }
        auto& animation = found->get().GetComponent<ECS::AnimationComponent>();
        if ( !animation.Animator || animation.Animator->GetRig() == nullptr )
        {
            return Common::MakeError<bool>( "the authoring context's entity has no built control rig" );
        }
        Animation::ControlHierarchy& hierarchy = animation.Animator->GetRig()->GetHierarchy();
        if ( *control >= hierarchy.Size() )
        {
            return Common::MakeFormattedError<bool>( "the selected control {} is not in a rig of {} controls",
                                                     *control, hierarchy.Size() );
        }
        // One call for the turn AND its undo entry, so the one-entry rule is the suite's (ClipEditUndo) to
        // measure.
        if ( auto turned = RotateControlRecorded( &hierarchy, *control, axis, degrees ); !turned.IsSuccess() )
        {
            return Common::MakeError<bool>( turned.GetError() );
        }
        return Common::MakeSuccess( true );
    }
} // namespace Desert::Editor
