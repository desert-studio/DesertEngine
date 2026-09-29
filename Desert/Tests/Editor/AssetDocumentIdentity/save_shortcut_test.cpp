// Ctrl+S: the asset of the document in front of the person, never the level behind it (UE's asset editors).
#include <Editor/Core/SaveShortcut.hpp>
#include <Editor/Panels/AnimationEditor/AnimationEditorIdentity.hpp>

#include <gtest/gtest.h>

using Desert::Assets::AssetHandle;
using Desert::Editor::AnimationEditorBase;
using Desert::Editor::ResolveSaveShortcut;
using Desert::Editor::SaveShortcutTarget;

namespace
{
    class DiskDocument final : public AnimationEditorBase
    {
    public:
        explicit DiskDocument( const DiskState state )
             : AnimationEditorBase( "Wave.anim", AssetHandle( 7u ) ), m_State( state )
        {
        }
        void OnUIRender() override
        {
        }
        [[nodiscard]] bool IsSubjectAlive() const override
        {
            return true;
        }
        [[nodiscard]] DiskState GetDiskState() const override
        {
            return m_State;
        }

    protected:
        void DestroyPreview() override
        {
        }

    private:
        DiskState m_State;
    };
} // namespace

TEST( SaveShortcut, AFocusedDocumentOnDiskSavesItselfAndNotTheScene )
{
    const DiskDocument dirty( DiskDocument::DiskState::Dirty );
    const DiskDocument clean( DiskDocument::DiskState::Clean );
    EXPECT_EQ( ResolveSaveShortcut( true, &dirty ), SaveShortcutTarget::FocusedDocument );
    EXPECT_EQ( ResolveSaveShortcut( true, &clean ), SaveShortcutTarget::FocusedDocument );
}

TEST( SaveShortcut, TheLastFocusedDocumentDoesNotSaveOnceTheKeyboardLeftIt )
{
    // m_FocusedDocument outlives the focus (Ctrl+Tab resumes from it); the scene is saved then.
    const DiskDocument dirty( DiskDocument::DiskState::Dirty );
    EXPECT_EQ( ResolveSaveShortcut( false, &dirty ), SaveShortcutTarget::Scene );
    EXPECT_EQ( ResolveSaveShortcut( false, nullptr ), SaveShortcutTarget::Scene );
    EXPECT_EQ( ResolveSaveShortcut( true, nullptr ), SaveShortcutTarget::Scene );
}

TEST( SaveShortcut, AFocusedDocumentWithNoFileSavesNothingRatherThanTheScene )
{
    const DiskDocument procedural( DiskDocument::DiskState::Untracked );
    EXPECT_EQ( ResolveSaveShortcut( true, &procedural ), SaveShortcutTarget::Nothing );
}
