// The identity stitch every scene load in this engine goes through.
//
// WHY THIS SUITE EXISTS AT ALL. Until it did, nothing in the repository compiled
// Engine/Core/Serialize/SceneSerializer.cpp - it includes Scene.hpp, Scene.hpp reaches the renderer, and no
// test project links that. So the loop that decides which entity a saved record becomes, which entity its
// components are written onto and what it is parented to could be broken deliberately and all 69 suites
// still passed. The logic now lives in a pure function (Rules::PlanSceneStitch) and this is the thing that
// reaches it.
//
// WHAT IS ASSERTED, and why each one is a defect that has actually happened or can:
//
//   1. An id written in the file is the id the entity gets, and an entity with no id is minted ONE id.
//      The two passes used to mint independently and disagree, and the entity came out bare.
//   2. Parent links resolve to the entity that carries the named id, in the file's own order.
//   3. A parent that is not in the file leaves the child unattached and is COUNTED, not swallowed.
//   4. A parent id of 0 is "no parent", not "the entity whose id is 0".
//   5. A prefab-root record is listed, not stitched, and does not answer parent links in pass 2 - which is
//      what the loader does, and is the difference between a documented limitation and a surprise.
//   6. Two records claiming one id load as the map makes them load (first claimant wins) and are counted.
//   7. The relation between the three lists: every record is either created or a prefab root, exactly once.
//   8. The two policy decisions are ONE decision - the same for a scene file, a prefab body and an editor
//      snapshot - and the single argument the function takes changes nothing but what a PrefabPath record
//      is. This is the part that was four hand-written copies disagreeing with each other.
//   9. THE CORPUS: every .desce the repository ships, planned by the same function, so that "no file on
//      disk can see this decision" is a thing that stays checked instead of a thing measured once.
//
// No Scene, no renderer: the whole point of the split. Section 9 reads files, and only files - it parses
// them with the loader's own parser and plans over the result, with no asset manager and no scene graph.

#include <Engine/Core/Serialize/SceneFormat.hpp> // SceneSerialized - the corpus block at the bottom
#include <Engine/Core/Serialize/SceneStitchRules.hpp>

#include <rflcpp/rfl/json.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <iostream>
#include <vector>

using Desert::Assets::EntityData;
using Desert::Core::Rules::kNoSlot;
using Desert::Core::Rules::PlanSceneStitch;
using Desert::Core::Rules::PrefabRecordPolicy;
using Desert::Core::Rules::StitchPlan;

namespace
{
    // A mint that is reproducible, so a test can name the id it expects. Ids start high enough that they
    // cannot be confused with the authored ones the tests write by hand.
    class CountingMint
    {
    public:
        Common::UUID operator()()
        {
            ++m_Calls;
            return Common::UUID( m_Next++ );
        }

        int Calls() const
        {
            return m_Calls;
        }

    private:
        uint64_t m_Next  = 900000;
        int      m_Calls = 0;
    };

    EntityData Plain( uint64_t id, const char* tag )
    {
        EntityData d;
        d.id  = Common::UUID( id );
        d.Tag = std::string( tag );
        return d;
    }

    EntityData Child( uint64_t id, uint64_t parent, const char* tag )
    {
        EntityData d = Plain( id, tag );
        d.parent     = Common::UUID( parent );
        return d;
    }

    EntityData Anonymous( const char* tag )
    {
        EntityData d;
        d.Tag = std::string( tag );
        return d;
    }

    EntityData PrefabRoot( uint64_t id, const char* file )
    {
        EntityData d = Plain( id, "PrefabRoot" );
        d.PrefabPath = std::string( file );
        return d;
    }
} // namespace

// 1a. The id in the file is the id of the entity, and it is decided once.
TEST( SceneStitch, AuthoredIdIsKept )
{
    const std::vector<EntityData> records = { Plain( 11, "A" ), Plain( 22, "B" ) };
    CountingMint                  mint;

    const StitchPlan plan = PlanSceneStitch( records, mint, PrefabRecordPolicy::InstantiatedLater );

    ASSERT_EQ( plan.Created.size(), 2u );
    EXPECT_EQ( (uint64_t)plan.Created[0].Id, 11u );
    EXPECT_EQ( (uint64_t)plan.Created[1].Id, 22u );
    EXPECT_FALSE( plan.Created[0].IdMinted );
    EXPECT_FALSE( plan.Created[1].IdMinted );
    EXPECT_EQ( mint.Calls(), 0 );
    EXPECT_EQ( plan.Minted, 0u );
}

// 1b. THE REGRESSION. A record with no id is minted exactly ONE id, and pass 2 lands its payload on the
// entity that id created. The old loader minted in both passes, so pass 2 looked the entity up under an id
// nothing had been created with, missed, and skipped the record entirely: the entity survived the load
// carrying nothing but its tag.
TEST( SceneStitch, RecordWithoutIdIsMintedOnceAndStillReceivesItsPayload )
{
    const std::vector<EntityData> records = { Anonymous( "NoId" ) };
    CountingMint                  mint;

    const StitchPlan plan = PlanSceneStitch( records, mint, PrefabRecordPolicy::InstantiatedLater );

    ASSERT_EQ( plan.Created.size(), 1u );
    EXPECT_TRUE( plan.Created[0].IdMinted );
    EXPECT_EQ( mint.Calls(), 1 );
    EXPECT_EQ( plan.Minted, 1u );

    ASSERT_EQ( plan.Loads.size(), 1u );
    EXPECT_EQ( plan.Loads[0].Record, 0u );
    EXPECT_EQ( plan.Loads[0].Target, 0u ); // the entity that was actually created, not a second mint
}

// 1c. One mint per idless record, however many there are - and never for a record that names an id.
TEST( SceneStitch, MintIsCalledOncePerIdlessRecord )
{
    const std::vector<EntityData> records = { Anonymous( "a" ), Plain( 7, "b" ), Anonymous( "c" ),
                                              Anonymous( "d" ) };
    CountingMint                  mint;

    const StitchPlan plan = PlanSceneStitch( records, mint, PrefabRecordPolicy::InstantiatedLater );

    EXPECT_EQ( mint.Calls(), 3 );
    EXPECT_EQ( plan.Minted, 3u );
    EXPECT_NE( (uint64_t)plan.Created[0].Id, (uint64_t)plan.Created[2].Id );
    EXPECT_NE( (uint64_t)plan.Created[2].Id, (uint64_t)plan.Created[3].Id );
}

// 2. A parent link names an id; it resolves to the slot of the entity created under it - forwards...
TEST( SceneStitch, ParentLinkResolvesToTheNamedEntity )
{
    const std::vector<EntityData> records = { Plain( 11, "Parent" ), Child( 22, 11, "Child" ) };

    const StitchPlan plan = PlanSceneStitch( records, CountingMint(), PrefabRecordPolicy::InstantiatedLater );

    ASSERT_EQ( plan.Loads.size(), 2u );
    EXPECT_EQ( plan.Loads[0].Parent, kNoSlot );
    EXPECT_EQ( plan.Loads[1].Parent, 0u );
    EXPECT_EQ( plan.UnresolvedParents, 0u );
}

// ...and backwards. Pass 1 creates every entity before pass 2 links any, so a child written ABOVE its
// parent in the file still finds it. A single-pass loader would not, and this is why there are two.
TEST( SceneStitch, ParentLinkResolvesWhenTheParentComesLaterInTheFile )
{
    const std::vector<EntityData> records = { Child( 22, 11, "Child" ), Plain( 11, "Parent" ) };

    const StitchPlan plan = PlanSceneStitch( records, CountingMint(), PrefabRecordPolicy::InstantiatedLater );

    ASSERT_EQ( plan.Loads.size(), 2u );
    EXPECT_EQ( plan.Loads[0].Parent, 1u );
    EXPECT_EQ( plan.Loads[1].Parent, kNoSlot );
}

// 3. A parent nothing answers to leaves the child at the root, and is COUNTED. Silently attaching it to
// whatever the map happened to return would move geometry; silently dropping it would lose a hierarchy
// nobody is told about.
TEST( SceneStitch, ParentThatIsNotInTheFileIsCountedAndLeavesTheChildUnattached )
{
    const std::vector<EntityData> records = { Child( 22, 999, "Orphan" ) };

    const StitchPlan plan = PlanSceneStitch( records, CountingMint(), PrefabRecordPolicy::InstantiatedLater );

    ASSERT_EQ( plan.Loads.size(), 1u );
    EXPECT_EQ( plan.Loads[0].Parent, kNoSlot );
    EXPECT_EQ( plan.UnresolvedParents, 1u );
}

// 4. A null (0) parent is the spelling of "no parent" - see Common/Core/UUID.hpp on why 0 is the safe
// value. It must not be looked up, or an entity that genuinely carries id 0 becomes everybody's parent.
TEST( SceneStitch, NullParentIsNoParentAndIsNotCountedAsUnresolved )
{
    std::vector<EntityData> records = { Plain( 0, "IdZero" ), Child( 22, 0, "Loose" ) };

    const StitchPlan plan = PlanSceneStitch( records, CountingMint(), PrefabRecordPolicy::InstantiatedLater );

    ASSERT_EQ( plan.Loads.size(), 2u );
    EXPECT_EQ( plan.Loads[1].Parent, kNoSlot );
    EXPECT_EQ( plan.UnresolvedParents, 0u );
}

// 5a. A prefab root is LISTED for pass 3, never created here: it comes out of another file and the record
// alone cannot make it.
TEST( SceneStitch, PrefabRootsAreListedAndNotCreated )
{
    const std::vector<EntityData> records = { Plain( 11, "A" ), PrefabRoot( 33, "Prefabs/Tree.deprefab" ),
                                              Plain( 22, "B" ) };

    const StitchPlan plan = PlanSceneStitch( records, CountingMint(), PrefabRecordPolicy::InstantiatedLater );

    ASSERT_EQ( plan.PrefabRecords.size(), 1u );
    EXPECT_EQ( plan.PrefabRecords[0].Record, 1u );
    EXPECT_EQ( plan.PrefabRecords[0].Slot, kNoSlot ); // listed, so it is in no slot
    ASSERT_EQ( plan.Created.size(), 2u );
    EXPECT_EQ( plan.Created[0].Record, 0u );
    EXPECT_EQ( plan.Created[1].Record, 2u );
}

// 5c. WHERE A LISTED PREFAB'S BODY HANGS (Ю19). Under InstantiatedLater the record becomes no entity, so
// "Slot" cannot say where the instantiated root goes — and before this field existed each caller walked
// the records again with its own copy of the parent rule. PrefabFactory needed it when a nested prefab
// stopped being COPIED into the outer file: the nesting record is now a link, and creating an entity for
// it put a component-less level between the outer prefab and the nested body, one more per capture.
TEST( SceneStitch, AListedPrefabNamesTheSlotItsBodyHangsOff )
{
    const std::vector<EntityData> records = { Plain( 11, "Root" ), Child( 22, 11, "Panel" ), []
                                              {
                                                  EntityData d = PrefabRoot( 33, "Prefabs/Stripe.deprefab" );
                                                  d.parent     = Common::UUID( 22 );
                                                  return d;
                                              }() };

    const StitchPlan plan = PlanSceneStitch( records, CountingMint(), PrefabRecordPolicy::InstantiatedLater );

    ASSERT_EQ( plan.PrefabRecords.size(), 1u );
    EXPECT_EQ( plan.PrefabRecords[0].Slot, kNoSlot );
    ASSERT_EQ( plan.Created.size(), 2u );
    EXPECT_EQ( plan.PrefabRecords[0].Parent, 1u ) << "the slot record 22 (\"Panel\") was created in";
}

// 5d. And a listed prefab whose parent the file does not contain hangs off NOTHING rather than off slot
// zero. This is the normal shape for a prefab cut from an entity that had a parent: the first record keeps
// the parent id it had in the scene it came from, and no record here answers to it. It is deliberately NOT
// counted in UnresolvedParents — doing so would fire on every well-formed prefab in existence.
TEST( SceneStitch, AListedPrefabWithAForeignParentHangsOffNothing )
{
    std::vector<EntityData> records = { Plain( 11, "Root" ), PrefabRoot( 33, "Prefabs/Stripe.deprefab" ) };
    records[1].parent               = Common::UUID( 9999 );

    const StitchPlan plan = PlanSceneStitch( records, CountingMint(), PrefabRecordPolicy::InstantiatedLater );

    ASSERT_EQ( plan.PrefabRecords.size(), 1u );
    EXPECT_EQ( plan.PrefabRecords[0].Parent, kNoSlot );
    EXPECT_EQ( plan.UnresolvedParents, 0u );
}

// 5b. And it does not answer a plain entity's parent link, because the loader registers prefab roots only
// AFTER pass 2 has run. Pinned deliberately: it is the engine's behaviour today, so a change to it should
// break a test rather than quietly re-parent somebody's scene.
TEST( SceneStitch, PlainEntityParentedToAPrefabRootDoesNotResolveInPassTwo )
{
    const std::vector<EntityData> records = { PrefabRoot( 33, "Prefabs/Tree.deprefab" ),
                                              Child( 22, 33, "HangsOffThePrefab" ) };

    const StitchPlan plan = PlanSceneStitch( records, CountingMint(), PrefabRecordPolicy::InstantiatedLater );

    ASSERT_EQ( plan.Loads.size(), 1u );
    EXPECT_EQ( plan.Loads[0].Parent, kNoSlot );
    EXPECT_EQ( plan.UnresolvedParents, 1u );
}

// 6. Two records claiming one id. The loader registers with map::insert, so the FIRST keeps the id and the
// second record's payload is written onto the first record's entity - the second entity stays bare. Pinned
// as-is and counted, because changing what a corrupt file loads as is a decision about scene data.
TEST( SceneStitch, DuplicateIdLoadsOntoTheFirstClaimantAndIsCounted )
{
    const std::vector<EntityData> records = { Plain( 11, "First" ), Plain( 11, "Second" ) };

    const StitchPlan plan = PlanSceneStitch( records, CountingMint(), PrefabRecordPolicy::InstantiatedLater );

    ASSERT_EQ( plan.Created.size(), 2u ); // both entities are still created
    ASSERT_EQ( plan.Loads.size(), 2u );
    EXPECT_EQ( plan.Loads[0].Target, 0u );
    EXPECT_EQ( plan.Loads[1].Target, 0u ); // record 1's payload lands on record 0's entity
    EXPECT_EQ( plan.Shadowed, 1u );
}

// A child of a duplicated id attaches to the claimant, matching the map the loader builds.
TEST( SceneStitch, ChildOfADuplicatedIdAttachesToTheClaimant )
{
    const std::vector<EntityData> records = { Plain( 11, "First" ), Plain( 11, "Second" ),
                                              Child( 22, 11, "Child" ) };

    const StitchPlan plan = PlanSceneStitch( records, CountingMint(), PrefabRecordPolicy::InstantiatedLater );

    ASSERT_EQ( plan.Loads.size(), 3u );
    EXPECT_EQ( plan.Loads[2].Parent, 0u );
}

// 7. The relation, on a tree with one of everything: every record is accounted for exactly once, the two
// pass lists line up, and every slot a Load names exists.
TEST( SceneStitch, EveryRecordIsAccountedForExactlyOnce )
{
    const std::vector<EntityData> records = { Plain( 11, "A" ), Anonymous( "B" ),
                                              PrefabRoot( 33, "Prefabs/Tree.deprefab" ), Child( 22, 11, "C" ),
                                              PrefabRoot( 44, "Prefabs/Rock.deprefab" ) };

    const StitchPlan plan = PlanSceneStitch( records, CountingMint(), PrefabRecordPolicy::InstantiatedLater );

    EXPECT_EQ( plan.Created.size() + plan.PrefabRecords.size(), records.size() );
    EXPECT_EQ( plan.Loads.size(), plan.Created.size() );

    std::vector<int> seen( records.size(), 0 );
    for ( const auto& created : plan.Created )
        ++seen[created.Record];
    for ( const auto& prefab : plan.PrefabRecords )
        ++seen[prefab.Record];
    for ( const int count : seen )
        EXPECT_EQ( count, 1 );

    for ( size_t slot = 0; slot < plan.Loads.size(); ++slot )
    {
        EXPECT_EQ( plan.Loads[slot].Record, plan.Created[slot].Record );
        EXPECT_LT( plan.Loads[slot].Target, plan.Created.size() );
        EXPECT_TRUE( plan.Loads[slot].Parent == kNoSlot || plan.Loads[slot].Parent < plan.Created.size() );
    }
}

// An empty file is a scene with nothing in it, not a crash and not a phantom entity.
TEST( SceneStitch, EmptyTreePlansNothing )
{
    const std::vector<EntityData> records;

    const StitchPlan plan = PlanSceneStitch( records, CountingMint(), PrefabRecordPolicy::InstantiatedLater );

    EXPECT_TRUE( plan.Created.empty() );
    EXPECT_TRUE( plan.Loads.empty() );
    EXPECT_TRUE( plan.PrefabRecords.empty() );
    EXPECT_EQ( plan.Minted, 0u );
    EXPECT_EQ( plan.Shadowed, 0u );
    EXPECT_EQ( plan.UnresolvedParents, 0u );
}

// ---------------------------------------------------------------------------------------------------
// 8. THE POLICY IS ONE DECISION, NOT FOUR.
//
// This stitch was written out by hand four times — the scene loader, PrefabFactory, PrefabAsset and the
// editor's snapshot restore — and no suite compiled any of the four, so each copy answered the two
// questions below however its author's container happened to answer them. The tests here state the
// answers, and they state them for BOTH kinds of caller, because "the same everywhere" is the property
// that was missing and not merely "correct here".
// ---------------------------------------------------------------------------------------------------

// 8a. A record with no id is minted one, so two of them are two entities. The editor's snapshot restore
// keyed its map with `id.value_or( Null() )`: every idless record collided on the key 0, so the FIRST
// anonymous entity received the second's components on top of its own and the second was created and left
// bare. It is not a policy any more, it is arithmetic — two mints cannot collide.
TEST( SceneStitch, TwoRecordsWithoutIdsDoNotCollapseOntoOneEntity )
{
    const std::vector<EntityData> records = { Anonymous( "first" ), Anonymous( "second" ) };

    for ( const PrefabRecordPolicy policy :
          { PrefabRecordPolicy::InstantiatedLater, PrefabRecordPolicy::CreatedInPlace } )
    {
        const StitchPlan plan = PlanSceneStitch( records, CountingMint(), policy );

        ASSERT_EQ( plan.Created.size(), 2u );
        ASSERT_EQ( plan.Loads.size(), 2u );
        EXPECT_EQ( plan.Loads[0].Target, 0u );
        EXPECT_EQ( plan.Loads[1].Target, 1u ); // NOT 0 - this is the defect
        EXPECT_EQ( plan.Shadowed, 0u );
        EXPECT_NE( (uint64_t)plan.Created[0].Id, (uint64_t)plan.Created[1].Id );
    }
}

// 8b. A duplicated id goes to the FIRST claimant whichever kind of file the records came from.
// PrefabFactory registered with `entityMap[id] = e` — last writer wins — while the scene loader and
// PrefabAsset used `insert` — first claimant wins. One prefab, two answers, depending only on which
// function you reached it through.
TEST( SceneStitch, TheDuplicateIdRuleIsTheSameWhicheverKindOfFileTheRecordsCameFrom )
{
    const std::vector<EntityData> records = { Plain( 11, "First" ), Plain( 11, "Second" ),
                                              Child( 22, 11, "Child" ) };

    for ( const PrefabRecordPolicy policy :
          { PrefabRecordPolicy::InstantiatedLater, PrefabRecordPolicy::CreatedInPlace } )
    {
        const StitchPlan plan = PlanSceneStitch( records, CountingMint(), policy );

        ASSERT_EQ( plan.Loads.size(), 3u );
        EXPECT_EQ( plan.Loads[1].Target, 0u ); // first claimant, not last writer
        EXPECT_EQ( plan.Loads[2].Parent, 0u );
        EXPECT_EQ( plan.Shadowed, 1u );
    }
}

// 8c. CreatedInPlace: inside a prefab body (or an editor snapshot) a PrefabPath record is an entity of
// THIS file that has another prefab nested under it. So it is created, it is stitched, it answers parent
// links — and it is still listed, with the slot it landed in, because whoever instantiates the nested body
// has to know what to hang it off.
TEST( SceneStitch, APrefabRecordIsCreatedInPlaceAndAnswersParentLinks )
{
    const std::vector<EntityData> records = { Plain( 11, "Body" ), PrefabRoot( 33, "Prefabs/Tree.deprefab" ),
                                              Child( 22, 33, "UnderTheNestedOne" ) };

    const StitchPlan plan = PlanSceneStitch( records, CountingMint(), PrefabRecordPolicy::CreatedInPlace );

    ASSERT_EQ( plan.Created.size(), 3u );
    ASSERT_EQ( plan.PrefabRecords.size(), 1u );
    EXPECT_EQ( plan.PrefabRecords[0].Record, 1u );
    EXPECT_EQ( plan.PrefabRecords[0].Slot, 1u ); // the slot, so the caller does not re-derive it
    ASSERT_EQ( plan.Loads.size(), 3u );
    EXPECT_EQ( plan.Loads[2].Parent, 1u ); // and it answers a parent link, unlike a listed one
    EXPECT_EQ( plan.UnresolvedParents, 0u );
}

// 8d. THE RELATION between the two policies: they differ on records that name a prefab, and on NOTHING
// else. Stated as a test because it is the whole justification for there being an argument at all — if a
// policy could change the plan for an ordinary record, it would be a second stitch wearing an enum.
TEST( SceneStitch, ThePoliciesDifferOnlyOnRecordsThatNameAPrefab )
{
    const std::vector<EntityData> records = { Plain( 11, "A" ), Anonymous( "B" ), Child( 22, 11, "C" ),
                                              Child( 33, 999, "Orphan" ), Plain( 11, "DuplicateOfA" ) };

    const StitchPlan listed  = PlanSceneStitch( records, CountingMint(), PrefabRecordPolicy::InstantiatedLater );
    const StitchPlan inPlace = PlanSceneStitch( records, CountingMint(), PrefabRecordPolicy::CreatedInPlace );

    ASSERT_EQ( listed.Created.size(), inPlace.Created.size() );
    for ( size_t slot = 0; slot < listed.Created.size(); ++slot )
    {
        EXPECT_EQ( listed.Created[slot].Record, inPlace.Created[slot].Record );
        EXPECT_EQ( (uint64_t)listed.Created[slot].Id, (uint64_t)inPlace.Created[slot].Id );
        EXPECT_EQ( listed.Created[slot].IdMinted, inPlace.Created[slot].IdMinted );
        EXPECT_EQ( listed.Loads[slot].Record, inPlace.Loads[slot].Record );
        EXPECT_EQ( listed.Loads[slot].Target, inPlace.Loads[slot].Target );
        EXPECT_EQ( listed.Loads[slot].Parent, inPlace.Loads[slot].Parent );
    }
    EXPECT_EQ( listed.Minted, inPlace.Minted );
    EXPECT_EQ( listed.Shadowed, inPlace.Shadowed );
    EXPECT_EQ( listed.UnresolvedParents, inPlace.UnresolvedParents );
    EXPECT_TRUE( listed.PrefabRecords.empty() );
    EXPECT_TRUE( inPlace.PrefabRecords.empty() );
}

// 8e. WHEN THE POLICY IS INVISIBLE. First-claimant and last-writer answer the same for any file whose ids
// are distinct, and that is the whole argument that changing PrefabFactory's rule cannot move an existing
// prefab or scene: a well-formed file cannot tell which rule is in force. The corpus block below is what
// checks that the files on disk are in fact of that shape.
TEST( SceneStitch, WithDistinctIdsNothingIsShadowedAndEveryPayloadLandsOnItsOwnEntity )
{
    const std::vector<EntityData> records = { Plain( 11, "A" ), Child( 22, 11, "B" ), Child( 33, 22, "C" ) };

    const StitchPlan plan = PlanSceneStitch( records, CountingMint(), PrefabRecordPolicy::CreatedInPlace );

    ASSERT_EQ( plan.Loads.size(), 3u );
    for ( size_t slot = 0; slot < plan.Loads.size(); ++slot )
        EXPECT_EQ( plan.Loads[slot].Target, slot );
    EXPECT_EQ( plan.Shadowed, 0u );
}

// ---------------------------------------------------------------------------------------------------
// 9. THE CORPUS. Everything above is a hand-written tree; this is every scene the repository actually
// ships, parsed by the loader's own parser and planned by the loader's own stitch.
//
// WHY IT IS HERE. The argument for changing PrefabFactory's duplicate rule is "no file on disk can see the
// difference". That was a measurement somebody took once, by hand, and a measurement taken once is a
// comment: the day a scene is committed with a duplicated id, or with a record carrying no id, the
// argument silently stops being true and nothing says so. Asserted instead, over the whole corpus, so that
// such a file turns this suite red on the commit that adds it.
// ---------------------------------------------------------------------------------------------------

namespace
{
    // Walks up from the working directory looking for a file only the repository has — the test runner's
    // working directory is not fixed. Same shape as Desert/Tests/Engine/CloudProtocolScene.
    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + "Desert/Desert/Source/Engine/Core/SceneSettings.hpp" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    std::vector<std::filesystem::path> RepositoryScenes()
    {
        std::vector<std::filesystem::path> scenes;
        std::error_code                    ec;
        const std::filesystem::path        root = RepoRoot() + "Editor/Resources/Assets/Scenes";
        for ( const auto& entry : std::filesystem::recursive_directory_iterator( root, ec ) )
        {
            if ( entry.is_regular_file() && entry.path().extension() == ".desce" )
                scenes.push_back( entry.path() );
        }
        return scenes;
    }

    std::string ReadAll( const std::filesystem::path& path )
    {
        std::ifstream      in( path, std::ios::binary );
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }
} // namespace

// 9a. The corpus exists and this suite found it. Without this, a wrong working directory turns every
// assertion below into a vacuous pass over zero files - the exact failure mode that lets a corpus test
// report "green" while checking nothing.
TEST( SceneStitchCorpus, TheScenesAreWhereThisSuiteThinksTheyAre )
{
    EXPECT_GE( RepositoryScenes().size(), 40u );
}

// 9b. Every scene in the repository stitches cleanly: no record claims an id another record already
// claimed, no record arrives without one, and every parent link names an entity the file contains. The
// first two are why the duplicate-id and idless-record decisions cannot move any scene we ship; the third
// is a load-time warning nobody should be seeing.
TEST( SceneStitchCorpus, EverySceneStitchesWithNothingShadowedMintedOrUnresolved )
{
    for ( const auto& path : RepositoryScenes() )
    {
        const auto parsed = rfl::json::read<Desert::Core::SceneSerialized>( ReadAll( path ) );
        ASSERT_TRUE( parsed.has_value() ) << path.string();

        const StitchPlan plan =
             PlanSceneStitch( parsed->Entities, CountingMint(), PrefabRecordPolicy::InstantiatedLater );

        EXPECT_EQ( plan.Shadowed, 0u ) << path.string();
        EXPECT_EQ( plan.Minted, 0u ) << path.string();
        EXPECT_EQ( plan.UnresolvedParents, 0u ) << path.string();
        EXPECT_EQ( plan.Created.size() + plan.PrefabRecords.size(), parsed->Entities.size() ) << path.string();
    }
}

// 9c. And the plan is the IDENTITY stitch on every one of them: the records that ARE created become
// entities in sibling order (file order before scene v25) and each payload lands on itself. This is the round trip
// the change had to leave untouched, stated over the real files - if any decision taken in SceneStitchRules.hpp
// moved a scene, it would move it here.
//
// "RECORD i BECOMES ENTITY i" WAS THE OLD WORDING AND IT ONLY HELD WHILE NO SCENE NAMED A PREFAB. A
// prefab record is LISTED, not created, so it takes no slot and everything after it shifts by one. Until
// Ю19 the corpus had no such record and the stronger sentence was true by accident; the first scene to
// carry one would have broken this by luck of ordering alone (the witness scene passed it, because its
// canvas happens to be the first record). The relation that is actually true is stated instead: the
// created entities are the NON-prefab records, in file order.
TEST( SceneStitchCorpus, EverySceneRecordBecomesItsOwnEntityInSiblingOrder )
{
    for ( const auto& path : RepositoryScenes() )
    {
        const auto parsed = rfl::json::read<Desert::Core::SceneSerialized>( ReadAll( path ) );
        ASSERT_TRUE( parsed.has_value() ) << path.string();

        const StitchPlan plan =
             PlanSceneStitch( parsed->Entities, CountingMint(), PrefabRecordPolicy::InstantiatedLater );

        std::vector<size_t> nonPrefab;
        for ( size_t record = 0; record < parsed->Entities.size(); ++record )
        {
            if ( !parsed->Entities[record].PrefabPath.has_value() )
            {
                nonPrefab.push_back( record );
            }
        }
        // Since scene v25 the file is sorted by id and the sibling order is STATED (siblingIndex), so the plan
        // creates in that order, file order breaking ties and records without an index going last
        // (SceneStitchRules.hpp PlanSceneStitch). Same rule, spelled here from the records themselves.
        std::stable_sort( nonPrefab.begin(), nonPrefab.end(),
                          [&]( size_t a, size_t b )
                          {
                              return Desert::Core::Rules::SiblingIndexOf( parsed->Entities[a] ) <
                                     Desert::Core::Rules::SiblingIndexOf( parsed->Entities[b] );
                          } );
        ASSERT_EQ( plan.Created.size(), nonPrefab.size() ) << path.string();

        for ( size_t slot = 0; slot < plan.Created.size(); ++slot )
        {
            EXPECT_EQ( plan.Created[slot].Record, nonPrefab[slot] ) << path.string();
            EXPECT_EQ( plan.Loads[slot].Target, slot ) << path.string();
            EXPECT_FALSE( plan.Created[slot].IdMinted ) << path.string();
        }
    }
}

// 9d. THE DAY A SCENE NAMES A PREFAB HAS ARRIVED, and this test said in writing that somebody would have
// to look at it when it did.
//
// It used to assert that NO scene on disk can tell the two policies apart, which was true because the
// corpus held no prefab record at all - zero `.deprefab` files and zero scenes naming one, for the whole
// life of the prefab subsystem. Ю19 added the first two (UI_PrefabWitness and its control), and the
// policies now differ on them by construction, which is the POINT of the policy rather than a regression:
// a `.desce` record naming a prefab file becomes an entity out of THAT file, so the loader must not
// create it here.
//
// So the assertion is the partition, and it keeps both halves honest: a scene without a prefab record
// must still be policy-blind (that is the round trip nothing was allowed to move), and a scene with one
// must differ in exactly the documented way and no other.
TEST( SceneStitchCorpus, ThePoliciesDifferExactlyOnTheScenesThatNameAPrefab )
{
    std::size_t scenesNamingAPrefab = 0;

    for ( const auto& path : RepositoryScenes() )
    {
        const auto parsed = rfl::json::read<Desert::Core::SceneSerialized>( ReadAll( path ) );
        ASSERT_TRUE( parsed.has_value() ) << path.string();

        const StitchPlan listed =
             PlanSceneStitch( parsed->Entities, CountingMint(), PrefabRecordPolicy::InstantiatedLater );
        const StitchPlan inPlace =
             PlanSceneStitch( parsed->Entities, CountingMint(), PrefabRecordPolicy::CreatedInPlace );

        if ( listed.PrefabRecords.empty() )
        {
            ASSERT_EQ( listed.Created.size(), inPlace.Created.size() ) << path.string();
            for ( size_t slot = 0; slot < listed.Created.size(); ++slot )
            {
                EXPECT_EQ( listed.Loads[slot].Target, inPlace.Loads[slot].Target ) << path.string();
                EXPECT_EQ( listed.Loads[slot].Parent, inPlace.Loads[slot].Parent ) << path.string();
            }
            continue;
        }

        ++scenesNamingAPrefab;

        // The difference is exactly the prefab records: CreatedInPlace makes an entity for each of them,
        // InstantiatedLater does not, and nothing else moves.
        EXPECT_EQ( inPlace.Created.size(), listed.Created.size() + listed.PrefabRecords.size() ) << path.string();

        for ( const auto& prefab : listed.PrefabRecords )
        {
            EXPECT_EQ( prefab.Slot, kNoSlot ) << path.string() << ": a listed prefab takes no slot";
            EXPECT_TRUE( parsed->Entities[prefab.Record].PrefabPath.has_value() ) << path.string();
            // And it knows where its body hangs, which is what the loader and PrefabFactory read.
            if ( parsed->Entities[prefab.Record].parent.has_value() )
            {
                EXPECT_LT( prefab.Parent, listed.Created.size() )
                     << path.string()
                     << ": the prefab record names a parent the file contains, so the "
                        "plan has to resolve it to a created slot";
            }
        }
    }

    // Printed rather than pinned: the number is the corpus's, and it was ZERO for the whole life of the
    // prefab subsystem - which is why nothing below a prefab instance's root was ever known to be lost.
    std::cout << "[corpus] scenes naming a prefab: " << scenesNamingAPrefab << std::endl;
    EXPECT_GT( scenesNamingAPrefab, 0u )
         << "no scene on disk names a prefab any more. If the witness scenes were removed, this suite has "
            "stopped covering the loader's pass 3 and the prefab subsystem is untested by the corpus "
            "again — say so deliberately rather than deleting this line.";
}

// 9e. And the records everything above is planned over are the records the FILE has - checked against the
// same file read with no type at all (rfl::Generic: whatever JSON is actually in there).
//
// WHY THIS IS NEEDED. 9b..9d assert things about `parsed->Entities`, which is what SURVIVED the typed
// read. If EntityData had no home for a field, or dropped a record, the plan would be clean and this suite
// would report the corpus clean - about a corpus it had never seen. So the three inputs the stitch
// actually decides on - how many records there are, the id each one claims, and the parent it names - are
// compared against the untyped read, which cannot have dropped anything because it has no schema to drop
// things against.
//
// The comparison is deliberately NOT `write(typed) == write(raw)` over the whole file. Ids are written as
// UNSIGNED 64-bit and every scene has some above INT64_MAX, so a schema-free reader hands them back as
// negative numbers: identical bits, different spelling, and forty-six failures that mean nothing. The bits
// are what is compared here.
TEST( SceneStitchCorpus, EveryRecordTheFileHasIsARecordTheStitchSees )
{
    for ( const auto& path : RepositoryScenes() )
    {
        const std::string source = ReadAll( path );

        const auto typed = rfl::json::read<Desert::Core::SceneSerialized>( source );
        ASSERT_TRUE( typed.has_value() ) << path.string();

        const auto raw = rfl::json::read<rfl::Generic>( source );
        ASSERT_TRUE( raw.has_value() ) << path.string();
        const auto rawObject = raw->to_object();
        ASSERT_TRUE( rawObject.has_value() ) << path.string();
        const auto rawEntities = rawObject.value().get( "Entities" ).value().to_array();
        ASSERT_TRUE( rawEntities.has_value() ) << path.string();

        ASSERT_EQ( rawEntities.value().size(), typed->Entities.size() ) << path.string();

        for ( size_t record = 0; record < typed->Entities.size(); ++record )
        {
            const auto rawRecord = rawEntities.value()[record].to_object();
            ASSERT_TRUE( rawRecord.has_value() ) << path.string();

            const auto rawId = rawRecord.value().get( "id" );
            ASSERT_EQ( rawId.has_value(), typed->Entities[record].id.has_value() ) << path.string();
            if ( rawId.has_value() )
            {
                const auto bits = static_cast<uint64_t>( rawId.value().to_int64().value() );
                EXPECT_EQ( bits, (uint64_t)*typed->Entities[record].id ) << path.string();
            }

            const auto rawParent = rawRecord.value().get( "parent" );
            EXPECT_EQ( rawParent.has_value(), typed->Entities[record].parent.has_value() ) << path.string();

            const auto rawPrefab = rawRecord.value().get( "PrefabPath" );
            EXPECT_EQ( rawPrefab.has_value(), typed->Entities[record].PrefabPath.has_value() ) << path.string();
        }
    }
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
