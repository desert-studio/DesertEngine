// VFX-04. The emitter stack compiler (Engine/VFX/VFXStackCompiler): an emitter's module stack becomes one
// `Domain Particle` fragment. What is pinned here is what makes the design worth having:
//
//   - the text is a function of the stack's STRUCTURE only: the same stack gives the same bytes, a changed
//     VALUE gives the same text and key (a slider never rebuilds SPIR-V) and no value's digits are in it;
//   - any structural change (order, source, binding, enabled, a module's own source) moves the key;
//   - the attribute layout is UE's BuildLayout: per class (float / int32) prefix sums;
//   - the parameter buffer rows follow the slots; refusals name what is wrong;
//   - the generated fragment, included by a host compute program that defines the contract's storage
//     functions, compiles through shaderc with the engine's own includer.

#include "../../TestSupport/engine_dir.hpp"
#include <gtest/gtest.h>

#include <Engine/Core/ShaderCompiler/DShader/DShaderParser.hpp>
#include <Engine/Core/ShaderCompiler/Includer/ShaderIncluder.hpp>
#include <Engine/VFX/VFXStackCompiler.hpp>

#include <Common/Core/Constants.hpp>

#include <shaderc/shaderc.hpp>

#include <format>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

namespace
{
    namespace S   = Desert::Assets::Serialization;
    namespace VFX = Desert::VFX;

    S::VFXModuleInput ValueInput( std::string name, S::VFXValueType type, glm::vec4 value )
    {
        S::VFXModuleInput in;
        in.Name   = std::move( name );
        in.Type   = type;
        in.Source = S::VFXInputSource::Value;
        in.Value  = value;
        return in;
    }

    S::VFXModuleInput BindingInput( std::string name, S::VFXValueType type, std::string binding )
    {
        S::VFXModuleInput in;
        in.Name    = std::move( name );
        in.Type    = type;
        in.Source  = S::VFXInputSource::Binding;
        in.Binding = std::move( binding );
        return in;
    }

    S::VFXModuleInput RandomInput( std::string name, S::VFXValueType type, glm::vec4 min, glm::vec4 max )
    {
        S::VFXModuleInput in;
        in.Name   = std::move( name );
        in.Type   = type;
        in.Source = S::VFXInputSource::Random;
        in.Random = S::VFXRandomRange{ min, max };
        return in;
    }

    S::VFXModuleUse Use( std::string module, std::vector<S::VFXModuleInput> inputs )
    {
        S::VFXModuleUse use;
        use.Module = std::move( module );
        use.Inputs = std::move( inputs );
        return use;
    }

    // A local (scratch pad) module: a lifetime from its input, a start velocity bound to the position, a bool.
    const char* kInitSource = "Shader \"VFX/Local/Init\"\n{\n    Domain Particle\n    Particle\n    {\n"
                              "        // the declarations come first\n"
                              "        Attribute Lifetime float\n"
                              "        Attribute Velocity vec3\n"
                              "        Attribute Alive bool\n"
                              "        Attribute Spin int\n"
                              "        Input Life float\n"
                              "        Input Origin vec3\n"
                              "        Input Turns int\n"
                              "        void Module( inout ParticleCtx p, inout VFXSim sim, in ModuleInputs i )\n"
                              "        {\n"
                              "            p.Lifetime = i.Life;\n"
                              "            p.Velocity = i.Origin * 0.0;\n"
                              "            p.Alive = true;\n"
                              "            p.Spin = i.Turns;\n"
                              "        }\n    }\n}\n";

    // Falling sparks: spawn = local Init; update = Gravity, SolveForcesAndVelocity (engine library).
    S::VFXSystemData Sparks()
    {
        S::VFXSystemData system;
        system.UserParams.push_back( { "Limit", S::VFXValueType::Float, glm::vec4( 5000.0f, 0, 0, 0 ) } );
        system.LocalModules.push_back( { "Init", "Init", kInitSource } );
        S::VFXEmitterData emitter;
        emitter.Name                = "Sparks";
        emitter.Stack.ParticleSpawn = {
             Use( "local:Init", { RandomInput( "Life", S::VFXValueType::Float, glm::vec4( 1.25f, 0, 0, 0 ),
                                               glm::vec4( 2.75f, 0, 0, 0 ) ),
                                  BindingInput( "Origin", S::VFXValueType::Vec3, "Particles.Position" ),
                                  RandomInput( "Turns", S::VFXValueType::Int, glm::vec4( 2, 0, 0, 0 ),
                                               glm::vec4( 7, 0, 0, 0 ) ) } ) };
        emitter.Stack.ParticleUpdate = {
             Use( "engine:Gravity",
                  { ValueInput( "Gravity", S::VFXValueType::Vec3, glm::vec4( 0, 0, -981.25f, 0 ) ) } ),
             Use( "engine:SolveForcesAndVelocity",
                  { BindingInput( "SpeedLimit", S::VFXValueType::Float, "User.Limit" ) } ) };
        system.Emitters.push_back( emitter );
        return system;
    }

    VFX::VFXCompiledEmitter Compile( const S::VFXSystemData& system )
    {
        auto compiled = VFX::CompileEmitterStack( system, 0, VFX::EngineModuleDir() );
        EXPECT_TRUE( compiled.IsSuccess() ) << compiled.GetError();
        return compiled.IsSuccess() ? compiled.ExtractValue() : VFX::VFXCompiledEmitter{};
    }

    std::string Refusal( const S::VFXSystemData& system )
    {
        const auto compiled = VFX::CompileEmitterStack( system, 0, VFX::EngineModuleDir() );
        EXPECT_FALSE( compiled.IsSuccess() );
        return compiled.IsSuccess() ? std::string{} : compiled.GetError();
    }
} // namespace

TEST( VFXStackCompile, AnEngineModuleParsesIntoDeclarationsAndBody )
{
    const auto         path = VFX::EngineModuleDir() / "Gravity.shader";
    std::ifstream      in( path );
    std::ostringstream text;
    text << in.rdbuf();
    const auto module = VFX::ParseParticleModule( text.str(), path.string() );
    ASSERT_TRUE( module.IsSuccess() ) << module.GetError();
    const auto& m = module.GetValue();
    EXPECT_EQ( m.Attributes, ( std::vector<VFX::VFXModuleDecl>{ { "PhysicsForce", S::VFXValueType::Vec3 },
                                                                { "Mass", S::VFXValueType::Float } } ) );
    EXPECT_EQ( m.Inputs, ( std::vector<VFX::VFXModuleDecl>{ { "Gravity", S::VFXValueType::Vec3 } } ) );
    EXPECT_NE( m.Body.find( "void Module(" ), std::string::npos );
    EXPECT_EQ( m.Body.find( "Attribute" ), std::string::npos );
}

// UE FNiagaraDataSetCompiledData::BuildLayout: each class's start is the running total of that class.
TEST( VFXStackCompile, TheLayoutIsPerClassPrefixSums )
{
    const auto layout = VFX::BuildLayout( { { "A", S::VFXValueType::Vec3 },
                                            { "B", S::VFXValueType::Int },
                                            { "C", S::VFXValueType::Float },
                                            { "D", S::VFXValueType::Bool },
                                            { "E", S::VFXValueType::Vec2 } } );
    ASSERT_EQ( layout.Attributes.size(), 5u );
    const auto row = [&]( std::size_t i ) { return layout.Attributes[i]; };
    EXPECT_EQ( row( 0 ).FloatStart, 0u );
    EXPECT_EQ( row( 0 ).FloatCount, 3u );
    EXPECT_EQ( row( 1 ).IntStart, 0u );
    EXPECT_EQ( row( 1 ).IntCount, 1u );
    EXPECT_EQ( row( 1 ).FloatCount, 0u );
    EXPECT_EQ( row( 2 ).FloatStart, 3u );
    EXPECT_EQ( row( 3 ).IntStart, 1u );
    EXPECT_EQ( row( 4 ).FloatStart, 4u );
    EXPECT_EQ( row( 4 ).FloatCount, 2u );
    EXPECT_EQ( layout.TotalFloatComponents, 6u );
    EXPECT_EQ( layout.TotalIntComponents, 2u );
}

TEST( VFXStackCompile, TheStackDerivesItsAttributesSortedByName )
{
    const auto               compiled = Compile( Sparks() );
    std::vector<std::string> names;
    for ( const auto& a : compiled.Layout.Attributes )
        names.push_back( a.Name );
    // Init + Gravity + Solve declarations, plus Position from the Particles.Position binding.
    EXPECT_EQ( names, ( std::vector<std::string>{ "Age", "Alive", "Lifetime", "Mass", "PhysicsDrag",
                                                  "PhysicsForce", "Position", "Spin", "Velocity" } ) );
    EXPECT_EQ( compiled.Layout.TotalFloatComponents, 1u + 1u + 1u + 1u + 3u + 3u + 3u );
    EXPECT_EQ( compiled.Layout.TotalIntComponents, 2u );
}

TEST( VFXStackCompile, TheTextIsDeterministicAndParsesAsAParticleFragment )
{
    const auto a = Compile( Sparks() );
    const auto b = Compile( Sparks() );
    EXPECT_EQ( a.ShaderText, b.ShaderText );
    EXPECT_EQ( a.Key, b.Key );
    EXPECT_EQ( a.ShaderName, std::format( "VFX/Emitter/{:016x}", a.Key ) );

    const auto parsed = Desert::Core::Preprocess::DShaderParser::Parse( a.ShaderText );
    ASSERT_TRUE( parsed.IsSuccess() ) << parsed.GetError() << "\n" << a.ShaderText;
    EXPECT_EQ( parsed.GetValue().Meta.Domain, Desert::Core::Formats::ShaderDomain::Particle );
    EXPECT_NE( parsed.GetValue().Meta.ParticleSource.find( "void VFX_SimulateParticle(" ), std::string::npos );
}

TEST( VFXStackCompile, AValueIsNeverInTheTextAndChangingItKeepsTheKey )
{
    auto       system = Sparks();
    const auto before = Compile( system );
    for ( const char* digits : { "981.25", "1.25", "2.75", "5000" } )
        EXPECT_EQ( before.ShaderText.find( digits ), std::string::npos ) << digits;

    system.Emitters[0].Stack.ParticleUpdate[0].Inputs[0].Value = glm::vec4( 0, 0, -50.0f, 0 );
    system.Emitters[0].Stack.ParticleSpawn[0].Inputs[0].Random =
         S::VFXRandomRange{ glm::vec4( 3, 0, 0, 0 ), glm::vec4( 4, 0, 0, 0 ) };
    system.UserParams[0].Default = glm::vec4( 10.0f, 0, 0, 0 );
    const auto after             = Compile( system );
    EXPECT_EQ( after.ShaderText, before.ShaderText );
    EXPECT_EQ( after.Key, before.Key );

    // The values live in the parameter buffer, row by row in slot order: spawn Life (Min, Max), Turns (Min, Max),
    // update Gravity, User.Limit.
    ASSERT_EQ( after.Slots.size(), 6u );
    const auto rows = VFX::BuildEmitterParams( after, system, 0 );
    ASSERT_TRUE( rows.IsSuccess() ) << rows.GetError();
    EXPECT_EQ( rows.GetValue()[0], glm::vec4( 3, 0, 0, 0 ) );
    EXPECT_EQ( rows.GetValue()[1], glm::vec4( 4, 0, 0, 0 ) );
    EXPECT_EQ( rows.GetValue()[2], glm::vec4( 2, 0, 0, 0 ) );
    EXPECT_EQ( rows.GetValue()[3], glm::vec4( 7, 0, 0, 0 ) );
    EXPECT_EQ( rows.GetValue()[4], glm::vec4( 0, 0, -50.0f, 0 ) );
    EXPECT_EQ( rows.GetValue()[5], glm::vec4( 10.0f, 0, 0, 0 ) );
    EXPECT_EQ( after.Slots[5].SlotKind, VFX::VFXParamSlot::Kind::User );
    EXPECT_EQ( after.Slots[5].User, "Limit" );
}

TEST( VFXStackCompile, EveryStructuralChangeMovesTheKey )
{
    const auto         base = Compile( Sparks() ).Key;
    std::set<uint64_t> keys{ base };
    const auto         moved = [&]( const S::VFXSystemData& system, const char* what )
    {
        const auto key = Compile( system ).Key;
        EXPECT_NE( key, base ) << what;
        EXPECT_TRUE( keys.insert( key ).second ) << what << " collides with another change";
    };

    auto reordered = Sparks();
    std::swap( reordered.Emitters[0].Stack.ParticleUpdate[0], reordered.Emitters[0].Stack.ParticleUpdate[1] );
    moved( reordered, "module order" );

    auto valueNotBinding = Sparks();
    valueNotBinding.Emitters[0].Stack.ParticleUpdate[1].Inputs[0] =
         ValueInput( "SpeedLimit", S::VFXValueType::Float, glm::vec4( 5000.0f, 0, 0, 0 ) );
    moved( valueNotBinding, "input source" );

    auto rebound                                                 = Sparks();
    rebound.Emitters[0].Stack.ParticleSpawn[0].Inputs[1].Binding = "Particles.Velocity";
    moved( rebound, "binding" );

    auto disabled                                        = Sparks();
    disabled.Emitters[0].Stack.ParticleUpdate[0].Enabled = false;
    moved( disabled, "enabled" );

    auto edited                   = Sparks();
    edited.LocalModules[0].Source = std::string( kInitSource );
    const auto at                 = edited.LocalModules[0].Source.find( "p.Alive = true;" );
    edited.LocalModules[0].Source.replace( at, 15, "p.Alive = false;" );
    moved( edited, "local module source" );
}

TEST( VFXStackCompile, RefusalsNameWhatIsWrong )
{
    auto missing                                       = Sparks();
    missing.Emitters[0].Stack.ParticleUpdate[0].Module = "engine:NoSuchModule";
    EXPECT_NE( Refusal( missing ).find( "NoSuchModule.shader" ), std::string::npos );

    auto unknown = Sparks();
    unknown.Emitters[0].Stack.ParticleUpdate[0].Inputs.push_back(
         ValueInput( "Wind", S::VFXValueType::Vec3, glm::vec4( 0.0f ) ) );
    EXPECT_NE( Refusal( unknown ).find( "'Wind': the module declares no such input" ), std::string::npos );

    auto absent = Sparks();
    absent.Emitters[0].Stack.ParticleUpdate[0].Inputs.clear();
    EXPECT_NE( Refusal( absent ).find( "input 'Gravity' is declared by the module and not given" ),
               std::string::npos );

    auto mistyped = Sparks();
    mistyped.Emitters[0].Stack.ParticleUpdate[0].Inputs[0] =
         ValueInput( "Gravity", S::VFXValueType::Float, glm::vec4( 1, 0, 0, 0 ) );
    EXPECT_NE( Refusal( mistyped ).find( "the row says float, the module declares vec3" ), std::string::npos );

    auto  curve = Sparks();
    auto& life  = curve.Emitters[0].Stack.ParticleSpawn[0].Inputs[0];
    life.Source = S::VFXInputSource::Curve;
    life.Random.reset();
    life.Curve = std::vector<std::vector<S::VFXCurveKey>>{ { S::VFXCurveKey{} } };
    EXPECT_NE( Refusal( curve ).find( "VFX-05" ), std::string::npos );

    // Two modules disagreeing about an attribute's type.
    auto clash                   = Sparks();
    clash.LocalModules[0].Source = std::string( kInitSource );
    const auto at                = clash.LocalModules[0].Source.find( "Attribute Velocity vec3" );
    clash.LocalModules[0].Source.replace( at, 23, "Attribute Mass vec2" );
    const auto clashError = Refusal( clash );
    EXPECT_NE( clashError.find( "attribute 'Mass'" ), std::string::npos ) << clashError;

    auto empty              = Sparks();
    empty.Emitters[0].Stack = {};
    EXPECT_NE( Refusal( empty ).find( "no particle attribute" ), std::string::npos );
}

// The compiled fragment inside a host compute program that defines the contract's five storage functions.
TEST( VFXStackCompile, TheCompiledStackCompilesInsideAHostProgram )
{
    const auto compiled = Compile( Sparks() );
    const auto parsed   = Desert::Core::Preprocess::DShaderParser::Parse( compiled.ShaderText );
    ASSERT_TRUE( parsed.IsSuccess() ) << parsed.GetError();

    const std::string host = std::format(
         "#version 450\n"
         "layout( local_size_x = 64 ) in;\n"
         "layout( std430, set = 0, binding = 0 ) buffer Floats {{ float F[]; }};\n"
         "layout( std430, set = 0, binding = 1 ) buffer Ints {{ int I[]; }};\n"
         "layout( std430, set = 0, binding = 2 ) readonly buffer Params {{ vec4 P[]; }};\n"
         "layout( push_constant ) uniform Push {{ uint Count; uint Base; float Dt; uint Seed; }} pc;\n"
         "{}\n"
         "vec4 VFX_Param( uint slot ) {{ return P[pc.Base + slot]; }}\n"
         "float VFX_ReadFloat( uint particle, uint c ) {{ return F[c * pc.Count + particle]; }}\n"
         "int VFX_ReadInt( uint particle, uint c ) {{ return I[c * pc.Count + particle]; }}\n"
         "void VFX_WriteFloat( uint particle, uint c, float v ) {{ F[c * pc.Count + particle] = v; }}\n"
         "void VFX_WriteInt( uint particle, uint c, int v ) {{ I[c * pc.Count + particle] = v; }}\n"
         "void main()\n{{\n"
         "    const uint id = gl_GlobalInvocationID.x;\n"
         "    if ( id >= pc.Count ) return;\n"
         "    VFXSim sim;\n"
         "    sim.DeltaTime = pc.Dt; sim.EmitterAge = 0.0; sim.Seed = pc.Seed; sim.ParticleId = id;\n"
         "    sim.Step = 0u; sim.Spawned = id == 0u; sim.Kill = false;\n"
         "    VFX_SimulateParticle( id, sim );\n}}\n",
         parsed.GetValue().Meta.ParticleSource );

    const auto              path = VFX::EngineModuleDir() / "Gravity.shader";
    shaderc::Compiler       compiler;
    shaderc::CompileOptions options;
    options.SetIncluder( std::make_unique<Desert::Core::ShaderIncluder>( path ) );
    options.SetTargetEnvironment( shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_1 );
    options.SetWarningsAsErrors();
    const auto result = compiler.CompileGlslToSpv( host, shaderc_compute_shader, path.string().c_str(), options );
    EXPECT_EQ( result.GetCompilationStatus(), shaderc_compilation_status_success )
         << result.GetErrorMessage() << "\n"
         << host;
}

int main( int argc, char** argv )
{
    Desert::TestSupport::SetSuiteEngineDir();
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
