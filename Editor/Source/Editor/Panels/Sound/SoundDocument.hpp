#pragma once

#include <Editor/Core/IPanel.hpp>
#include <Editor/Core/SubjectEditorRegistry.hpp>

#include <Common/Core/Core.hpp> // Common::Filepath, which AssetMetadata.hpp names without including
#include <Engine/Assets/AssetMetadata.hpp>

#include <Audio/SoundProperties.hpp>

#include <filesystem>
#include <string>
#include <type_traits>

namespace Desert::Editor
{
    /**
     * @brief One window per `.desound` - UE's sound wave in the Details panel: Source (read only), Loading and the
     * sound CLASS the sound plays in (UE USoundBase::SoundClassObject), picked among the class set the project's
     * class assets state (AudioMix::Classes - what SoundClassRegistry loaded; the window keeps no second list).
     *
     * A sound is a content-registry row, not an AssetManager record (it is resolved by GUID when a voice
     * starts), so the window is about the FILE the row names: it reads it at open and writes it whole through
     * SoundAsset::Save.
     *
     * ONE CHOICE = ONE UNDO STEP: a pick is a byte record of the window's own edit (trivially copyable), recorded
     * as an edit OF THAT OBJECT (CommandHistory::FieldObject) so no scene file is dirtied; the destructor forgets
     * the records (DropFor). DIRTY IS DERIVED: the edit against what was last read or saved.
     */
    class SoundDocument final : public ISubjectDocument
    {
    public:
        explicit SoundDocument( const Assets::AssetHandle& subject );
        ~SoundDocument() override;

        [[nodiscard]] glm::vec2 GetDefaultSize() const override
        {
            return { 460.0f, 220.0f };
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
        // What the window edits. Source is the file's and is shown, not edited.
        struct Edit
        {
            Audio::SoundClassId Class;
            Audio::SoundLoading Loading = Audio::SoundLoading::Resident;

            bool operator==( const Edit& ) const = default;
        };
        static_assert( std::is_trivially_copyable_v<Edit>, "a pick is recorded as bytes (CommandHistory::Push)" );

        // Records @p next as one undo step and applies it.
        void Choose( const Edit& next );

        void DrawClassPicker();
        void DrawLoadingPicker();

        std::filesystem::path      m_File;      // the `.desound` the registry row names
        Common::Content::AssetGuid m_Guid;      // the file's header GUID, written back unchanged
        std::string                m_Source;    // as written: relative to the `.desound`
        Edit                       m_Edit;      // what the window shows; undo writes it back
        Edit                       m_Saved;     // the file's state at open and after each save
        std::string                m_LoadError; // why the window shows no fields; empty when it does
        std::string                m_Refusal;   // the last refused save, shown inline
    };

    // The `.desound` path opener: read the file's GUID, find its content-registry row, then ask the document well
    // to open-or-focus the window. NotMine for another extension or a path with no file; Failed, with the reason
    // logged, when the file does not parse, the registry does not list it, or no Sound editor is registered.
    [[nodiscard]] SubjectEditorRegistry::PathOpenOutcome
    RequestSoundDocument( const std::string& path, const SubjectEditorRegistry& editors );
} // namespace Desert::Editor
