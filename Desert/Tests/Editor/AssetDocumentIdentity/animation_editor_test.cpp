// The Animation Editor (ANV1a): its identity and slot answers (AnimationEditorBase) and its transport
// (AnimationTransport) — the two halves of the window that need no device.

#include <Editor/Core/OpenDocuments.hpp>
#include <Editor/Panels/AnimationEditor/AnimationEditorIdentity.hpp>
#include <Editor/Panels/AnimationEditor/AnimationTransport.hpp>

#include <gtest/gtest.h>

#include <memory>

using Desert::Assets::AssetHandle;
using Desert::Assets::AssetTypeID;
using Desert::Editor::AnimationEditorBase;
using Desert::Editor::AnimationEditorSubject;
using Desert::Editor::AnimationTransport;

namespace
{
    class TestAnimationEditor final : public AnimationEditorBase
    {
    public:
        explicit TestAnimationEditor( uint64_t handle, int* destroyed = nullptr )
             : AnimationEditorBase( "Wave.anim", AssetHandle( handle ) ), m_Destroyed( destroyed )
        {
        }
        void OnUIRender() override
        {
        }
        [[nodiscard]] bool IsSubjectAlive() const override
        {
            return true;
        }
        void BuildPreview()
        {
            m_PreviewLive = true;
        }

    protected:
        void DestroyPreview() override
        {
            if ( m_Destroyed != nullptr )
                ++*m_Destroyed;
        }

    private:
        int* m_Destroyed = nullptr;
    };

    AnimationTransport OneSecondAtTen()
    {
        AnimationTransport t;
        t.DurationSeconds = 1.0;
        t.DisplayRate     = { 10, 1 };
        return t;
    }
} // namespace

TEST( AnimationEditorIdentity, IsFoundByTheClipHandleUnderTheAnimationType )
{
    Desert::Editor::OpenDocuments well;
    well.Open( std::make_unique<TestAnimationEditor>( 920 ) );

    const auto* found = well.Find( AnimationEditorSubject( AssetHandle( 920 ) ) );
    ASSERT_NE( found, nullptr ) << "a second Open of the same clip would build a second window and slot";
    EXPECT_EQ( well.Find( Desert::Editor::AssetSubject( AssetHandle( 920 ),
                                                        static_cast<uint32_t>( AssetTypeID::Mesh ) ) ),
               nullptr );
    EXPECT_EQ( well.Find( AnimationEditorSubject( AssetHandle( 921 ) ) ), nullptr );
}

// Closing the window must DESTROY the preview scene — a hidden scene still holds its SceneRenderer, which is
// the slot — and only then answer that the slot is free.
TEST( AnimationEditorIdentity, ReleasingTheViewDestroysThePreviewAndGivesTheSlotBack )
{
    int                 destroyed = 0;
    TestAnimationEditor editor( 922, &destroyed );
    EXPECT_TRUE( editor.ClaimsView() );
    EXPECT_FALSE( editor.HoldsView() );

    editor.BuildPreview();
    EXPECT_TRUE( editor.HoldsView() );

    editor.ReleaseView();
    EXPECT_EQ( destroyed, 1 );
    EXPECT_FALSE( editor.HoldsView() );
}

TEST( AnimationTransport, CountsFramesOnTheDisplayGridIncludingTheLastPose )
{
    auto t = OneSecondAtTen();
    EXPECT_EQ( t.LastFrame(), 10 );
    t.SetTime( 0.3 );
    EXPECT_EQ( t.FrameIndex(), 3 );
    t.SetTime( 5.0 );
    EXPECT_DOUBLE_EQ( t.Time, 1.0 ) << "time is clamped to the clip";
}

TEST( AnimationTransport, AStepPausesAndLandsOnTheGrid )
{
    auto t    = OneSecondAtTen();
    t.Playing = true;
    t.SetTime( 0.25 );
    t.StepFrames( 1 );
    EXPECT_FALSE( t.Playing );
    EXPECT_EQ( t.FrameIndex(), 3 );
    EXPECT_NEAR( t.Time, 0.3, 1e-9 );
    t.StepFrames( -1 );
    EXPECT_NEAR( t.Time, 0.2, 1e-9 );
}

TEST( AnimationTransport, AStepPastEitherEndWrapsWhenLoopingAndClampsWhenNot )
{
    auto t = OneSecondAtTen();
    t.StepFrames( -1 );
    EXPECT_EQ( t.FrameIndex(), 10 );
    t.StepFrames( 1 );
    EXPECT_EQ( t.FrameIndex(), 0 );

    t.Loop = false;
    t.StepFrames( -1 );
    EXPECT_EQ( t.FrameIndex(), 0 );
    t.ToEnd();
    t.StepFrames( 1 );
    EXPECT_EQ( t.FrameIndex(), 10 );
}

TEST( AnimationTransport, PlaybackAdvancesBySecondsTimesSpeed )
{
    auto t    = OneSecondAtTen();
    t.Speed   = 0.25f;
    t.Playing = true;
    t.Advance( 0.4 );
    EXPECT_NEAR( t.Time, 0.1, 1e-9 );

    // Frame-rate independent: ten small steps and one big one land on the same time.
    auto a    = OneSecondAtTen();
    auto b    = OneSecondAtTen();
    a.Playing = b.Playing = true;
    for ( int i = 0; i < 10; ++i )
        a.Advance( 0.05 );
    b.Advance( 0.5 );
    EXPECT_NEAR( a.Time, b.Time, 1e-9 );
}

TEST( AnimationTransport, TheEndWrapsWhenLoopingAndStopsOnTheLastPoseWhenNot )
{
    auto t    = OneSecondAtTen();
    t.Playing = true;
    t.SetTime( 0.9 );
    t.Advance( 0.3 );
    EXPECT_NEAR( t.Time, 0.2, 1e-9 );
    EXPECT_TRUE( t.Playing );

    t.Loop = false;
    t.SetTime( 0.9 );
    t.Advance( 0.3 );
    EXPECT_DOUBLE_EQ( t.Time, 1.0 );
    EXPECT_FALSE( t.Playing );

    t.TogglePlay();
    EXPECT_TRUE( t.Playing );
    EXPECT_DOUBLE_EQ( t.Time, 0.0 ) << "Play at the end of a one-shot clip restarts it";
}
