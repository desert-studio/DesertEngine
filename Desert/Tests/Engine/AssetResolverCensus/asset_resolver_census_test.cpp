// EVERY ASSET TYPE A COMPONENT CAN DECLARE HAS A BRANCH IN THE ONE PLACE THAT PERSISTS IT.
//
// WHAT THE DEFECT IS. `Core::MakeAssetResolver` turns a reflected `AssetHandle` field into the string a
// `.desce` carries and back. It dispatches on the field's `AssetType` METADATA STRING through a
// hand-written chain of `if ( type == "..." )`, and until I13 the chain ENDED in the mesh branch: a type
// with no branch of its own was looked up as a mesh, found nothing, and returned an empty string. Since
// the Details panel builds its picker from the same `Asset<...>` annotation and knows nothing about the
// resolver, `PROPERTY( Asset<AudioAsset> )` added tomorrow would give its author a working file picker
// over a slot that saves as "" and loads as 0 — a dead setting (DC 1.3) delivered by a silent fallback
// (DC 1.4), in the one place that decides whether a scene reference survives a save.
//
// MEASURED WHEN THIS SUITE WAS WRITTEN: seven `AssetType` values are declared and all seven have a
// branch. So the trap was ARMED and had not fired — which is the only time it can be closed cheaply, and
// exactly the state this project keeps discovering too late.
//
// TWO GUARDS, AND THEY CATCH DIFFERENT THINGS. This census fires at the moment a field is DECLARED, in
// CI, before anyone opens the editor — it reads the generated reflection table, which is the same table
// the picker is built from. The resolver's own refusal (a LOG_ERROR naming the type, added by the same
// change) catches everything a census cannot see: a type that reaches the resolver from somewhere other
// than a reflected component field, and the window between adding a field and running the suite.
//
// WHY IT READS THE SOURCE, AND WHY WITH LITERALS INTACT. The branch chain is not enumerable at runtime —
// it is a sequence of statements inside a lambda — so the handled set can only be read as text. The
// subject of the question IS a string literal (`type == "FontAsset"`), so the shared reader is used in
// its comments-only mode: reading RAW source would let a literal inside a COMMENT satisfy the census,
// which is a false PASS, and a census that certifies a type as handled when it is not is worse than no
// census at all. That is one reader with two entry points, not a second opinion about literals.

#include "../SettingConsumers/setting_consumers_reader.hpp"

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Constants.hpp>
#include <Engine/Reflection/ReflectionRegistry.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using Desert::Tests::ConsumerText::StripComments;

namespace
{
    constexpr const char* kResolver  = "Desert/Desert/Source/Engine/Core/Serialize/ComponentRegistry.cpp";
    // THE WRITE HALF MOVED, AND THIS SUITE FOLLOWS IT RATHER THAN BEING RELAXED. Until T2.4 the
    // resolver answered "which file is this handle" twelve times, once per type, inside the lambda in
    // kResolver; the cooked asset registry answers it once for every type, and what is left per type
    // is only the FORM the reference is spelled in. That table is here, in a translation unit a suite
    // can actually link — the same reason TextureSlot.cpp was extracted, stated in its own header.
    //
    // Both files are read and concatenated: the census's question is "does this type have a branch
    // SOMEWHERE in the resolver", and splitting one answer across two files must not split the census.
    constexpr const char* kForms     = "Desert/Desert/Source/Engine/Core/Serialize/StoredAssetForm.cpp";
    constexpr const char* kConstants = "Desert/Common/Source/Common/Core/Constants.hpp";

    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 8; ++up )
        {
            std::ifstream probe( prefix + kResolver );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    std::string ReadAll( const std::string& path )
    {
        std::ifstream     in( path, std::ios::binary );
        std::stringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    // Every distinct `AssetType` the live reflection table declares — the same metadata the Details
    // panel builds its asset picker from, so this is the set of types a person can actually author.
    std::set<std::string> DeclaredAssetTypes()
    {
        using namespace Desert::Reflection;

        std::set<std::string> types;
        for ( const auto& [name, type] : ReflectionRegistry::Get().All() )
        {
            for ( const FieldInfo& field : type.Fields )
            {
                if ( field.Type == FieldType::AssetHandle && !field.Meta.AssetType.empty() )
                    types.insert( field.Meta.AssetType );
            }
        }
        return types;
    }
} // namespace

// 0. THE SUITE CAN SEE WHAT IT CLAIMS TO CHECK. Without this every loop below runs over an empty set and
// reports green — the failure that looks exactly like a census which found nothing wrong.
TEST( AssetResolverCensus, TheSourcesThisSuiteReadsAreWhereItThinksTheyAre )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "repository root not found from the test's working directory";

    EXPECT_FALSE( ReadAll( root + kResolver ).empty() ) << kResolver << " is missing or empty";
    EXPECT_FALSE( ReadAll( root + kForms ).empty() ) << kForms << " is missing or empty";
    EXPECT_FALSE( ReadAll( root + kConstants ).empty() ) << kConstants << " is missing or empty";
    EXPECT_FALSE( DeclaredAssetTypes().empty() )
         << "the reflection table declares no asset-bearing field at all, which cannot be true while the "
            "Details panel offers asset pickers - the generated table is stale, not the tree";
}

// THE RELATION.
TEST( AssetResolverCensus, EveryDeclaredAssetTypeHasABranchInTheResolver )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    std::string code = StripComments( ReadAll( root + kResolver ) ) + StripComments( ReadAll( root + kForms ) );
    ASSERT_FALSE( code.empty() );
    // Whitespace out, so a branch broken across lines by the formatter still reads as one comparison.
    std::erase_if( code, []( unsigned char c ) { return std::isspace( c ) != 0; } );

    const std::set<std::string> declared = DeclaredAssetTypes();
    ASSERT_FALSE( declared.empty() );

    for ( const std::string& type : declared )
    {
        EXPECT_NE( code.find( "type==\"" + type + "\"" ), std::string::npos )
             << "a component declares PROPERTY( ..., Asset<" << type
             << "> ) and Core::MakeAssetResolver has no branch for it. The Details panel will offer a "
                "working picker for that field and the slot will save as an empty string and load as "
                "unset - the value is lost on every round trip, and the resolver's own refusal is the "
                "only thing that will say so. Add a branch beside the others in ComponentRegistry.cpp.";
    }
}

// The refusal is the other half, and it has to BE there: without it a type this census has not yet run
// against is lost in silence. Asserted as text for the same reason the branches are — the tail of a
// lambda is not reachable from a test without an AssetManager and a live project.
TEST( AssetResolverCensus, AnUnknownAssetTypeIsRefusedRatherThanTreatedAsAMesh )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    std::string code = StripComments( ReadAll( root + kResolver ) ) + StripComments( ReadAll( root + kForms ) );
    ASSERT_FALSE( code.empty() );
    std::erase_if( code, []( unsigned char c ) { return std::isspace( c ) != 0; } );

    // The mesh branch must TEST its own types. It used to end the function unconditionally, which is
    // what made an unknown type a mesh lookup instead of an error.
    EXPECT_NE( code.find( "type==\"StaticMeshAsset\"" ), std::string::npos )
         << "the mesh branch does not test its own type, so it is the fall-through again and every "
            "unhandled asset type resolves as a mesh that does not exist.";

    // And both directions must say so. Two occurrences: one in ToPath, one in FromPath.
    const std::string marker = "hasnobranchinCore::MakeAssetResolver";
    std::size_t       found  = 0;
    for ( std::size_t at = code.find( marker ); at != std::string::npos; at = code.find( marker, at + 1 ) )
        ++found;
    EXPECT_EQ( found, 2u )
         << "the resolver must refuse an unknown asset type in BOTH directions - a write that drops the "
            "value and a read that discards what the file states are two different losses, and a bare "
            "`return 0` is indistinguishable from 'the scene named nothing'.";
}

// ── THE TWO ENGINE ROOTS, AND WHY THEY ARE NOT CENSUS ROWS ───────────────────────────────────────────
//
// I10 needed "which content root does this path belong to" for a font and an icon, and could not ask the
// ContentDir census: FONTS_PATH and ICONS_PATH are not rows of it. They are plain constants, so the
// answer had to be a two-root lexical match written by hand instead of one census row read twice.
//
// THE DECISION IS THAT THEY STAY OUT, and this test is that decision written down where it cannot be
// mistaken for an oversight. The census exists to derive directories THAT MOVE WITH THE PROJECT — every
// row is `<root>/<relative part>` where the root comes from the open `.deproj`, and `SetProjectRoot`
// rewrites all of them at once. The engine trees do not move: they sit beside the binary, they are the
// same for every project this editor opens, and the packager ships them under their own dev-time
// relative paths precisely because nothing ever remaps them. A row for `Resources/Fonts/` would have to
// name a THIRD root that the derivation does not have and that no `.deproj` can supply, and `DirRoot`
// has exactly two values on purpose.
//
// WHAT IS NOT ACCEPTABLE is the state I10 found: the reason existing only in somebody's head. So it is
// asserted here — the two constants must remain OUTSIDE the derived storage, and the day someone tries
// to make them rows, this fails and points at the paragraph above.
TEST( AssetResolverCensus, TheEngineResourceTreesAreDeliberatelyNotProjectCensusRows )
{
    namespace P = Common::Constants::Path;

    // Not derived: their addresses are not in the census storage, so no remap can move them. Compared by
    // ADDRESS rather than by spelling, because that is what "outside the derivation" actually means.
    for ( std::size_t i = 0; i < P::CONTENT_DIR_COUNT; ++i )
    {
        const std::filesystem::path& row = P::Dir( static_cast<P::ContentDir>( i ) );
        EXPECT_NE( &row, &P::FONTS_PATH ) << "FONTS_PATH became census row " << i;
        EXPECT_NE( &row, &P::ICONS_PATH ) << "ICONS_PATH became census row " << i;
        EXPECT_NE( &row, &P::RESOURCE_PATH ) << "RESOURCE_PATH became census row " << i;
        EXPECT_NE( &row, &P::SHADERDIR_PATH ) << "SHADERDIR_PATH became census row " << i;
    }

    // And they do not move when a project is opened, which is the property the decision rests on.
    const std::filesystem::path fontsBefore  = P::FONTS_PATH;
    const std::filesystem::path iconsBefore  = P::ICONS_PATH;
    const std::filesystem::path assetsBefore = P::ASSETS_PATH;

    P::SetProjectRoot( "/tmp/desert_census_probe", "GameAssets" );
    EXPECT_EQ( P::FONTS_PATH, fontsBefore ) << "an engine tree followed the project root";
    EXPECT_EQ( P::ICONS_PATH, iconsBefore ) << "an engine tree followed the project root";
    EXPECT_NE( P::ASSETS_PATH, assetsBefore )
         << "the assets root did NOT follow the project root, so this test proves nothing about the "
            "difference between the two kinds";
    P::ResetToSandbox();

    // The two kinds must be TOLD APART by the one table that has to know: AssetHandle's root tags. This
    // is what let I10 write `engine:Fonts/x.ttf` and `assets:Fonts/x.ttf` as different references at all.
    EXPECT_NE( Common::AssetHandle::EngineTag(), Common::AssetHandle::AssetsTag() );
    EXPECT_FALSE( Common::AssetHandle::EngineTag().empty() );
    EXPECT_FALSE( Common::AssetHandle::AssetsTag().empty() );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
