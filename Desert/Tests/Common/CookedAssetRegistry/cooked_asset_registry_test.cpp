// THE COOKED ASSET REGISTRY — the file both hosts now boot from instead of walking the content roots.
//
// WHAT IS AT STAKE. Since GAP_ANALYSIS T2.4 this file IS the list of what a project has: a content
// file with no row does not reach the engine and does not reach a packaged game. Everything below is
// therefore about one of two properties.
//
//   * THE ROUND TRIP IS EXACT. A registry written on one machine and read on another must describe the
//     same assets. The interesting half is that a row's PATH-DERIVED HANDLE is not stored at all — it
//     is `AssetHandle::FromKey( Key )` — so the reader must arrive at the number the writer would have,
//     by derivation rather than by agreement. That is deliberate: a stored handle would be a second
//     spelling of a value the key already determines, and this repository's most repeated defect is two
//     things that must agree with nothing checking that they do.
//   * A DISAGREEMENT IS A REFUSAL. Two rows for one file, a key with a newline in it, a malformed
//     column: each is answered with an error that names the row, never with a row silently dropped or
//     overwritten. A registry that quietly loses a line is a game that quietly ships without a texture.

#include <Common/Content/ContentScan.hpp>
#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/AssetPathIndex.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Utilities/AssetRegistry.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <map>
#include <string>
#include <utility>
#include <vector>

using Common::Utils::AssetRegistry;
using Common::Utils::AssetRegistryEntry;

namespace
{
    AssetRegistryEntry Row( std::string key, std::string kind, uint64_t size = 0 )
    {
        AssetRegistryEntry entry;
        entry.Key  = std::move( key );
        entry.Kind = std::move( kind );
        entry.Size = size;
        return entry;
    }

    // The project root moves under several cases below (a registry means the same thing on two
    // machines only if the KEY, not the path, is what travels), so it is restored afterwards for the
    // same reason every other suite that re-roots does it: the roots are process-wide.
    class SandboxRootGuard
    {
    public:
        SandboxRootGuard() : m_Saved( Common::Constants::Path::CurrentProjectRoot() )
        {
        }
        ~SandboxRootGuard()
        {
            Common::Constants::Path::SetProjectRoot( m_Saved.ProjectDir, m_Saved.AssetsRoot );
        }

    private:
        Common::Constants::Path::ProjectRootState m_Saved;
    };
} // namespace

// ── THE DERIVATION THE FORMAT LEAVES OUT ────────────────────────────────────────────────────────────

TEST( CookedAssetRegistry, ARowsHandleIsDerivedFromItsKeyAndIsNotAStoredColumn )
{
    const AssetRegistryEntry row = Row( "assets:Materials/M_Rock.demat", "Material", 512 );

    EXPECT_EQ( row.PathHandle(),
               static_cast<uint64_t>( Common::AssetHandle::FromKey( "assets:Materials/M_Rock.demat" ) ) )
         << "a row's handle must BE the derivation over its key. If these ever differ, the registry is "
            "naming one file by a number nothing else in the engine will produce for it.";

    // With no declared identity, the number the engine knows the asset by IS the derived one.
    EXPECT_EQ( row.EffectiveHandle(), row.PathHandle() );
}

TEST( CookedAssetRegistry, ADeclaredIdentityOverridesTheDerivedHandleAndBothStillFindTheRow )
{
    // A `.tex` carries a `Handle` of its own and a `.demat` a `MaterialId`; the file is REGISTERED
    // under its path and REFERENCED by the number inside it. A lookup that knew only one of the two
    // would answer for half the corpus, so this asserts both.
    constexpr uint64_t declared = 0xD00DFEEDCAFEBABEull;

    AssetRegistryEntry row = Row( "cooked:Textures/T_Probe.tex", "Texture", 128 );
    row.Identity           = declared;

    const uint64_t derived = row.PathHandle();
    ASSERT_NE( derived, declared );
    EXPECT_EQ( row.EffectiveHandle(), declared );

    AssetRegistry registry;
    ASSERT_TRUE( registry.Insert( row ) );

    ASSERT_NE( registry.FindByHandle( declared ), nullptr )
         << "a texture is referenced by the id inside it; a registry that cannot answer for that number "
            "cannot write a texture slot back out.";
    ASSERT_NE( registry.FindByHandle( derived ), nullptr )
         << "and it is still registered under its path, which is how the loader finds it.";
    EXPECT_EQ( registry.FindByHandle( declared )->Key, registry.FindByHandle( derived )->Key );
}

// ── THE ROUND TRIP ──────────────────────────────────────────────────────────────────────────────────

TEST( CookedAssetRegistry, EveryColumnSurvivesSerializeAndParse )
{
    AssetRegistry written;

    AssetRegistryEntry texture = Row( "cooked:Textures/T_Probe.tex", "Texture", 4096 );
    texture.Identity           = 0x0123456789ABCDEFull;
    texture.Dependencies       = { 7, 3, 11 };
    ASSERT_TRUE( written.Insert( texture ) );

    // A key with a SPACE in it. The key is the last column precisely so this round-trips; a file whose
    // name contains a space is ordinary content, and a format that mangled it would lose the asset.
    ASSERT_TRUE( written.Insert( Row( "assets:Materials/M Rock Wet.demat", "Material", 512 ) ) );
    ASSERT_TRUE( written.Insert( Row( "engine:Shaders/PBR.shader", "Shader", 9000 ) ) );

    const auto parsed = AssetRegistry::Parse( written.Serialize() );
    ASSERT_TRUE( parsed ) << parsed.GetError();

    const AssetRegistry& read = parsed.GetValue();
    ASSERT_EQ( read.Count(), written.Count() );

    const AssetRegistryEntry* back = read.FindByKey( "cooked:Textures/T_Probe.tex" );
    ASSERT_NE( back, nullptr );
    EXPECT_EQ( back->Kind, "Texture" );
    EXPECT_EQ( back->Size, 4096u );
    EXPECT_EQ( back->Identity, 0x0123456789ABCDEFull );
    // Sorted on the way out, because two cooks of one tree must produce one byte string whatever order
    // the slots were read in.
    EXPECT_EQ( back->Dependencies, ( std::vector<uint64_t>{ 3, 7, 11 } ) );

    ASSERT_NE( read.FindByKey( "assets:Materials/M Rock Wet.demat" ), nullptr )
         << "a key containing a space did not survive the round trip";
}

TEST( CookedAssetRegistry, TwoCooksOfOneTreeSerializeToTheSameBytes )
{
    // THE PROPERTY THAT MAKES THE FILE COMMITTABLE. It is checked in, read in a diff, and compared by a
    // gate; if the byte string depended on the order the filesystem happened to walk, none of those
    // three would mean anything.
    AssetRegistry forwards;
    AssetRegistry backwards;

    const std::vector<std::string> keys  = { "assets:A.demat", "cooked:B.tex", "engine:C.shader",
                                             "assets:D.derig" };
    const std::vector<std::string> kinds = { "Material", "Texture", "Shader", "ControlRig" };

    for ( std::size_t i = 0; i < keys.size(); ++i )
        ASSERT_TRUE( forwards.Insert( Row( keys[i], kinds[i], i ) ) );
    for ( std::size_t i = keys.size(); i-- > 0; )
        ASSERT_TRUE( backwards.Insert( Row( keys[i], kinds[i], i ) ) );

    EXPECT_EQ( forwards.Serialize(), backwards.Serialize() );
}

// ── THE REFUSALS ────────────────────────────────────────────────────────────────────────────────────

TEST( CookedAssetRegistry, OneFileCannotBecomeTwoRows )
{
    AssetRegistry registry;
    ASSERT_TRUE( registry.Insert( Row( "assets:Materials/M.demat", "Material", 1 ) ) );

    const auto again = registry.Insert( Row( "assets:Materials/M.demat", "Material", 2 ) );
    EXPECT_FALSE( again ) << "a second row for one key was accepted. Whichever won would make the "
                             "registry depend on the order the cook walked the disk.";
    EXPECT_EQ( registry.Count(), 1u );
    EXPECT_EQ( registry.Entries().front().Size, 1u ) << "the FIRST row must stand, as AssetPathIndex's "
                                                        "colliding-handle refusal does";
}

TEST( CookedAssetRegistry, AKeyThatCannotBeWrittenAsOneLineIsRefusedAtTheDoor )
{
    AssetRegistry registry;

    // Legal on POSIX, and it would be read back as two rows — the second of them malformed.
    EXPECT_FALSE( registry.Insert( Row( "assets:Materials/M\nRock.demat", "Material" ) ) );
    EXPECT_FALSE( registry.Insert( Row( "", "Material" ) ) );
    // The kind is a MIDDLE column, so a space in it would shift every column after it.
    EXPECT_FALSE( registry.Insert( Row( "assets:M.demat", "Surface Material" ) ) );
    EXPECT_TRUE( registry.Empty() );
}

TEST( CookedAssetRegistry, AMalformedFileIsARefusalAndNotAnEmptyProject )
{
    // THE DISTINCTION THAT MATTERS MOST IN THIS FILE. An empty registry and an unreadable one are
    // indistinguishable to anything downstream — both mean "no content" — and the second must say so.
    EXPECT_FALSE( AssetRegistry::Parse( "" ) );
    EXPECT_FALSE( AssetRegistry::Parse( "DesertContentManifest 1\n" ) ) << "another Desert text format "
                                                                           "was accepted as a registry";
    EXPECT_FALSE( AssetRegistry::Parse( "DesertAssetRegistry 4\n" ) ) << "a future version was read as "
                                                                         "though it were this one";
    EXPECT_FALSE( AssetRegistry::Parse( "DesertAssetRegistry 2\n512 Material - -\n" ) )
         << "a row missing its key column was accepted";
    EXPECT_FALSE( AssetRegistry::Parse( "DesertAssetRegistry 2\nbig Material - - - assets:M.demat\n" ) )
         << "a size that is not a number was accepted";
    EXPECT_FALSE( AssetRegistry::Parse( "DesertAssetRegistry 2\n5 Material zz - - assets:M.demat\n" ) )
         << "an identity that is neither '-' nor a 16-digit hex handle was accepted";

    // And the header alone, with no rows, IS a valid registry — an empty project is a real state.
    const auto empty = AssetRegistry::Parse( "DesertAssetRegistry 2\n" );
    ASSERT_TRUE( empty ) << empty.GetError();
    EXPECT_TRUE( empty.GetValue().Empty() );
}

// ── THE BOUNDS COLUMN (version 2) ───────────────────────────────────────────────────────────────────

namespace
{
    Common::Math::AABB Box( glm::vec3 min, glm::vec3 max )
    {
        return Common::Math::AABB{ min, max };
    }
} // namespace

// EXACT, NOT CLOSE. The column is bits, so a value no decimal spelling of six digits could carry — a
// third, a denormal-adjacent tiny, a negative zero — comes back as the same float, and a row that
// round-trips is byte-identical on the next write.
TEST( CookedAssetRegistry, BoundsSurviveARoundTripBitForBit )
{
    AssetRegistry            written;
    AssetRegistryEntry       mesh = Row( "cooked:Meshes/Bridge.stmesh", "StaticMesh", 100 );
    const Common::Math::AABB box =
         Box( glm::vec3( -50000.0f, -0.0f, 1.0f / 3.0f ), glm::vec3( 50000.0f, 1.0e-30f, 12345.678f ) );
    mesh.Bounds = box;
    ASSERT_TRUE( written.Insert( mesh ) );
    ASSERT_TRUE( written.Insert( Row( "assets:Materials/M.demat", "Material", 5 ) ) );

    const std::string text   = written.Serialize();
    const auto        parsed = AssetRegistry::Parse( text );
    ASSERT_TRUE( parsed ) << parsed.GetError();

    const AssetRegistryEntry* back = parsed.GetValue().FindByKey( "cooked:Meshes/Bridge.stmesh" );
    ASSERT_NE( back, nullptr );
    ASSERT_TRUE( back->Bounds.has_value() );
    EXPECT_TRUE( Common::Utils::SameBounds( back->Bounds, box ) );
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    EXPECT_TRUE( std::signbit( back->Bounds.value().Min.y ) )
         << "negative zero lost its sign"; // NOLINT(bugprone-unchecked-optional-access)
    // NOLINTEND(bugprone-unchecked-optional-access)

    const AssetRegistryEntry* none = parsed.GetValue().FindByKey( "assets:Materials/M.demat" );
    ASSERT_NE( none, nullptr );
    EXPECT_FALSE( none->Bounds.has_value() ) << "a row with no extent came back with one";

    EXPECT_EQ( parsed.GetValue().Serialize(), text ) << "a second write of what was read is a different file";
    EXPECT_EQ( text.rfind( "DesertAssetRegistry 3\n", 0 ), 0u );
}

// VERSION 1 IS READ, AS ROWS WITH NO BOUNDS, AND WRITTEN BACK AS 2 — the migration is Parse itself. Every
// other column must survive it: a migration that dropped the identities would be the middle link that
// loses a property.
TEST( CookedAssetRegistry, AVersionOneFileMigratesToRowsWithNoBoundsAndKeepsEveryOtherColumn )
{
    const auto parsed =
         AssetRegistry::Parse( "DesertAssetRegistry 1\n"
                               "3536 StaticMesh - - cooked:Meshes/StaticProbe.stmesh\n"
                               "229 Material a3c34fd7f85f1d7b 0000000000000007 assets:Materials/M A.demat\n" );
    ASSERT_TRUE( parsed ) << parsed.GetError();
    const AssetRegistry& read = parsed.GetValue();
    ASSERT_EQ( read.Count(), 2u );

    const AssetRegistryEntry* material = read.FindByKey( "assets:Materials/M A.demat" );
    ASSERT_NE( material, nullptr ) << "a version-1 key with a space was read as a bounds column";
    EXPECT_EQ( material->Identity, 0xa3c34fd7f85f1d7bull );
    EXPECT_EQ( material->Dependencies, ( std::vector<uint64_t>{ 7 } ) );
    EXPECT_FALSE( material->Bounds.has_value() );

    EXPECT_EQ( read.Serialize(), "DesertAssetRegistry 3\n"
                                 "229 Material - a3c34fd7f85f1d7b 0000000000000007 - assets:Materials/M A.demat\n"
                                 "3536 StaticMesh - - - - cooked:Meshes/StaticProbe.stmesh\n" );
}

// A BOX NO POINT IS INSIDE IS NOT DATA. Five fields, a NaN, an inverted box and a decimal spelling are
// all refusals with the line number, never a row whose record would be placed nowhere.
TEST( CookedAssetRegistry, AMalformedBoundsColumnIsRefused )
{
    const std::string zero = "00000000";
    const std::string one  = "3f800000";
    const std::string nan  = "7fc00000";
    const auto        row  = []( const std::string& bounds )
    { return "DesertAssetRegistry 2\n9 StaticMesh - - " + bounds + " cooked:Meshes/M.stmesh\n"; };

    ASSERT_TRUE(
         AssetRegistry::Parse( row( zero + "," + zero + "," + zero + "," + one + "," + one + "," + one ) ) );
    EXPECT_FALSE( AssetRegistry::Parse( row( zero + "," + zero + "," + zero + "," + one + "," + one ) ) );
    EXPECT_FALSE( AssetRegistry::Parse(
         row( zero + "," + zero + "," + zero + "," + one + "," + one + "," + one + "," + one ) ) );
    EXPECT_FALSE(
         AssetRegistry::Parse( row( nan + "," + zero + "," + zero + "," + one + "," + one + "," + one ) ) );
    EXPECT_FALSE(
         AssetRegistry::Parse( row( one + "," + zero + "," + zero + "," + zero + "," + one + "," + one ) ) )
         << "an inverted box (min x above max x) was accepted";
    EXPECT_FALSE( AssetRegistry::Parse( row( "0,0,0,1,1,1" ) ) ) << "a decimal spelling was accepted";
}

// SetBounds FOLLOWS SetIdentity: it refuses a key that is not a row instead of inserting one, and it can
// take a box away again.
TEST( CookedAssetRegistry, SetBoundsTouchesOnlyAnExistingRow )
{
    AssetRegistry registry;
    ASSERT_TRUE( registry.Insert( Row( "cooked:Meshes/M.stmesh", "StaticMesh", 9 ) ) );

    EXPECT_FALSE(
         registry.SetBounds( "cooked:Meshes/Other.stmesh", Box( glm::vec3( 0.0f ), glm::vec3( 1.0f ) ) ) );
    EXPECT_EQ( registry.Count(), 1u );

    ASSERT_TRUE( registry.SetBounds( "cooked:Meshes/M.stmesh", Box( glm::vec3( -1.0f ), glm::vec3( 2.0f ) ) ) );
    EXPECT_TRUE( registry.FindByKey( "cooked:Meshes/M.stmesh" )->Bounds.has_value() );
    ASSERT_TRUE( registry.SetBounds( "cooked:Meshes/M.stmesh", std::nullopt ) );
    EXPECT_FALSE( registry.FindByKey( "cooked:Meshes/M.stmesh" )->Bounds.has_value() );
}

// A SCENE REFERENCE IS A HANDLE, OR FAILING THAT A PATH. Both must find the row, and a declared identity
// must too, because a scene holds whichever number the engine knew the asset by.
TEST( CookedAssetRegistry, AReferenceFindsItsRowByHandleOrByPath )
{
    AssetRegistry      registry;
    AssetRegistryEntry mesh = Row( "cooked:Meshes/M.stmesh", "StaticMesh", 9 );
    mesh.Identity           = 0x1111222233334444ull;
    ASSERT_TRUE( registry.Insert( mesh ) );
    const AssetRegistryEntry* row = registry.FindByKey( "cooked:Meshes/M.stmesh" );

    EXPECT_EQ( registry.FindByReference( row->PathHandle(), "" ), row );
    EXPECT_EQ( registry.FindByReference( 0x1111222233334444ull, "" ), row );
    EXPECT_EQ( registry.FindByReference( 0, "" ), nullptr );
    EXPECT_EQ( registry.FindByReference( 0x9999ull, "" ), nullptr ) << "an unknown handle named a row";

    // By path: the stable key of the cooked mesh root plus the file name — the spelling a `.desce` writes.
    const std::string path = ( Common::Constants::Path::MESH_PATH_COOKED / "M.stmesh" ).generic_string();
    EXPECT_EQ( registry.FindByReference( 0, path ), row ) << path;
    EXPECT_EQ( registry.FindByReference( 0x9999ull, path ), row ) << "a stale handle must not hide the path";
}

// ── WHAT THE BOOT ACTUALLY USES IT FOR ──────────────────────────────────────────────────────────────

TEST( CookedAssetRegistry, PublishingTheRowsMakesEveryHandleNameItsFileWithNothingLoaded )
{
    // THE SENTENCE THE WHOLE TIER IS ABOUT. `AssetHandle::FromCookedPath` is a one-way hash, so before
    // T2.4 a number read out of a `.desce` on a cold start named a file only because the boot had just
    // walked eight content roots hashing every path in them. This is the replacement, and this test is
    // the cold start: nothing is loaded, nothing is walked, and the numbers resolve.
    const SandboxRootGuard guard;
    Common::AssetPathIndex::Clear();

    AssetRegistry registry;
    ASSERT_TRUE( registry.Insert( Row( "assets:Materials/M_Rock.demat", "Material", 512 ) ) );
    ASSERT_TRUE( registry.Insert( Row( "cooked:Meshes/Probe.stmesh", "StaticMesh", 900 ) ) );

    EXPECT_EQ( Common::AssetPathIndex::Size(), 0u );
    EXPECT_EQ( registry.PublishIdentities(), 2u );

    const uint64_t material =
         static_cast<uint64_t>( Common::AssetHandle::FromKey( "assets:Materials/M_Rock.demat" ) );
    EXPECT_EQ( Common::AssetPathIndex::KeyFor( material ), "assets:Materials/M_Rock.demat" );
    EXPECT_EQ( Common::AssetPathIndex::PathFor( material ),
               Common::AssetHandle::PathForStableKey( "assets:Materials/M_Rock.demat" ) );

    Common::AssetPathIndex::Clear();
}

TEST( CookedAssetRegistry, ADeclaredIdentityIsNotPublishedIntoThePathIndex )
{
    // MEASURED, AND IT COST A FALSE ALARM. The first version of `PublishIdentities` bound a `.tex`'s
    // declared identity to the COOKED file's key — and `TextureAsset::Load` binds that same number to
    // the SOURCE image's key a moment later, because that is the string the number is a hash of. The
    // run printed `two files cannot share one identity` over two perfectly ordinary cooked textures,
    // which is a refusal that exists to catch a serious defect being taught to fire on the normal case.
    //
    // The two questions are different and so are their answers: the index says which STRING a number
    // was hashed from, the registry says which FILE a number names.
    const SandboxRootGuard guard;
    Common::AssetPathIndex::Clear();

    AssetRegistryEntry texture = Row( "cooked:Textures/T_Probe.tex", "Texture", 128 );
    texture.Identity           = 0xABCDEF0123456789ull;

    AssetRegistry registry;
    ASSERT_TRUE( registry.Insert( texture ) );

    EXPECT_EQ( registry.PublishIdentities(), 1u ) << "only the path-derived handle may be published";
    EXPECT_TRUE( Common::AssetPathIndex::KeyFor( 0xABCDEF0123456789ull ).empty() );
    EXPECT_NE( registry.FindByHandle( 0xABCDEF0123456789ull ), nullptr )
         << "and the registry must still answer for it — that is the half the index cannot do";

    Common::AssetPathIndex::Clear();
}

TEST( CookedAssetRegistry, RowsAreSelectableByKindBecauseThatIsWhatReplacedTheDirectoryWalk )
{
    AssetRegistry registry;
    ASSERT_TRUE( registry.Insert( Row( "cooked:Meshes/A.stmesh", "StaticMesh" ) ) );
    ASSERT_TRUE( registry.Insert( Row( "cooked:Meshes/B.stmesh", "StaticMesh" ) ) );
    ASSERT_TRUE( registry.Insert( Row( "cooked:Meshes/C.skmesh", "SkinnedMesh" ) ) );

    const auto statics = registry.OfKind( "StaticMesh" );
    ASSERT_EQ( statics.size(), 2u );
    EXPECT_EQ( statics[0]->Key, "cooked:Meshes/A.stmesh" ) << "rows of a kind must come back in key "
                                                              "order, or the preload order becomes a "
                                                              "property of the walk again";
    EXPECT_EQ( statics[1]->Key, "cooked:Meshes/B.stmesh" );
    EXPECT_EQ( registry.OfKind( "SkinnedMesh" ).size(), 1u );
    EXPECT_TRUE( registry.OfKind( "Skybox" ).empty() );
}

TEST( CookedAssetRegistry, RemovingARowTakesBothOfItsNumbersWithIt )
{
    // A row whose file was deleted leaves; if only one of its two numbers left the index, a lookup
    // would succeed and then find no row — a worse answer than a miss.
    AssetRegistryEntry texture = Row( "cooked:Textures/T.tex", "Texture" );
    texture.Identity           = 0xFEEDul;
    const uint64_t derived     = texture.PathHandle();

    AssetRegistry registry;
    ASSERT_TRUE( registry.Insert( texture ) );
    ASSERT_TRUE( registry.Remove( "cooked:Textures/T.tex" ) );

    EXPECT_EQ( registry.FindByHandle( 0xFEEDul ), nullptr );
    EXPECT_EQ( registry.FindByHandle( derived ), nullptr );
    EXPECT_FALSE( registry.Remove( "cooked:Textures/T.tex" ) );
}

// ── THE COMPARATOR THE COOK GATE IS MADE OF ─────────────────────────────────────────────────────────
//
// WHY THESE EXIST, AND IT IS A MUTATION THAT PUT THEM HERE. `Desert/Tests/Editor/CookedRegistryGate`
// holds the committed registry against the committed tree, and it is the thing CI fails on. But it
// asserts the DATA, not the COMPARATOR: with `Compare` mutated to never report an orphan row,
// and an orphan row planted in the registry, the gate came back GREEN. A green mutation means the test
// does not reach the property, and the answer is to reach it rather than to record the green.
//
// `Compare` is PURE — a registry, a map and a noun in, a list of sentences out — so the four ways a
// registry can disagree with a tree are assertable here, with no disk at all.

namespace
{
    std::map<std::string, Common::Content::ContentFile>
    Disk( std::initializer_list<std::pair<std::string, Common::Content::ContentFile>> rows )
    {
        std::map<std::string, Common::Content::ContentFile> out;
        for ( const auto& row : rows )
            out.emplace( row.first, row.second );
        return out;
    }

    bool Reports( const std::vector<Common::Content::RegistryDisagreement>& problems,
                  Common::Content::RegistryDisagreement::Kind what, const std::string& key )
    {
        return std::any_of( problems.begin(), problems.end(),
                            [&]( const Common::Content::RegistryDisagreement& problem )
                            { return problem.What == what && problem.Key == key; } );
    }
} // namespace

TEST( CookedAssetRegistry, AnAgreeingRegistryAndTreeProduceNoDisagreements )
{
    // THE POSITIVE CONTROL. Without it every case below could pass on a comparator that reports
    // everything, which is a gate nobody can ever satisfy and therefore a gate somebody deletes.
    AssetRegistry registry;
    ASSERT_TRUE( registry.Insert( Row( "assets:Materials/M.demat", "Material", 512 ) ) );

    const auto onDisk =
         Disk( { { "assets:Materials/M.demat", { Common::Content::ContentKind::Material, 512 } } } );

    EXPECT_TRUE( Common::Content::Compare( registry, onDisk, "on disk" ).empty() );
}

TEST( CookedAssetRegistry, AFileWithNoRowIsReportedBecauseItWouldNotReachAPackagedBuild )
{
    // The T2.7 hazard itself: since the boot stopped scanning the content roots, a file with no row
    // is a file the engine does not have — and the failure is packaged-build-only.
    const AssetRegistry registry;

    const auto onDisk =
         Disk( { { "assets:Materials/M.demat", { Common::Content::ContentKind::Material, 512 } } } );

    const auto problems = Common::Content::Compare( registry, onDisk, "on disk" );
    ASSERT_EQ( problems.size(), 1u );
    EXPECT_TRUE( Reports( problems, Common::Content::RegistryDisagreement::Kind::MissingRow,
                          "assets:Materials/M.demat" ) );
    // The remedy has to BE in the sentence: a gate that names a problem without naming the command
    // that fixes it is a gate people learn to disable.
    EXPECT_NE( problems.front().Detail.find( "AssetRegistryTool cook" ), std::string::npos )
         << problems.front().Detail;
}

TEST( CookedAssetRegistry, ARowWithNoFileIsReportedBecauseTheLoaderWillChaseItForEver )
{
    // THE CASE A MUTATION FOUND UNGUARDED. `Compare` was changed to skip this loop entirely
    // and the CI gate stayed green over a registry with a planted orphan row.
    AssetRegistry registry;
    ASSERT_TRUE( registry.Insert( Row( "assets:Materials/Gone.demat", "Material", 512 ) ) );

    const auto problems = Common::Content::Compare( registry, {}, "on disk" );
    ASSERT_EQ( problems.size(), 1u );
    EXPECT_TRUE( Reports( problems, Common::Content::RegistryDisagreement::Kind::OrphanRow,
                          "assets:Materials/Gone.demat" ) );
}

TEST( CookedAssetRegistry, AKindOrASizeThatDisagreesIsReportedAndTheRowIsStillFound )
{
    // Two independent ways one row can be wrong while the file exists, and they must be reported
    // SEPARATELY: the wrong kind means the loader builds the wrong class, the wrong size means the
    // file was edited after the cook and the identity column may name a number it no longer carries.
    AssetRegistry registry;
    ASSERT_TRUE( registry.Insert( Row( "assets:Materials/M.demat", "Texture", 512 ) ) );

    const auto onDisk =
         Disk( { { "assets:Materials/M.demat", { Common::Content::ContentKind::Material, 900 } } } );

    const auto problems = Common::Content::Compare( registry, onDisk, "on disk" );
    EXPECT_TRUE(
         Reports( problems, Common::Content::RegistryDisagreement::Kind::WrongKind, "assets:Materials/M.demat" ) );
    EXPECT_TRUE(
         Reports( problems, Common::Content::RegistryDisagreement::Kind::StaleSize, "assets:Materials/M.demat" ) );
    EXPECT_EQ( problems.size(), 2u ) << "one row, two independent faults, two sentences";
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

// ── AF7: THE HEADER COLUMN ────────────────────────────────────────────────────────────────────────
//
// The registry records what each file's OWN HEADER states - GUID and subsystem versions - read without
// the body, the way UE's asset registry reads a package summary. It is the one identity a Common-only
// cook can compute, so it is the one `check` asserts.

namespace
{
    Common::Content::AssetGuid GuidOf( uint64_t hi, uint64_t lo )
    {
        Common::Content::AssetGuid guid;
        guid.Hi = hi;
        guid.Lo = lo;
        return guid;
    }
} // namespace

TEST( CookedAssetRegistry, TheHeaderColumnSurvivesARoundTripWithItsVersionsSorted )
{
    AssetRegistryEntry row = Row( "assets:Materials/M.demat", "Material", 229 );
    row.Guid               = GuidOf( 0xb7de7b6da944bdedull, 0x0382e39126712944ull );
    row.Versions           = { { Common::Content::FourCC( "UNIT" ), 1 }, { Common::Content::FourCC( "MATL" ), 2 } };
    AssetRegistry registry;
    ASSERT_TRUE( registry.Insert( row ) );

    const std::string text = registry.Serialize();
    EXPECT_NE( text.find( " b7de7b6da944bded0382e39126712944;MATL=2,UNIT=1 " ), std::string::npos ) << text;

    const auto parsed = AssetRegistry::Parse( text );
    ASSERT_TRUE( parsed ) << parsed.GetError();
    const AssetRegistryEntry* back = parsed.GetValue().FindByKey( "assets:Materials/M.demat" );
    ASSERT_NE( back, nullptr );
    ASSERT_TRUE( back->Guid.has_value() );
    EXPECT_EQ( *back->Guid, *row.Guid );
    EXPECT_EQ( back->Versions.size(), 2u );
    EXPECT_EQ( parsed.GetValue().Serialize(), text );
}

TEST( CookedAssetRegistry, AVersionTwoFileIsReadAsRowsWithNoHeaderColumn )
{
    const auto parsed = AssetRegistry::Parse( "DesertAssetRegistry 2\n229 Material 0000000000000009 - - assets:M.demat\n" );
    ASSERT_TRUE( parsed ) << parsed.GetError();
    const AssetRegistryEntry* row = parsed.GetValue().FindByKey( "assets:M.demat" );
    ASSERT_NE( row, nullptr );
    EXPECT_EQ( row->Identity, 9u );
    EXPECT_FALSE( row->Guid.has_value() );
}

TEST( CookedAssetRegistry, AMalformedHeaderColumnIsRefused )
{
    EXPECT_FALSE( AssetRegistry::Parse( "DesertAssetRegistry 3\n5 Material zz - - - assets:M.demat\n" ) );
    EXPECT_FALSE( AssetRegistry::Parse(
         "DesertAssetRegistry 3\n5 Material 00000000000000000000000000000000; - - - assets:M.demat\n" ) )
         << "a null GUID is no identity";
    EXPECT_FALSE( AssetRegistry::Parse(
         "DesertAssetRegistry 3\n5 Material b7de7b6da944bded0382e39126712944;MATL - - - assets:M.demat\n" ) );
}

TEST( CookedAssetRegistry, ARowWhoseGuidIsNotTheFilesHeaderIsReported )
{
    AssetRegistryEntry row = Row( "assets:Materials/M.demat", "Material", 900 );
    row.Guid               = GuidOf( 1, 2 );
    row.Versions           = { { Common::Content::FourCC( "MATL" ), 1 } };
    AssetRegistry registry;
    ASSERT_TRUE( registry.Insert( row ) );

    auto onDisk = Disk( { { "assets:Materials/M.demat", { Common::Content::ContentKind::Material, 900 } } } );
    Common::Content::AssetHeader header;
    header.Kind       = Common::Content::ContentKind::Material;
    header.Guid       = GuidOf( 1, 2 );
    header.Subsystems = { { Common::Content::FourCC( "MATL" ), 1 } };
    onDisk.begin()->second.Header = header;
    EXPECT_TRUE( Common::Content::Compare( registry, onDisk, "on disk" ).empty() ) << "an agreeing header";

    onDisk.begin()->second.Header->Guid = GuidOf( 1, 3 );
    EXPECT_TRUE( Reports( Common::Content::Compare( registry, onDisk, "on disk" ),
                          Common::Content::RegistryDisagreement::Kind::StaleHeader, "assets:Materials/M.demat" ) );

    onDisk.begin()->second.Header->Guid = GuidOf( 1, 2 );
    onDisk.begin()->second.Header->Subsystems[0].Version = 2;
    EXPECT_TRUE( Reports( Common::Content::Compare( registry, onDisk, "on disk" ),
                          Common::Content::RegistryDisagreement::Kind::StaleHeader, "assets:Materials/M.demat" ) )
         << "a version raise the registry did not record";

    onDisk.begin()->second.Header->Kind = Common::Content::ContentKind::Scene;
    EXPECT_TRUE( Reports( Common::Content::Compare( registry, onDisk, "on disk" ),
                          Common::Content::RegistryDisagreement::Kind::BadHeader, "assets:Materials/M.demat" ) );

    onDisk.begin()->second.Header.reset();
    onDisk.begin()->second.HeaderError = "text header: null GUID";
    EXPECT_TRUE( Reports( Common::Content::Compare( registry, onDisk, "on disk" ),
                          Common::Content::RegistryDisagreement::Kind::BadHeader, "assets:Materials/M.demat" ) );
}
