// AF10b: the redirector file (UE's UObjectRedirector pattern) and the registry's walk through it.
//
// A redirector is a DAST header with kind Redirector, its own GUID, ONE dependency (the target) and a Meta
// section naming the stable key it was written for; no body. The registry scan records it as a row at the
// old path, and FindByReference / FindByGuidReference follow it to the moved asset.

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Content/AssetRedirector.hpp>
#include <Common/Content/ContentKinds.hpp>
#include <Common/Content/ContentScan.hpp>
#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Utilities/AssetRegistry.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;
using Common::Content::AssetGuid;
using Common::Content::AssetRedirector;
using Common::Content::ContentKind;

namespace
{
    AssetGuid Guid( uint64_t n )
    {
        return AssetGuid{ 0xAF10B00000000000ull | n, n };
    }

    // A fresh project root with scenes under it; restores the previous root when done.
    struct Project
    {
        fs::path Root;
        fs::path SavedProject = Common::Constants::Path::CurrentProjectRoot().ProjectDir;

        explicit Project( const std::string& name )
        {
            Root = fs::temp_directory_path() / name;
            fs::remove_all( Root );
            fs::create_directories( Root );
            Root = fs::canonical( Root );
            Common::Constants::Path::SetProjectRoot( Root, "Resources/Assets" );
        }
        ~Project()
        {
            Common::Constants::Path::SetProjectRoot( SavedProject, "Resources/Assets" );
            fs::remove_all( Root );
        }
        Project( const Project& )            = delete;
        Project& operator=( const Project& ) = delete;
        Project( Project&& )                 = delete;
        Project& operator=( Project&& )      = delete;

        [[nodiscard]] fs::path Scene( const std::string& name ) const
        {
            const fs::path root = Common::Constants::Path::SCENE_PATH.is_absolute()
                                       ? Common::Constants::Path::SCENE_PATH
                                       : Root / Common::Constants::Path::SCENE_PATH;
            fs::create_directories( root );
            return root / ( name + std::string( Common::Content::KindSpec( ContentKind::Scene ).Extension ) );
        }
    };

    std::string Key( const fs::path& file )
    {
        return Common::AssetHandle::StableKeyForPath( file );
    }

    // A header-bearing scene: the text header is what gives it a GUID; the body is irrelevant here.
    void WriteScene( const fs::path& file, const AssetGuid& guid )
    {
        Common::Content::AssetEnvelope envelope;
        envelope.Asset.Kind = ContentKind::Scene;
        envelope.Asset.Guid = guid;
        ASSERT_TRUE( Common::Content::WriteAssetEnvelopeFile( file, envelope ) );
    }

    void WriteRedirector( const fs::path& file, const AssetGuid& self, const AssetGuid& target )
    {
        ASSERT_TRUE( Common::Content::WriteRedirectorFile( file, AssetRedirector{ self, target, Key( file ) } ) );
    }

    void Scan( Common::Utils::AssetRegistry& registry, const fs::path& file )
    {
        auto row = Common::Content::RegistryRowFor(
             Key( file ), Common::Content::DescribeContentFile( file, ContentKind::Scene ) );
        if ( !row )
        {
            ADD_FAILURE() << row.GetError();
            return;
        }
        ASSERT_TRUE( registry.Insert( row.GetValue() ) );
    }
} // namespace

TEST( AssetRedirector, RoundTripsAndStatesItsKindWithoutABody )
{
    const AssetRedirector written{ Guid( 1 ), Guid( 2 ), "scenes:Old.desce" };
    auto                  bytes = Common::Content::EncodeRedirector( written );
    ASSERT_TRUE( bytes ) << bytes.GetError();
    auto read = Common::Content::DecodeRedirector( bytes.GetValue() );
    ASSERT_TRUE( read ) << read.GetError();
    EXPECT_EQ( read.GetValue(), written );

    const Project  project( "AF10b_Codec" );
    const fs::path file = project.Scene( "Old" );
    WriteRedirector( file, Guid( 1 ), Guid( 2 ) );
    auto header = Common::Content::ReadAssetHeader( file, Common::Content::AssetHeaderReadContext{ {}, true } );
    ASSERT_TRUE( header ) << header.GetError();
    EXPECT_EQ( header.GetValue().Kind, ContentKind::Redirector );
    EXPECT_EQ( header.GetValue().Guid, Guid( 1 ) );
    ASSERT_EQ( header.GetValue().Dependencies.size(), 1u );
    EXPECT_EQ( header.GetValue().Dependencies.front(), Guid( 2 ) );
}

TEST( AssetRedirector, RefusesWhatIsNotARedirector )
{
    EXPECT_FALSE( Common::Content::EncodeRedirector( { Guid( 1 ), AssetGuid{}, "k" } ) ) << "null target";
    EXPECT_FALSE( Common::Content::EncodeRedirector( { Guid( 1 ), Guid( 1 ), "k" } ) ) << "names itself";
    EXPECT_FALSE( Common::Content::EncodeRedirector( { Guid( 1 ), Guid( 2 ), "" } ) ) << "no old key";

    auto bytes = Common::Content::EncodeRedirector( { Guid( 1 ), Guid( 2 ), "k" } );
    ASSERT_TRUE( bytes );
    auto envelope = Common::Content::ReadAssetEnvelope( bytes.GetValue(), { {}, true } );
    ASSERT_TRUE( envelope );

    auto withBody = envelope.GetValue();
    withBody.Sections.push_back( { Common::Content::EnvelopeSection::Payload,
                                   Common::Content::EnvelopeCodec::Stored, std::vector<std::byte>( 4 ) } );
    auto twoTargets = envelope.GetValue();
    twoTargets.Asset.Dependencies.push_back( Guid( 3 ) );
    auto otherKind       = envelope.GetValue();
    otherKind.Asset.Kind = ContentKind::Scene;
    for ( const auto& bad : { withBody, twoTargets, otherKind } )
    {
        auto written = Common::Content::WriteAssetEnvelope( bad );
        ASSERT_TRUE( written );
        EXPECT_FALSE( Common::Content::DecodeRedirector( written.GetValue() ) );
    }
}

TEST( AssetRedirector, AMovedSceneIsFoundThroughTheRedirectorAtItsOldPath )
{
    const Project  project( "AF10b_Move" );
    const fs::path oldPath = project.Scene( "Before" );
    const fs::path newPath = project.Scene( "After" );
    WriteScene( newPath, Guid( 7 ) );
    WriteRedirector( oldPath, Guid( 8 ), Guid( 7 ) );

    Common::Utils::AssetRegistry registry;
    Scan( registry, newPath );
    Scan( registry, oldPath );
    const auto* redirector = registry.FindByKey( Key( oldPath ) );
    ASSERT_NE( redirector, nullptr );
    EXPECT_EQ( redirector->Kind, "Redirector" );

    const auto* found = registry.FindByReference( 0, oldPath.string() );
    ASSERT_NE( found, nullptr );
    EXPECT_EQ( found->Key, Key( newPath ) );
    const auto* byGuidHint = registry.FindByGuidReference( AssetGuid{}, oldPath.string() );
    ASSERT_NE( byGuidHint, nullptr );
    EXPECT_EQ( byGuidHint->Key, Key( newPath ) );
}

// A loader that opens the OLD path by file (the editor's scene open, a string table's LoadFromFile) reads the
// redirector's bytes; it must refuse them by name and point at the moved asset, never parse them as text.
TEST( AssetRedirector, OpeningTheOldPathByFileIsRefusedByNameAndPointsAtTheMovedAsset )
{
    const Project  project( "AF10b_OpenByFile" );
    const fs::path oldPath = project.Scene( "Before" );
    const fs::path newPath = project.Scene( "After" );
    WriteScene( newPath, Guid( 7 ) );
    WriteRedirector( oldPath, Guid( 8 ), Guid( 7 ) );
    Common::Utils::AssetRegistry registry;
    Scan( registry, newPath );
    Scan( registry, oldPath );

    std::ifstream     in( oldPath, std::ios::binary );
    const std::string bytes( ( std::istreambuf_iterator<char>( in ) ), std::istreambuf_iterator<char>() );

    const auto keyOf = [&registry]( const AssetGuid& target ) -> std::string
    {
        const auto* row = registry.FindByHandle( Common::Content::HandleForGuid( target ) );
        return row != nullptr ? row->Key : std::string();
    };
    const auto named = Common::Content::RefuseRedirectorBytes( oldPath.string(), bytes, keyOf );
    ASSERT_FALSE( named );
    EXPECT_NE( named.GetError().find( "is a redirector" ), std::string::npos ) << named.GetError();
    EXPECT_NE( named.GetError().find( Key( newPath ) ), std::string::npos ) << named.GetError();

    // Without a registry that knows the target, the GUID is the name.
    const auto unresolved = Common::Content::RefuseRedirectorBytes( oldPath.string(), bytes );
    ASSERT_FALSE( unresolved );
    EXPECT_NE( unresolved.GetError().find( Common::Content::AssetGuidToText( Guid( 7 ) ) ), std::string::npos )
         << unresolved.GetError();

    // Text and other DAST files are the loader's own parser's business.
    EXPECT_TRUE( Common::Content::RefuseRedirectorBytes( "t", "{ \"Header\": {} }", keyOf ) );
    std::ifstream     scene( newPath, std::ios::binary );
    const std::string sceneBytes( ( std::istreambuf_iterator<char>( scene ) ), std::istreambuf_iterator<char>() );
    EXPECT_TRUE( Common::Content::RefuseRedirectorBytes( newPath.string(), sceneBytes, keyOf ) );
}

TEST( AssetRedirector, AChainResolvesACycleAndAMissingTargetAreRefusedByName )
{
    const Project  project( "AF10b_Chain" );
    const fs::path a = project.Scene( "A" );
    const fs::path b = project.Scene( "B" );
    const fs::path c = project.Scene( "C" );
    WriteRedirector( a, Guid( 1 ), Guid( 2 ) );
    WriteRedirector( b, Guid( 2 ), Guid( 3 ) );
    WriteScene( c, Guid( 3 ) );
    {
        Common::Utils::AssetRegistry registry;
        Scan( registry, a );
        Scan( registry, b );
        Scan( registry, c );
        const auto* found = registry.FindByReference( 0, a.string() );
        ASSERT_NE( found, nullptr );
        EXPECT_EQ( found->Key, Key( c ) );
    }

    WriteRedirector( b, Guid( 2 ), Guid( 1 ) ); // B now points back at A
    {
        Common::Utils::AssetRegistry registry;
        Scan( registry, a );
        Scan( registry, b );
        const auto* row = registry.FindByKey( Key( a ) );
        ASSERT_NE( row, nullptr );
        auto followed = registry.FollowRedirectors( *row );
        if ( followed )
        {
            ADD_FAILURE() << "a cycle resolved to " << followed.GetValue()->Key;
            return;
        }
        EXPECT_NE( followed.GetError().find( "cycle" ), std::string::npos ) << followed.GetError();
        EXPECT_NE( followed.GetError().find( Key( a ) ), std::string::npos ) << followed.GetError();
        EXPECT_NE( followed.GetError().find( Key( b ) ), std::string::npos ) << followed.GetError();
        EXPECT_EQ( registry.FindByReference( 0, a.string() ), nullptr );
    }
    {
        Common::Utils::AssetRegistry registry;
        Scan( registry, a ); // names Guid( 2 ), and no row states it
        auto followed = registry.FollowRedirectors( *registry.FindByKey( Key( a ) ) );
        if ( followed )
        {
            ADD_FAILURE() << "a dangling redirector resolved";
            return;
        }
        EXPECT_NE( followed.GetError().find( Key( a ) ), std::string::npos ) << followed.GetError();
    }
}

TEST( AssetRedirector, ARedirectorFoundAwayFromItsOldKeyIsRefused )
{
    const Project  project( "AF10b_Moved" );
    const fs::path at    = project.Scene( "Here" );
    const fs::path other = project.Scene( "There" );
    ASSERT_TRUE(
         Common::Content::WriteRedirectorFile( at, AssetRedirector{ Guid( 1 ), Guid( 2 ), Key( other ) } ) );
    auto row = Common::Content::RegistryRowFor( Key( at ),
                                                Common::Content::DescribeContentFile( at, ContentKind::Scene ) );
    EXPECT_FALSE( row ) << "a redirector answered for a path it was not written for";
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
