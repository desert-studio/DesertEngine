#pragma once

#include <Editor/Core/SubjectEditorRegistry.hpp>
#include <Editor/Panels/IPanel.hpp>

#include <Common/Core/Core.hpp> // Common::Filepath, which AssetMetadata.hpp names without including

#include <array>
#include <Engine/Assets/AssetMetadata.hpp>
#include <Engine/Assets/Serialization/VFXSystem.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::Editor::UI
{
    class UIHelper;
}

namespace Desert::Editor
{
    class PreviewViewport;

    /**
     * @brief One window per `.dfx`: UE's Niagara System editor (VFX-12, first slice 12a/12b).
     *
     * Three columns as the VfxSystemWindow mockup: SYSTEM (system fields, User parameters, the emitter list) |
     * PREVIEW (a PreviewViewport whose scene holds one VFXComponent playing this system, realtime) | STACK (the
     * selected emitter's modules by stage - Emitter Update, Particle Spawn, Particle Update - then Render rows).
     *
     * WORKING AND SAVED (12c): the window edits m_Working, a copy of the asset's data; Dirty is m_Working !=
     * m_Saved (derived, never a flag). A stack input's gesture (a drag, a typed number, a `set` from the control
     * socket) is ONE CommandHistory entry (VFXSystemEditCommand, the whole system before/after), refused by name
     * when ValidateVFXSystemData rejects the result. Save writes m_Working through VFXSystemAsset::Save, re-reads
     * the held asset and refreshes VFXSystemService, so level effects play what was written.
     *
     * THE PREVIEW PLAYS THE WORKING COPY (12a-override, UE's Niagara editor plays its own system copy): whenever
     * m_Working differs from what the preview last got - an edit, an undo, a redo - a new immutable copy goes to
     * the preview's VFXComponent::EditedSystem, and the new pointer restarts the simulation (UE ResetSimulation on
     * every stack change). Levels keep playing the asset until Save.
     *
     * THE TRANSPORT (12e, UE Niagara's timeline transport): the window owns the preview's time. m_Time runs while
     * playing (wall delta x speed), wraps at the playback end when looping (UE jumps back to the range start,
     * which resets the simulation) or stops there; scrubbing and stepping pause. The preview's effects clock is in
     * DesiredAge mode at m_Time (VFXWorld::SetDesiredAge), so the picture at t is a function of t alone - a replay
     * from zero by fixed steps from the system's Seed - whether t was reached by playing, scrubbing or stepping.
     * An edit replays to the same t. Transport state is the window's, not the asset's: it is never an undo step.
     * The control socket sees it as the `Transport.*` properties.
     */
    class VFXSystemDocument final : public ISubjectDocument
    {
    public:
        VFXSystemDocument( const Assets::AssetHandle& subject, Assets::AssetManager* assets );
        ~VFXSystemDocument() override; // forgets the history's entries, which point at m_Working

        [[nodiscard]] glm::vec2 GetDefaultSize() const override
        {
            return { 1280.0f, 720.0f };
        }

        void OnPreUpdate() override;
        void OnUIRender() override;

        [[nodiscard]] bool IsSubjectAlive() const override;

        [[nodiscard]] bool ClaimsView() const override
        {
            return true;
        }

        [[nodiscard]] bool HoldsView() const override
        {
            return m_PreviewLive;
        }

        // ~PreviewViewport idles the device and releases the renderer, which is what returns the slot.
        void ReleaseView() override;

        [[nodiscard]] bool HasPreview() const override
        {
            return true;
        }

        [[nodiscard]] DiskState GetDiskState() const override
        {
            if ( !m_Loaded )
                return DiskState::Untracked;
            return m_Working != m_Saved ? DiskState::Dirty : DiskState::Clean;
        }

        bool SaveDocument() override;

        /// Every stack input a number sets: `<Emitter>.<Stage>.<Module>.<Input>` for a Value, plus `.Min`/`.Max`
        /// for a Random range (a module repeated in one stage is `<Module>#2`, ...). Curve and Binding inputs are
        /// listed as not settable, saying why.
        [[nodiscard]] std::vector<EditableProperty> EditableProperties() const override;
        [[nodiscard]] Common::BoolResultStr         SetEditableProperty( const std::string&        name,
                                                                         const std::vector<float>& value ) override;

        void ExtendToolbar( AssetEditorToolbar& toolbar ) override;

        /// The stack's structure as palette / control-socket actions, for the selected emitter (UE's stack row
        /// menu): `Add <module> to <Emitter>.<Stage>` for every library module of a particle stage, and
        /// `Remove` / `Move Up` / `Move Down` `<Emitter>.<Stage>.<Module>` per row. Each is one undo step.
        [[nodiscard]] std::vector<DocumentAction> Actions() override;

    private:
        void EnsurePreview();
        void DrawSystemColumn();
        void DrawStackColumn();
        void DrawTransport();
        // The end of the transport's range: the system's Duration (one system loop), m_PlaybackEnd when it has
        // none.
        [[nodiscard]] double PlaybackEnd() const;
        // Pauses and moves to the fixed step @p steps away from the current time (UE Step Forward / Backward).
        void StepTransport( int steps );
        // Moves to @p seconds, clamped to the range; pauses, as a scrub does in UE.
        void ScrubTo( double seconds );
        void DrawStage( const char* title, std::size_t emitter, std::size_t stage );
        void DrawInputEditor( std::size_t emitter, std::size_t stage, std::size_t module, std::size_t input );
        // Ends a gesture that began from @p before: one undo step labelled @p label, or - when the result is not a
        // valid system - m_Working back to @p before and the refusal shown.
        Common::BoolResultStr Commit( Assets::Serialization::VFXSystemData before, const std::string& label );

        // THE STACK'S STRUCTURE (12d, UE's stack: add from the module library, delete, reorder, the row's
        // enable checkbox). Each is one Commit - one undo step - and, being a change of m_Working, restarts the
        // preview. A module added gets its inputs from the module's own declarations (ResolveParticleModule),
        // each a Value at zero.
        // @p channel: the DataChannel a BindingRequired input (SpawnFromChannel's Channel) is bound to - UE picks
        // the channel asset on the new row; a module with such an input and no channel is refused.
        /// VFX-15: the emitter is born at the placement's point set @p set (UE: a shape-location module reading
        /// an ArrayPosition User parameter): its spawn-group shape modules are disabled and ShapePointSet
        /// is added with Location bound to PointSet.<set>. One undo step.
        Common::BoolResultStr SpawnFromPointSet( std::size_t emitter, const std::string& set );
        Common::BoolResultStr AddModule( std::size_t emitter, std::size_t stage, const std::string& module,
                                         const std::string& channel = {} );
        Common::BoolResultStr RemoveModule( std::size_t emitter, std::size_t stage, std::size_t module );
        Common::BoolResultStr MoveModule( std::size_t emitter, std::size_t stage, std::size_t module, int delta );
        Common::BoolResultStr SetModuleEnabled( std::size_t emitter, std::size_t stage, std::size_t module,
                                                bool enabled );
        // VFX-LIGHT: one renderer row of an emitter replaced by @p value (its Enabled box, a Light row's
        // settings), one undo step; refused like any edit the format refuses.
        Common::BoolResultStr SetRenderer( std::size_t emitter, std::size_t row,
                                           Assets::Serialization::VFXRendererData value,
                                           const std::string&                     label );
        // `<Emitter>.Render.<row>.<Field>` for a renderer row's Enabled and a Light row's settings; nullopt = not
        // such a name.
        std::optional<Common::BoolResultStr> SetRendererProperty( const std::string&        name,
                                                                  const std::vector<float>& value );
        // One row of a stage's module picker: a module whose Usage includes the stage, or one that cannot be described
        // (Error set - shown disabled with the reason, never silently left out).
        struct ModuleChoice
        {
            std::string Ref; // the module's registry path ({Guid, Path} row key), what AddModule takes
            std::string Error;
            bool        NeedsChannel = false; // an input is BindingRequired: the picker asks for the channel
        };
        // What stage @p stage's picker offers (UE: the module picker filtered by Module Usage): every `.dfxmod` of
        // the content registry (engine library and project Content) whose Usage takes the stage.
        [[nodiscard]] std::vector<ModuleChoice> ModuleLibrary( std::size_t stage ) const;

        Assets::AssetManager*             m_Assets = nullptr;
        bool                              m_Loaded = false;
        std::string                       m_LoadError;
        Assets::Serialization::VFXSystemData m_Working; // what the window edits (CommandHistory target)
        Assets::Serialization::VFXSystemData m_Saved;   // what the file holds
        // What the preview plays: a frozen copy of m_Working, replaced whenever the two differ.
        std::shared_ptr<const Assets::Serialization::VFXSystemData> m_Played;
        // m_Working when the active widget's gesture began (set while a drag is held).
        std::optional<Assets::Serialization::VFXSystemData> m_GestureBefore;
        // The stack input the last control-channel `set` named: its row scrolls into view on the next draw.
        std::string                                         m_Reveal;
        std::string                                         m_Refusal;
        std::array<char, 64>                                m_ChannelPick{}; // the picker's channel name field
        std::size_t                       m_SelectedEmitter = 0;

        std::unique_ptr<PreviewViewport> m_Preview;
        std::unique_ptr<UI::UIHelper>    m_UIHelper;
        bool                             m_PreviewLive   = false;
        bool                             m_DrewThisFrame = false;
        glm::uvec2                       m_ViewSize{ 0u, 0u };

        // The transport (see the class comment).
        double m_Time        = 0.0; // the age the preview shows, seconds
        bool   m_Playing     = true;
        float  m_Speed       = 1.0f; // 0.1x .. 2x
        bool   m_Loop        = true;
        float  m_PlaybackEnd = 5.0f; // the range of a system without Duration (editable on the transport bar)
    };

    // The `.dfx` path opener: find-or-create the VFXSystemAsset, make sure it is read, then open it through the one
    // handle route, Core::RequestOpenAsset. NotMine for any other extension or a path with no file.
    [[nodiscard]] SubjectEditorRegistry::PathOpenOutcome
    RequestVFXSystemDocument( Assets::AssetManager* assets, const std::string& path,
                              const SubjectEditorRegistry& editors );
} // namespace Desert::Editor
