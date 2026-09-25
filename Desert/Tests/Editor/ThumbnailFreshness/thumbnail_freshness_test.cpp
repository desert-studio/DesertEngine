// ThumbnailFreshness — is a cached PNG the picture to SHOW or the one to REPLACE? (TH1)
//
// The decision is keyed on the source's BYTES, recorded beside the PNG when it is written. These tests
// pin the relation the owner's defect broke: the same bytes must give the same answer however the file's
// clock, path or load order moved, and an edit must give "capture" exactly once.

#include <Editor/Widgets/ThumbnailFreshness.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

using namespace Desert::Editor;
namespace fs = std::filesystem;

namespace
{
    struct TempDir
    {
        fs::path Root;
        TempDir()
        {
            Root = fs::temp_directory_path() /
                   ( "th1_freshness_" + std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) );
            fs::create_directories( Root );
        }
        ~TempDir()
        {
            std::error_code ec;
            fs::remove_all( Root, ec );
        }
    };

    void WriteFile( const fs::path& path, const std::string& bytes )
    {
        std::ofstream out( path, std::ios::binary | std::ios::trunc );
        out << bytes;
    }

    ThumbnailFreshness::Verdict Verdict( const fs::path& png, const fs::path& source )
    {
        return ThumbnailFreshness::Judge( ThumbnailFreshness::Observe( png, source ) );
    }

    // What a capture does: PNG written, then the source hash taken at dispatch recorded beside it.
    void Capture( const fs::path& png, const fs::path& source )
    {
        const auto hash = ThumbnailFreshness::ContentHash( source );
        ASSERT_TRUE( hash.has_value() );
        WriteFile( png, "png-bytes" );
        ASSERT_TRUE( ThumbnailFreshness::Record( png, *hash ).IsSuccess() );
    }
} // namespace

TEST( ThumbnailFreshness, EveryObservationHasExactlyOneVerdict )
{
    using V = ThumbnailFreshness::Verdict;
    for ( const bool exists : { false, true } )
        for ( const std::optional<uint64_t> recorded : { std::optional<uint64_t>{}, std::optional<uint64_t>{ 1 } } )
            for ( const std::optional<uint64_t> current :
                  { std::optional<uint64_t>{}, std::optional<uint64_t>{ 1 }, std::optional<uint64_t>{ 2 } } )
            {
                const ThumbnailFreshness::Observation seen{ exists, recorded, current };
                const V                               v = ThumbnailFreshness::Judge( seen );
                EXPECT_TRUE( v == V::Show || v == V::Capture );
                if ( !exists )
                    EXPECT_EQ( v, V::Capture );
            }
}

TEST( ThumbnailFreshness, SameBytesAreShown_WhateverTheClockSays )
{
    // The owner's defect: every launch re-captured unchanged materials. A checkout, a pull or a launcher
    // update moves the source's modtime past the PNG's without touching its bytes.
    TempDir         dir;
    const fs::path  source = dir.Root / "M_Red.demat";
    const fs::path  png    = dir.Root / "assets_Materials_M_Red.png";
    WriteFile( source, "{ \"AlbedoColor\": [1, 0, 0, 1] }" );
    Capture( png, source );
    EXPECT_EQ( Verdict( png, source ), ThumbnailFreshness::Verdict::Show );

    fs::last_write_time( source, fs::last_write_time( png ) + std::chrono::hours( 1 ) );
    EXPECT_EQ( Verdict( png, source ), ThumbnailFreshness::Verdict::Show ) << "a newer modtime alone re-captured";

    WriteFile( source, "{ \"AlbedoColor\": [1, 0, 0, 1] }" ); // re-saved unchanged
    EXPECT_EQ( Verdict( png, source ), ThumbnailFreshness::Verdict::Show ) << "an unchanged re-save re-captured";
}

TEST( ThumbnailFreshness, TheKeyIsTheBytes_NotThePath )
{
    TempDir        dir;
    const fs::path a = dir.Root / "one" / "M.demat";
    const fs::path b = dir.Root / "two" / "M_moved.demat";
    fs::create_directories( a.parent_path() );
    fs::create_directories( b.parent_path() );
    WriteFile( a, "same bytes" );
    WriteFile( b, "same bytes" );
    EXPECT_EQ( ThumbnailFreshness::ContentHash( a ), ThumbnailFreshness::ContentHash( b ) );
    WriteFile( b, "other bytes" );
    EXPECT_NE( ThumbnailFreshness::ContentHash( a ), ThumbnailFreshness::ContentHash( b ) );
}

TEST( ThumbnailFreshness, AnEditIsCapturedExactlyOnce )
{
    TempDir        dir;
    const fs::path source = dir.Root / "M.demat";
    const fs::path png    = dir.Root / "M.png";
    WriteFile( source, "{ \"Roughness\": 0.5 }" );
    Capture( png, source );
    ASSERT_EQ( Verdict( png, source ), ThumbnailFreshness::Verdict::Show );

    // Same size, and the modtime may well round to the same tick: the memo must still see the new bytes.
    WriteFile( source, "{ \"Roughness\": 0.9 }" );
    fs::last_write_time( source, fs::last_write_time( source ) + std::chrono::seconds( 2 ) );
    EXPECT_EQ( Verdict( png, source ), ThumbnailFreshness::Verdict::Capture ) << "an edited material kept its old picture";

    Capture( png, source );
    EXPECT_EQ( Verdict( png, source ), ThumbnailFreshness::Verdict::Show ) << "the re-capture did not settle";
}

TEST( ThumbnailFreshness, APictureWithoutARecordIsCapturedOnce )
{
    // A PNG from before TH1, or a writer that died between the PNG and the record.
    TempDir        dir;
    const fs::path source = dir.Root / "M.demat";
    const fs::path png    = dir.Root / "M.png";
    WriteFile( source, "x" );
    WriteFile( png, "png-bytes" );
    EXPECT_EQ( Verdict( png, source ), ThumbnailFreshness::Verdict::Capture );
    Capture( png, source );
    EXPECT_EQ( Verdict( png, source ), ThumbnailFreshness::Verdict::Show );
}

TEST( ThumbnailFreshness, AMissingThumbnailIsScheduled )
{
    TempDir        dir;
    const fs::path source = dir.Root / "M.demat";
    WriteFile( source, "x" );
    EXPECT_EQ( Verdict( dir.Root / "absent.png", source ), ThumbnailFreshness::Verdict::Capture );
}

TEST( ThumbnailFreshness, AnUnreadableSourceKeepsTheExistingPicture )
{
    TempDir        dir;
    const fs::path png = dir.Root / "M.png";
    WriteFile( png, "png-bytes" );
    EXPECT_EQ( Verdict( png, dir.Root / "gone.demat" ), ThumbnailFreshness::Verdict::Show );
}

// THE OWNER'S COMPLAINT (2026-09-25): "a flat colour, not a sphere, until everything has loaded". While
// a capture is owed, the reader must still draw the picture on disk; the swatch is for "no picture at all".
TEST( ThumbnailFreshness, AnOutdatedPictureIsDrawnWhileItsReplacementIsCaptured )
{
    TempDir        dir;
    const fs::path source = dir.Root / "M.demat";
    const fs::path png    = dir.Root / "M.png";
    WriteFile( source, "before" );
    WriteFile( png, "png-bytes" ); // no record: a v9 picture from before TH1
    const auto seen = ThumbnailFreshness::Observe( png, source );
    EXPECT_EQ( ThumbnailFreshness::Judge( seen ), ThumbnailFreshness::Verdict::Capture );
    EXPECT_EQ( ThumbnailFreshness::Choose( seen ), ThumbnailFreshness::Picture::CachedPng );

    Capture( png, source );
    WriteFile( source, "after" ); // edited: the record no longer matches
    const auto edited = ThumbnailFreshness::Observe( png, source );
    EXPECT_EQ( ThumbnailFreshness::Judge( edited ), ThumbnailFreshness::Verdict::Capture );
    EXPECT_EQ( ThumbnailFreshness::Choose( edited ), ThumbnailFreshness::Picture::CachedPng );
}

TEST( ThumbnailFreshness, ThePlaceholderIsOnlyForNoPictureAtAll )
{
    for ( const bool exists : { false, true } )
        for ( const std::optional<uint64_t> recorded : { std::optional<uint64_t>{}, std::optional<uint64_t>{ 1 } } )
            for ( const std::optional<uint64_t> current :
                  { std::optional<uint64_t>{}, std::optional<uint64_t>{ 1 }, std::optional<uint64_t>{ 2 } } )
            {
                const ThumbnailFreshness::Observation seen{ exists, recorded, current };
                EXPECT_EQ( ThumbnailFreshness::Choose( seen ) == ThumbnailFreshness::Picture::Placeholder, !exists );
                // Every state is drawn as something AND, if it is a placeholder, a capture is owed.
                if ( ThumbnailFreshness::Choose( seen ) == ThumbnailFreshness::Picture::Placeholder )
                    EXPECT_EQ( ThumbnailFreshness::Judge( seen ), ThumbnailFreshness::Verdict::Capture );
            }
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
