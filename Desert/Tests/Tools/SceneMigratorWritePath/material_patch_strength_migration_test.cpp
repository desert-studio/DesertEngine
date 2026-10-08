// FIELD-GRAIN-b: MATL 4 -> 5. The cloud material's PatchStrength stopped being the slope of the ramp
// W = saturate(1 - 2 s u) and became the clear share of the sky (W = 1 elsewhere). The step keeps the clear
// share: s -> max(0, 1 - 1/(2s)); everything else and the GUID are kept, and what it writes is what the engine
// reads.

#include <SceneMigration.hpp>

#include <Engine/Assets/MaterialFormat.hpp>
#include <Engine/Assets/TextAssetHeaderStamp.hpp>

#include <gtest/gtest.h>

#include <regex>
#include <string>

namespace
{
    constexpr const char* kGuid = "f748ea218bfda9055083bc5a0cb94d64";

    Desert::Assets::MaterialData CloudMaterial( float patchStrength )
    {
        Desert::Assets::MaterialData material;
        material.Header        = Common::Content::TextAssetHeaderSerialized{};
        material.Header->Guid  = kGuid;
        material.Params.push_back( { "Coverage", glm::vec4( 0.762f, 0.0f, 0.0f, 0.0f ) } );
        material.Params.push_back( { "PatchStrength", glm::vec4( patchStrength, 0.0f, 0.0f, 0.0f ) } );
        return material;
    }

    // A MATL 4 file: the current writer's text with the generation stated as 4.
    std::string AsV4( const Desert::Assets::MaterialData& material )
    {
        const auto text = Desert::Assets::WriteMaterialJson( material );
        EXPECT_TRUE( text ) << text.GetError();
        return std::regex_replace( text.GetValue(), std::regex( R"("MATL"\s*:\s*5)" ), "\"MATL\": 4" );
    }

    float StatedPatchStrength( const Desert::Assets::MaterialData& material )
    {
        for ( const auto& p : material.Params )
            if ( p.Name == "PatchStrength" )
                return p.Value.x;
        return -1.0f;
    }
} // namespace

// MUTATION: return s unchanged from MigratePatchStrengthV4ToV5 and the 0.6 row goes red.
TEST( MaterialPatchStrengthMigration, TheClearShareOfTheSkyIsKept )
{
    EXPECT_FLOAT_EQ( Desert::Migration::MigratePatchStrengthV4ToV5( 0.0f ), 0.0f );
    EXPECT_FLOAT_EQ( Desert::Migration::MigratePatchStrengthV4ToV5( 0.5f ), 0.0f );
    EXPECT_NEAR( Desert::Migration::MigratePatchStrengthV4ToV5( 0.6f ), 1.0f / 6.0f, 1e-6f );
    EXPECT_NEAR( Desert::Migration::MigratePatchStrengthV4ToV5( 0.7f ), 1.0f - 1.0f / 1.4f, 1e-6f );
    EXPECT_FLOAT_EQ( Desert::Migration::MigratePatchStrengthV4ToV5( 1.0f ), 0.5f );
}

TEST( MaterialPatchStrengthMigration, AVersionFourFileIsRaisedToWhatTheEngineReads )
{
    const std::string v4 = AsV4( CloudMaterial( 0.6f ) );
    EXPECT_FALSE( Desert::Assets::ParseMaterialJson( "v4", v4 ) ) << "the engine still reads MATL 4";

    const auto raised = Desert::Migration::MigrateMaterialV4ToV5( v4 );
    ASSERT_TRUE( raised ) << raised.GetError();
    const auto read = Desert::Assets::ParseMaterialJson( "v5", raised.GetValue() );
    ASSERT_TRUE( read ) << read.GetError();

    EXPECT_EQ( read.GetValue().Header->Guid, kGuid ) << "the raise minted a new identity";
    EXPECT_NEAR( StatedPatchStrength( read.GetValue() ), 1.0f / 6.0f, 1e-6f );
    EXPECT_FLOAT_EQ( read.GetValue().Params[0].Value.x, 0.762f ) << "a parameter that did not change moved";

    // A second raise refuses: the step raises MATL 4 only, so a migrated file is never raised twice.
    EXPECT_FALSE( Desert::Migration::MigrateMaterialV4ToV5( raised.GetValue() ) );
}
