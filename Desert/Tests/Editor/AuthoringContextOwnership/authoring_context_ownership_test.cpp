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
// AND FOUR CENSUSES OVER THE REPOSITORY'S SOURCE TEXT (sections 6 and 7), because the interesting claim
// — "the global is not read anywhere any more" — is about the tree and not about any run of this binary.
// Every count is DERIVED from the walk; none is written down.
//
// SECTION 8 IS 07 §14.2's FOUR MODES, added when the viewport got one switcher instead of a toggle:
// `Mode::Control` arrived with its reader and `Editor::Core::ControlRigEditMode` — three more statics on
// the process, the identical defect on the control side — was dissolved the same way. Its relations are
// that no two modes answer the readers alike (a mode nobody can distinguish is a dead segment), that
// each mode drops the selection it cannot mean, and that the bind-pose preview is Skeleton ALONE, which
// is 07 §1.3's defect and the one thing §5.3 deliberately left broken so that its change stayed a move
// of ownership.

#include <Editor/Core/Selection/AuthoringContext.hpp>

#include "../../Engine/SettingConsumers/setting_consumers_reader.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <tuple>
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

// ── 8. THE FOUR MODES (07 §14.2) ──────────────────────────────────────────────────────────────────
//
// The switcher's whole claim is "four modes, and each one changes something". Three relations say it:
// the modes are DISTINGUISHED (no two answer the readers identically), each mode DROPS the selections it
// cannot mean, and — the one that is a bug fix rather than a new feature — the bind-pose preview belongs
// to Skeleton ALONE (07 §1.3). The last one lives here and not in the viewport because
// `ViewportPanel.cpp` is compiled by no suite at all.

TEST( AuthoringContextModes, NoTwoModesAnswerTheReadersTheSameWay )
{
    // A MODE THAT NOBODY CAN TELL FROM ANOTHER MODE IS A DEAD SEGMENT ON THE STRIP — contract §3, and the
    // exact reason `Mode::Control` was kept out until this change. Written as "the answer vectors are all
    // different" rather than as four hand-checked truth tables so that a fifth mode cannot be added as a
    // duplicate of an existing one and pass.
    std::set<std::tuple<bool, bool, bool>> answers;
    for ( const AuthoringMode mode : Desert::Editor::Core::kAuthoringModes )
    {
        AuthoringContext context;
        context.EnterMode( mode );
        answers.insert( { context.ShowsBones(), context.ShowsControls(), context.PreviewsBindPose() } );
    }
    EXPECT_EQ( answers.size(), Desert::Editor::Core::kAuthoringModes.size() );
}

TEST( AuthoringContextModes, TheBindPosePreviewIsSkeletonAloneAndPoseNoLongerGetsTheRigsAnswer )
{
    // 07 §1.3. The viewport asked `ShowsBones()` — true for Skeleton AND Pose — so a clip being authored
    // was drawn in BIND pose, which hides the very edit the bone gizmo is making. The previous change
    // (§5.3) preserved the defect deliberately so that it stayed a move of ownership; this is where it is
    // fixed, and the two halves are asserted TOGETHER because the whole defect was that one was used for
    // the other.
    AuthoringContext context;

    context.EnterMode( AuthoringMode::Skeleton );
    EXPECT_TRUE( context.ShowsBones() );
    EXPECT_TRUE( context.PreviewsBindPose() );

    context.EnterMode( AuthoringMode::Pose );
    EXPECT_TRUE( context.ShowsBones() ) << "pose authoring still draws the bones; only the preview changed";
    EXPECT_FALSE( context.PreviewsBindPose() ) << "07 §1.3: the clip's pose IS what the user is editing";

    context.EnterMode( AuthoringMode::Control );
    EXPECT_FALSE( context.ShowsBones() );
    EXPECT_FALSE( context.PreviewsBindPose() );

    context.EnterMode( AuthoringMode::Object );
    EXPECT_FALSE( context.PreviewsBindPose() );
}

TEST( AuthoringContextModes, EachModeDropsTheSelectionItCannotMean )
{
    // An index only means something inside the mode that produced it. A control index carried into
    // Skeleton would highlight a BONE of that number, and a bone index carried into Control a control of
    // it — the stale-handle defect with two smaller names. Asserted over the whole table rather than for
    // the two transitions that happen to be easy to reach from the toolbar.
    for ( const AuthoringMode mode : Desert::Editor::Core::kAuthoringModes )
    {
        AuthoringContext context;
        context.SelectedBone    = 4u;
        context.SelectedControl = 9u;
        context.EnterMode( mode );

        EXPECT_EQ( context.SelectedBone.has_value(), context.ShowsBones() ) << AuthoringModeName( mode );
        EXPECT_EQ( context.SelectedControl.has_value(), context.ShowsControls() ) << AuthoringModeName( mode );
    }
}

TEST( AuthoringContextOwnership, TwoCharactersCannotOverwriteEachOthersControlSelection )
{
    // SECTION 3's DEFECT, ON THE CONTROL SIDE. `ControlRigEditMode` held ONE `s_Selected` for the whole
    // process, so the Control Rig panel and the viewport overlay of a second character shared an index
    // into the FIRST character's hierarchy — and a rig with fewer controls made it an out-of-range read
    // the panel then had to guess about. Neither half could refuse the other; now the second one can.
    AuthoringContextHost host;

    const Common::UUID hero    = Character( 21 );
    const Common::UUID villain = Character( 22 );

    const auto       viewport = AuthoringOwner::ForSceneView( 0 );
    AuthoringContext heroCtx  = ContextFor( hero );
    host.Focus( viewport, heroCtx );
    ASSERT_TRUE( host.SetMode( viewport, heroCtx, AuthoringMode::Control ).IsSuccess() );
    ASSERT_TRUE( host.SetSelectedControl( viewport, heroCtx, 6u ).IsSuccess() );
    ASSERT_TRUE( host.SetControlRotate( viewport, heroCtx, true ).IsSuccess() );

    const auto       panel      = AuthoringOwner::ForPanel( "Control Rig" );
    AuthoringContext villainCtx = ContextFor( villain );

    // The panel writing WITHOUT taking the context is refused and changes nothing — the old statics
    // accepted exactly this and it is the whole §5.3 defect.
    const auto refused = host.SetSelectedControl( panel, villainCtx, 0u );
    EXPECT_FALSE( refused.IsSuccess() );
    EXPECT_EQ( host.SelectedControl(), std::optional<uint32_t>( 6u ) );

    // And when it DOES take it, adoption is by entity: a different character starts from its own state
    // rather than inheriting the hero's control index.
    host.Focus( panel, villainCtx );
    EXPECT_EQ( host.Entity(), villain );
    EXPECT_FALSE( host.SelectedControl().has_value() );
    EXPECT_FALSE( host.ControlRotate() ) << "the manipulator bit is per rig, like everything else here";
}

TEST( AuthoringContextOwnership, OneCharacterKeepsItsControlAcrossThePanelAndTheViewport )
{
    // The other half of section 4, for the control side: the Control Rig panel says WHICH control and the
    // viewport overlay DRAGS it, so a selection that did not survive the move between them would make the
    // two halves unusable together — which is the arrangement the panel's own header promises.
    AuthoringContextHost host;

    const Common::UUID hero  = Character( 31 );
    const auto         panel = AuthoringOwner::ForPanel( "Control Rig" );
    AuthoringContext   panelCtx = ContextFor( hero );

    host.Focus( panel, panelCtx );
    ASSERT_TRUE( host.SetMode( panel, panelCtx, AuthoringMode::Control ).IsSuccess() );
    ASSERT_TRUE( host.SetSelectedControl( panel, panelCtx, 3u ).IsSuccess() );

    const auto       viewport = AuthoringOwner::ForSceneView( 0 );
    AuthoringContext viewCtx  = ContextFor( hero );
    host.Focus( viewport, viewCtx );

    EXPECT_EQ( host.Mode(), AuthoringMode::Control );
    EXPECT_EQ( host.SelectedControl(), std::optional<uint32_t>( 3u ) );
    // The viewport's OWN copy was written back by Focus(), which is what lets it read its member for the
    // rest of the frame instead of the publication.
    EXPECT_EQ( viewCtx.SelectedControl, std::optional<uint32_t>( 3u ) );
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
            // THE PROBE NAMES A FILE THIS SUITE IS ABOUT AND DOES NOT DELETE. It used to name
            // `ControlRigEditMode.hpp`, which §14.2 then removed — and a root-finder anchored on a file a
            // census EXISTS TO SEE REMOVED returns "" the day it succeeds, after which every census below
            // fails on its ASSERT for a reason that has nothing to do with the tree.
            const std::ifstream probe( prefix + "Editor/Source/Editor/Core/Selection/AuthoringContext.hpp" );
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

    // DOES THIS FILE MUTATE THE AUTHORING CONTEXT? Matched on the TYPE'S OWN CONTRACT — every mutating
    // entry point takes the owner FIRST — rather than on the method name alone.
    //
    // THE NAME ALONE IS NOT ENOUGH, AND THAT IS MEASURED, NOT FEARED. This used to be "the file contains
    // `.Focus(` (etc.) AND the word ActiveAuthoringContext somewhere", two independent substrings over a
    // whole translation unit. EditorLayer.cpp went red the day it began READING the context for the
    // control channel's snapshot, because 2000 lines away a viewport camera has a `Focus()` of its own.
    // A census that fires on an innocent file is a census somebody switches off — and it would have
    // taken a real finding with it (07 §8.3).
    NO_DISCARD bool WritesTheAuthoringContext( const std::string& code )
    {
        static const std::vector<std::string> kMutators = {
             ".Focus(", ".SetMode(", ".SetSelectedBone(", ".SetShowBoneNames(", ".SetSelectedControl(",
             ".SetControlRotate(",
        };

        for ( const std::string& mutator : kMutators )
        {
            for ( std::size_t at = code.find( mutator ); at != std::string::npos;
                  at             = code.find( mutator, at + 1 ) )
            {
                // The first argument, as written. `AuthoringContextHost` accepts a mutation only from an
                // owner, so an owner in that position IS the signature of a write to it.
                std::size_t first = at + mutator.size();
                while ( first < code.size() && std::isspace( static_cast<unsigned char>( code[first] ) ) )
                    ++first;
                std::size_t end = first;
                while ( end < code.size() &&
                        ( std::isalnum( static_cast<unsigned char>( code[end] ) ) || code[end] == '_' ||
                          code[end] == ':' ) )
                    ++end;

                std::string argument = code.substr( first, end - first );
                std::transform( argument.begin(), argument.end(), argument.begin(),
                                []( unsigned char c ) { return static_cast<char>( std::tolower( c ) ); } );
                if ( argument.size() >= 5 && argument.compare( argument.size() - 5, 5, "owner" ) == 0 )
                    return true;
            }
        }
        return false;
    }

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

TEST( AuthoringContextCensus, ControlRigEditModeIsReadNowhereAndItsHeaderIsGone )
{
    // The same census as the one above it, for the three statics 07 §14.2 dissolved: `s_Active`,
    // `s_Selected` and `s_Rotate`, written by the Control Rig panel and the viewport overlay and toggled
    // by a palette entry. It had the IDENTICAL defect for the identical reason and it was left standing
    // on purpose until `Mode::Control` had a reader — which is this change.
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "run from the repository (or a build directory under it)";

    // Deleted, not deprecated: contract §3. Without this line the identifier census below stays green
    // while the class is alive and simply unused — and the next agent finds two ways to do one thing.
    EXPECT_FALSE( fs::exists( root + "Editor/Source/Editor/Core/Selection/ControlRigEditMode.hpp" ) );

    const Walk walk = CountIdentifier(
         root, { "Editor/Source", "Desert/Desert/Source", "Desert/Tests", "Runtime" }, "ControlRigEditMode" );

    EXPECT_GT( walk.Files, 200 ) << "the walk found almost nothing — the roots are wrong, not the tree";

    std::string where;
    for ( const auto& hit : walk.Hits )
        where += "\n  " + hit;
    EXPECT_TRUE( walk.Hits.empty() ) << walk.Hits.size() << " live reference(s) to the removed global:" << where;
}

TEST( AuthoringContextCensus, EveryModesOwnReaderExistsOutsideThisHeader )
{
    // WHY THIS CENSUS EXISTS. The reason `Mode::Control` was absent for a whole task is written into the
    // header it was absent from: a mode nothing reads is a knob that moves nothing, and the four-way
    // switcher would have shipped one dead segment out of four. That argument is only enforceable over
    // the TREE — the predicates below are inline in a header no suite's failure would notice, and the
    // files that call them (ViewportPanel.cpp, LightGizmoRenderer.cpp) are compiled by no suite either.
    //
    // A REGISTER OF NAMED ROWS, and the rows are the DISTINGUISHING readers: the thing each mode does
    // that no other mode does. `Object` has none of its own by construction — it is the absence of the
    // other three — so it is not a row, and saying that here is what stops the next reader adding a
    // hollow one for symmetry.
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    // EACH ROW NAMES THE FILE THAT MUST MAKE THE DECISION, not merely "somebody reads it somewhere".
    // The weaker form was tried and it is too weak: deleting the control overlay from the viewport
    // leaves the Control Rig panel's checkbox reading the same predicate, so the census stays green
    // while `Control` has stopped changing anything a viewport shows — which is precisely the dead knob
    // the row exists to forbid. A named file also survives the predicate being inlined as
    // `Mode() == AuthoringMode::Control` at five call sites, which is the other way a single source of
    // truth quietly stops being one.
    struct Row
    {
        std::string Predicate;
        std::string MustBeReadBy; // repo-relative path
        std::string Why;
    };

    const std::vector<Row> rows = {
         { "ShowsBones", "Editor/Source/Editor/Panels/ViewportPanel/LightGizmoRenderer.cpp",
           "Skeleton + Pose: the bone overlay" },
         { "PreviewsBindPose", "Editor/Source/Editor/Panels/ViewportPanel/ViewportPanel.cpp",
           "Skeleton alone: 07 §1.3's bind-pose preview" },
         { "IsPoseAuthoring", "Editor/Source/Editor/Panels/ViewportPanel/Tools/GizmoController.cpp",
           "Pose alone: the gizmo writes the animator's pose buffer instead of the bind transform" },
         { "ShowsControls", "Editor/Source/Editor/Panels/ViewportPanel/LightGizmoRenderer.cpp",
           "Control: the control-shape overlay is drawn at all" },
         { "SelectedControl", "Editor/Source/Editor/Panels/ViewportPanel/LightGizmoRenderer.cpp",
           "Control: which shape is highlighted and grabbed" },
         { "ControlRotate", "Editor/Source/Editor/Panels/ViewportPanel/LightGizmoRenderer.cpp",
           "Control: what a grab writes — translate or rotate" },
    };

    std::map<std::string, std::vector<std::string>> readers;
    int                                             files = 0;
    for ( const auto& entry : fs::recursive_directory_iterator( fs::path( root + "Editor/Source" ) ) )
    {
        if ( !entry.is_regular_file() || !IsSource( entry.path() ) )
            continue;
        const std::string relative = fs::relative( entry.path(), root ).string();
        if ( relative.find( "AuthoringContext.hpp" ) != std::string::npos )
            continue; // the definitions themselves are not readers of themselves
        ++files;

        const std::string code =
             Desert::Tests::ConsumerText::StripCommentsAndLiterals( ReadWhole( entry.path() ) );
        for ( const Row& row : rows )
        {
            if ( !Desert::Tests::ConsumerText::WordPositions( code, row.Predicate ).empty() )
                readers[row.Predicate].push_back( relative );
        }
    }

    EXPECT_GT( files, 200 ) << "the walk found almost nothing — the root is wrong, not the tree";

    for ( const Row& row : rows )
    {
        const std::vector<std::string>& where = readers[row.Predicate];
        EXPECT_FALSE( where.empty() ) << "no file reads '" << row.Predicate
                                      << "', so the mode it distinguishes moves nothing — " << row.Why;
        EXPECT_NE( std::find( where.begin(), where.end(), row.MustBeReadBy ), where.end() )
             << row.MustBeReadBy << " no longer reads '" << row.Predicate << "' — " << row.Why;
    }
}

TEST( AuthoringContextCensus, TheWriteScannerSeesAWriteAndNotSomebodyElsesFocus )
{
    // POSITIVE AND NEGATIVE CONTROL FOR THE SECOND SCANNER. The register below derives its answer from
    // this function, and a scanner that sees nothing reports "only the allowed surfaces write it" in
    // exactly the voice it uses when that is true.
    EXPECT_TRUE( WritesTheAuthoringContext( "host.SetMode( m_AuthoringOwner, m_Authoring, mode );" ) );
    EXPECT_TRUE( WritesTheAuthoringContext( "ctx.Focus( BoneTreeOwner(), mine );" ) );
    EXPECT_TRUE( WritesTheAuthoringContext( "authoring.SetSelectedControl( owner, mine, control );" ) );

    // The exact line that made the old scanner red: a viewport camera framing a point, in a file that
    // reads the authoring context for the control channel two thousand lines away.
    EXPECT_FALSE( WritesTheAuthoringContext(
         "camera.Focus( ViewportCameraFocalPoint( position, forward ), distance );\n"
         "ActiveAuthoringContext().Mode();" ) );
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
         // The two control-rig surfaces, added by 07 §14.2 when `ControlRigEditMode`'s three statics were
         // dissolved into this type. They are the SAME two halves the bone side already had — a panel
         // that says which control, and a viewport overlay that draws and drags it — and they arrive as
         // named rows rather than as a bumped count.
         "Editor/Source/Editor/Panels/Animation/ControlRigPanel.cpp",
         "Editor/Source/Editor/Panels/ViewportPanel/LightGizmoRenderer.cpp",
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
        if ( !WritesTheAuthoringContext( code ) )
            continue;

        writers.insert( fs::relative( entry.path(), root ).string() );
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
