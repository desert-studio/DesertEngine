// THE PALETTE OFFERS WHAT THE PROJECT HAS, AND EVERY ENTRY NAMES EXACTLY ONE FILE.
//
// Two rules, one file, both paid for.
//
// RULE ONE -- ENUMERATE FROM WHAT DESCRIBES EXISTENCE. The `Open` group used to be built from the asset
// manager's cache: whatever the startup preloader had registered so far. That is a container whose
// contents are derived from the same source as the question being asked of it, and it failed the way that
// shape always fails. Measured through the control channel, polling once per frame on this repository's
// own project: the group goes 0 -> 106 -> 130 entries, because FIVE separate startup stages fill the
// cache. Between the third and the eighth the palette successfully offers every material in the project
// and NOT ONE of its twenty-four cloud assets -- for 3.3 seconds of every boot, on an idle machine. A
// client that asked once concluded the project has no cloud documents, and nothing in the reply said
// otherwise.
//
// RULE TWO -- AN ADDRESS NAMES ONE THING. Labels were `filename()`, and this project has three files
// called `model.demat`. Three palette rows spelled identically; `run Open model.demat` matched all three
// and the last one silently won. The lesson was already learnt one group over and written down there --
// SceneLabel names a level by its path relative to the scenes root, "not the bare filename... two
// Test.desce in different folders read identically" -- and the `Open` group simply never got the fix.
//
// Editor/Core/OpenableAssets.hpp is pure: files in, labels out. No filesystem walk, no asset manager, no
// ImGui. That is the only reason either rule can be asserted -- EditorLayer.cpp, where the palette is
// assembled, is compiled by no suite at all (scripts/CI/UnreachedSources.sh).

#include <Editor/Core/OpenableAssets.hpp>

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

using Desert::Editor::CollectOpenableAssets;
using Desert::Editor::LowercaseExtension;
using Desert::Editor::OpenableAsset;

namespace
{
    const std::filesystem::path kRoot = "/project/Resources/Assets";

    std::vector<std::string> Claimed()
    {
        return { ".demat", ".dcnv", ".decloudtype", ".dcmv", ".dclayout" };
    }

    std::vector<std::string> LabelsOf( const std::vector<OpenableAsset>& assets )
    {
        std::vector<std::string> labels;
        labels.reserve( assets.size() );
        for ( const OpenableAsset& asset : assets )
            labels.push_back( asset.Label );
        return labels;
    }
} // namespace

// ---------------------------------------------------------------------------------------------------
// RULE TWO: an address names one thing.
// ---------------------------------------------------------------------------------------------------

// THE DEFECT, IN ITS OWN SHAPE. These three paths are real: they are what this repository ships today,
// and under `filename()` they produced three rows a client could not tell apart.
TEST( OpenableAssets, ThreeFilesOfOneNameInThreeFoldersAreThreeDISTINGUISHABLEEntries )
{
    const std::vector<std::filesystem::path> files = {
         kRoot / "Materials/base/model.demat",
         kRoot / "Materials/base_basic_pbr/model.demat",
         kRoot / "Materials/base_basic_shaded/model.demat",
    };

    const std::vector<std::string> labels = LabelsOf( CollectOpenableAssets( files, Claimed(), kRoot ) );

    ASSERT_EQ( labels.size(), 3u );
    EXPECT_EQ( std::set<std::string>( labels.begin(), labels.end() ).size(), 3u )
         << "two files answering to one address is a command that runs on whichever the loop saw last";
    EXPECT_EQ( labels[0], "Materials/base/model.demat" );
    EXPECT_EQ( labels[1], "Materials/base_basic_pbr/model.demat" );
    EXPECT_EQ( labels[2], "Materials/base_basic_shaded/model.demat" );
}

// THE GENERAL PROPERTY, not the instance above: distinct files can never share a label. Asserted over a
// set of paths chosen to attack it -- same stem in different folders, same folder name at two depths, a
// name that is a prefix of another.
TEST( OpenableAssets, DistinctFilesNeverShareALabel )
{
    const std::vector<std::filesystem::path> files = {
         kRoot / "a/model.demat",
         kRoot / "b/model.demat",
         kRoot / "a/b/model.demat",
         kRoot / "model.demat",
         kRoot / "a/model2.demat",
         kRoot / "Clouds/Types/Cirrus.decloudtype",
         kRoot / "Clouds/Volumes/Cirrus.dcmv",
    };

    const std::vector<std::string> labels = LabelsOf( CollectOpenableAssets( files, Claimed(), kRoot ) );

    EXPECT_EQ( labels.size(), files.size() );
    EXPECT_EQ( std::set<std::string>( labels.begin(), labels.end() ).size(), files.size() );
}

// The label is what a client SENDS, so it must not depend on the platform's separator. Generic form is
// also exactly the key form a content manifest uses, which keeps one spelling of "where a file is".
TEST( OpenableAssets, ALabelIsWrittenWithForwardSlashesWhateverThePlatformUses )
{
    const auto assets =
         CollectOpenableAssets( { kRoot / "Clouds" / "Types" / "Cirrus.decloudtype" }, Claimed(), kRoot );

    ASSERT_EQ( assets.size(), 1u );
    EXPECT_EQ( assets[0].Label, "Clouds/Types/Cirrus.decloudtype" );
    EXPECT_EQ( assets[0].Label.find( '\\' ), std::string::npos );
}

// A file outside the content root keeps its whole path rather than a "../../.." chain, which names
// nothing a person could act on and nothing a client could reproduce. Still unambiguous, which is the
// property that actually matters.
TEST( OpenableAssets, AFileOutsideTheContentRootIsNamedInFullRatherThanByAChainOfDotDots )
{
    const auto assets = CollectOpenableAssets( { "/elsewhere/Shared/Rock.demat" }, Claimed(), kRoot );

    ASSERT_EQ( assets.size(), 1u );
    EXPECT_EQ( assets[0].Label.rfind( "..", 0 ), std::string::npos );
    EXPECT_NE( assets[0].Label.find( "Rock.demat" ), std::string::npos );
}

// The palette's order must be a property of the PROJECT, not of the order the filesystem happened to walk
// it in. Two runs of one client offering two different lists is a difference a report would quote.
TEST( OpenableAssets, TheOrderIsTheLabelsAndNotTheOrderTheFilesArrivedIn )
{
    const std::vector<std::filesystem::path> forwards = {
         kRoot / "a.demat",
         kRoot / "b.demat",
         kRoot / "c.demat",
    };
    const std::vector<std::filesystem::path> backwards = {
         kRoot / "c.demat",
         kRoot / "b.demat",
         kRoot / "a.demat",
    };

    EXPECT_EQ( LabelsOf( CollectOpenableAssets( forwards, Claimed(), kRoot ) ),
               LabelsOf( CollectOpenableAssets( backwards, Claimed(), kRoot ) ) );
}

// The entry carries the FULL path beside the label, because that is what a path opener resolves. A label
// handed to the opener instead would be a relative string resolved against whatever the working directory
// happened to be -- and the editor's own working directory is not the content root.
TEST( OpenableAssets, TheEntryCarriesWhatTheOpenerNeedsAsWellAsWhatTheHumanReads )
{
    const auto assets = CollectOpenableAssets( { kRoot / "Materials/base/model.demat" }, Claimed(), kRoot );

    ASSERT_EQ( assets.size(), 1u );
    EXPECT_EQ( assets[0].Label, "Materials/base/model.demat" );
    EXPECT_EQ( assets[0].Path, "/project/Resources/Assets/Materials/base/model.demat" );
}

// ---------------------------------------------------------------------------------------------------
// RULE ONE: the filter is the registrations', and it is total.
// ---------------------------------------------------------------------------------------------------

// Only what some opener claims. A `.png` beside a material is content and is not a document; offering it
// would produce a palette entry that refuses when run, which is a worse answer than not offering it.
TEST( OpenableAssets, OnlyTheExtensionsAnOpenerClaimsAreOffered )
{
    const std::vector<std::filesystem::path> files = {
         kRoot / "Materials/Rock.demat",
         kRoot / "Textures/Rock.tex",
         kRoot / "Meshes/Rock.stmesh",
         kRoot / "Meshes/texture_diffuse.png",
         kRoot / "Clouds/Types/Cirrus.decloudtype",
    };

    EXPECT_EQ( LabelsOf( CollectOpenableAssets( files, Claimed(), kRoot ) ),
               ( std::vector<std::string>{ "Clouds/Types/Cirrus.decloudtype", "Materials/Rock.demat" } ) );
}

// AN EMPTY CLAIM LIST OFFERS NOTHING, and that is the honest answer rather than a fallback to "everything
// looks openable". It is also why SubjectEditorRegistry refuses to register an opener that claims no
// extension at all: the formats would be openable by double-clicking and by nothing else.
TEST( OpenableAssets, AnEditorWithNoRegisteredOpenersOffersNothingRatherThanEverything )
{
    const std::vector<std::filesystem::path> files = { kRoot / "Materials/Rock.demat" };
    EXPECT_TRUE( CollectOpenableAssets( files, {}, kRoot ).empty() );
}

// Case is not part of the identity of a format. A `.DEMAT` written by a tool on a case-insensitive
// filesystem is the same material, and skipping it would leave a file that opens by double-click and is
// missing from the one list that is supposed to be the editor's whole vocabulary.
TEST( OpenableAssets, AnExtensionIsMatchedWithoutRegardToCase )
{
    EXPECT_EQ( LowercaseExtension( "Rock.DEMAT" ), ".demat" );
    EXPECT_EQ( LowercaseExtension( "Rock.DeMat" ), ".demat" );
    EXPECT_EQ( LowercaseExtension( "Rock" ), "" );

    const auto assets = CollectOpenableAssets( { kRoot / "Materials/Rock.DEMAT" }, Claimed(), kRoot );
    EXPECT_EQ( assets.size(), 1u );
}

// A project with nothing openable in it produces an EMPTY list, and that is a fact about the project
// rather than about the editor's state -- which is the whole difference this change bought. The value of
// the empty answer is that it is now only ever reachable when it is true: the channel does not answer at
// all until the editor has settled, so "no entries" cannot mean "ask again later" any more.
TEST( OpenableAssets, AProjectWithNothingOpenableAnswersEmptyAndThatIsNowATruth )
{
    const std::vector<std::filesystem::path> files = {
         kRoot / "Textures/Rock.tex",
         kRoot / "Meshes/Rock.stmesh",
    };
    EXPECT_TRUE( CollectOpenableAssets( files, Claimed(), kRoot ).empty() );
}
