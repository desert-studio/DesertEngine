// BONE AUTHORING STATE HAS AN OWNER, AND THE OWNERSHIP IS CHECKED RATHER THAN DESCRIBED.
//
// What this replaced: `Editor::Core::SkeletonEditMode`, four `static inline` values on the process
// (`s_Active`, `s_SelectedBone`, `s_ShowAllNames`, `s_PoseMode`) with public setters, read and written
// from fifteen places in five files. No test asserted anything about it and no test COULD: every setter
// was callable by anyone and always succeeded, so "the right panel wrote it" and "some other panel wrote
// it" were the same observable.
//
// The defect that follows is not hypothetical. Docs/Animation/07_panels_design.md §5.3: every open
// Sequencer wrote `SetPoseMode( IsActive() && editClip )` unconditionally, every frame, from its own copy
// of that line — so two documents authoring two characters (the thing making the Sequencer a document
// bought, SequencerPanel.hpp: "Two characters could not be compared side by side") decided each other's
// pose mode by draw order. Section 3 below is that defect written as a relation.
//
// FIVE RELATIONS, and each of them is a thing the four statics could not express:
//
//   1. ONLY THE HOLDER WRITES. A write from anyone else changes nothing and SAYS SO.
//   2. NO CONTEXT IS A STATE, NOT A STALE VALUE. Before anyone claims and after the last owner
//      releases, every reader answers from the neutral value and every write is refused by name.
//   3. TWO CHARACTERS DO NOT SHARE A CONTEXT. The §5.3 defect, reproduced against the new type and
//      shown refused.
//   4. ONE CHARACTER SURVIVES CHANGING SURFACE. Details -> viewport -> Sequencer keeps the selected
//      bone and the mode, which is what the shared global used to give the user and what any
//      ownership scheme has to keep giving them.
//   5. RELEASE IS THE HOLDER'S ALONE. A window closing cannot take authoring away from the window the
//      user is working in.
//
// AND TWO CENSUSES OVER THE REPOSITORY'S SOURCE TEXT (sections 6 and 7), because the interesting claim
// — "the global is not read anywhere any more" — is about the tree and not about any run of this binary.
// Both counts are DERIVED from the walk; neither is written down.

#include <Editor/Core/Selection/AuthoringContext.hpp>

#include "../../Engine/SettingConsumers/setting_consumers_reader.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using Desert::Editor::SubjectDomain;
using Desert::Editor::SubjectId;
using Desert::Editor::Core::ActiveAuthoringContext;
using Desert::Editor::Core::AuthoringContext;
using Desert::Editor::Core::AuthoringContextHost;
using Desert::Editor::Core::AuthoringMode;
using Desert::Editor::Core::AuthoringModeName;
using Desert::Editor::Core::AuthoringOwner;

namespace
{
    Common::UUID Character( uint64_t n )
    {
        return Common::UUID( n );
    }

    // A Sequencer document over one character, spelled the way SequencerPanel spells it: the subject's
    // owner IS the entity, which is what makes "one document, one character" checkable.
    AuthoringOwner SequencerOver( const Common::UUID& entity )
    {
        SubjectId subject;
        subject.Domain = SubjectDomain::EntityComponent;
        subject.Facet  = 1729u;
        subject.Owner  = entity;
        return AuthoringOwner::ForDocument( subject );
    }

    AuthoringContext ContextFor( const Common::UUID& entity )
    {
        AuthoringContext context;
        context.Entity = entity;
        return context;
    }
} // namespace

// ── 1. ONLY THE HOLDER WRITES ─────────────────────────────────────────────────────────────────────

TEST( AuthoringContextOwnership, AWriteFromANonHolderChangesNothingAndNamesBothParties )
{
    AuthoringContextHost host;

    const auto       viewport = AuthoringOwner::ForSceneView( 0 );
    const auto       stranger = AuthoringOwner::ForSceneView( 7 );
    AuthoringContext mine     = ContextFor( Character( 11 ) );

    host.Focus( viewport, mine );
    ASSERT_TRUE( host.SetMode( viewport, mine, AuthoringMode::Skeleton ).IsSuccess() );
    ASSERT_TRUE( host.SetSelectedBone( viewport, mine, 4u ).IsSuccess() );

    const auto refused = host.SetSelectedBone( stranger, mine, 99u );
    EXPECT_FALSE( refused.IsSuccess() );
    // The refusal has to name WHO was refused and WHO holds it: a dropped UI event with no trace is the
    // shape this editor has paid for before, and "refused" alone is not actionable.
    EXPECT_NE( refused.GetError().find( "scene view 7" ), std::string::npos ) << refused.GetError();
    EXPECT_NE( refused.GetError().find( "scene view 0" ), std::string::npos ) << refused.GetError();

    EXPECT_EQ( host.SelectedBoneIndex(), 4 );
    EXPECT_EQ( host.Mode(), AuthoringMode::Skeleton );
}

TEST( AuthoringContextOwnership, EveryWriterIsGatedTheSameWay )
{
    AuthoringContextHost host;

    const auto       holder   = AuthoringOwner::ForPanel( "Details/Bone Tree" );
    const auto       stranger = AuthoringOwner::ForSceneView( 3 );
    AuthoringContext mine     = ContextFor( Character( 12 ) );
    host.Focus( holder, mine );

    ASSERT_TRUE( host.SetMode( holder, mine, AuthoringMode::Pose ).IsSuccess() );
    ASSERT_TRUE( host.SetShowBoneNames( holder, mine, true ).IsSuccess() );
    ASSERT_TRUE( host.SetSelectedBone( holder, mine, 2u ).IsSuccess() );

    // All three mutations, not just the one that happened to be tried: a gate that covers two of three
    // setters is a gate the third setter walks around, and that is exactly how the old type failed.
    EXPECT_FALSE( host.SetMode( stranger, mine, AuthoringMode::Object ).IsSuccess() );
    EXPECT_FALSE( host.SetShowBoneNames( stranger, mine, false ).IsSuccess() );
    EXPECT_FALSE( host.SetSelectedBone( stranger, mine, std::nullopt ).IsSuccess() );

    EXPECT_EQ( host.Mode(), AuthoringMode::Pose );
    EXPECT_TRUE( host.ShowBoneNames() );
    EXPECT_EQ( host.SelectedBoneIndex(), 2 );
}

// ── 2. NO CONTEXT AT ALL ──────────────────────────────────────────────────────────────────────────

TEST( AuthoringContextOwnership, WithNothingPublishedEveryReaderIsNeutralAndEveryWriteIsRefused )
{
    AuthoringContextHost host;

    EXPECT_FALSE( host.Published().has_value() );
    EXPECT_TRUE( host.Holder().IsNone() );
    EXPECT_EQ( host.Mode(), AuthoringMode::Object );
    EXPECT_FALSE( host.ShowsBones() );
    EXPECT_FALSE( host.IsPoseAuthoring() );
    EXPECT_FALSE( host.SelectedBone().has_value() );
    EXPECT_EQ( host.SelectedBoneIndex(), -1 );
    EXPECT_FALSE( host.ShowBoneNames() );
    EXPECT_TRUE( host.Entity().IsNull() );

    AuthoringContext orphan;
    // A write with no context is a DIFFERENT fact from a write by the wrong owner, and the message says
    // which: the caller's fix is "Focus() first", not "wait your turn".
    const auto refused = host.SetMode( AuthoringOwner::ForSceneView( 0 ), orphan, AuthoringMode::Skeleton );
    EXPECT_FALSE( refused.IsSuccess() );
    EXPECT_NE( refused.GetError().find( "no authoring context is published" ), std::string::npos )
         << refused.GetError();
    EXPECT_FALSE( host.Published().has_value() );
}

// ── 3. THE §5.3 DEFECT, AS A RELATION ─────────────────────────────────────────────────────────────

TEST( AuthoringContextOwnership, TwoSequencersOverTwoCharactersCannotOverwriteEachOther )
{
    AuthoringContextHost host;

    const Common::UUID hero    = Character( 100 );
    const Common::UUID villain = Character( 200 );

    const auto       docA = SequencerOver( hero );
    const auto       docB = SequencerOver( villain );
    AuthoringContext ctxA = ContextFor( hero );
    AuthoringContext ctxB = ContextFor( villain );

    // The user is authoring the hero: pose mode on, bone 5 keyed.
    host.Focus( docA, ctxA );
    ASSERT_TRUE( host.SetMode( docA, ctxA, AuthoringMode::Pose ).IsSuccess() );
    ASSERT_TRUE( host.SetSelectedBone( docA, ctxA, 5u ).IsSuccess() );

    // The second Sequencer draws in the same frame and runs ITS copy of the same unconditional line.
    // This is verbatim what the old code did through a process-wide setter that always succeeded.
    const auto stolen = host.SetMode( docB, ctxB, AuthoringMode::Skeleton );
    EXPECT_FALSE( stolen.IsSuccess() ) << "the unfocused Sequencer must not decide the focused one's mode";

    EXPECT_EQ( host.Entity(), hero );
    EXPECT_EQ( host.Mode(), AuthoringMode::Pose );
    EXPECT_EQ( host.SelectedBoneIndex(), 5 );

    // And when the user DOES move to the villain's window, that window's context is its own — not the
    // hero's bone index pointing into a skeleton that may not have five bones.
    host.Focus( docB, ctxB );
    EXPECT_EQ( host.Entity(), villain );
    EXPECT_EQ( host.SelectedBoneIndex(), -1 );
    EXPECT_EQ( host.Mode(), AuthoringMode::Object );

    // Going back restores the hero's own state, because it lives in docA's storage and not in the host.
    host.Focus( docA, ctxA );
    EXPECT_EQ( host.Mode(), AuthoringMode::Pose );
    EXPECT_EQ( host.SelectedBoneIndex(), 5 );
}

// ── 4. ONE CHARACTER, THREE SURFACES ──────────────────────────────────────────────────────────────

TEST( AuthoringContextOwnership, OneCharacterKeepsItsBoneAcrossDetailsViewportAndSequencer )
{
    AuthoringContextHost host;

    const Common::UUID hero = Character( 42 );

    const auto       details      = AuthoringOwner::ForPanel( "Details/Bone Tree" );
    const auto       viewport     = AuthoringOwner::ForSceneView( 0 );
    const auto       sequencer    = SequencerOver( hero );
    AuthoringContext detailsCtx   = ContextFor( hero );
    AuthoringContext viewportCtx  = ContextFor( hero );
    AuthoringContext sequencerCtx = ContextFor( hero );

    host.Focus( viewport, viewportCtx );
    ASSERT_TRUE( host.SetMode( viewport, viewportCtx, AuthoringMode::Skeleton ).IsSuccess() );
    ASSERT_TRUE( host.SetShowBoneNames( viewport, viewportCtx, true ).IsSuccess() );

    // Clicking a bone in the Details tree takes the context over. THE USER MUST NOT NOTICE: this is the
    // behaviour the single global gave them, and it is the half of the change that could regress silently.
    host.Focus( details, detailsCtx );
    EXPECT_EQ( host.Mode(), AuthoringMode::Skeleton );
    EXPECT_TRUE( host.ShowBoneNames() );
    ASSERT_TRUE( host.SetSelectedBone( details, detailsCtx, 9u ).IsSuccess() );

    // ... and the Sequencer, focused next, sees the bone the tree picked.
    host.Focus( sequencer, sequencerCtx );
    EXPECT_EQ( host.SelectedBoneIndex(), 9 );
    EXPECT_EQ( host.Mode(), AuthoringMode::Skeleton );
    ASSERT_TRUE( host.SetMode( sequencer, sequencerCtx, AuthoringMode::Pose ).IsSuccess() );

    // ... and the viewport, focused again, is posing rather than editing the rig.
    host.Focus( viewport, viewportCtx );
    EXPECT_TRUE( host.IsPoseAuthoring() );
    EXPECT_EQ( host.SelectedBoneIndex(), 9 );
}

TEST( AuthoringContextOwnership, LeavingBoneAuthoringDropsTheBoneItWasAbout )
{
    AuthoringContextHost host;

    const auto       viewport = AuthoringOwner::ForSceneView( 0 );
    AuthoringContext mine     = ContextFor( Character( 5 ) );
    host.Focus( viewport, mine );
    ASSERT_TRUE( host.SetMode( viewport, mine, AuthoringMode::Pose ).IsSuccess() );
    ASSERT_TRUE( host.SetSelectedBone( viewport, mine, 3u ).IsSuccess() );

    // What SkeletonEditMode::SetActive(false) did, kept: a selected bone surviving into Object mode is a
    // stale index nothing clears, and the next entry starts on a bone the user did not pick.
    ASSERT_TRUE( host.SetMode( viewport, mine, AuthoringMode::Object ).IsSuccess() );
    EXPECT_FALSE( host.ShowsBones() );
    EXPECT_EQ( host.SelectedBoneIndex(), -1 );
}

// ── 5. RELEASE ────────────────────────────────────────────────────────────────────────────────────

TEST( AuthoringContextOwnership, AClosingWindowCannotReleaseSomebodyElsesContext )
{
    AuthoringContextHost host;

    const Common::UUID hero = Character( 77 );
    const auto         docA = SequencerOver( hero );
    const auto         docB = SequencerOver( Character( 78 ) );
    AuthoringContext   ctxA = ContextFor( hero );

    host.Focus( docA, ctxA );
    ASSERT_TRUE( host.SetMode( docA, ctxA, AuthoringMode::Pose ).IsSuccess() );

    const auto refused = host.Release( docB );
    EXPECT_FALSE( refused.IsSuccess() );
    EXPECT_TRUE( host.Published().has_value() );
    EXPECT_EQ( host.Mode(), AuthoringMode::Pose );

    ASSERT_TRUE( host.Release( docA ).IsSuccess() );
    EXPECT_FALSE( host.Published().has_value() );
    EXPECT_TRUE( host.Holder().IsNone() );
    EXPECT_EQ( host.Mode(), AuthoringMode::Object );

    // Releasing twice is a refusal and not a silent success: the second caller believed it held something.
    EXPECT_FALSE( host.Release( docA ).IsSuccess() );
}

TEST( AuthoringContextOwnership, TheEditorsOneInstanceIsNotTheOneTestsUse )
{
    // The four statics could not offer this, which is why none of them was ever asserted: state on the
    // process leaks between tests, so every assertion about it depends on what ran before.
    AuthoringContextHost mine;
    const auto           owner = AuthoringOwner::ForSceneView( 1234 );
    AuthoringContext     ctx   = ContextFor( Character( 999 ) );
    mine.Focus( owner, ctx );

    // The editor's own instance is NAMED here and never written by a test — which is the point: it is
    // reachable only through that name, so nothing can touch it by accident the way a static could.
    EXPECT_TRUE( ActiveAuthoringContext().Holder().IsNone() );
    EXPECT_FALSE( mine.Holder().IsNone() );
}

TEST( AuthoringContextOwnership, EveryModeCanNameItself )
{
    // A switch over the enum with no default: adding a mode without giving it a name is a compile
    // warning here rather than a log line reading "Object" about something that is not Object.
    EXPECT_STREQ( AuthoringModeName( AuthoringMode::Object ), "Object" );
    EXPECT_STREQ( AuthoringModeName( AuthoringMode::Skeleton ), "Skeleton" );
    EXPECT_STREQ( AuthoringModeName( AuthoringMode::Pose ), "Pose" );
}

// ── 6 & 7. CENSUSES OVER THE REPOSITORY'S SOURCE TEXT ─────────────────────────────────────────────
//
// WHY A CENSUS AND NOT A TEST. "The global is read nowhere" is a statement about every translation unit
// in the tree, including the 258 of 305 that no suite compiles (scripts/CI/UnreachedSources.sh). No run
// of this binary can see it.
//
// COMMENTS AND LITERALS ARE BLANKED FIRST, and that is not tidiness. Twice in one week a census went red
// on its own prose — once on a comment quoting the very thing it forbids, and the second time in the same
// run as a REAL finding that would have drowned with it. The headers here deliberately name
// `SkeletonEditMode` in their history notes, so a census matching raw text would be red on arrival and
// switched off within the day.

namespace
{
    namespace fs = std::filesystem;

    std::string ReadWhole( const fs::path& path )
    {
        const std::ifstream in( path, std::ios::binary );
        std::stringstream   ss;
        ss << in.rdbuf();
        return ss.str();
    }

    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            const std::ifstream probe( prefix + "Editor/Source/Editor/Core/Selection/ControlRigEditMode.hpp" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    bool IsSource( const fs::path& path )
    {
        const std::string ext = path.extension().string();
        return ext == ".cpp" || ext == ".hpp" || ext == ".h" || ext == ".inl";
    }

    struct Walk
    {
        int                      Files = 0; // derived from the walk; never written down
        std::vector<std::string> Hits;      // "<repo-relative path>:<line>"
    };

    // Every occurrence of @p word as a whole identifier in the code (not the prose) under @p roots.
    Walk CountIdentifier( const std::string& repoRoot, const std::vector<std::string>& roots,
                          const std::string& word )
    {
        Walk walk;
        for ( const auto& root : roots )
        {
            const fs::path base = repoRoot + root;
            if ( !fs::exists( base ) )
                continue;
            for ( const auto& entry : fs::recursive_directory_iterator( base ) )
            {
                if ( !entry.is_regular_file() || !IsSource( entry.path() ) )
                    continue;
                ++walk.Files;

                const std::string code =
                     Desert::Tests::ConsumerText::StripCommentsAndLiterals( ReadWhole( entry.path() ) );
                for ( const std::size_t at : Desert::Tests::ConsumerText::WordPositions( code, word ) )
                {
                    const int line = 1 + static_cast<int>( std::count(
                                              code.begin(), code.begin() + static_cast<long>( at ), '\n' ) );
                    walk.Hits.push_back( fs::relative( entry.path(), repoRoot ).string() + ":" +
                                         std::to_string( line ) );
                }
            }
        }
        return walk;
    }
} // namespace

TEST( AuthoringContextCensus, TheScannerCanSeeWhatItForbidsAndIgnoresProse )
{
    // POSITIVE CONTROL FOR THE INSTRUMENT ITSELF. A census whose scanner is blind answers "nobody reads
    // this" in exactly the confident voice it uses when that is true.
    const std::string code = "void f() { SkeletonEditMode::IsActive(); }";
    EXPECT_EQ( Desert::Tests::ConsumerText::WordPositions(
                    Desert::Tests::ConsumerText::StripCommentsAndLiterals( code ), "SkeletonEditMode" )
                    .size(),
               1u );

    // And the negative one, which is why the blanking is there at all.
    const std::string prose = "// this used to be SkeletonEditMode\nconst char* s = \"SkeletonEditMode\";\n";
    EXPECT_TRUE( Desert::Tests::ConsumerText::WordPositions(
                      Desert::Tests::ConsumerText::StripCommentsAndLiterals( prose ), "SkeletonEditMode" )
                      .empty() );
}

TEST( AuthoringContextCensus, SkeletonEditModeIsReadNowhereAndItsHeaderIsGone )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "run from the repository (or a build directory under it)";

    // The header is DELETED, not deprecated: contract §3, the old path goes with the change that
    // replaces it. If it came back, the census below would still be green while the class was alive.
    EXPECT_FALSE( fs::exists( root + "Editor/Source/Editor/Core/Selection/SkeletonEditMode.hpp" ) );

    const Walk walk = CountIdentifier(
         root, { "Editor/Source", "Desert/Desert/Source", "Desert/Tests", "Runtime" }, "SkeletonEditMode" );

    EXPECT_GT( walk.Files, 200 ) << "the walk found almost nothing — the roots are wrong, not the tree";

    std::string where;
    for ( const auto& hit : walk.Hits )
        where += "\n  " + hit;
    EXPECT_TRUE( walk.Hits.empty() ) << walk.Hits.size() << " live reference(s) to the removed global:" << where;
}

TEST( AuthoringContextCensus, OnlyTheThreeOwningSurfacesWriteTheContext )
{
    // A REGISTER OF NAMED ROWS, NOT A NUMBER. A census pinning a count can be satisfied by editing the
    // count; this names each surface that is allowed to take or mutate the authoring context, and derives
    // the number from the walk. A fourth writer — the next panel that wants "just this one bit" — is what
    // this is here to catch, because that is how the thing it replaced grew to five files.
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::set<std::string> allowed = {
         "Editor/Source/Editor/Panels/ViewportPanel/ViewportPanel.cpp",
         "Editor/Source/Editor/Panels/Sequencer/SequencerPanel.cpp",
         "Editor/Source/Editor/Panels/SceneProperties/ComponentWidgets/SkinnedMeshComponentWidget.cpp",
    };

    std::set<std::string> writers;
    int                   files = 0;
    for ( const auto& entry : fs::recursive_directory_iterator( fs::path( root + "Editor/Source" ) ) )
    {
        if ( !entry.is_regular_file() || !IsSource( entry.path() ) )
            continue;
        ++files;
        const std::string code =
             Desert::Tests::ConsumerText::StripCommentsAndLiterals( ReadWhole( entry.path() ) );

        // The mutating half of the surface. Reads (`ShowsBones`, `SelectedBoneIndex`, ...) are deliberately
        // NOT here: anybody may read, and the overlay and the gizmo do.
        const bool writes = code.find( ".Focus(" ) != std::string::npos ||
                            code.find( ".SetMode(" ) != std::string::npos ||
                            code.find( ".SetSelectedBone(" ) != std::string::npos ||
                            code.find( ".SetShowBoneNames(" ) != std::string::npos;
        if ( !writes )
            continue;
        if ( code.find( "ActiveAuthoringContext" ) == std::string::npos )
            continue; // some other type's Focus/SetMode

        std::string rel = fs::relative( entry.path(), root ).string();
        writers.insert( rel );
    }

    EXPECT_GT( files, 200 );

    for ( const auto& writer : writers )
        EXPECT_TRUE( allowed.count( writer ) == 1 ) << "a new surface writes the authoring context: " << writer;

    // Each named row is really there. A register whose rows have all gone stale reaches an empty set and
    // passes, which is the way a gate stops guarding anything without anyone noticing.
    for ( const auto& row : allowed )
        EXPECT_TRUE( writers.count( row ) == 1 ) << "this row no longer writes the context: " << row;
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
