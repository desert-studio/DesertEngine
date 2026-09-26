// SceneAssetGuidRoundTrip — the {Guid, Path} rule a scene writes a material's shader and a UIRenderTexture's
// scene through (SCNE 31), pinned on the ONE function both serializer halves call
// (Engine/Assets/AssetRefSerialization.cpp). The halves themselves sit in ComponentRegistry.cpp and
// ShaderAsset.cpp, which link the renderer; this suite links only the rule and Common.
//
// The asset table below stands in for the registry the callers inject: the shader half resolves through the
// asset manager to a NAME, the scene half through the content registry to a PATH, and both write from a row.

#include <Engine/Assets/AssetRefSerialization.hpp>

#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Core/Constants.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

namespace
{
    using Desert::Assets::AssetGuidRef;
    using Desert::Assets::AssetRefSite;
    using Common::Content::AssetGuid;
    namespace fs = std::filesystem;

    struct Row
    {
        AssetGuid   Guid;
        fs::path    RuntimePath; // absolute, as the runtime holds it
        std::string Binding;     // what the component binds: a shader's name, a scene's path
    };

    const AssetRefSite kShaderSite{ "shader", "Material.Shader", "entity 'Crate'" };
    const AssetRefSite kSceneSite{ "scene", "UIRenderTexture.Scene", "entity 'Minimap'" };

    fs::path AbsoluteAsset( const char* relative )
    {
        return fs::absolute( Common::Constants::Path::ASSETS_PATH / relative ).lexically_normal();
    }

    std::vector<Row> Table()
    {
        return {
             Row{ AssetGuid{ 0x1111222233334444ull, 0x5555666677778888ull }, AbsoluteAsset( "Shaders/Rock.shader" ),
                  "Rock" },
             Row{ AssetGuid{ 0xaaaabbbbccccddddull, 0xeeeeffff00001111ull }, AbsoluteAsset( "Scenes/Menu.desce" ),
                  AbsoluteAsset( "Scenes/Menu.desce" ).generic_string() },
        };
    }

    Desert::Assets::AssetGuidResolver ResolverOver( const std::vector<Row>& rows )
    {
        return [rows]( const AssetGuid& guid ) -> std::optional<std::string>
        {
            for ( const Row& row : rows )
                if ( row.Guid == guid )
                    return row.Binding;
            return std::nullopt;
        };
    }

    // Write -> read -> write again: the binding comes back and the second write states the same GUID.
    void ExpectRoundTrip( const Row& row, const AssetRefSite& site )
    {
        const auto written = Desert::Assets::WriteAssetGuidRef( row.Guid, row.RuntimePath, site );
        ASSERT_TRUE( written ) << written.GetError();
        EXPECT_EQ( written.GetValue().Guid, Common::Content::AssetGuidToText( row.Guid ) );

        const auto read = Desert::Assets::ResolveAssetGuidRef( written.GetValue(), ResolverOver( Table() ), site );
        ASSERT_TRUE( read ) << read.GetError();
        EXPECT_EQ( read.GetValue(), row.Binding );

        const auto again = Desert::Assets::WriteAssetGuidRef( row.Guid, row.RuntimePath, site );
        ASSERT_TRUE( again ) << again.GetError();
        EXPECT_EQ( again.GetValue(), written.GetValue() );
    }
} // namespace

TEST( SceneAssetGuidRoundTrip, ShaderRoundTripKeepsItsGuid )
{
    ExpectRoundTrip( Table()[0], kShaderSite );
}

TEST( SceneAssetGuidRoundTrip, SceneRoundTripKeepsItsGuid )
{
    ExpectRoundTrip( Table()[1], kSceneSite );
}

// A GUID nothing carries is refused, and the refusal names the field, the entity and both halves of the
// reference — never a silent empty binding.
TEST( SceneAssetGuidRoundTrip, UnknownGuidIsRefusedNamingTheField )
{
    const AssetGuidRef orphan{ "0123456789abcdef0123456789abcdef", "assets:Scenes/Gone.desce" };
    const auto         read = Desert::Assets::ResolveAssetGuidRef( orphan, ResolverOver( Table() ), kSceneSite );
    ASSERT_FALSE( read );
    EXPECT_NE( read.GetError().find( "UIRenderTexture.Scene" ), std::string::npos ) << read.GetError();
    EXPECT_NE( read.GetError().find( "entity 'Minimap'" ), std::string::npos ) << read.GetError();
    EXPECT_NE( read.GetError().find( orphan.Guid ), std::string::npos ) << read.GetError();
    EXPECT_NE( read.GetError().find( orphan.Path ), std::string::npos ) << read.GetError();
}

TEST( SceneAssetGuidRoundTrip, MissingOrMalformedGuidIsRefusedNamingTheField )
{
    for ( const char* text : { "", "not-a-guid", "00000000000000000000000000000000" } )
    {
        const auto read = Desert::Assets::ResolveAssetGuidRef( AssetGuidRef{ text, "assets:Shaders/Rock.shader" },
                                                               ResolverOver( Table() ), kShaderSite );
        ASSERT_FALSE( read ) << "'" << text << "' resolved to " << read.GetValue();
        EXPECT_NE( read.GetError().find( "Material.Shader on entity 'Crate'" ), std::string::npos )
             << read.GetError();
    }
}

// The path is a hint: a file moved after the reference was written still resolves by its GUID, and the
// stale path never reaches the lookup.
TEST( SceneAssetGuidRoundTrip, StalePathWithValidGuidResolves )
{
    const Row          scene = Table()[1];
    const AssetGuidRef stale{ Common::Content::AssetGuidToText( scene.Guid ), "assets:Old/Renamed.desce" };
    const auto         read = Desert::Assets::ResolveAssetGuidRef( stale, ResolverOver( Table() ), kSceneSite );
    ASSERT_TRUE( read ) << read.GetError();
    EXPECT_EQ( read.GetValue(), scene.Binding );
}

// The runtime holds absolute paths; the file must hold the root-tagged key, which is the same on every
// machine and in a package.
TEST( SceneAssetGuidRoundTrip, AbsoluteRuntimePathSerializesToAssetsKey )
{
    const Row  scene   = Table()[1];
    const auto written = Desert::Assets::WriteAssetGuidRef( scene.Guid, scene.RuntimePath, kSceneSite );
    ASSERT_TRUE( written ) << written.GetError();
    EXPECT_EQ( written.GetValue().Path, "assets:Scenes/Menu.desce" );
}

TEST( SceneAssetGuidRoundTrip, AssetWithNoHeaderGuidIsRefusedNamingTheField )
{
    const auto written = Desert::Assets::WriteAssetGuidRef( AssetGuid{}, AbsoluteAsset( "Shaders/Bare.shader" ),
                                                            kShaderSite );
    ASSERT_FALSE( written ) << written.GetValue().Guid;
    EXPECT_NE( written.GetError().find( "Material.Shader on entity 'Crate'" ), std::string::npos )
         << written.GetError();
    EXPECT_NE( written.GetError().find( "Bare.shader" ), std::string::npos ) << written.GetError();
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
