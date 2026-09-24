// A TEXTURE REFERENCE THAT SURVIVES THE TRIP — a suite about a RELATION, not about a function.
//
// The relation: THE HANDLE A SCENE STORES MUST COME BACK AS THE SAME TEXTURE, on a machine that shares
// no directory with the one that wrote it. Both halves belong to it — a name that only this computer can
// read is not a reference, and neither is a name that reads back as a different (or a null) asset.
//
// What was broken, measured before anything was changed:
//
//   1. `MakeAssetResolver::ToPath`'s TextureAsset branch wrote `GetMetadata().Filepath` VERBATIM. Every
//      content root turns absolute the moment a `.deproj` is opened, so the string that reached the file
//      was `/Users/<somebody>/.../Cooked/Textures/T.tex` — the exact defect the MaterialAsset branch
//      beside it had already been fixed for, in a file that gets committed. The material's fix could not
//      be copied here: a material lives under ASSETS_PATH and a cooked texture under COOKED_PATH, a
//      SIBLING of it, where `relative(path, ASSETS_PATH)` gives `../Cooked/...` and falls back to the
//      absolute spelling anyway. That is why the stored form is the root-TAGGED key.
//   2. The read side was `FindByPath` and nothing else. That lookup compared filepaths VERBATIM (unlike
//      CreateAsset, which deduplicated on the spelling-independent stable key), so a miss was ordinary
//      and the branch answered it with a bare `0` — no asset created, nothing logged. Three separate
//      spellings of a real cooked texture were reported as failing to resolve, and not one said why.
//      HALF OF (2) HAS SINCE BEEN FIXED AT ITS SOURCE and this paragraph is history: the registry no
//      longer holds two answers to "is this file registered" — `FindByPath` asks `CreateAsset`'s
//      question, on `CreateAsset`'s key (Desert/Tests/Engine/AssetPathIdentity). What this suite still
//      owns is the other half, which is not the registry's: the STORED FORM has to be expanded into a
//      path before any identity can be derived from it, because a root-tagged key is not a path.
//
// Why this file exists at all. Both branches lived inside ComponentRegistry.cpp, which reaches the
// ResourceRegistry and through it the whole renderer, so NO suite in the repository could execute them.
// They are now in Engine/Core/Serialize/TextureSlot.cpp, which needs the AssetManager and nothing else,
// and this is that unit under test.

#include <gtest/gtest.h>

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/TextureAsset.hpp>
#include <Engine/Assets/TextureSourceAsset.hpp>
#include <Engine/Assets/Serialization/TextureBinary.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Core/Serialize/TextureSlot.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/AssetPathIndex.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>

#include <spdlog/sinks/ostream_sink.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

using Desert::Assets::AssetManager;
using Desert::Assets::AssetPriority;
using Desert::Assets::TextureAsset;
using Desert::Core::Serialize::TextureSlotFromPath;
using Desert::Core::Serialize::TextureSlotToPath;

namespace
{
    // A handle far above 2^53, which is what a real one looks like: a texture takes its id from the
    // `Handle` field of its own cooked file, and the ones in this repository are 19-digit numbers. The
    // value is the one measured going wrong through the JSON double round trip (5355760296319878840 came
    // back as 5355760296319879168), so a regression there shows up here as well as in the serializer's
    // own suite.
    constexpr uint64_t kProbeHandle = 5355760296319878840ull;
    constexpr uint64_t kOtherHandle = 5355760296319878841ull;

    // Restores the project root SetProjectRoot rewrites — the content directories are process-wide
    // state, so a test that opens a project and walks away leaves every test after it measuring that
    // project. This used to save and restore seventeen path copies one assignment at a time; the paths
    // are all derived from the ONE root pair now, so the pair is the whole state worth saving.
    class ProjectRootGuard
    {
    public:
        ProjectRootGuard() : m_Saved( Common::Constants::Path::CurrentProjectRoot() )
        {
            // AND THE TWO PROCESS-WIDE TABLES THAT KEY ON THE ROOTS. Since T2.4 the write side of a
            // texture reference answers from the cooked asset registry rather than from an
            // AssetManager, and both the registry and the path index are per PROCESS while the roots
            // this suite moves are too: a case that re-roots the project mints the same relative keys
            // behind new absolute roots, and without these two lines the second case would be judged
            // against the first case's rows. This is the arrangement each of those files' own Clear /
            // ResetForTest was written for, and it is named there.
            Common::AssetPathIndex::Clear();
            Desert::Assets::ContentRegistry::ResetForTest();
        }

        ~ProjectRootGuard()
        {
            Common::Constants::Path::SetProjectRoot( m_Saved.ProjectDir, m_Saved.AssetsRoot );
            Common::AssetPathIndex::Clear();
            Desert::Assets::ContentRegistry::ResetForTest();
        }

        ProjectRootGuard( const ProjectRootGuard& )            = delete;
        ProjectRootGuard& operator=( const ProjectRootGuard& ) = delete;

    private:
        Common::Constants::Path::ProjectRootState m_Saved;
    };

    // A REAL texture asset (the TXAS envelope, AF7), because the claim is about what the loader does with
    // the identity the file itself states — written by the engine's own writer, since a second writer of one
    // format is exactly the drift this suite exists to catch elsewhere. The source is a single byte: the
    // loader reads the header and IMPT only, and the pixels are not what is under test, the identity is.
    void WriteCookedTexture( const std::filesystem::path& at, uint64_t handle )
    {
        std::filesystem::create_directories( at.parent_path() );
        const Desert::Assets::TextureSourceAsset asset = Desert::Assets::MakeTextureSourceAsset(
             Common::Content::ContentKind::Texture, Common::UUID( handle ),
             "assets:Textures/" + at.stem().string() + ".png", { std::byte{ 0x7F } }, {} );
        const auto written = Desert::Assets::WriteTextureSourceAssetFile( at, asset );
        ASSERT_TRUE( written.IsSuccess() )
             << "could not write the fixture at " << at.string() << ": " << written.GetError();
    }

    // Two developers' checkouts, sharing no directory above the project and not even agreeing on what the
    // assets folder is called. Everything below is built inside one of these.
    struct Checkout
    {
        std::filesystem::path Dir;
        std::filesystem::path AssetsRootName;
    };

    std::filesystem::path ScratchRoot()
    {
        return std::filesystem::temp_directory_path() / "desert_texture_slot_round_trip";
    }

    Checkout MakeCheckout( const char* who, const char* assetsRootName )
    {
        const Checkout c{ ScratchRoot() / who, assetsRootName };
        std::filesystem::create_directories( c.Dir );
        return c;
    }

    void Open( const Checkout& c )
    {
        Common::Constants::Path::SetProjectRoot( c.Dir, c.AssetsRootName );
    }

    // Captures everything the logger emits for the duration of one call. The default logger is restored
    // afterwards, because a test that leaves a sink behind silences every test after it.
    class LogCapture
    {
    public:
        LogCapture() : m_Previous( spdlog::default_logger() )
        {
            auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>( m_Stream );
            spdlog::set_default_logger( std::make_shared<spdlog::logger>( "capture", std::move( sink ) ) );
            spdlog::set_level( spdlog::level::trace );
        }

        ~LogCapture()
        {
            spdlog::set_default_logger( m_Previous );
        }

        LogCapture( const LogCapture& )            = delete;
        LogCapture& operator=( const LogCapture& ) = delete;

        std::string Text() const
        {
            return m_Stream.str();
        }

    private:
        std::ostringstream              m_Stream;
        std::shared_ptr<spdlog::logger> m_Previous;
    };
} // namespace

// ---------------------------------------------------------------------------------------------------
// THE RELATION.
// ---------------------------------------------------------------------------------------------------

TEST( TextureSlotRoundTrip, AHandleStoredOnOneMachineNamesTheSameTextureOnAnother )
{
    ProjectRootGuard guard;
    std::filesystem::remove_all( ScratchRoot() );

    // --- the machine that saves the scene --------------------------------------------------------
    const Checkout ann = MakeCheckout( "ann", "Content" );
    WriteCookedTexture( ann.Dir / "Cooked" / "Textures" / "T_Probe.tex", kProbeHandle );
    Open( ann );

    AssetManager annsManager;
    const auto   annsTexture = annsManager.CreateAsset<TextureAsset>(
         AssetPriority::Medium, Common::Filepath( ann.Dir / "Cooked" / "Textures" / "T_Probe.tex" ) );
    ASSERT_NE( annsTexture, nullptr );
    const uint64_t saved = static_cast<uint64_t>( annsTexture->GetMetadata().Handle );
    ASSERT_EQ( saved, kProbeHandle ) << "a texture's identity comes from its own file; the fixture is wrong";

    const std::string stored = TextureSlotToPath( saved );

    // What actually goes into the file. Asserted as a VALUE and not merely as "not absolute", because
    // "not absolute" is also true of the empty string this branch used to produce for an unknown type.
    EXPECT_EQ( stored, "cooked:Textures/T_Probe.tex" );

    // --- the machine that opens it ----------------------------------------------------------------
    const Checkout ci = MakeCheckout( "ci", "Assets" );
    WriteCookedTexture( ci.Dir / "Cooked" / "Textures" / "T_Probe.tex", kProbeHandle );
    Open( ci );

    AssetManager   cisManager;
    const uint64_t loaded = TextureSlotFromPath( cisManager, stored );

    EXPECT_EQ( loaded, saved ) << "a texture reference written down by one checkout did not come back as "
                                  "the same texture in another. This is the defect as an artist meets it: "
                                  "the scene still names the image, the image is still on disk, and the "
                                  "slot is empty.";

    // And it is THIS checkout's file that was resolved, not a stale record of the other one.
    const auto resolved = cisManager.FindByHandle<TextureAsset>( Common::AssetHandle( loaded ) );
    ASSERT_NE( resolved, nullptr );
    EXPECT_EQ( resolved->GetMetadata().Filepath.lexically_normal(),
               ( ci.Dir / "Cooked" / "Textures" / "T_Probe.tex" ).lexically_normal() );

    std::filesystem::remove_all( ScratchRoot() );
}

TEST( TextureSlotRoundTrip, TheStoredFormCarriesNoPartOfTheMachineItWasWrittenOn )
{
    // The property the round trip above rests on, asserted directly so a failure says WHAT leaked rather
    // than only that something did. 42 of 50 shipped scenes once carried a home directory this way.
    ProjectRootGuard guard;
    std::filesystem::remove_all( ScratchRoot() );

    const Checkout ann = MakeCheckout( "ann", "Content" );
    WriteCookedTexture( ann.Dir / "Cooked" / "Textures" / "T_Probe.tex", kProbeHandle );
    Open( ann );

    AssetManager manager;
    const auto   texture = manager.CreateAsset<TextureAsset>(
         AssetPriority::Medium, Common::Filepath( ann.Dir / "Cooked" / "Textures" / "T_Probe.tex" ) );
    ASSERT_NE( texture, nullptr );

    const std::string stored = TextureSlotToPath( static_cast<uint64_t>( texture->GetMetadata().Handle ) );

    EXPECT_EQ( stored.find( ann.Dir.generic_string() ), std::string::npos )
         << "the stored reference '" << stored << "' contains the checkout directory";
    EXPECT_FALSE( std::filesystem::path( stored ).is_absolute() ) << stored;

    std::filesystem::remove_all( ScratchRoot() );
}

TEST( TextureSlotRoundTrip, AContentTextureAndACookedOneTakeDifferentRootsAndBothComeBack )
{
    // Both roots, because the whole reason the stored form is TAGGED is that a texture can sit under
    // either and the two are siblings. A form relative to the assets root can only spell one of them.
    ProjectRootGuard guard;
    std::filesystem::remove_all( ScratchRoot() );

    const Checkout ann = MakeCheckout( "ann", "Content" );
    WriteCookedTexture( ann.Dir / "Cooked" / "Textures" / "T_Cooked.tex", kProbeHandle );
    WriteCookedTexture( ann.Dir / "Content" / "Textures" / "T_Content.tex", kOtherHandle );
    Open( ann );

    AssetManager manager;
    ASSERT_NE( manager.CreateAsset<TextureAsset>(
                    AssetPriority::Medium, Common::Filepath( ann.Dir / "Cooked" / "Textures" / "T_Cooked.tex" ) ),
               nullptr );
    ASSERT_NE(
         manager.CreateAsset<TextureAsset>(
              AssetPriority::Medium, Common::Filepath( ann.Dir / "Content" / "Textures" / "T_Content.tex" ) ),
         nullptr );

    EXPECT_EQ( TextureSlotToPath( kProbeHandle ), "cooked:Textures/T_Cooked.tex" );
    EXPECT_EQ( TextureSlotToPath( kOtherHandle ), "assets:Textures/T_Content.tex" );

    // And back, in a manager that knows nothing, which is what a cold start is.
    AssetManager fresh;
    EXPECT_EQ( TextureSlotFromPath( fresh, "cooked:Textures/T_Cooked.tex" ), kProbeHandle );
    EXPECT_EQ( TextureSlotFromPath( fresh, "assets:Textures/T_Content.tex" ), kOtherHandle );

    std::filesystem::remove_all( ScratchRoot() );
}

TEST( TextureSlotRoundTrip, TwoTexturesDoNotCollapseOntoOneReference )
{
    // The companion every round-trip assertion needs: a writer that returned one constant, or a reader
    // that answered every name with the first texture it had, would satisfy the tests above.
    ProjectRootGuard guard;
    std::filesystem::remove_all( ScratchRoot() );

    const Checkout ann = MakeCheckout( "ann", "Content" );
    WriteCookedTexture( ann.Dir / "Cooked" / "Textures" / "A.tex", kProbeHandle );
    WriteCookedTexture( ann.Dir / "Cooked" / "Textures" / "B.tex", kOtherHandle );
    Open( ann );

    AssetManager manager;
    ASSERT_NE( manager.CreateAsset<TextureAsset>( AssetPriority::Medium,
                                                  Common::Filepath( ann.Dir / "Cooked" / "Textures" / "A.tex" ) ),
               nullptr );
    ASSERT_NE( manager.CreateAsset<TextureAsset>( AssetPriority::Medium,
                                                  Common::Filepath( ann.Dir / "Cooked" / "Textures" / "B.tex" ) ),
               nullptr );

    const std::string a = TextureSlotToPath( kProbeHandle );
    const std::string b = TextureSlotToPath( kOtherHandle );
    EXPECT_NE( a, b );
    EXPECT_EQ( TextureSlotFromPath( manager, a ), kProbeHandle );
    EXPECT_EQ( TextureSlotFromPath( manager, b ), kOtherHandle );

    std::filesystem::remove_all( ScratchRoot() );
}

TEST( TextureSlotRoundTrip, EverySpellingOfOneFileResolvesToOneTexture )
{
    // The read side used to be `FindByPath` alone, and that lookup compared filepaths VERBATIM. So a
    // scene that named a preloaded texture by any other spelling of the same file missed it and got 0 —
    // while AssetManager::CreateAsset, one line away, deduplicated on the spelling-independent key and
    // would have found it. Two lookups that had to agree about what "the same file" means, and did not.
    //
    // The registry's half of that is closed at the source now (Desert/Tests/Engine/AssetPathIdentity
    // asserts the two entry points agree). This test is kept and is not a duplicate of it: it measures
    // the WHOLE read path — expand the stored form, then ask — over the spellings a real `.desce`
    // carries, which is the level at which a scene's texture reference either survives or does not.
    ProjectRootGuard guard;
    std::filesystem::remove_all( ScratchRoot() );

    const Checkout ann = MakeCheckout( "ann", "Content" );
    WriteCookedTexture( ann.Dir / "Cooked" / "Textures" / "T_Probe.tex", kProbeHandle );
    Open( ann );

    AssetManager manager;
    ASSERT_NE( manager.CreateAsset<TextureAsset>(
                    AssetPriority::Medium, Common::Filepath( ann.Dir / "Cooked" / "Textures" / "T_Probe.tex" ) ),
               nullptr );

    EXPECT_EQ( TextureSlotFromPath( manager, "cooked:Textures/T_Probe.tex" ), kProbeHandle );
    EXPECT_EQ( TextureSlotFromPath( manager, ( ann.Dir / "Cooked" / "Textures" / "T_Probe.tex" ).string() ),
               kProbeHandle )
         << "the absolute spelling — what every file written before the tagged form carries — no longer "
            "resolves";
    EXPECT_EQ( TextureSlotFromPath( manager, "cooked:Textures/../Textures/T_Probe.tex" ), kProbeHandle );

    // One record, not four: the reader must not manufacture a second asset per spelling.
    EXPECT_EQ( manager.FindAllByType<TextureAsset>().size(), 1u );

    std::filesystem::remove_all( ScratchRoot() );
}

TEST( TextureSlotRoundTrip, AnUnsetSlotIsEmptyAndStaysUnset )
{
    // 0 is a MEANINGFUL value — "no texture" — so it must survive the trip as itself, and must not be
    // reported as a failure. This is the case that stops the logging below from becoming noise on every
    // empty slot of every scene.
    ProjectRootGuard guard;
    AssetManager     manager;

    LogCapture log;
    EXPECT_EQ( TextureSlotToPath( 0 ), "" );
    EXPECT_EQ( TextureSlotFromPath( manager, "" ), 0u );
    EXPECT_EQ( log.Text(), "" ) << "an empty slot logged something; every scene has dozens of them";
}

// ---------------------------------------------------------------------------------------------------
// A MISS SPEAKS (DC §1.4).
// ---------------------------------------------------------------------------------------------------

TEST( TextureSlotRoundTrip, ANameThatResolvesToNothingSaysSoWithTheNameAndTheRoots )
{
    ProjectRootGuard guard;
    std::filesystem::remove_all( ScratchRoot() );

    const Checkout ann = MakeCheckout( "ann", "Content" );
    Open( ann );

    AssetManager manager;

    std::string text;
    uint64_t    resolved = 1;
    {
        LogCapture log;
        resolved = TextureSlotFromPath( manager, "cooked:Textures/NotThere.tex" );
        text     = log.Text();
    }

    EXPECT_EQ( resolved, 0u );
    EXPECT_NE( text.find( "cooked:Textures/NotThere.tex" ), std::string::npos )
         << "the failure did not name the reference that failed. Three spellings of a real texture were "
            "reported as not resolving and none of them said why, because this branch returned 0 in "
            "silence; a bare 0 is indistinguishable from an empty slot.\nlogged: "
         << text;
    EXPECT_NE( text.find( "Textures" ), std::string::npos ) << text;
}

TEST( TextureSlotRoundTrip, AHandleWithNoRegisteredTextureSaysSoRatherThanWritingAnEmptySlot )
{
    // The other direction of the same rule, and the more damaging one: writing "" for a handle that IS
    // set destroys the reference in the file, so the next load has nothing to fail on.
    ProjectRootGuard guard;
    AssetManager     manager;

    std::string text;
    std::string stored = "unset";
    {
        LogCapture log;
        stored = TextureSlotToPath( kProbeHandle );
        text   = log.Text();
    }

    EXPECT_EQ( stored, "" );
    EXPECT_NE( text.find( std::to_string( kProbeHandle ) ), std::string::npos )
         << "the slot was written out empty without a word about the handle it lost.\nlogged: " << text;
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
