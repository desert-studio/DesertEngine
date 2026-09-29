#include <gtest/gtest.h>

#include <Engine/Core/ShaderCompiler/DShader/DShaderParser.hpp>
#include <Common/Content/ShaderAssetHeader.hpp>

using Desert::Core::Preprocess::DShaderParser;
using namespace Desert::Core::Formats;

namespace
{
    const char* kUnlit = R"(
// comment before the shader
Shader "Unlit"
{
    Domain Surface

    Properties Binding(1) TextureBinding(2)
    {
        Color     Color       ("Base Color")               = (0.8, 0.4, 0.1, 1)
        Float     Tiling      ("Tiling", Range(0.25, 64))  = 4
        Texture2D u_AlbedoTex ("Albedo")                   = "white"
    }

    State
    {
        Cull Back
        ZTest LEqual
        ZWrite On
        Blend Alpha
    }

    Vertex
    {
        void main() { gl_Position = vec4(0.0); }
    }

    Fragment
    {
        layout( location = 0 ) out vec4 o_Color;
        void main() { o_Color = texture( u_AlbedoTex, vec2(0.5) ) * u_Material.Color; }
    }
}
)";
}

TEST( DShaderParser, DetectsFormat )
{
    EXPECT_TRUE( DShaderParser::IsDShader( kUnlit ) );
    EXPECT_TRUE( DShaderParser::IsDShader( "  /* hi */ Shader \"X\" {}" ) );
    EXPECT_FALSE( DShaderParser::IsDShader( "#pragma program Grid\n#pragma use_stage vertex \"a.vert\"" ) );
    EXPECT_FALSE( DShaderParser::IsDShader( "" ) );
}

TEST( DShaderParser, ParsesNameDomainAndStages )
{
    auto res = DShaderParser::Parse( kUnlit );
    ASSERT_TRUE( res.IsSuccess() ) << res.GetError();
    const auto& p = res.GetValue();

    EXPECT_EQ( p.Name, "Unlit" );
    EXPECT_EQ( p.Meta.Domain, ShaderDomain::Surface );
    ASSERT_EQ( p.Stages.size(), 2u );
    ASSERT_TRUE( p.Stages.count( ShaderStage::Vertex ) );
    ASSERT_TRUE( p.Stages.count( ShaderStage::Fragment ) );
}

TEST( DShaderParser, ParsesProperties )
{
    auto res = DShaderParser::Parse( kUnlit );
    ASSERT_TRUE( res.IsSuccess() ) << res.GetError();
    const auto& params = res.GetValue().Meta.Params;

    ASSERT_EQ( params.size(), 3u );

    EXPECT_EQ( params[0].Name, "Color" );
    EXPECT_EQ( params[0].DisplayName, "Base Color" );
    EXPECT_EQ( params[0].Type, ShaderValueType::Float4 );
    EXPECT_EQ( params[0].Widget, ShaderParamWidget::Color );
    EXPECT_FLOAT_EQ( params[0].Default.x, 0.8f );
    EXPECT_FLOAT_EQ( params[0].Default.w, 1.0f );

    EXPECT_EQ( params[1].Name, "Tiling" );
    EXPECT_EQ( params[1].Type, ShaderValueType::Float );
    ASSERT_TRUE( params[1].Min.has_value() );
    EXPECT_FLOAT_EQ( *params[1].Min, 0.25f );
    EXPECT_FLOAT_EQ( *params[1].Max, 64.0f );
    EXPECT_EQ( params[1].Widget, ShaderParamWidget::Slider );
    EXPECT_FLOAT_EQ( params[1].Default.x, 4.0f );

    EXPECT_TRUE( params[2].IsTexture );
    EXPECT_EQ( params[2].DefaultTexture, DefaultTextureKind::White );
}

// The default texture is a CLOSED SET, and the parser is the only place a typo in it can still be
// caught. It used to be a free `std::string` that nothing downstream read, so `= "wihte"` parsed, the
// shader compiled and loaded, and the evidence was a surface somebody eventually noticed was the wrong
// colour. Both directions are asserted: the four legal names survive the round trip, and a fifth is a
// refusal that NAMES the property and lists the alternatives — a message that only says "parse error"
// sends the author back to counting braces.
TEST( DShaderParser, EveryDefaultTextureNameParsesAndAnUnknownOneIsRefusedByName )
{
    const auto parseWithDefault = []( const char* name )
    {
        const std::string src = std::string( R"(
Shader "DefaultTextureProbe"
{
    Domain Surface
    Properties Binding(1) TextureBinding(2)
    {
        Texture2D u_Slot ("Slot") = ")" ) +
                                name + R"("
    }
    Fragment { void main() {} }
}
)";
        return DShaderParser::Parse( src );
    };

    for ( const DefaultTextureKind kind : kAllDefaultTextureKinds )
    {
        auto res = parseWithDefault( DefaultTextureKindName( kind ) );
        ASSERT_TRUE( res.IsSuccess() ) << DefaultTextureKindName( kind ) << ": " << res.GetError();
        ASSERT_EQ( res.GetValue().Meta.Params.size(), 1u );
        EXPECT_EQ( res.GetValue().Meta.Params[0].DefaultTexture, kind )
             << "'" << DefaultTextureKindName( kind ) << "' did not survive the round trip";
    }

    auto bad = parseWithDefault( "wihte" );
    ASSERT_FALSE( bad.IsSuccess() ) << "an unknown default texture name was accepted";
    EXPECT_NE( bad.GetError().find( "wihte" ), std::string::npos ) << bad.GetError();
    EXPECT_NE( bad.GetError().find( "u_Slot" ), std::string::npos ) << bad.GetError();
    EXPECT_NE( bad.GetError().find( "\"white\"" ), std::string::npos )
         << "the refusal must list the legal set: " << bad.GetError();
}

// A TextureCube property is CUBE IN THE SCHEMA, not merely in the generated sampler: the editor's
// material window decides by ShaderParam::IsCubeTexture which asset a slot takes (an HDR skybox vs a
// 2D texture) and which slot the cubemap preview wraps onto its ball. The flag used to exist only as a
// parser-local array parallel to Params, where no consumer outside the generator could read it — and
// two lists that must agree by index is the defect shape this engine keeps a name for.
TEST( DShaderParser, CubePropertiesAreCubeInTheSchemaAndInTheGeneratedSampler )
{
    const char* src = R"(
Shader "CubeSky"
{
    Domain Skybox

    Properties TextureBinding(4)
    {
        TextureCube u_Sky    ("Cubemap")
        Texture2D   u_Detail ("Detail")
    }

    Vertex   { void main() { gl_Position = vec4(0.0); } }
    Fragment
    {
        layout( location = 0 ) out vec4 o;
        void main() { o = texture( u_Sky, vec3(0.0, 1.0, 0.0) ) + texture( u_Detail, vec2(0.5) ); }
    }
}
)";
    auto        res = DShaderParser::Parse( src );
    ASSERT_TRUE( res.IsSuccess() ) << res.GetError();
    const auto& p = res.GetValue();

    EXPECT_EQ( p.Meta.Domain, ShaderDomain::Skybox );

    ASSERT_EQ( p.Meta.Params.size(), 2u );
    EXPECT_TRUE( p.Meta.Params[0].IsTexture );
    EXPECT_TRUE( p.Meta.Params[0].IsCubeTexture );
    EXPECT_TRUE( p.Meta.Params[1].IsTexture );
    EXPECT_FALSE( p.Meta.Params[1].IsCubeTexture );

    // The generated declarations must agree with the schema, each from the SAME field.
    const auto& frag = p.Stages.at( ShaderStage::Fragment );
    EXPECT_NE( frag.find( "layout( binding = 4 ) uniform samplerCube u_Sky;" ), std::string::npos );
    EXPECT_NE( frag.find( "layout( binding = 5 ) uniform sampler2D u_Detail;" ), std::string::npos );
}

TEST( DShaderParser, ParsesRenderState )
{
    auto res = DShaderParser::Parse( kUnlit );
    ASSERT_TRUE( res.IsSuccess() ) << res.GetError();
    const auto& s = res.GetValue().Meta.State;

    EXPECT_EQ( s.Cull, StateCull::Back );
    EXPECT_EQ( s.DepthCompare, StateCompare::LessOrEqual );
    EXPECT_EQ( s.DepthTest, true );
    EXPECT_EQ( s.DepthWrite, true );
    EXPECT_EQ( s.Blend, true );
}

// `Properties Binding(n)` generates a ROW of the shared `Materials[]` storage buffer plus the `#define`
// that names this draw's row — the one parameter transport in the engine. It used to generate a `uniform
// MaterialUB` block instead, which IS the parameters and therefore held exactly one set of values for
// every object drawn with the shader; Engine/Core/Formats/MaterialParamRow.hpp records the probe scene
// where three spheres differing only in a parameter all rendered the first one's colour.
TEST( DShaderParser, GeneratesAMaterialRowAndSamplersInFragmentOnly )
{
    auto res = DShaderParser::Parse( kUnlit );
    ASSERT_TRUE( res.IsSuccess() ) << res.GetError();
    const auto& stages = res.GetValue().Stages;

    const auto& frag = stages.at( ShaderStage::Fragment );
    EXPECT_NE( frag.find( "layout( std430, binding = 1 ) readonly buffer Materials" ), std::string::npos );
    EXPECT_NE( frag.find( "MaterialParams u_Materials[];" ), std::string::npos );
    EXPECT_NE( frag.find( "#define u_Material u_Materials[m_PushConstants.MaterialIndex]" ), std::string::npos );
    EXPECT_NE( frag.find( "#include <Common/MaterialTransport.glslh>" ), std::string::npos );
    EXPECT_NE( frag.find( "vec4 Color;" ), std::string::npos );
    EXPECT_NE( frag.find( "float Tiling;" ), std::string::npos );
    EXPECT_NE( frag.find( "layout( binding = 2 ) uniform sampler2D u_AlbedoTex;" ), std::string::npos );

    // THE TRANSPORT THAT LOST. Nothing may generate it again: a shader that has one cannot give two
    // objects different values, and the census in Tests/Engine/ShippedShaderPasses is the count of
    // shipped shaders that do.
    EXPECT_EQ( frag.find( "uniform MaterialUB" ), std::string::npos );

    const auto& vert = stages.at( ShaderStage::Vertex );
    EXPECT_EQ( vert.find( "buffer Materials" ), std::string::npos );
    EXPECT_EQ( vert.find( "sampler2D" ), std::string::npos );

    // Every stage gets the version header exactly once, as the first line.
    EXPECT_EQ( frag.rfind( "#version 460\n", 0 ), 0u );
    EXPECT_EQ( vert.rfind( "#version 460\n", 0 ), 0u );
    // Neither stage names a ray-query type, so neither may carry the extension a device without ray
    // query would refuse.
    EXPECT_EQ( frag.find( "GL_EXT_ray_query" ), std::string::npos );
    EXPECT_EQ( vert.find( "GL_EXT_ray_query" ), std::string::npos );
}

// THE RELATION the C++ packer stands on: a parameter of any type occupies a whole 16-byte slot, so
// parameter i is at 16*i and DataDrivenMaterial's row is a plain vec4 array with no layout rules in it.
// Asserted on the emitted TEXT here (the layout is a property of what is generated); asserted again on
// the reflected SPIR-V of the shipped shaders in Tests/Engine/ShaderCacheKey, which is the half that
// would catch a GLSL rule this reasoning got wrong.
//
// The padding is what makes it true, so the padding is what is checked: `float` is 4 bytes of a 16-byte
// slot and needs 3 floats behind it, `vec2` 2, `vec3` 1, `vec4` none. Drop any of them and the next
// parameter slides, silently, to an offset the CPU is not writing.
TEST( DShaderParser, EveryMaterialParameterOccupiesAWholeSixteenByteSlot )
{
    const char* src = R"(
Shader "Slots"
{
    Properties Binding(3)
    {
        Float Scalar ("Scalar") = 1
        Vec2  Pair   ("Pair")   = (1, 2)
        Vec3  Triple ("Triple") = (1, 2, 3)
        Color Quad   ("Quad")   = (1, 2, 3, 4)
    }
    Fragment { layout( location = 0 ) out vec4 c; void main() { c = u_Material.Quad; } }
}
)";
    auto        res = DShaderParser::Parse( src );
    ASSERT_TRUE( res.IsSuccess() ) << res.GetError();
    const auto& frag = res.GetValue().Stages.at( ShaderStage::Fragment );

    EXPECT_NE( frag.find( "float Scalar;\n    float _slotPad0[3];" ), std::string::npos ) << frag;
    EXPECT_NE( frag.find( "vec2 Pair;\n    float _slotPad1[2];" ), std::string::npos ) << frag;
    EXPECT_NE( frag.find( "vec3 Triple;\n    float _slotPad2[1];" ), std::string::npos ) << frag;
    // A vec4 already fills its slot, so it must be followed by NO padding at all — the struct closes.
    EXPECT_NE( frag.find( "vec4 Quad;\n};" ), std::string::npos ) << frag;
}

TEST( DShaderParser, EmitsLineDirectivesForErrorMapping )
{
    auto res = DShaderParser::Parse( kUnlit );
    ASSERT_TRUE( res.IsSuccess() ) << res.GetError();
    EXPECT_NE( res.GetValue().Stages.at( ShaderStage::Fragment ).find( "#line " ), std::string::npos );
}

TEST( DShaderParser, MetadataOnlyModeGeneratesNothing )
{
    const char* src = R"(
Shader "Manual"
{
    Properties
    {
        Color Tint ("Tint") = (1, 1, 1, 1)
    }
    Fragment
    {
        void main() {}
    }
}
)";
    auto        res = DShaderParser::Parse( src );
    ASSERT_TRUE( res.IsSuccess() ) << res.GetError();
    EXPECT_EQ( res.GetValue().Stages.at( ShaderStage::Fragment ).find( "buffer Materials" ), std::string::npos );
    ASSERT_EQ( res.GetValue().Meta.Params.size(), 1u );
}

TEST( DShaderParser, IncludeBlockIsSharedAcrossStages )
{
    const char* src = R"(
Shader "WithInclude"
{
    Include
    {
        float shared_helper( float x ) { return x * 2.0; }
    }
    Vertex   { void main() { gl_Position = vec4( shared_helper( 1.0 ) ); } }
    Fragment { layout( location = 0 ) out vec4 c; void main() { c = vec4( shared_helper( 2.0 ) ); } }
}
)";
    auto        res = DShaderParser::Parse( src );
    ASSERT_TRUE( res.IsSuccess() ) << res.GetError();
    EXPECT_NE( res.GetValue().Stages.at( ShaderStage::Vertex ).find( "shared_helper" ), std::string::npos );
    EXPECT_NE( res.GetValue().Stages.at( ShaderStage::Fragment ).find( "shared_helper" ), std::string::npos );
}

TEST( DShaderParser, ErrorsCarryLineNumbers )
{
    // Unknown State command on line 5.
    const char* src = "Shader \"Broken\"\n{\n    State\n    {\n        Frobnicate On\n    }\n}\n";
    auto        res = DShaderParser::Parse( src );
    ASSERT_FALSE( res.IsSuccess() );
    EXPECT_NE( res.GetError().find( "line 5" ), std::string::npos ) << res.GetError();
    EXPECT_NE( res.GetError().find( "Frobnicate" ), std::string::npos ) << res.GetError();
}

TEST( DShaderParser, RejectsVersionInStageBlock )
{
    const char* src = R"(
Shader "Versioned"
{
    Fragment
    {
        #version 450
        void main() {}
    }
}
)";
    auto        res = DShaderParser::Parse( src );
    ASSERT_FALSE( res.IsSuccess() );
    EXPECT_NE( res.GetError().find( "#version" ), std::string::npos );
}

TEST( DShaderParser, RejectsMissingStages )
{
    auto res = DShaderParser::Parse( "Shader \"Empty\" { Domain Surface }" );
    ASSERT_FALSE( res.IsSuccess() );
    EXPECT_NE( res.GetError().find( "no stage blocks" ), std::string::npos );
}

TEST( DShaderParser, BracesInsideGlslCommentsDoNotBreakBlocks )
{
    const char* src = R"(
Shader "Tricky"
{
    Fragment
    {
        // a stray } in a comment
        /* and another } here */
        void main() {}
    }
}
)";
    auto        res = DShaderParser::Parse( src );
    ASSERT_TRUE( res.IsSuccess() ) << res.GetError();
    EXPECT_NE( res.GetValue().Stages.at( ShaderStage::Fragment ).find( "void main()" ), std::string::npos );
}

// ─── v2: multi-pass ─────────────────────────────────────────────────────────────

namespace
{
    const char* kMultiPass = R"(
Shader "Lit"
{
    Domain Surface

    Properties Binding(1)
    {
        Color Tint ("Tint") = (1, 1, 1, 1)
    }

    State
    {
        Cull Back
        ZTest Less
        Blend On
    }

    Vertex   { void main() { gl_Position = vec4(0.0); } }
    Fragment { layout( location = 0 ) out vec4 c; void main() { c = u_Material.Tint; } }

    Pass "Shadow"
    {
        State
        {
            Cull Front
            Blend Off
        }
        Vertex { void main() { gl_Position = vec4(1.0); } }
    }

    Pass "Outline"
    {
        Vertex   { void main() { gl_Position = vec4(2.0); } }
        Fragment { layout( location = 0 ) out vec4 c; void main() { c = vec4(1.0); } }
    }
}
)";
}

TEST( DShaderParserPasses, CollectsAllPasses )
{
    auto res = DShaderParser::Parse( kMultiPass );
    ASSERT_TRUE( res.IsSuccess() ) << res.GetError();
    const auto& p = res.GetValue();

    ASSERT_EQ( p.Passes.size(), 3u );
    EXPECT_EQ( p.Passes[0].Name, "" ); // default program first
    EXPECT_EQ( p.Passes[1].Name, "Shadow" );
    EXPECT_EQ( p.Passes[2].Name, "Outline" );

    // The meta advertises the NAMED passes only (the default one is the shader itself).
    ASSERT_EQ( p.Meta.PassNames.size(), 2u );
    EXPECT_EQ( p.Meta.PassNames[0], "Shadow" );
    EXPECT_EQ( p.Meta.PassNames[1], "Outline" );

    EXPECT_NE( p.FindPass( "" ), nullptr );
    EXPECT_NE( p.FindPass( "Shadow" ), nullptr );
    EXPECT_EQ( p.FindPass( "Missing" ), nullptr );
}

TEST( DShaderParserPasses, DefaultProgramKeepsTopLevelStagesAndState )
{
    auto res = DShaderParser::Parse( kMultiPass );
    ASSERT_TRUE( res.IsSuccess() ) << res.GetError();
    const auto& p = res.GetValue();

    // result.Stages / Meta.State mirror the default pass (backward compatibility).
    EXPECT_EQ( p.Stages.size(), p.Passes[0].Stages.size() );
    EXPECT_EQ( p.Meta.State.Cull, StateCull::Back );
    EXPECT_EQ( p.Meta.State.Blend, true );
}

TEST( DShaderParserPasses, PassStateInheritsAndOverrides )
{
    auto res = DShaderParser::Parse( kMultiPass );
    ASSERT_TRUE( res.IsSuccess() ) << res.GetError();
    const auto* shadow = res.GetValue().FindPass( "Shadow" );
    ASSERT_NE( shadow, nullptr );

    EXPECT_EQ( shadow->State.Cull, StateCull::Front );           // overridden
    EXPECT_EQ( shadow->State.Blend, false );                     // overridden (On -> Off)
    EXPECT_EQ( shadow->State.DepthCompare, StateCompare::Less ); // inherited from file State
    EXPECT_EQ( shadow->State.DepthTest, true );                  // inherited

    const auto* outline = res.GetValue().FindPass( "Outline" );
    ASSERT_NE( outline, nullptr );
    EXPECT_EQ( outline->State.Cull, StateCull::Back ); // fully inherited (no overrides)
    EXPECT_EQ( outline->State.Blend, true );
}

TEST( DShaderParserPasses, PassStagesAreIndependent )
{
    auto res = DShaderParser::Parse( kMultiPass );
    ASSERT_TRUE( res.IsSuccess() ) << res.GetError();
    const auto& p = res.GetValue();

    const auto* shadow = p.FindPass( "Shadow" );
    ASSERT_NE( shadow, nullptr );
    ASSERT_EQ( shadow->Stages.size(), 1u ); // vertex-only depth pass
    EXPECT_TRUE( shadow->Stages.count( ShaderStage::Vertex ) );
    EXPECT_NE( shadow->Stages.at( ShaderStage::Vertex ).find( "vec4(1.0)" ), std::string::npos );

    // Auto-generated resources reach pass fragments too.
    const auto* outline = p.FindPass( "Outline" );
    ASSERT_NE( outline, nullptr );
    EXPECT_NE( outline->Stages.at( ShaderStage::Fragment ).find( "buffer Materials" ), std::string::npos );
}

TEST( DShaderParserPasses, PassOnlyShaderUsesFirstPassAsDefault )
{
    const char* src = R"(
Shader "PassOnly"
{
    Pass "Main"
    {
        State  { Cull None }
        Vertex { void main() { gl_Position = vec4(0.0); } }
    }
}
)";
    auto        res = DShaderParser::Parse( src );
    ASSERT_TRUE( res.IsSuccess() ) << res.GetError();
    const auto& p = res.GetValue();

    ASSERT_EQ( p.Passes.size(), 1u );
    EXPECT_EQ( p.Passes[0].Name, "Main" );
    EXPECT_EQ( p.Stages.size(), 1u ); // first pass doubles as the default program
    EXPECT_EQ( p.Meta.State.Cull, StateCull::None );
    ASSERT_EQ( p.Meta.PassNames.size(), 1u );
}

TEST( DShaderParserPasses, RejectsDuplicatePassNames )
{
    const char* src = R"(
Shader "Dup"
{
    Pass "A" { Vertex { void main() {} } }
    Pass "A" { Vertex { void main() {} } }
}
)";
    auto        res = DShaderParser::Parse( src );
    ASSERT_FALSE( res.IsSuccess() );
    EXPECT_NE( res.GetError().find( "duplicate Pass" ), std::string::npos ) << res.GetError();
}

TEST( DShaderParserPasses, RejectsEmptyPass )
{
    const char* src = R"(
Shader "Empty"
{
    Pass "NoStages" { State { Cull None } }
}
)";
    auto        res = DShaderParser::Parse( src );
    ASSERT_FALSE( res.IsSuccess() );
    EXPECT_NE( res.GetError().find( "no stage blocks" ), std::string::npos ) << res.GetError();
}

TEST( DShaderParserPasses, RejectsUnknownPassSection )
{
    const char* src = R"(
Shader "Bad"
{
    Pass "P"
    {
        Properties { Float X ("X") }
        Vertex { void main() {} }
    }
}
)";
    auto        res = DShaderParser::Parse( src );
    ASSERT_FALSE( res.IsSuccess() );
    EXPECT_NE( res.GetError().find( "unknown Pass section" ), std::string::npos ) << res.GetError();
}

TEST( DShaderParserPasses, RejectsUnnamedPass )
{
    auto res = DShaderParser::Parse( "Shader \"X\" { Pass \"\" { Vertex { void main() {} } } }" );
    ASSERT_FALSE( res.IsSuccess() );
}

TEST( DShaderParserPasses, SinglePassShaderHasNoPassNames )
{
    auto res = DShaderParser::Parse( kUnlit );
    ASSERT_TRUE( res.IsSuccess() ) << res.GetError();
    EXPECT_TRUE( res.GetValue().Meta.PassNames.empty() );
    ASSERT_EQ( res.GetValue().Passes.size(), 1u );
    EXPECT_EQ( res.GetValue().Passes[0].Name, "" );
}

TEST( DShaderParser, TranslatesLayoutSugar )
{
    const char* kSugar = R"(
Shader "Sugar"
{
    Domain Surface
    Vertex
    {
        In(0) vec3 a_Position;
        Out(1) vec2 v_UV;
        PushConstant Push { mat4 M; } pc;
        void main() { gl_Position = pc.M * vec4(a_Position, 1.0); v_UV = vec2(0.0); }
    }
    Fragment
    {
        Uniform(3) sampler2D u_Tex;
        In(1) vec2 v_UV;
        Out(0) vec4 o;
        void main() { o = texture(u_Tex, v_UV); }
    }
}
)";
    auto        res    = DShaderParser::Parse( kSugar );
    ASSERT_TRUE( res.IsSuccess() ) << res.GetError();

    const auto& stages = res.GetValue().Stages;
    ASSERT_TRUE( stages.count( ShaderStage::Vertex ) );
    ASSERT_TRUE( stages.count( ShaderStage::Fragment ) );
    const std::string& vs = stages.at( ShaderStage::Vertex );
    const std::string& fs = stages.at( ShaderStage::Fragment );

    EXPECT_NE( vs.find( "layout(location = 0) in vec3 a_Position;" ), std::string::npos );
    EXPECT_NE( vs.find( "layout(location = 1) out vec2 v_UV;" ), std::string::npos );
    EXPECT_NE( vs.find( "layout(push_constant) uniform Push { mat4 M; } pc;" ), std::string::npos );
    EXPECT_NE( fs.find( "layout(binding = 3) uniform sampler2D u_Tex;" ), std::string::npos );
    EXPECT_NE( fs.find( "layout(location = 0) out vec4 o;" ), std::string::npos );

    // The sugar keywords must be fully consumed (no leftover In(/Out(/Uniform(/PushConstant).
    EXPECT_EQ( vs.find( "In(0)" ), std::string::npos );
    EXPECT_EQ( fs.find( "Uniform(3)" ), std::string::npos );
}

TEST( DShaderParser, ParsesBlendFactorsAndStencil )
{
    const char* kSrc = R"(
Shader "BS"
{
    State
    {
        Blend One OneMinusSrcAlpha
        Stencil Equal 1 Keep Replace Keep
    }
    Vertex   { void main() { gl_Position = vec4(0.0); } }
    Fragment { Out(0) vec4 o; void main() { o = vec4(1.0); } }
}
)";
    auto        res  = DShaderParser::Parse( kSrc );
    ASSERT_TRUE( res.IsSuccess() ) << res.GetError();
    const auto& s = res.GetValue().Meta.State;

    ASSERT_TRUE( s.Blend.has_value() );
    EXPECT_TRUE( *s.Blend );
    ASSERT_TRUE( s.BlendSrc.has_value() );
    ASSERT_TRUE( s.BlendDst.has_value() );
    EXPECT_EQ( *s.BlendSrc, StateBlendFactor::One );
    EXPECT_EQ( *s.BlendDst, StateBlendFactor::OneMinusSrcAlpha );

    ASSERT_TRUE( s.StencilTest.value_or( false ) );
    EXPECT_EQ( *s.StencilCompare, StateCompare::Equal );
    EXPECT_EQ( *s.StencilRef, 1u );
    EXPECT_EQ( *s.StencilPass, StateStencilOp::Replace );
}

// --- Auto-allocated layout numbers (drop the parentheses) ------------------------------------------------

static const char* kAutoLoc = R"(
Shader "AutoLoc"
{
    Vertex
    {
        In vec3 a_Position;
        Out vec2 v_UV;
        Out vec4 v_Color;
        void main() { gl_Position = vec4( a_Position, 1.0 ); v_UV = vec2( 0.0 ); v_Color = vec4( 1.0 ); }
    }
    Fragment
    {
        In vec2 v_UV;
        In vec4 v_Color;
        Out vec4 o_Color;
        void main() { o_Color = v_Color; }
    }
}
)";

TEST( DShaderParser, AutoAllocatesLocations )
{
    auto res = DShaderParser::Parse( kAutoLoc );
    ASSERT_TRUE( res.IsSuccess() ) << res.GetError();
    const auto& v = res.GetValue().Stages.at( ShaderStage::Vertex );
    // `in` (attributes) and `out` (varyings) are independent spaces, each allocated from 0 in order.
    EXPECT_NE( v.find( "layout(location = 0) in vec3 a_Position" ), std::string::npos );
    EXPECT_NE( v.find( "layout(location = 0) out vec2 v_UV" ), std::string::npos );
    EXPECT_NE( v.find( "layout(location = 1) out vec4 v_Color" ), std::string::npos );

    const auto& f = res.GetValue().Stages.at( ShaderStage::Fragment );
    EXPECT_NE( f.find( "layout(location = 0) in vec2 v_UV" ), std::string::npos );
    EXPECT_NE( f.find( "layout(location = 1) in vec4 v_Color" ), std::string::npos );
    EXPECT_NE( f.find( "layout(location = 0) out vec4 o_Color" ), std::string::npos );
}

static const char* kAutoBind = R"(
Shader "AutoBind"
{
    Compute
    {
        LocalSize( 64, 1, 1 );
        Uniform(0) CameraUB { mat4 vp; };
        Buffer Particles { vec4 p[]; };
        Buffer Counter { uint c; };
        void main() {}
    }
}
)";

TEST( DShaderParser, AutoBindingsSkipExplicit )
{
    auto res = DShaderParser::Parse( kAutoBind );
    ASSERT_TRUE( res.IsSuccess() ) << res.GetError();
    const auto& c = res.GetValue().Stages.at( ShaderStage::Compute );
    EXPECT_NE( c.find( "layout(binding = 0) uniform CameraUB" ), std::string::npos );         // explicit kept
    EXPECT_NE( c.find( "layout(std430, binding = 1) buffer Particles" ), std::string::npos ); // auto skips 0
    EXPECT_NE( c.find( "layout(std430, binding = 2) buffer Counter" ), std::string::npos );   // auto next free
}

// Г17 made the translation skip the rules whose keyword is not present, because running fifteen
// std::regex passes over every shader and every included header at startup cost 5.6 s of the 7.9 s
// shader-preload phase in Debug. The saving is only sound if the cheap presence scan and the regexes
// agree, character for character, about what counts as an occurrence — and the two spellings of "a
// whole word" are exactly the shape this repository keeps paying for.
//
// So both directions, in one shader: identifiers that CONTAIN a keyword must not make the pass run as
// if a declaration were there, and a real declaration standing next to them must still be translated.
static const char* kKeywordLookalikes = R"(
Shader "Lookalikes"
{
    Compute
    {
        LocalSize( 8, 8, 1 );
        Uniform(3) CameraUB { mat4 vp; };
        Buffer Particles { vec4 p[]; };
        float UniformScale = 2.0;
        float Inscattering = 0.5;
        float FadeOut = 1.0;
        vec4  BufferedValue = vec4( 0.0 );
        void main() { }
    }
}
)";

TEST( DShaderParser, IdentifiersContainingAKeywordAreNotDeclarations )
{
    auto res = DShaderParser::Parse( kKeywordLookalikes );
    ASSERT_TRUE( res.IsSuccess() ) << res.GetError();
    const auto& c = res.GetValue().Stages.at( ShaderStage::Compute );

    // The real declarations still translate: the explicit one keeps its number, the automatic one takes
    // the lowest free slot, which is 0 because only 3 is spoken for.
    EXPECT_NE( c.find( "layout(binding = 3) uniform CameraUB" ), std::string::npos ) << c;
    EXPECT_NE( c.find( "layout(std430, binding = 0) buffer Particles" ), std::string::npos ) << c;

    // And the four look-alikes come through untouched — neither rewritten nor counted as an occupancy.
    EXPECT_NE( c.find( "float UniformScale = 2.0;" ), std::string::npos ) << c;
    EXPECT_NE( c.find( "float Inscattering = 0.5;" ), std::string::npos ) << c;
    EXPECT_NE( c.find( "float FadeOut = 1.0;" ), std::string::npos ) << c;
    EXPECT_NE( c.find( "vec4  BufferedValue = vec4( 0.0 );" ), std::string::npos ) << c;
}

// The other half of the same agreement: a text in which NO keyword stands alone anywhere is returned
// unchanged. This is the case the gate short-circuits entirely, and "unchanged" is the only acceptable
// meaning of "skipped".
TEST( DShaderParser, ATextWithNoSugarKeywordIsReturnedVerbatim )
{
    const char* kPlainGlsl = R"(#version 450
// Inscattering, UniformScale and BufferedValue are prose here, and code below.
layout( binding = 0 ) uniform sampler2D u_Albedo;
layout( location = 0 ) in  vec2 v_UV;
layout( location = 0 ) out vec4 o_Color;
float Inscattering( float x ) { return x * 0.5; }
void main() { o_Color = texture( u_Albedo, v_UV ) * Inscattering( 1.0 ); }
)";

    EXPECT_EQ( DShaderParser::TranslateSugar( kPlainGlsl ), std::string( kPlainGlsl ) );
}

// A pass that samples a 3D noise volume needs `sampler3D` and `image3D` to survive the
// DSL untouched. They were EXPECTED to: the sugar rewrites `Uniform(n) T name;` without looking at T,
// and storage-image format qualifiers are deliberately left as raw `layout(...)`. Expected, but never
// asserted — and "probably passes through" is not something to find out from a garbage render.
TEST( DShaderParser, PassesThrough3DSamplersAndStorageImages )
{
    const char* kVolume = R"(
Shader "Volume"
{
    Compute
    {
        Uniform(0) sampler3D u_ShapeNoise;
        Uniform sampler3D u_DetailNoise;
        layout(binding = 4, rgba8) restrict writeonly uniform image3D u_OutVolume;

        LocalSize(8, 8, 8);
        void main()
        {
            vec4 n = texture(u_ShapeNoise, vec3(0.5)) + texture(u_DetailNoise, vec3(0.5));
            imageStore(u_OutVolume, ivec3(gl_GlobalInvocationID), n);
        }
    }
}
)";
    auto        res     = DShaderParser::Parse( kVolume );
    ASSERT_TRUE( res.IsSuccess() ) << res.GetError();
    const auto& cs = res.GetValue().Stages.at( ShaderStage::Compute );

    EXPECT_NE( cs.find( "layout(binding = 0) uniform sampler3D u_ShapeNoise;" ), std::string::npos );

    // Auto-numbering shares ONE binding space with both the explicit Uniform(0) and the raw
    // layout(binding = 4), so the free slot here is 1 — a sampler3D takes part like any other.
    EXPECT_NE( cs.find( "layout(binding = 1) uniform sampler3D u_DetailNoise;" ), std::string::npos );

    // Format qualifiers are un-sugared by design: the raw storage-image line must survive verbatim,
    // format included — rewriting it would silently change the image's format.
    EXPECT_NE( cs.find( "layout(binding = 4, rgba8) restrict writeonly uniform image3D u_OutVolume;" ),
               std::string::npos );

    EXPECT_NE( cs.find( "layout(local_size_x = 8, local_size_y = 8, local_size_z = 8) in;" ), std::string::npos );
    EXPECT_EQ( cs.find( "Uniform(0)" ), std::string::npos );
}

// ─── Comments are prose, not declarations ────────────────────────────────────────────────────────
//
// The sugar rules matched the WHOLE file text, comments included, and the five paren-less keywords
// (In / Out / Uniform / Buffer / PushConstant) are ordinary English words. The live instance was
// Programs/Particles/ParticleSimulate.shader:
//
//     // Integrate alive particles. In LOCAL mode (u_Counts.w) the integrated state is the offset
//
// which the sugar rewrote into `... layout(location = 0) in LOCAL mode ...` and which really did
// consume location 0. It was harmless only because a COMPUTE stage declares no automatic In/Out, so
// the eaten number was never asked for. The first author to add `In`/`Out` to a stage whose prose
// happens to contain one of the five words would have got attributes shifted by one — and would have
// gone looking in the code, not in the paragraph above it.
//
// WHAT COUNTS AS A COMMENT here is exactly what GLSL says: `//` to end of line and `/* */`
// (non-nesting). Nothing else. `#if 0` is NOT a comment — see PreprocessorOffBranchIsStillCode.

static const char* kCommentTrapLocations = R"(
Shader "CommentTrap"
{
    Vertex
    {
        // Integrate alive particles. In LOCAL mode the state is an offset.
        /* Out of the pool comes exactly one entry. */
        In vec3 a_Position;
        Out vec2 v_UV;
        void main() { gl_Position = vec4( a_Position, 1.0 ); v_UV = vec2( 0.0 ); }
    }
    Fragment
    {
        In vec2 v_UV;
        Out vec4 o_Color;
        void main() { o_Color = vec4( v_UV, 0.0, 1.0 ); }
    }
}
)";

TEST( DShaderParserComments, ProseDoesNotConsumeAutoLocations )
{
    auto res = DShaderParser::Parse( kCommentTrapLocations );
    ASSERT_TRUE( res.IsSuccess() ) << res.GetError();
    const auto& v = res.GetValue().Stages.at( ShaderStage::Vertex );

    // Before the fix these were location 1, because the comments took 0 first.
    EXPECT_NE( v.find( "layout(location = 0) in vec3 a_Position" ), std::string::npos ) << v;
    EXPECT_NE( v.find( "layout(location = 0) out vec2 v_UV" ), std::string::npos ) << v;
}

TEST( DShaderParserComments, CommentTextSurvivesVerbatim )
{
    const std::string translated = DShaderParser::TranslateSugar( R"(
// The PushConstant carries the row index. In LOCAL mode the Buffer is an Out parameter.
/* A Uniform is a Buffer with a different name. */
In vec3 a_Position;
)" );

    EXPECT_NE( translated.find(
                    "// The PushConstant carries the row index. In LOCAL mode the Buffer is an Out parameter." ),
               std::string::npos )
         << translated;
    EXPECT_NE( translated.find( "/* A Uniform is a Buffer with a different name. */" ), std::string::npos )
         << translated;
    EXPECT_NE( translated.find( "layout(location = 0) in vec3 a_Position;" ), std::string::npos ) << translated;
}

TEST( DShaderParserComments, NumberedFormInAProseCommentNeitherRewritesNorReservesTheNumber )
{
    // Both halves matter. The comment must survive as written (it is documentation of the OLD
    // spelling), and it must not seed the allocator — before the fix `In(0)` here reserved location 0
    // and the real declaration below it was pushed to 1.
    const std::string translated = DShaderParser::TranslateSugar( R"(
// Until 2026-01-01 this was spelled In(0) vec3 a_Position; the number is now automatic.
In vec3 a_Position;
/* Uniform(3) sampler2D u_Tex; was here too. */
Uniform sampler2D u_Tex;
)" );

    EXPECT_NE( translated.find( "spelled In(0) vec3 a_Position;" ), std::string::npos ) << translated;
    EXPECT_NE( translated.find( "/* Uniform(3) sampler2D u_Tex; was here too. */" ), std::string::npos )
         << translated;
    EXPECT_NE( translated.find( "layout(location = 0) in vec3 a_Position;" ), std::string::npos ) << translated;
    EXPECT_NE( translated.find( "layout(binding = 0) uniform sampler2D u_Tex;" ), std::string::npos )
         << translated;
}

TEST( DShaderParserComments, ABlockCommentSpanningLinesIsSkippedWhole )
{
    const std::string translated = DShaderParser::TranslateSugar( R"(
/* Out
   In
   Buffer */
Out vec4 o_Color;
)" );
    EXPECT_NE( translated.find( "/* Out\n   In\n   Buffer */" ), std::string::npos ) << translated;
    EXPECT_NE( translated.find( "layout(location = 0) out vec4 o_Color;" ), std::string::npos ) << translated;
}

TEST( DShaderParserComments, CommentOpenersInsideEachOtherAreNotComments )
{
    // `/*` inside a line comment does not open a block, and `//` inside a block comment does not end
    // it. Getting either wrong would blank out real code and delete declarations silently.
    const std::string translated = DShaderParser::TranslateSugar( R"(
// a /* that never opens
In vec3 a_Position;
/* a // that never closes
   In vec3 a_NotDeclared; */
Out vec4 o_Color;
)" );
    EXPECT_NE( translated.find( "layout(location = 0) in vec3 a_Position;" ), std::string::npos ) << translated;
    EXPECT_NE( translated.find( "In vec3 a_NotDeclared; */" ), std::string::npos ) << translated;
    EXPECT_NE( translated.find( "layout(location = 0) out vec4 o_Color;" ), std::string::npos ) << translated;
}

TEST( DShaderParserComments, PreprocessorOffBranchIsStillCode )
{
    // The stated boundary: only GLSL comments are skipped. An `#if 0` branch is still translated,
    // and it still consumes a number — exactly as it did before this change. Asserted rather than
    // assumed, because the next person to hit it deserves to find the decision instead of the
    // symptom: text you want the sugar to ignore goes in a comment, not behind `#if 0`.
    const std::string translated = DShaderParser::TranslateSugar( R"(
#if 0
In vec3 a_Old;
#endif
In vec3 a_Position;
)" );
    EXPECT_NE( translated.find( "layout(location = 0) in vec3 a_Old;" ), std::string::npos ) << translated;
    EXPECT_NE( translated.find( "layout(location = 1) in vec3 a_Position;" ), std::string::npos ) << translated;
}

// MAT1b: a template's IMPORT CONTRACT. The mock is a two-property surface; the rows name source keys
// (`gltf.*`/`fbx.*`), the Property they feed and, for a texture, the source channels.
namespace
{
    std::string ImportMock( const std::string& importBody )
    {
        return R"(Shader "ImportMock"
{
    Domain Surface
    Import
    {
)" + importBody +
               R"(
    }
    Properties Binding(1) TextureBinding(2)
    {
        Color     AlbedoColor     ("Albedo") = (1, 1, 1, 1)
        Texture2D u_MetallicTexture ("Metallic")
    }
    Vertex
    {
        void main() { gl_Position = vec4(0.0); }
    }
    Fragment
    {
        layout( location = 0 ) out vec4 o_Color;
        void main() { o_Color = u_Material.AlbedoColor * texture( u_MetallicTexture, vec2(0.5) ); }
    }
}
)";
    }
} // namespace

TEST( DShaderImportContract, RowsAndRequiresReachTheManifest )
{
    const std::string src    = ImportMock( R"(        Requires "gltf.KHR_materials_unlit"
        "gltf.baseColorFactor" -> AlbedoColor;   // a value
        "gltf.metallicRoughnessTexture" -> u_MetallicTexture.b)" );
    const auto        parsed = DShaderParser::Parse( src );
    ASSERT_TRUE( parsed.IsSuccess() ) << parsed.GetError();

    const auto manifest = Common::Content::ReadShaderManifest( src );
    ASSERT_TRUE( manifest.IsSuccess() ) << manifest.GetError();
    const auto& m = manifest.GetValue();
    EXPECT_TRUE( m.DeclaresImport );
    ASSERT_EQ( m.ImportRequires.size(), 1u );
    EXPECT_EQ( m.ImportRequires[0], "gltf.KHR_materials_unlit" );
    ASSERT_EQ( m.Import.size(), 2u );
    EXPECT_EQ( m.Import[0].SourceKey, "gltf.baseColorFactor" );
    EXPECT_EQ( m.Import[0].Property, "AlbedoColor" );
    EXPECT_EQ( m.Import[0].Channels, "" );
    EXPECT_EQ( m.Import[1].Property, "u_MetallicTexture" );
    EXPECT_EQ( m.Import[1].Channels, "b" );
}

TEST( DShaderImportContract, AnUnknownPropertyIsAParseErrorNamingIt )
{
    const auto parsed = DShaderParser::Parse( ImportMock( R"(        "gltf.normalTexture" -> u_NormalTexture)" ) );
    ASSERT_FALSE( parsed.IsSuccess() );
    EXPECT_NE( parsed.GetError().find( "u_NormalTexture" ), std::string::npos ) << parsed.GetError();
    EXPECT_NE( parsed.GetError().find( "line 6" ), std::string::npos ) << parsed.GetError();
}

TEST( DShaderImportContract, OnlyATextureTakesSourceChannels )
{
    const auto parsed =
         DShaderParser::Parse( ImportMock( R"(        "gltf.baseColorFactor" -> AlbedoColor.rgb)" ) );
    ASSERT_FALSE( parsed.IsSuccess() );
    EXPECT_NE( parsed.GetError().find( "not a texture" ), std::string::npos ) << parsed.GetError();
}

TEST( DShaderImportContract, MalformedRowsAreRefused )
{
    for ( const char* row : { R"(        "baseColorTexture" -> u_MetallicTexture)", // no <source>.
                              R"(        "gltf.x" u_MetallicTexture)",              // no ->
                              R"(        "gltf.x" -> u_MetallicTexture.rr)",        // repeated channel
                              R"(        "gltf.x" -> u_MetallicTexture.xyz)" } )    // not rgba
    {
        EXPECT_FALSE( DShaderParser::Parse( ImportMock( row ) ).IsSuccess() ) << row;
        EXPECT_FALSE( Common::Content::ReadShaderManifest( ImportMock( row ) ).IsSuccess() ) << row;
    }
}

TEST( DShaderImportContract, AnUnclosedImportBlockIsRefusedByTheManifestReader )
{
    const auto manifest =
         Common::Content::ReadShaderManifest( "Shader \"X\"\n{\n    Import\n    {\n        \"gltf.a\" -> B\n" );
    ASSERT_FALSE( manifest.IsSuccess() );
    EXPECT_NE( manifest.GetError().find( "not closed" ), std::string::npos ) << manifest.GetError();
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

// MAT1s: `Sampler(WrapU, WrapV, Filter)` on a Texture2D is the template's sampling state; a property without
// it keeps Repeat/Repeat/Linear (the state every sampler had before), and a misspelling or a non-texture is
// refused by name at parse time.
TEST( DShaderParser, ATexturesSamplerAttributeIsTheTemplateStateAndABadOneIsRefused )
{
    const auto parse = []( const std::string& property )
    {
        return DShaderParser::Parse( R"(
Shader "SamplerProbe"
{
    Domain Surface
    Properties Binding(1) TextureBinding(2)
    {
        )" + property + R"(
        Texture2D u_Plain ("Plain")
    }
    Fragment { void main() {} }
}
)" );
    };

    auto res = parse( R"(Texture2D u_Slot ("Slot", Category("T"), Sampler(Clamp, Mirror, Nearest)) = "black")" );
    ASSERT_TRUE( res.IsSuccess() ) << res.GetError();
    const auto& params = res.GetValue().Meta.Params;
    ASSERT_EQ( params.size(), 2u );
    EXPECT_EQ( params[0].Sampler, ( SamplerState{ SamplerWrap::Clamp, SamplerWrap::Mirror, SamplerFilter::Nearest } ) );
    EXPECT_EQ( params[0].DefaultTexture, DefaultTextureKind::Black );
    EXPECT_EQ( params[1].Sampler, SamplerState{} ) << "no attribute: Repeat/Repeat/Linear";

    auto typo = parse( R"(Texture2D u_Slot ("Slot", Sampler(Clmap, Repeat, Linear)))" );
    ASSERT_FALSE( typo.IsSuccess() );
    EXPECT_NE( typo.GetError().find( "clmap" ), std::string::npos ) << typo.GetError();
    EXPECT_NE( typo.GetError().find( "u_Slot" ), std::string::npos ) << typo.GetError();

    auto notTexture = parse( R"(float u_F ("F", Sampler(Clamp, Clamp, Linear)) = 1.0)" );
    ASSERT_FALSE( notTexture.IsSuccess() );
    EXPECT_NE( notTexture.GetError().find( "not a Texture2D" ), std::string::npos ) << notTexture.GetError();
}
