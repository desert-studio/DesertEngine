#include <gtest/gtest.h>

#include <MaterialAOParam.hpp>

#include <Engine/Assets/MaterialFormat.hpp>

#include <string>

// MAT-AO-ONEHOME: a `.demat` stating the retired OcclusionStrength states the one Ambient Occlusion input under
// the old name; the migrator renames it, refuses a material that states both, and leaves every other material
// alone.
namespace
{
    std::string MaterialText( std::initializer_list<std::pair<const char*, float>> params )
    {
        Desert::Assets::MaterialData material;
        material.Params.push_back( { "RoughnessFactor", { 0.5f, 0, 0, 0 } } );
        for ( const auto& [name, value] : params )
            material.Params.push_back( { name, { value, 0, 0, 0 } } );
        const auto text = Desert::Assets::WriteMaterialJson( material );
        EXPECT_TRUE( text.IsSuccess() ) << ( text.IsSuccess() ? "" : text.GetError() );
        return text.IsSuccess() ? text.GetValue() : std::string{};
    }
} // namespace

TEST( MaterialAOParam, TheRetiredOcclusionStrengthIsRenamedAndKeepsItsValue )
{
    const std::string before = MaterialText( { { "OcclusionStrength", 0.5f } } );
    const auto        raised = Desert::Migration::MaterialWithOneAOParam( "M_Old.demat", before );
    ASSERT_TRUE( raised.IsSuccess() ) << raised.GetError();
    ASSERT_TRUE( raised.GetValue().has_value() );

    const auto read = Desert::Assets::ParseMaterialJson( "M_Old.demat", *raised.GetValue() );
    ASSERT_TRUE( read.IsSuccess() ) << read.GetError();
    const auto& params = read.GetValue().Params;
    ASSERT_EQ( params.size(), 2u );
    EXPECT_EQ( params[1].Name, "AOStrength" );
    EXPECT_FLOAT_EQ( params[1].Value.x, 0.5f );

    // A second run finds nothing to rename: the step is idempotent.
    const auto again = Desert::Migration::MaterialWithOneAOParam( "M_Old.demat", *raised.GetValue() );
    ASSERT_TRUE( again.IsSuccess() ) << again.GetError();
    EXPECT_FALSE( again.GetValue().has_value() );
}

TEST( MaterialAOParam, AMaterialStatingBothIsRefusedByPath )
{
    const auto both = Desert::Migration::MaterialWithOneAOParam(
         "Content/M_Both.demat", MaterialText( { { "AOStrength", 1.0f }, { "OcclusionStrength", 0.5f } } ) );
    ASSERT_FALSE( both.IsSuccess() );
    EXPECT_NE( both.GetError().find( "Content/M_Both.demat" ), std::string::npos ) << both.GetError();
}

TEST( MaterialAOParam, AMaterialWithOnlyAOStrengthIsLeftAlone )
{
    const auto current =
         Desert::Migration::MaterialWithOneAOParam( "M_New.demat", MaterialText( { { "AOStrength", 1.0f } } ) );
    ASSERT_TRUE( current.IsSuccess() ) << current.GetError();
    EXPECT_FALSE( current.GetValue().has_value() );
}
