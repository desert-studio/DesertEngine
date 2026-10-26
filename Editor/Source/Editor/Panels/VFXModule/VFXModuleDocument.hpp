#pragma once

#include <Editor/Core/SubjectEditorRegistry.hpp>
#include <Editor/Panels/IPanel.hpp>

#include <Common/Core/Core.hpp> // Common::Filepath, which AssetMetadata.hpp names without including
#include <Engine/Assets/AssetMetadata.hpp>
#include <Engine/VFX/VFXModuleFormat.hpp>

#include <Common/Core/ResultStr.hpp>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::Editor
{
    /**
     * @brief One window per `.dfxmod`: a VFX module - UE's Niagara module script editor, ours as text and tables
     * (VFX-MODULE-ASSET).
     *
     * The module's metadata (name, category, description, the stages it may be added to), its Inputs and
     * Attributes as tables, its GLSL in a code editor, the stack check of the working copy (the module compiled in
     * a one-emitter stack, as the particle world would - the declaration and attribute errors the compiler names)
     * and the systems that use it (the registry's referrers of this file).
     *
     * EVERY edit goes through Editor/Core/Commands/VFXModuleEdit.hpp (ApplyVFXModuleEdit): the window owns no rule
     * about what a module may hold; a refused edit is the format's refusal, shown in the window, and changes
     * nothing. Each accepted edit is one CommandHistory entry whose EditedObject is m_Working, so the destructor
     * forgets this window's entries with DropFor(&m_Working). Text fields edit a draft and commit when the field
     * is left, so one typed word is one entry.
     *
     * WORKING AND SAVED: Dirty is m_Working != m_Saved (derived, never a flag); Save writes m_Working through
     * VFXModuleAsset::Save, which records the write in the ContentRegistry, so every system using the module is
     * recompiled (VFXSystemService::ReissueOnModuleWrites).
     *
     * NO RENDERER SLOT: ImGui only, no PreviewViewport.
     */
    class VFXModuleDocument final : public ISubjectDocument
    {
    public:
        VFXModuleDocument( const Assets::AssetHandle& subject, Assets::AssetManager* assets );
        ~VFXModuleDocument() override;

        [[nodiscard]] glm::vec2 GetDefaultSize() const override
        {
            return { 720.0f, 640.0f };
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

    private:
        /// The drafts the text fields edit, re-read from m_Working whenever it changed (an edit, an undo).
        struct Drafts
        {
            std::string              Name;
            std::string              Category;
            std::string              Description;
            std::string              Body;
            std::vector<std::string> Inputs;
            std::vector<glm::vec4>   Defaults; ///< per input, the Default column's value until it is committed
            std::vector<std::string> Attributes;
        };

        void SyncDrafts();
        void DrawMetadata();
        void DrawDeclarations( const char* id, bool inputs );
        /// Input @p i's Default cell: `= Binding`, or one field per component of its type.
        void DrawInputDefault( std::size_t i );
        void DrawCode();
        void DrawStackCheck();
        void DrawUsedBy();
        void Edit( const std::string&                                                       label,
                   const std::function<Common::BoolResultStr( VFX::VFXModuleDefinition& )>& edit );
        void Report( const Common::BoolResultStr& result );

        Assets::AssetManager*    m_Assets = nullptr;
        bool                     m_Loaded = false; // the asset was found and read at open
        std::string              m_LoadError;
        std::filesystem::path    m_File;
        VFX::VFXModuleDefinition m_Working;  // what the window edits (CommandHistory target)
        VFX::VFXModuleDefinition m_Saved;    // what the file holds
        VFX::VFXModuleDefinition m_DraftsOf; // the working copy the drafts were read from
        Drafts                   m_Drafts;
        std::string              m_Refusal; // the last refused edit or save, shown inline
        VFX::VFXModuleDefinition m_CheckedOf; // the working copy m_Check was computed for
        std::string              m_Check;     // the stack check's outcome, recomputed when m_Working changes
        std::vector<std::string> m_UsedBy;    // the registry keys of the assets naming this module
    };

    [[nodiscard]] SubjectEditorRegistry::PathOpenOutcome
    RequestVFXModuleDocument( Assets::AssetManager* assets, const std::string& path,
                              const SubjectEditorRegistry& editors );
} // namespace Desert::Editor
