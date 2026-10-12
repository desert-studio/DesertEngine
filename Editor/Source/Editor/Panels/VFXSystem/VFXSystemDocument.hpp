#pragma once

#include "VFXEmitterTemplates.hpp"

#include <Editor/Core/SubjectEditorRegistry.hpp>
#include <Editor/Panels/IPanel.hpp>

#include <Common/Core/Core.hpp> // Common::Filepath, which AssetMetadata.hpp names without including
#include <Engine/Assets/AssetMetadata.hpp>
#include <Engine/Assets/Serialization/VFXSystem.hpp>
#include <Engine/VFX/VFXModuleLibrary.hpp>

#include <Common/Core/ResultStr.hpp>

#include <array>
#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::Editor
{
    /**
     * @brief One window per `.dfx`: UE's Niagara System editor (VFX-12).
     *
     * LEFT, the SYSTEM OVERVIEW: the system row, then every emitter with its module stack by stage — Emitter
     * Update, Particle Spawn, Particle Update — and its renderers; a module row toggles, moves and is removed in
     * place, and each stage ends with "Add module" over the engine module library (VFX/VFXModuleLibrary.hpp).
     * "Add emitter" offers the EMITTER TEMPLATES (VFXEmitterTemplates.hpp: Fire, Smoke, Sparks, Magic, Explosion).
     * RIGHT, the SELECTION DETAILS: the system's, an emitter's, a renderer's properties, or a module's inputs —
     * each with its source (Value / Random / Curve / Binding) and its UNIT and slider bounds from the module's
     * declaration (`Input ... Units= Range=`), so a speed reads "cm/s" and a gravity "-980 cm/s2".
     *
     * Every edit goes through Editor/Core/Commands/VFXSystemEdit.hpp: validated by ValidateVFXSystemData before it
     * lands, one CommandHistory entry per gesture (a slider drag is one entry, committed on release), whose
     * EditedObject is m_Working, so the destructor forgets this window's entries with DropFor(&m_Working).
     *
     * WORKING AND SAVED: the document edits a copy; Dirty is m_Working != m_Saved (derived, never a flag), and
     * Save writes m_Working through VFXSystemAsset::Save, then has the held asset re-read its file.
     *
     * NO RENDERER SLOT YET: the live preview viewport is REMAINDER (the window is ImGui only).
     */
    class VFXSystemDocument final : public ISubjectDocument
    {
    public:
        VFXSystemDocument( const Assets::AssetHandle& subject, Assets::AssetManager* assets );
        ~VFXSystemDocument() override;

        [[nodiscard]] glm::vec2 GetDefaultSize() const override
        {
            return { 980.0f, 640.0f };
        }

        void OnUIRender() override;

        [[nodiscard]] bool IsSubjectAlive() const override;

        [[nodiscard]] bool HoldsView() const override
        {
            return false;
        }

        [[nodiscard]] bool ClaimsView() const override
        {
            return false;
        }

        [[nodiscard]] DiskState GetDiskState() const override;

        bool SaveDocument() override;

        /// The stack stages, in UE's order.
        enum class Stage
        {
            EmitterUpdate,
            ParticleSpawn,
            ParticleUpdate,
        };

    private:
        /// What the details column shows.
        struct Selection
        {
            enum class Kind
            {
                System,
                Emitter,
                Module,
                Renderer,
            };
            Kind        What    = Kind::System;
            std::size_t Emitter = 0;
            Stage       Group   = Stage::EmitterUpdate;
            std::size_t Index   = 0; // the module or renderer row

            [[nodiscard]] bool operator==( const Selection& ) const = default;
        };

        void DrawToolbar();
        void DrawOverview();
        void DrawEmitterStack( std::size_t emitter );
        void DrawStage( std::size_t emitter, Stage stage );
        void DrawDetails();
        void DrawSystemDetails();
        void DrawEmitterDetails( std::size_t emitter );
        void DrawRendererDetails( std::size_t emitter, std::size_t renderer );
        void DrawModuleDetails( std::size_t emitter, Stage stage, std::size_t module );
        void DrawInput( std::size_t emitter, Stage stage, std::size_t module, std::size_t input );

        /// A continuous widget's edit: call after the widget; it snapshots on activation and commits on release.
        void TrackGesture( const char* label );
        /// Queues a discrete edit; it runs (through ApplyVFXSystemEdit) after the frame's widgets, so no widget
        /// holds a reference into m_Working across it.
        void Defer( std::string                                                                   label,
                    std::function<Common::BoolResultStr( Assets::Serialization::VFXSystemData& )> edit );
        void Report( const Common::BoolResultStr& result );
        void ClampSelection();

        [[nodiscard]] std::span<const VFX::VFXModuleInfo> LibraryFor( Stage stage ) const;

        Assets::AssetManager*                m_Assets = nullptr;
        bool                                 m_Loaded = false; // the asset was found and read at open
        std::string                          m_LoadError;
        Assets::Serialization::VFXSystemData m_Working; // what the window edits (CommandHistory target)
        Assets::Serialization::VFXSystemData m_Saved;   // what the file holds
        std::string                          m_Refusal; // the last refused edit or save, shown inline

        std::vector<VFX::VFXModuleInfo> m_ParticleModules; // the engine module library, read at open
        std::vector<VFXEmitterTemplate> m_Templates;
        std::string                     m_LibraryError; // why the module library or the templates are missing

        Selection m_Selection;
        // The working copy as it was when the held widget was activated (a drag in progress).
        std::optional<Assets::Serialization::VFXSystemData> m_GestureBefore;
        Assets::Serialization::VFXSystemData                m_FrameStart; // m_Working as this frame began
        std::string                                         m_DeferredLabel;
        std::function<Common::BoolResultStr( Assets::Serialization::VFXSystemData& )> m_Deferred;
        std::optional<Selection> m_DeferredSelection; // selected once it lands

        Assets::Serialization::VFXCategoryRegister m_Categories; // the project's, for the Category combo
        // The emitter name being typed: ImGui hands the edit back only while the item is active, so the text
        // lives here until it deactivates and is committed as ONE rename.
        std::array<char, 128> m_NameBuffer{};
        bool                  m_NameTyping = false;
    };

    /// Every module row of @p stage of @p emitter.
    [[nodiscard]] std::vector<Assets::Serialization::VFXModuleUse>&
    StageModules( Assets::Serialization::VFXEmitterData& emitter, VFXSystemDocument::Stage stage );

    // The `.dfx` path opener: find-or-create the VFXSystemAsset, make sure it is read, then open it through the
    // one handle route, Core::RequestOpenAsset. NotMine for any other extension or a path with no file; Failed,
    // with the reason logged, when it will not load or open.
    [[nodiscard]] SubjectEditorRegistry::PathOpenOutcome
    RequestVFXSystemDocument( Assets::AssetManager* assets, const std::string& path,
                              const SubjectEditorRegistry& editors );
} // namespace Desert::Editor
