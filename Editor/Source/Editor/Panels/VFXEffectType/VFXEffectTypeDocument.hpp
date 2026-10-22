#pragma once

#include <Editor/Core/SubjectEditorRegistry.hpp>
#include <Editor/Panels/IPanel.hpp>

#include <Common/Core/Core.hpp> // Common::Filepath, which AssetMetadata.hpp names without including
#include <Engine/Assets/AssetMetadata.hpp>
#include <Engine/Assets/Serialization/VFXEffectType.hpp>

#include <string>
#include <vector>

namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::Editor
{
    /**
     * @brief One window per `.dfxtype`: the type's per-Effects-level scalability - UE's Niagara Effect Type asset
     * editor (its System / Emitter Scalability Settings arrays), VFX-11.
     *
     * A table of the five Effects levels (Low .. Cinematic), each a MaxDistance (cm, 0 = never culled) and a
     * SpawnCountScale (0..1). One drag is ONE CommandHistory entry (the whole level list before and after - five
     * pairs of numbers, a snapshot with no inverse to get wrong) whose EditedObject is m_Working; a result
     * ValidateVFXEffectTypeData refuses is shown inline and not kept.
     *
     * WORKING AND SAVED: Dirty is m_Working.Levels != m_Saved.Levels (derived, never a flag); Save writes through
     * VFXEffectTypeAsset::Save and has the held asset re-read the file, so every VFXWorld culls by the new levels
     * on its next tick (the same path AssetHotReload takes for an edit made outside the editor).
     *
     * CHANGED ON DISK: the held asset is the file's truth (AssetHotReload re-reads it on an outside edit), so
     * every frame its levels are compared with m_Saved. A clean window adopts them; a window with unsaved edits
     * shows a conflict (UE's "file changed on disk") - Reload discards the edits, Keep adopts the file as the new
     * base so the edits stay dirty - and Save is refused until one is chosen, so a Save never silently overwrites
     * it.
     */
    class VFXEffectTypeDocument final : public ISubjectDocument
    {
    public:
        VFXEffectTypeDocument( const Assets::AssetHandle& subject, Assets::AssetManager* assets );
        ~VFXEffectTypeDocument() override;

        [[nodiscard]] glm::vec2 GetDefaultSize() const override
        {
            return { 520.0f, 300.0f };
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
        // The drag in flight: the levels before it, pushed as one entry when the item deactivates.
        void TrackDrag( const char* label );

        // Compares the held asset's levels (the file) with m_Saved: adopts them when clean, else raises
        // m_DiskConflict.
        void SyncWithDisk();
        // The conflict banner: Reload (discard the edits) or Keep (the file becomes the base; Save overwrites it).
        void DrawDiskConflict();

        Assets::AssetManager*                           m_Assets = nullptr;
        bool                                            m_Loaded = false;
        std::string                                     m_LoadError;
        Assets::Serialization::VFXEffectTypeData        m_Working; // what the window edits (CommandHistory target)
        Assets::Serialization::VFXEffectTypeData        m_Saved;   // what the file holds
        std::vector<Assets::Serialization::VFXEffectTypeLevel> m_DragBefore;
        std::string                                     m_Refusal;
        bool m_DiskConflict = false; // the file changed under unsaved edits
    };

    /// Opens (or focuses) the effect type editor for a `.dfxtype` at @p path: NotMine for any other file.
    SubjectEditorRegistry::PathOpenOutcome RequestVFXEffectTypeDocument( Assets::AssetManager*        assets,
                                                                         const std::string&           path,
                                                                         const SubjectEditorRegistry& editors );
} // namespace Desert::Editor
