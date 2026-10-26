#pragma once

#include <Editor/Core/CommandPalette.hpp>
#include <Editor/Core/Selection/AuthoringContext.hpp>

#include <Common/Core/Core.hpp>

#include <functional>
#include <memory>
#include <vector>

namespace Desert::Core
{
    class EditorCamera;
    class Scene;
} // namespace Desert::Core

namespace Desert::Editor
{
    class SubjectEditorRegistry;

    // The Entity, Camera and Control Rig groups of the command palette: select / lock / delete / pilot / open any
    // object of the open scene, pose a control rig without a mouse, place and convert, collapse into instances.
    //
    // A CLASS, because the palette is a WRITER of the authoring context (it can put the editor into Control mode
    // and pick a control), and a writer holds the context like every other writer does — Kind::Panel, its own
    // durable copy, refusals that name it. Without this the entries would reach into a viewport's context, the
    // process-wide-statics shape AuthoringContext.hpp was written to end.
    //
    // The scene and the camera are the EDITOR'S slots, read when an entry RUNS (the slot may be re-pointed after
    // the build), so a scene switch needs no re-registration.
    class EntityCommands
    {
    public:
        using ActiveCamera = std::function<::Desert::Core::EditorCamera*()>;

        EntityCommands( const std::shared_ptr<::Desert::Core::Scene>& mainScene,
                        SubjectEditorRegistry& subjectEditors, ActiveCamera activeCamera );

        // Everything but the collapse, in palette order (entities, camera, control rig, place, convert).
        void Append( std::vector<PaletteCommand>& commands );
        // "Collapse selection into Instanced Static Mesh" — its own call because Modeling's Mesh To Collision
        // sits between it and the rest of the group in the palette's order.
        static void AppendCollapse( std::vector<PaletteCommand>& commands );

    private:
        /// Turns the authoring context's selected control about its own @p axis and records one undo entry.
        [[nodiscard]] Common::BoolResultStr RotateSelectedControl( int axis, float degrees );

        [[nodiscard]] const std::shared_ptr<::Desert::Core::Scene>& MainScene() const
        {
            return *m_MainSceneSlot;
        }
        [[nodiscard]] ::Desert::Core::EditorCamera* ActiveEditorCamera() const
        {
            return m_ActiveCamera();
        }

        const std::shared_ptr<::Desert::Core::Scene>* m_MainSceneSlot;
        SubjectEditorRegistry*                        m_SubjectEditors;
        ActiveCamera                                  m_ActiveCamera;
        Core::AuthoringContext                        m_PaletteAuthoring;
        Core::AuthoringOwner m_PaletteAuthoringOwner = Core::AuthoringOwner::ForPanel( "Command Palette" );
    };
} // namespace Desert::Editor
