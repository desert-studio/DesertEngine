// NO-PBR: A MATERIAL'S ENGINE SHADER LOCATOR FOLLOWS ITS GUID (ShaderLocatorFollow.hpp). The GUID names the
// shader; the `engine:` path is only where it was. A stale path is rewritten to where the GUID lives now, a
// current one is left alone, a GUID no engine shader states is refused naming both, a non-engine path is not
// this step's.
#include <ShaderLocatorFollow.hpp>

#include <gtest/gtest.h>

#include <map>
#include <string>

namespace
{
    using Desert::Migration::FollowShaderLocator;

    constexpr const char* kGuid = "0123456789abcdef0123456789abcdef";

    std::string Material( const std::string& path )
    {
        return std::string( R"({"Header":{"Versions":{"MATL":4}},"Shader":{"Guid":")" ) + kGuid + R"(","Path":")" +
               path + R"("},"Parameters":{}})";
    }

    const std::map<std::string, std::string> kEngine = {
         { kGuid, "engine:Shaders/Programs/Surface/Surface.dshader" } };
} // namespace

TEST( ShaderLocatorFollow, StaleLocatorIsRewrittenToWhereTheGuidLives )
{
    const auto followed = FollowShaderLocator( Material( "engine:Shaders/Programs/PBR/PBR.dshader" ), kEngine );
    EXPECT_TRUE( followed.Error.empty() ) << followed.Error;
    ASSERT_TRUE( followed.Text.has_value() ) << "a stale engine locator must be rewritten";
    EXPECT_EQ( followed.Text.value_or( "" ), Material( "engine:Shaders/Programs/Surface/Surface.dshader" ) );
}

TEST( ShaderLocatorFollow, CurrentLocatorIsLeftAlone )
{
    const auto followed =
         FollowShaderLocator( Material( "engine:Shaders/Programs/Surface/Surface.dshader" ), kEngine );
    EXPECT_TRUE( followed.Error.empty() ) << followed.Error;
    EXPECT_FALSE( followed.Text.has_value() ) << "a locator already naming its GUID's file is not rewritten";
}

TEST( ShaderLocatorFollow, UnknownGuidIsRefusedNamingGuidAndLocator )
{
    const std::map<std::string, std::string> none;
    const auto followed = FollowShaderLocator( Material( "engine:Shaders/Programs/PBR/PBR.dshader" ), none );
    EXPECT_FALSE( followed.Text.has_value() );
    EXPECT_NE( followed.Error.find( kGuid ), std::string::npos ) << followed.Error;
    EXPECT_NE( followed.Error.find( "engine:Shaders/Programs/PBR/PBR.dshader" ), std::string::npos )
         << followed.Error;
}

TEST( ShaderLocatorFollow, NonEngineLocatorIsNotThisStepsBusiness )
{
    const auto followed = FollowShaderLocator( Material( "Content/Shaders/Mine.dshader" ), kEngine );
    EXPECT_TRUE( followed.Error.empty() ) << followed.Error;
    EXPECT_FALSE( followed.Text.has_value() );
}
