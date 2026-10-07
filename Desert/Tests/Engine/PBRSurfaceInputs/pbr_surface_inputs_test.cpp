// The PBR surface inputs — Editor/Resources/Shaders/Common/PBRSurfaceInputs.glslh — compiled AS C++ and
// held to two properties per parameter:
//
//   * every parameter CHANGES the result when it moves off its identity (a parameter that does not is a
//     dead setting, whatever the Details panel shows);
//   * at its identity it changes NOTHING — the forward shaders pass identities for the inputs their
//     transport does not carry yet, and that is only a no-op if this holds exactly.
//
// The inputs are a mock material written in this file; no asset, no renderer.

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <cmath>
#include <Common/Core/GlslAsCpp.hpp>

namespace Desert::Tests::PBRSurfaceInputsRef
{
    namespace
    {
        using vec2 = glm::vec2;
        using vec3 = glm::vec3;
        using vec4 = glm::vec4;

        using glm::cos;
        using glm::normalize;
        using glm::sin;

        DESERT_GLSL_AS_CPP_BEGIN
#include <Common/PBRSurfaceInputs.glslh>
        DESERT_GLSL_AS_CPP_END
    } // namespace
} // namespace Desert::Tests::PBRSurfaceInputsRef

using namespace Desert::Tests::PBRSurfaceInputsRef;

namespace
{
    // The mock material: every factor off its identity, so each assertion moves exactly one of them.
    const vec2 kUV       = vec2( 0.3f, 0.7f );
    const vec2 kUV1      = vec2( 0.9f, 0.1f );
    const vec3 kTexel    = vec3( 0.5f, 0.25f, 0.75f );
    const vec3 kFactor   = vec3( 0.8f, 0.6f, 0.4f );
    const vec3 kOrm      = vec3( 0.5f, 0.6f, 0.7f );
    const vec3 kTangentN = normalize( vec3( 0.3f, -0.2f, 0.9f ) );

    bool Same( const vec3& a, const vec3& b )
    {
        return glm::all( glm::lessThanEqual( glm::abs( a - b ), vec3( 1e-6f ) ) );
    }
    bool Same( const vec2& a, const vec2& b )
    {
        return glm::all( glm::lessThanEqual( glm::abs( a - b ), vec2( 1e-6f ) ) );
    }
} // namespace

TEST( PBRSurfaceInputs, UVTransformIdentityIsExact )
{
    EXPECT_TRUE( Same( PBRTransformUV( kUV, vec2( 0.0f ), vec2( 1.0f ), 0.0f ), kUV ) );
}

TEST( PBRSurfaceInputs, EachUVTransformParameterMovesTheUV )
{
    const vec2 base = PBRTransformUV( kUV, vec2( 0.0f ), vec2( 1.0f ), 0.0f );
    EXPECT_FALSE( Same( PBRTransformUV( kUV, vec2( 0.25f, 0.0f ), vec2( 1.0f ), 0.0f ), base ) ) << "offset";
    EXPECT_FALSE( Same( PBRTransformUV( kUV, vec2( 0.0f ), vec2( 2.0f, 3.0f ), 0.0f ), base ) ) << "scale";
    EXPECT_FALSE( Same( PBRTransformUV( kUV, vec2( 0.0f ), vec2( 1.0f ), 0.5f ), base ) ) << "rotation";
}

TEST( PBRSurfaceInputs, UVTransformOrderIsScaleThenRotateThenOffset )
{
    // KHR_texture_transform: T * R * S. A quarter turn maps (u, v) to (v, -u) in this convention, and
    // the offset is added AFTER, so it is not itself scaled or rotated.
    const float quarter = 1.5707963f;
    const vec2  out     = PBRTransformUV( vec2( 1.0f, 0.0f ), vec2( 10.0f, 20.0f ), vec2( 2.0f, 5.0f ), quarter );
    EXPECT_NEAR( out.x, 10.0f, 1e-5f );
    EXPECT_NEAR( out.y, 20.0f - 2.0f, 1e-5f );
}

TEST( PBRSurfaceInputs, UVSetSelectsTheSet )
{
    EXPECT_TRUE( Same( PBRSelectUV( kUV, kUV1, 0 ), kUV ) );
    EXPECT_TRUE( Same( PBRSelectUV( kUV, kUV1, 1 ), kUV1 ) );
}

TEST( PBRSurfaceInputs, BaseColorIsFactorTimesTexelTimesVertexColor )
{
    const vec3 white( 1.0f );
    const vec3 base = PBRBaseColor( kFactor, kTexel, white );
    EXPECT_TRUE( Same( base, kFactor * kTexel ) ) << "white vertex colour is the identity";
    EXPECT_FALSE( Same( PBRBaseColor( white, kTexel, white ), base ) ) << "factor";
    EXPECT_FALSE( Same( PBRBaseColor( kFactor, white, white ), base ) ) << "texel";
    EXPECT_FALSE( Same( PBRBaseColor( kFactor, kTexel, vec3( 0.5f, 1.0f, 0.2f ) ), base ) ) << "vertex colour";
}

TEST( PBRSurfaceInputs, OrmChannelsAreGltfOrderAndEachFactorMovesItsOwnChannel )
{
    const vec3 r = PBRResolveORM( kOrm, 1.0f, 1.0f, 1.0f );
    EXPECT_NEAR( r.x, kOrm.x, 1e-6f ) << "R = occlusion";
    EXPECT_NEAR( r.y, kOrm.y, 1e-6f ) << "G = roughness";
    EXPECT_NEAR( r.z, kOrm.z, 1e-6f ) << "B = metallic";

    const vec3 rough = PBRResolveORM( kOrm, 1.0f, 0.5f, 1.0f );
    EXPECT_NEAR( rough.y, kOrm.y * 0.5f, 1e-6f );
    EXPECT_NEAR( rough.x, r.x, 1e-6f );
    EXPECT_NEAR( rough.z, r.z, 1e-6f );

    const vec3 metal = PBRResolveORM( kOrm, 1.0f, 1.0f, 0.25f );
    EXPECT_NEAR( metal.z, kOrm.z * 0.25f, 1e-6f );
    EXPECT_NEAR( metal.x, r.x, 1e-6f );
    EXPECT_NEAR( metal.y, r.y, 1e-6f );
}

TEST( PBRSurfaceInputs, OcclusionStrengthBlendsFromNoOcclusion )
{
    EXPECT_NEAR( PBRResolveORM( kOrm, 0.0f, 1.0f, 1.0f ).x, 1.0f, 1e-6f ) << "strength 0 = unoccluded";
    EXPECT_NEAR( PBRResolveORM( kOrm, 0.5f, 1.0f, 1.0f ).x, 0.75f, 1e-6f ) << "halfway to the texel";
    EXPECT_NEAR( PBRResolveORM( kOrm, 1.0f, 1.0f, 1.0f ).x, kOrm.x, 1e-6f );
}

TEST( PBRSurfaceInputs, WhiteOrmTexelPassesTheFactorsThrough )
{
    // The forward shaders' current call: no ORM texture bound. Must equal the factors exactly.
    const vec3 r = PBRResolveORM( vec3( 1.0f ), 1.0f, 0.35f, 0.8f );
    EXPECT_EQ( r.x, 1.0f );
    EXPECT_EQ( r.y, 0.35f );
    EXPECT_EQ( r.z, 0.8f );
}

TEST( PBRSurfaceInputs, NormalScaleTiltsTheNormalAndOneIsTheIdentity )
{
    EXPECT_TRUE( Same( PBRScaleTangentNormal( kTangentN, 1.0f ), kTangentN ) );

    const vec3 flat = PBRScaleTangentNormal( kTangentN, 0.0f );
    EXPECT_TRUE( Same( flat, vec3( 0.0f, 0.0f, 1.0f ) ) ) << "scale 0 flattens to +Z";

    const vec3 strong = PBRScaleTangentNormal( kTangentN, 2.0f );
    EXPECT_LT( strong.z, kTangentN.z ) << "scale > 1 tilts further from +Z";
    EXPECT_NEAR( glm::length( strong ), 1.0f, 1e-6f );
}

TEST( PBRSurfaceInputs, EmissionIsTexelTimesColorTimesStrength )
{
    const vec3 base = PBREmission( kTexel, kFactor, 2.0f );
    EXPECT_TRUE( Same( base, kTexel * kFactor * 2.0f ) );
    EXPECT_FALSE( Same( PBREmission( vec3( 1.0f ), kFactor, 2.0f ), base ) ) << "texel";
    EXPECT_FALSE( Same( PBREmission( kTexel, vec3( 1.0f ), 2.0f ), base ) ) << "colour";
    EXPECT_FALSE( Same( PBREmission( kTexel, kFactor, 3.0f ), base ) ) << "strength";
    EXPECT_TRUE( Same( PBREmission( kTexel, kFactor, 0.0f ), vec3( 0.0f ) ) ) << "strength 0 is dark";
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
