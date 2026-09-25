// The route from an asset handle to the editor window that opens it (Editor/Core/AssetOpen.hpp).
#include <Editor/Core/AssetOpen.hpp>

#include <gtest/gtest.h>

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

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
