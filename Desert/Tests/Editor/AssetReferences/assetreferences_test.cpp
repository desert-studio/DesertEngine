#include <gtest/gtest.h>

#include <Editor/Core/AssetReferences.hpp>

#include <Common/Utilities/ContentUpdate.hpp>

using Desert::Editor::AssetReferenceIndex;
using Desert::Editor::WithholdReferencedRemovals;

namespace
{
    AssetReferenceIndex::Entry Make( std::string path, std::string ext,
                                     std::vector<std::string> tokens, std::string text )
    {
        AssetReferenceIndex::Entry e;
        e.Path   = std::move( path );
        e.Ext    = std::move( ext );
        e.Tokens = std::move( tokens );
        e.Text   = std::move( text );
        return e;
    }
} // namespace

// A material that embeds a texture's handle is reported as a referencer of that texture.
TEST( AssetReferenceIndex, FindsHandleReference )
{
    AssetReferenceIndex idx;
    idx.Add( Make( "Textures/Albedo.png", ".png", { "555000111" }, "" ) ); // binary target
    idx.Add( Make( "Materials/M.demat", ".demat", { "999" }, "{\"Textures\":[555000111]}" ) );

    const auto refs = idx.ReferencersOf( "Textures/Albedo.png" );
    ASSERT_EQ( refs.size(), 1u );
    EXPECT_EQ( refs[0], "Materials/M.demat" );
    EXPECT_TRUE( idx.IsReferenced( "Textures/Albedo.png" ) );
}

// A handle token must not match as a substring of a longer number.
TEST( AssetReferenceIndex, HandleMatchIsDigitBounded )
{
    AssetReferenceIndex idx;
    idx.Add( Make( "Textures/T.png", ".png", { "123" }, "" ) );
    idx.Add( Make( "Materials/Bigger.demat", ".demat", {}, "{\"h\":91234}" ) ); // 123 inside 91234
    idx.Add( Make( "Materials/Exact.demat", ".demat", {}, "{\"h\":123}" ) );     // exact

    const auto refs = idx.ReferencersOf( "Textures/T.png" );
    ASSERT_EQ( refs.size(), 1u );
    EXPECT_EQ( refs[0], "Materials/Exact.demat" );
}

// References by path string are found too.
TEST( AssetReferenceIndex, FindsPathReference )
{
    AssetReferenceIndex idx;
    idx.Add( Make( "Textures/Grid.png", ".png", { "Assets/Textures/Grid.png" }, "" ) );
    idx.Add( Make( "Scenes/Main.desce", ".desce", {}, "tex = \"Assets/Textures/Grid.png\"" ) );

    EXPECT_TRUE( idx.IsReferenced( "Textures/Grid.png" ) );
}

// Binary entries (no text) are never counted as referencers.
TEST( AssetReferenceIndex, BinaryEntriesAreNotReferencers )
{
    AssetReferenceIndex idx;
    idx.Add( Make( "Textures/A.png", ".png", { "111" }, "" ) );
    idx.Add( Make( "Textures/B.png", ".png", { "222" }, "" ) ); // has the token but no text to scan

    EXPECT_FALSE( idx.IsReferenced( "Textures/A.png" ) );
    EXPECT_TRUE( idx.ReferencersOf( "Textures/A.png" ).empty() );
}

// Only unreferenced LEAF assets are orphans; roots (scenes) and referenced leaves are excluded.
TEST( AssetReferenceIndex, OrphansAreUnreferencedLeaves )
{
    AssetReferenceIndex idx;
    idx.Add( Make( "Textures/Used.png", ".png", { "700" }, "" ) );
    idx.Add( Make( "Textures/Unused.png", ".png", { "800" }, "" ) );
    idx.Add( Make( "Materials/M.demat", ".demat", { "900" }, "{\"Textures\":[700]}" ) );
    idx.Add( Make( "Scenes/Main.desce", ".desce", {}, "{\"mat\":900}" ) ); // references the material

    const auto orphans = idx.Orphans( { ".png", ".demat" } );

    // Used.png is referenced by M; M is referenced by the scene; the scene is a root (not a leaf ext).
    // Only Unused.png remains.
    ASSERT_EQ( orphans.size(), 1u );
    EXPECT_EQ( orphans[0], "Textures/Unused.png" );
}

// Unknown asset path yields no referencers rather than crashing.
TEST( AssetReferenceIndex, UnknownPathIsEmpty )
{
    AssetReferenceIndex idx;
    idx.Add( Make( "Textures/A.png", ".png", { "1" }, "" ) );
    EXPECT_TRUE( idx.ReferencersOf( "does/not/exist.png" ).empty() );
    EXPECT_FALSE( idx.IsReferenced( "does/not/exist.png" ) );
}

// ============================================================================================
// A CONTENT UPDATE THAT REMOVES FILES, MET BY THE INDEX. Both halves of one relation, because they
// fail in opposite directions: "never delete" satisfies the first and defeats the whole point of
// deletions, "always delete" satisfies the second and breaks somebody's scene.
//
// The collection installer plans removals from manifests alone — a manifest knows what the source
// stopped shipping and nothing at all about who is still pointing at it. This is the only moment
// anything can notice, and it has to notice now: the reference rewrite that will show the affected
// scenes in a dialog has not moved into the shared submodule yet, so without this the deletion half
// would ship ahead of the thing that makes it safe.
// ============================================================================================

namespace
{
    // The three manifests a real update is planned from, reduced to the one file under test: the
    // source shipped it, it is still on disk untouched, and the new release does not carry it.
    Common::Utils::ContentUpdatePlan PlanRemovalOf( const std::string& key )
    {
        const Common::Utils::ContentManifestEntry entry{ key, 4, 0xabcdull };

        Common::Utils::ContentManifest recorded;
        recorded.Insert( entry );
        Common::Utils::ContentManifest onDisk;
        onDisk.Insert( entry );
        const Common::Utils::ContentManifest incoming; // the source dropped it

        auto plan = Common::Utils::PlanContentUpdate( recorded, onDisk, incoming,
                                                      Common::Utils::ContentAuthorship::LocallyAuthored );
        // The premise of both tests below. If this ever stops being a removal they prove nothing.
        EXPECT_EQ( plan.Steps.size(), 1u );
        EXPECT_EQ( plan.Steps[0].Action, Common::Utils::ContentAction::Remove );
        return plan;
    }
} // namespace

TEST( AssetReferenceIndex, AFileASceneStillUsesSurvivesTheSourcesRemoval )
{
    AssetReferenceIndex idx;
    // A collection material, keyed the way the project index keys it: relative to the assets root.
    idx.Add( Make( "Collections/Foliage/meshes/Leaf.demat", ".demat", { "4400000001" }, "{}" ) );
    idx.Add( Make( "Scenes/Main.desce", ".desce", {}, "{\"Material\":4400000001}" ) );
    idx.Add( Make( "Scenes/Second.desce", ".desce", {}, "{\"Material\":4400000001}" ) );

    auto       plan     = PlanRemovalOf( "meshes/Leaf.demat" );
    const auto withheld = WithholdReferencedRemovals( plan, idx, "Collections/Foliage" );

    ASSERT_EQ( withheld.size(), 1u );
    EXPECT_EQ( withheld[0].Key, "meshes/Leaf.demat" );
    // Named, and counted: "still used by Main.desce" when two scenes use it is a message that gets
    // acted on once and is then wrong.
    EXPECT_EQ( withheld[0].FirstReferencer, "Scenes/Main.desce" );
    EXPECT_EQ( withheld[0].ReferencerCount, 2u );

    // THE ASSERTION: the plan no longer deletes it.
    EXPECT_EQ( plan.Steps[0].Action, Common::Utils::ContentAction::None );
    // And the OBSERVATION is untouched — the source did drop it, and that stays true whatever we
    // decide to do about it. This is what lets the next update ask the same question again.
    EXPECT_EQ( plan.Steps[0].State, Common::Utils::ContentFileState::SourceDeleted );
}

TEST( AssetReferenceIndex, AFileNothingUsesIsRemovedAsPlanned )
{
    AssetReferenceIndex idx;
    idx.Add( Make( "Collections/Foliage/meshes/Leaf.demat", ".demat", { "4400000001" }, "{}" ) );
    // A scene that references some OTHER material. Present on purpose: an index with nothing in it
    // would also pass this test, and would pass it for the wrong reason.
    idx.Add( Make( "Scenes/Main.desce", ".desce", {}, "{\"Material\":9900000002}" ) );

    auto       plan     = PlanRemovalOf( "meshes/Leaf.demat" );
    const auto withheld = WithholdReferencedRemovals( plan, idx, "Collections/Foliage" );

    EXPECT_TRUE( withheld.empty() );
    EXPECT_EQ( plan.Steps[0].Action, Common::Utils::ContentAction::Remove );
}

// The prefix is what makes the plan's key and the index's key the same string, and getting it wrong
// fails SILENTLY in the dangerous direction: an unknown path has no referencers, so every removal
// would sail through the guard.
TEST( AssetReferenceIndex, AKeyThatTheIndexDoesNotRecogniseWithholdsNothing )
{
    AssetReferenceIndex idx;
    idx.Add( Make( "Collections/Foliage/meshes/Leaf.demat", ".demat", { "4400000001" }, "{}" ) );
    idx.Add( Make( "Scenes/Main.desce", ".desce", {}, "{\"Material\":4400000001}" ) );

    auto plan = PlanRemovalOf( "meshes/Leaf.demat" );
    EXPECT_TRUE( WithholdReferencedRemovals( plan, idx, "Collections/WrongPack" ).empty() );
    EXPECT_EQ( plan.Steps[0].Action, Common::Utils::ContentAction::Remove );

    // Which is why the caller derives the prefix from the collection's own place under the assets
    // root rather than spelling it, and why this test exists to say so out loud.
    auto correct = PlanRemovalOf( "meshes/Leaf.demat" );
    EXPECT_EQ( WithholdReferencedRemovals( correct, idx, "Collections/Foliage" ).size(), 1u );
}

// Withholding is not a general "turn this step off": a caller must not be able to believe it saved a
// file that was never going to be deleted.
TEST( AssetReferenceIndex, OnlyAPlannedRemovalCanBeWithheld )
{
    Common::Utils::ContentManifest recorded;
    Common::Utils::ContentManifest onDisk;
    Common::Utils::ContentManifest incoming;
    incoming.Insert( { "new.demat", 4, 0x1234ull } ); // an ADDITION, not a removal

    auto plan = Common::Utils::PlanContentUpdate( recorded, onDisk, incoming,
                                                  Common::Utils::ContentAuthorship::LocallyAuthored );
    ASSERT_EQ( plan.Steps.size(), 1u );
    ASSERT_EQ( plan.Steps[0].Action, Common::Utils::ContentAction::Write );

    EXPECT_FALSE( plan.WithholdRemoval( "new.demat" ) );
    EXPECT_FALSE( plan.WithholdRemoval( "not/in/the/plan.demat" ) );
    EXPECT_EQ( plan.Steps[0].Action, Common::Utils::ContentAction::Write );
}

// ── THE FORWARD DIRECTION: what must travel with an asset ───────────────────────────────────────
//
// Added with Tools/AssetClosure, which is what decides the contents of a shipped package. The
// packager needs the opposite question from "find references", and the two must not be able to
// disagree — they are the same edges read in opposite directions, so they are the same code.

TEST( AssetReferenceIndex, ReferencedByIsTheInverseOfReferencersOf )
{
    AssetReferenceIndex idx;
    idx.Add( Make( "Textures/Albedo.png", ".png", { "555000111" }, "" ) );
    idx.Add( Make( "Materials/M.demat", ".demat", { "999" }, "{\"Textures\":[555000111]}" ) );

    const auto forward = idx.ReferencedBy( "Materials/M.demat" );
    ASSERT_EQ( forward.size(), 1u );
    EXPECT_EQ( forward[0], "Textures/Albedo.png" );

    // The relation, asserted rather than each side: A is in ReferencedBy(B) exactly when B is in
    // ReferencersOf(A).
    const auto backward = idx.ReferencersOf( "Textures/Albedo.png" );
    ASSERT_EQ( backward.size(), 1u );
    EXPECT_EQ( backward[0], "Materials/M.demat" );
}

// A binary names nothing, because there is no text to scan — and that must not be reported as
// "nothing yet". It is the same distinction the class draws for Text on the referencer side.
TEST( AssetReferenceIndex, ABinaryReferencesNothing )
{
    AssetReferenceIndex idx;
    idx.Add( Make( "Meshes/Rock.dmesh", ".dmesh", { "111" }, "" ) );
    idx.Add( Make( "Materials/M.demat", ".demat", { "999" }, "" ) );
    EXPECT_TRUE( idx.ReferencedBy( "Meshes/Rock.dmesh" ).empty() );
}

// The closure is TRANSITIVE. This is the property a package depends on: a scene naming a material
// that names a texture must ship the texture, and one hop is not enough.
TEST( AssetReferenceIndex, ClosureIsTransitive )
{
    AssetReferenceIndex idx;
    idx.Add( Make( "Textures/Albedo.png", ".png", { "555000111" }, "" ) );
    idx.Add( Make( "Materials/M.demat", ".demat", { "777000222" }, "{\"Textures\":[555000111]}" ) );
    idx.Add( Make( "Scenes/S.desce", ".desce", { "333000444" }, "{\"Mat\":777000222}" ) );
    idx.Add( Make( "Textures/Unused.png", ".png", { "888000999" }, "" ) );

    const auto closure = idx.ClosureFrom( "Scenes/S.desce" );
    ASSERT_EQ( closure.size(), 3u );
    EXPECT_EQ( closure[0], "Materials/M.demat" );
    EXPECT_EQ( closure[1], "Scenes/S.desce" );
    EXPECT_EQ( closure[2], "Textures/Albedo.png" );
}

// Two assets naming each other must not hang the walk. Prefabs do this in practice.
TEST( AssetReferenceIndex, ClosureTerminatesOnACycle )
{
    AssetReferenceIndex idx;
    idx.Add( Make( "A.deprefab", ".deprefab", { "111000111" }, "{\"other\":222000222}" ) );
    idx.Add( Make( "B.deprefab", ".deprefab", { "222000222" }, "{\"other\":111000111}" ) );

    const auto closure = idx.ClosureFrom( "A.deprefab" );
    ASSERT_EQ( closure.size(), 2u );
    EXPECT_EQ( closure[0], "A.deprefab" );
    EXPECT_EQ( closure[1], "B.deprefab" );
}

// AN UNKNOWN ROOT IS EMPTY, AND EMPTY IS WHY AssetClosure REFUSES ON IT. "this scene needs nothing"
// and "this scene is not in the index" are the same answer here, so the distinction has to be drawn
// by the caller — the tool treats an empty closure as a failure rather than as a short file list,
// because the alternative ships a package that starts and shows an empty world.
TEST( AssetReferenceIndex, ClosureOfAnUnknownRootIsEmpty )
{
    AssetReferenceIndex idx;
    idx.Add( Make( "Scenes/S.desce", ".desce", { "333000444" }, "{}" ) );
    EXPECT_TRUE( idx.ClosureFrom( "Scenes/NotHere.desce" ).empty() );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
