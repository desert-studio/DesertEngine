// The route from an asset handle to the editor window that opens it (Editor/Core/AssetOpen.hpp).
#include <Editor/Core/AssetOpen.hpp>

#include <gtest/gtest.h>

#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>

using namespace Desert;
using namespace Desert::Editor;
using namespace Desert::Editor::Core;

namespace
{
    constexpr auto kTexture = static_cast<uint32_t>( Assets::AssetTypeID::Texture2D );

    // A registry with an editor for textures only: enough to tell "has an editor" from "has none".
    SubjectEditorRegistry TexturesOnly()
    {
        SubjectEditorRegistry registry;
        registry.Register( AssetSubjectType( kTexture ),
                           SubjectEditorRegistry::Registration{ "Texture2D", "T",
                                                                []( const SubjectId& ) { return nullptr; },
                                                                []( const SubjectId& ) { return true; } } );
        return registry;
    }

    Assets::AssetMetadata Metadata( const uint64_t handle, const Assets::AssetTypeID type )
    {
        Assets::AssetMetadata metadata;
        metadata.Handle    = Common::UUID( handle );
        metadata.Filepath  = "Textures/T_Checker.detex";
        metadata.AssetType = type;
        return metadata;
    }

    // The queue is process-wide; every test starts from an empty one.
    class AssetOpenRoute : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            (void)SubjectOpenRequests::Drain();
        }
    };
} // namespace

TEST_F( AssetOpenRoute, AKnownTextureBecomesItsAssetSubject )
{
    const auto registry = TexturesOnly();
    const auto metadata = Metadata( 0x1234, Assets::AssetTypeID::Texture2D );

    const auto subject = AssetSubjectFor( &metadata, metadata.Handle, registry );
    if ( !subject.IsSuccess() )
    {
        ADD_FAILURE() << subject.GetError();
        return;
    }
    EXPECT_EQ( subject.GetValue(), AssetSubject( Common::UUID( 0x1234 ), kTexture ) );
    EXPECT_FALSE( SubjectOpenRequests::HasPending() ) << "asking for the subject must not open anything";
}

TEST_F( AssetOpenRoute, AnUnknownHandleIsRefusedWithItsNumber )
{
    const auto registry = TexturesOnly();

    const auto subject = RequestOpenAsset( nullptr, Common::UUID( 0xabcdef ), registry );
    ASSERT_FALSE( subject.IsSuccess() );
    EXPECT_NE( subject.GetError().find( "0000000000abcdef" ), std::string::npos ) << subject.GetError();
    EXPECT_FALSE( SubjectOpenRequests::HasPending() );
}

TEST_F( AssetOpenRoute, ATypeWithNoEditorIsRefusedWithTheTypeNumber )
{
    const auto registry = TexturesOnly();
    const auto metadata = Metadata( 0x77, Assets::AssetTypeID::Material );

    const auto subject = RequestOpenAsset( &metadata, metadata.Handle, registry );
    ASSERT_FALSE( subject.IsSuccess() );
    const std::string number = std::to_string( static_cast<uint32_t>( Assets::AssetTypeID::Material ) );
    EXPECT_NE( subject.GetError().find( "AssetTypeID " + number ), std::string::npos ) << subject.GetError();
    EXPECT_FALSE( SubjectOpenRequests::HasPending() );
}

TEST_F( AssetOpenRoute, AnUntypedAssetIsRefused )
{
    const auto registry = TexturesOnly();
    const auto metadata = Metadata( 0x78, Assets::AssetTypeID::Unknown );

    EXPECT_FALSE( RequestOpenAsset( &metadata, metadata.Handle, registry ).IsSuccess() );
    EXPECT_FALSE( SubjectOpenRequests::HasPending() );
}

TEST_F( AssetOpenRoute, TheSameAssetTwiceInOneFrameIsOneRequest )
{
    const auto registry = TexturesOnly();
    const auto metadata = Metadata( 0x1234, Assets::AssetTypeID::Texture2D );

    EXPECT_TRUE( RequestOpenAsset( &metadata, metadata.Handle, registry ).IsSuccess() );
    EXPECT_TRUE( RequestOpenAsset( &metadata, metadata.Handle, registry ).IsSuccess() );

    const auto drained = SubjectOpenRequests::Drain();
    ASSERT_EQ( drained.size(), 1u );
    EXPECT_EQ( drained.front(), AssetSubject( Common::UUID( 0x1234 ), kTexture ) );
}

TEST_F( AssetOpenRoute, TwoDifferentAssetsAreTwoRequests )
{
    const auto registry = TexturesOnly();
    const auto first    = Metadata( 0x1, Assets::AssetTypeID::Texture2D );
    const auto second   = Metadata( 0x2, Assets::AssetTypeID::Texture2D );

    EXPECT_TRUE( RequestOpenAsset( &first, first.Handle, registry ).IsSuccess() );
    EXPECT_TRUE( RequestOpenAsset( &second, second.Handle, registry ).IsSuccess() );
    EXPECT_EQ( SubjectOpenRequests::Drain().size(), 2u );
}

// THE OPEN REGISTER COVERS EVERY TYPE. Each AssetTypeID either becomes its asset subject or is refused with
// its name, its number and the register's reason — never the generic "nothing registered" of a type nobody
// decided about. The registry holds an editor for exactly the types the register says open.
TEST_F( AssetOpenRoute, EveryAssetTypeEitherOpensOrIsRefusedByName )
{
    SubjectEditorRegistry registry;
    for ( uint32_t t = 0; t < static_cast<uint32_t>( Assets::AssetTypeID::Count ); ++t )
        if ( AssetOpenRefusal( static_cast<Assets::AssetTypeID>( t ) ) == nullptr )
            registry.Register( AssetSubjectType( t ),
                               SubjectEditorRegistry::Registration{ "Any", "A",
                                                                    []( const SubjectId& ) { return nullptr; },
                                                                    []( const SubjectId& ) { return true; } } );

    size_t opens = 0;
    for ( uint32_t t = 0; t <= static_cast<uint32_t>( Assets::AssetTypeID::Count ); ++t )
    {
        const auto  type     = static_cast<Assets::AssetTypeID>( t );
        const auto  metadata = Metadata( 0x100 + t, type );
        const char* refusal  = AssetOpenRefusal( type );
        const auto  subject  = AssetSubjectFor( &metadata, metadata.Handle, registry );
        if ( refusal == nullptr )
        {
            ASSERT_TRUE( subject.IsSuccess() ) << Assets::AssetTypeName( type ) << ": " << subject.GetError();
            EXPECT_EQ( subject.GetValue(), AssetSubject( metadata.Handle, t ) );
            ++opens;
            continue;
        }
        ASSERT_FALSE( subject.IsSuccess() ) << Assets::AssetTypeName( type );
        const std::string& error = subject.GetError();
        EXPECT_NE( error.find( Assets::AssetTypeName( type ) ), std::string::npos ) << error;
        EXPECT_NE( error.find( "AssetTypeID " + std::to_string( t ) ), std::string::npos ) << error;
        EXPECT_NE( error.find( refusal ), std::string::npos ) << error;
    }
    EXPECT_GT( opens, 0u );
    EXPECT_FALSE( SubjectOpenRequests::HasPending() );
}

namespace
{
    std::string ReadRepoFile( const char* relative )
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up, prefix += "../" )
        {
            const std::ifstream in( prefix + relative );
            if ( !in )
                continue;
            std::ostringstream text;
            text << in.rdbuf();
            return text.str();
        }
        return {};
    }
} // namespace

// THE REGISTER AND THE EDITORS ARE ONE LIST. EditorLayer.cpp (compiled by no suite) registers the asset
// editors; the types it registers must be exactly the ones AssetOpenRefusal lets through, or an Open is
// either refused for a type that has a window or promised a window that no registration builds.
TEST( AssetOpenRegister, MatchesTheAssetEditorsEditorLayerRegisters )
{
    const std::string layer = ReadRepoFile( "Editor/Source/EditorLayer.cpp" );
    ASSERT_FALSE( layer.empty() ) << "Editor/Source/EditorLayer.cpp not found from the working directory";

    std::set<std::string> registered;
    const std::regex      registration(
         R"(AssetSubjectType\(\s*static_cast<uint32_t>\(\s*Assets::AssetTypeID::(\w+)\s*\)\s*\))" );
    for ( auto it = std::sregex_iterator( layer.begin(), layer.end(), registration ); it != std::sregex_iterator();
          ++it )
        registered.insert( ( *it )[1].str() );

    std::set<std::string> opens;
    for ( uint32_t t = 0; t < static_cast<uint32_t>( Assets::AssetTypeID::Count ); ++t )
        if ( AssetOpenRefusal( static_cast<Assets::AssetTypeID>( t ) ) == nullptr )
            opens.insert( Assets::AssetTypeName( static_cast<Assets::AssetTypeID>( t ) ) );

    EXPECT_FALSE( registered.empty() );
    EXPECT_EQ( registered, opens );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
