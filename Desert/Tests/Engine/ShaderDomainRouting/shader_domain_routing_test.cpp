// Which draw path may execute which material domain.
//
// The defect these tests pin: a `.demat` naming the `Terrain` shader, dropped into a
// StaticMeshComponent material slot, built a valid pipeline and drew. Every individual piece was
// correct -- the shader declares its domain, the parser reads it, the pipeline cache applies the
// State block it asks for -- and nothing anywhere compared the domain the material DECLARES with
// the domain the path executing it can DRAW. That is the relation, and it is what is asserted here.
//
// The trap that made it possible has a name now: IsUserAssignable() is the UNION of the draw paths,
// so a path that asks it instead of its own half accepts Terrain. These tests fail if that union is
// ever restated by hand instead of derived, or if either half quietly grows a domain.

#include <gtest/gtest.h>

#include <Engine/Core/Formats/ShaderProgramMeta.hpp>
#include <Engine/Core/ShaderCompiler/DShader/DShaderParser.hpp>

#include <array>
#include <string>

using Desert::Core::Preprocess::DShaderParser;
using namespace Desert::Core::Formats;

namespace
{
    // Every value of the enum. ShaderDomainName's switch is the compiler-enforced half of keeping
    // this current -- adding a domain without extending that switch is a -Wswitch warning, and the
    // build is warning-clean -- so this array only has to be extended in the same edit.
    constexpr std::array kAllDomains = { ShaderDomain::Unspecified, ShaderDomain::Surface, ShaderDomain::Terrain,
                                         ShaderDomain::Skybox, ShaderDomain::PostProcess };

    // A minimal shader whose ONLY interesting property is the domain line. Written as source rather
    // than as a ShaderProgramMeta literal on purpose: the value under test has to come through the
    // parser, because "the .shader says Terrain" and "the meta holds Terrain" are two claims.
    std::string ShaderDeclaring( const char* domain )
    {
        return std::string( "Shader \"Probe\"\n{\n    Domain " ) + domain +
               "\n\n    Vertex\n    {\n        void main() { gl_Position = vec4(0.0); }\n    }\n"
               "\n    Fragment\n    {\n        layout( location = 0 ) out vec4 o_Color;\n"
               "        void main() { o_Color = vec4(1.0); }\n    }\n}\n";
    }

    ShaderProgramMeta MetaOf( const char* domain )
    {
        auto res = DShaderParser::Parse( ShaderDeclaring( domain ) );
        EXPECT_TRUE( res.IsSuccess() ) << res.GetError();
        return res.IsSuccess() ? res.GetValue().Meta : ShaderProgramMeta{};
    }
} // namespace

// ---------------------------------------------------------------------------------------------
// The two halves, each on its own
// ---------------------------------------------------------------------------------------------

// RDG-FAULT1 (material/pass binding split): the MaterialExecutor gives a property ONLY to a sampler the schema's
// Properties block declares, of the declared kind; every other reflected sampler is a pass parameter bound
// through RDG::PassBindings, so no slot can be filled by both.
TEST( ShaderDomainRouting, OnlySchemaTexturesBecomeMaterialProperties )
{
    ShaderProgramMeta meta;
    ShaderParam       albedo;
    albedo.Name      = "u_Albedo";
    albedo.IsTexture = true;
    ShaderParam environment;
    environment.Name          = "u_Environment";
    environment.IsTexture     = true;
    environment.IsCubeTexture = true;
    ShaderParam asset;
    asset.Name      = "u_Layout";
    asset.IsTexture = true;
    asset.AssetKind = "CloudLayout";
    ShaderParam tint;
    tint.Name   = "u_Tint";
    meta.Params = { albedo, environment, asset, tint };

    ASSERT_NE( FindMaterialTextureParameter( meta, "u_Albedo", false ), nullptr );
    EXPECT_EQ( FindMaterialTextureParameter( meta, "u_Albedo", false )->Name, "u_Albedo" );
    EXPECT_NE( FindMaterialTextureParameter( meta, "u_Environment", true ), nullptr );
    // A pass parameter: reflected by the shader, not in Properties (shadow cascades, BRDF LUT, scene textures).
    EXPECT_EQ( FindMaterialTextureParameter( meta, "u_ShadowCascades", false ), nullptr );
    EXPECT_EQ( FindMaterialTextureParameter( meta, "u_BRDFLut", true ), nullptr );
    // Declared as the other kind, an asset reference, or not a texture: no texture property either.
    EXPECT_EQ( FindMaterialTextureParameter( meta, "u_Albedo", true ), nullptr );
    EXPECT_EQ( FindMaterialTextureParameter( meta, "u_Environment", false ), nullptr );
    EXPECT_EQ( FindMaterialTextureParameter( meta, "u_Layout", false ), nullptr );
    EXPECT_EQ( FindMaterialTextureParameter( meta, "u_Tint", false ), nullptr );
}

TEST( ShaderDomainRouting, MeshPathDrawsSurfaceAndNothingElse )
{
    EXPECT_TRUE( DrawnByMeshPath( ShaderDomain::Surface ) );

    // Terrain is the one that shipped. It is user-assignable, which is exactly why refusing it here
    // has to be a separate question from refusing an unassignable domain.
    EXPECT_FALSE( DrawnByMeshPath( ShaderDomain::Terrain ) );
    EXPECT_FALSE( DrawnByMeshPath( ShaderDomain::Skybox ) );
    EXPECT_FALSE( DrawnByMeshPath( ShaderDomain::PostProcess ) );
    EXPECT_FALSE( DrawnByMeshPath( ShaderDomain::Unspecified ) );
}

TEST( ShaderDomainRouting, TerrainPathDrawsTerrainAndNothingElse )
{
    EXPECT_TRUE( DrawnByTerrainPath( ShaderDomain::Terrain ) );

    EXPECT_FALSE( DrawnByTerrainPath( ShaderDomain::Surface ) );
    EXPECT_FALSE( DrawnByTerrainPath( ShaderDomain::Skybox ) );
    EXPECT_FALSE( DrawnByTerrainPath( ShaderDomain::PostProcess ) );
    EXPECT_FALSE( DrawnByTerrainPath( ShaderDomain::Unspecified ) );
}

// ---------------------------------------------------------------------------------------------
// The relations between them -- the assertions a per-function test cannot make
// ---------------------------------------------------------------------------------------------

TEST( ShaderDomainRouting, UserAssignableIsExactlyTheUnionOfTheDrawPaths )
{
    // The property that makes the domain mean something: a domain a user may assign is a domain
    // SOME path draws. A domain in the union with no path behind it is a material the editor
    // offers and no renderer can execute -- which is the defect, one level up.
    for ( const auto domain : kAllDomains )
    {
        ShaderProgramMeta meta;
        meta.Domain = domain;

        EXPECT_EQ( meta.IsUserAssignable(), DrawnByMeshPath( domain ) || DrawnByTerrainPath( domain ) )
             << "domain " << ShaderDomainName( domain );
    }
}

TEST( ShaderDomainRouting, NoDomainIsDrawnByBothPaths )
{
    // Disjointness is what makes "refuse what this path does not draw" a complete rule: if a domain
    // were drawn by two paths, refusing it in one of them would be wrong half the time.
    for ( const auto domain : kAllDomains )
    {
        EXPECT_FALSE( DrawnByMeshPath( domain ) && DrawnByTerrainPath( domain ) )
             << "domain " << ShaderDomainName( domain );
    }
}

TEST( ShaderDomainRouting, EngineOnlyDomainsAreDrawnByNeitherPath )
{
    // Skybox and PostProcess have their own renderers and are not reachable from a material slot;
    // Unspecified means the .shader declared no domain at all. None may be routed to a user path.
    for ( const auto domain : { ShaderDomain::Unspecified, ShaderDomain::Skybox, ShaderDomain::PostProcess } )
    {
        EXPECT_FALSE( DrawnByMeshPath( domain ) ) << ShaderDomainName( domain );
        EXPECT_FALSE( DrawnByTerrainPath( domain ) ) << ShaderDomainName( domain );
    }
}

// ---------------------------------------------------------------------------------------------
// End to end: a shader's own source text through the parser and into the routing decision
// ---------------------------------------------------------------------------------------------

TEST( ShaderDomainRouting, TerrainSourceIsAssignableButRefusedByTheMeshPath )
{
    const auto meta = MetaOf( "Terrain" );
    ASSERT_EQ( meta.Domain, ShaderDomain::Terrain );

    // Both halves of the defect in one place: the editor was right to offer this material, and the
    // mesh path was wrong to draw it. A fix that made either of these two lines flip would be
    // trading this defect for a different one.
    EXPECT_TRUE( meta.IsUserAssignable() );
    EXPECT_FALSE( DrawnByMeshPath( meta.Domain ) );
}

TEST( ShaderDomainRouting, SurfaceSourceIsDrawnByTheMeshPath )
{
    const auto meta = MetaOf( "Surface" );
    ASSERT_EQ( meta.Domain, ShaderDomain::Surface );

    EXPECT_TRUE( meta.IsUserAssignable() );
    EXPECT_TRUE( DrawnByMeshPath( meta.Domain ) );
}

TEST( ShaderDomainRouting, AShaderWithNoDomainLineIsRefusedEverywhere )
{
    // No `Domain` line at all -> Unspecified. The header calls this "internal", and a material
    // pointing at such a shader must not draw on a mesh just because nothing said otherwise.
    auto res = DShaderParser::Parse( "Shader \"NoDomain\"\n{\n    Vertex\n    {\n"
                                     "        void main() { gl_Position = vec4(0.0); }\n    }\n"
                                     "\n    Fragment\n    {\n        layout( location = 0 ) out vec4 o_Color;\n"
                                     "        void main() { o_Color = vec4(1.0); }\n    }\n}\n" );
    ASSERT_TRUE( res.IsSuccess() ) << res.GetError();

    const auto& meta = res.GetValue().Meta;
    EXPECT_EQ( meta.Domain, ShaderDomain::Unspecified );
    EXPECT_FALSE( meta.IsUserAssignable() );
    EXPECT_FALSE( DrawnByMeshPath( meta.Domain ) );
}

// ---------------------------------------------------------------------------------------------
// The name the refusal message prints
// ---------------------------------------------------------------------------------------------

TEST( ShaderDomainRouting, EveryDomainHasItsOwnName )
{
    // The refusal names the domain the material declares and the domain the path draws. Two domains
    // sharing a string would make that message say the two are the same while refusing them for
    // being different.
    for ( size_t i = 0; i < kAllDomains.size(); ++i )
    {
        for ( size_t j = i + 1; j < kAllDomains.size(); ++j )
        {
            EXPECT_STRNE( ShaderDomainName( kAllDomains[i] ), ShaderDomainName( kAllDomains[j] ) );
        }
    }

    // Spelled as the `.shader` spells them, so a log line can be grepped straight back to the file.
    EXPECT_STREQ( ShaderDomainName( ShaderDomain::Surface ), "Surface" );
    EXPECT_STREQ( ShaderDomainName( ShaderDomain::Terrain ), "Terrain" );
}

TEST( ShaderDomainRouting, EachPathConstantIsTheDomainItsPredicateAccepts )
{
    // The refusal message prints kMeshPathDomain while the branch tests DrawnByMeshPath. If those two
    // ever came apart, the log would state a rule the code was not applying -- the most expensive kind
    // of wrong message, because it reads as authoritative.
    EXPECT_TRUE( DrawnByMeshPath( kMeshPathDomain ) );
    EXPECT_TRUE( DrawnByTerrainPath( kTerrainPathDomain ) );
    EXPECT_NE( kMeshPathDomain, kTerrainPathDomain );
}
