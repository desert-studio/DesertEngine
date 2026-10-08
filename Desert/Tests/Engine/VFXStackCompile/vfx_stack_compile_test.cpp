// VFX-04. The emitter stack compiler (Engine/VFX/VFXStackCompiler): an emitter's module stack becomes one
// `Domain Particle` fragment. What is pinned here is what makes the design worth having:
//
//   - the text is a function of the stack's STRUCTURE only: the same stack gives the same bytes, a changed
//     VALUE gives the same text and key (a slider never rebuilds SPIR-V) and no value's digits are in it;
//   - any structural change (order, source, binding, enabled, a module's own source) moves the key;
//   - the attribute layout is UE's BuildLayout: per class (float / int32) prefix sums;
//   - the parameter buffer rows follow the slots; refusals name what is wrong;
//   - a Curve input (VFX-05) is one parameter row saying where its table sits in the system's curve atlas:
//     its keys are not in the text, and its time axis brings the Age / Lifetime attributes;
//   - the generated fragment, included by a host compute program that defines the contract's storage
//     functions, compiles through shaderc with the engine's own includer — for every module of the engine
//     library (VFX-06), in both GPU groups;
//   - every module call has its own random key, apart from every input slot.

#include "../../TestSupport/runner.hpp"
#include <gtest/gtest.h>

#include <Engine/Core/ShaderCompiler/DShader/DShaderParser.hpp>
#include <Engine/Core/ShaderCompiler/Includer/ShaderIncluder.hpp>
#include <Engine/VFX/VFXCurveLUT.hpp>
#include <Engine/VFX/VFXStackCompiler.hpp>

#include <Common/Core/Constants.hpp>

#include <shaderc/shaderc.hpp>

#include <algorithm>
#include <filesystem>

#include <format>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

namespace
{
    namespace S   = Desert::Assets::Serialization;
    namespace VFX = Desert::VFX;

    // The row stores a vec4 whatever the type, and the compiler refuses a non-zero component the type does not
    // use; every test value goes through this, so a scalar written as glm::vec4( x ) keeps only its x.
    glm::vec4 FitToType( S::VFXValueType type, glm::vec4 value )
    {
        for ( uint32_t c = S::ComponentCount( type ); c < 4; ++c )
            value[static_cast<glm::length_t>( c )] = 0.0f;
        return value;
    }

    S::VFXModuleInput ValueInput( std::string name, S::VFXValueType type, glm::vec4 value )
    {
        value = FitToType( type, value );
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
        in.Random = S::VFXRandomRange{ FitToType( type, min ), FitToType( type, max ) };
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

    // Falling sparks: spawn = local Init; update = Gravity, SolveForcesAndVelocity, UpdateAge (engine library).
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
                  { BindingInput( "SpeedLimit", S::VFXValueType::Float, "User.Limit" ) } ),
             Use( "engine:UpdateAge", {} ) };
        system.Emitters.push_back( emitter );
        return system;
    }

    VFX::VFXCompiledEmitter Compile( const S::VFXSystemData& system )
    {
        auto compiled = VFX::CompileEmitterStack( system, 0, VFX::EngineModuleDir() );
        EXPECT_TRUE( compiled.IsSuccess() ) << compiled.GetError();
        return compiled.IsSuccess() ? compiled.ExtractValue() : VFX::VFXCompiledEmitter{};
    }

    // The compiled fragment inside a host compute program that defines the contract's six storage functions;
    // empty when shaderc accepts it, else its message and the program.
    std::string HostCompileError( const VFX::VFXCompiledEmitter& compiled )
    {
        const auto parsed = Desert::Core::Preprocess::DShaderParser::Parse( compiled.ShaderText );
        if ( !parsed.IsSuccess() )
            return parsed.GetError();
        const std::string host = std::format(
             "#version 450\n"
             "layout( local_size_x = 64 ) in;\n"
             "layout( std430, set = 0, binding = 0 ) buffer Floats {{ float F[]; }};\n"
             "layout( std430, set = 0, binding = 1 ) buffer Ints {{ int I[]; }};\n"
             "layout( std430, set = 0, binding = 2 ) readonly buffer Params {{ vec4 P[]; }};\n"
             "layout( std430, set = 0, binding = 3 ) readonly buffer Curves {{ float C[]; }};\n"
             "layout( push_constant ) uniform Push {{ uint Count; uint Base; float Dt; uint Seed; }} pc;\n"
             "{}\n"
             "vec4 VFX_Param( uint slot ) {{ return P[pc.Base + slot]; }}\n"
             "float VFX_ReadFloat( uint particle, uint c ) {{ return F[c * pc.Count + particle]; }}\n"
             "int VFX_ReadInt( uint particle, uint c ) {{ return I[c * pc.Count + particle]; }}\n"
             "void VFX_WriteFloat( uint particle, uint c, float v ) {{ F[c * pc.Count + particle] = v; }}\n"
             "void VFX_WriteInt( uint particle, uint c, int v ) {{ I[c * pc.Count + particle] = v; }}\n"
             "float VFX_CurveLUT( uint index ) {{ return C[index]; }}\n"
             "void main()\n{{\n"
             "    const uint id = gl_GlobalInvocationID.x;\n"
             "    if ( id >= pc.Count ) return;\n"
             "    VFXSim sim;\n"
             "    sim.DeltaTime = pc.Dt; sim.EmitterAge = 0.0; sim.Seed = pc.Seed; sim.ParticleId = id;\n"
             "    sim.Step = 0u; sim.Spawned = id == 0u; sim.Kill = false;\n"
             "    VFX_SimulateParticle( id, sim );\n}}\n",
             parsed.GetValue().Meta.ParticleSource );

        const auto              path = VFX::EngineModuleDir() / "Gravity.shader";
        const shaderc::Compiler compiler;
        shaderc::CompileOptions options;
        options.SetIncluder( std::make_unique<Desert::Core::ShaderIncluder>( path ) );
        options.SetTargetEnvironment( shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_1 );
        options.SetWarningsAsErrors();
        const auto result =
             compiler.CompileGlslToSpv( host, shaderc_compute_shader, path.string().c_str(), options );
        if ( result.GetCompilationStatus() == shaderc_compilation_status_success )
            return {};
        return std::format( "{}\n{}", result.GetErrorMessage(), host );
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
    const std::ifstream in( path );
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
    names.reserve( compiled.Layout.Attributes.size() );
    for ( const auto& a : compiled.Layout.Attributes )
        names.push_back( a.Name );
    // Init + Gravity + Solve + UpdateAge declarations, plus Position from the Particles.Position binding.
    EXPECT_EQ( names,
               ( std::vector<std::string>{ "Age", "Alive", "Lifetime", "Mass", "PhysicsDrag", "PhysicsForce",
                                           "PhysicsRotationalDrag", "Position", "Spin", "Velocity" } ) );
    EXPECT_EQ( compiled.Layout.TotalFloatComponents, 1u + 1u + 1u + 1u + 3u + 1u + 3u + 3u );
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
    const auto atlas = VFX::BuildCurveAtlas( system );
    ASSERT_TRUE( atlas.IsSuccess() ) << atlas.GetError();
    const auto rows = VFX::BuildEmitterParams( after, system, 0, atlas.GetValue() );
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

    // A constant and a User.* parameter are both ONE parameter-buffer row read the same way: the program is
    // rightly the same and only the slot's filler (CPU side) differs. A Particles.* binding reads the attribute
    // instead, so that source change is structural.
    auto valueNotUser = Sparks();
    valueNotUser.Emitters[0].Stack.ParticleUpdate[1].Inputs[0] =
         ValueInput( "SpeedLimit", S::VFXValueType::Float, glm::vec4( 5000.0f, 0, 0, 0 ) );
    const auto valueRow = Compile( valueNotUser );
    EXPECT_EQ( valueRow.Key, base );
    EXPECT_EQ( valueRow.Slots.back().SlotKind, VFX::VFXParamSlot::Kind::Value );

    auto attributeNotUser = Sparks();
    attributeNotUser.Emitters[0].Stack.ParticleUpdate[1].Inputs[0] =
         BindingInput( "SpeedLimit", S::VFXValueType::Float, "Particles.Age" );
    moved( attributeNotUser, "input source: parameter row -> attribute" );

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

namespace
{
    // Sparks with the update Gravity driven by a curve over the particle's normalised age.
    S::VFXSystemData CurvedSparks( float z0, float z1 )
    {
        auto  system   = Sparks();
        auto& gravity  = system.Emitters[0].Stack.ParticleUpdate[0].Inputs[0];
        gravity.Source = S::VFXInputSource::Curve;
        gravity.Value.reset();
        const auto flat = []( float v ) { return std::vector<S::VFXCurveKey>{ S::VFXCurveKey{ 0.0f, v } }; };
        gravity.Curve   = std::vector<std::vector<S::VFXCurveKey>>{
             flat( 0.0f ), flat( 0.0f ), { S::VFXCurveKey{ 0.0f, z0 }, S::VFXCurveKey{ 1.0f, z1 } } };
        return system;
    }
} // namespace

// VFX-05: a curve input is a LUT row, its keys are data, its time axis is the normalised age.
TEST( VFXStackCompile, ACurveInputIsALUTRowAndItsKeysAreNotInTheText )
{
    auto       system = CurvedSparks( -123.5f, -987.75f );
    const auto before = Compile( system );
    for ( const char* digits : { "123.5", "987.75" } )
        EXPECT_EQ( before.ShaderText.find( digits ), std::string::npos ) << digits;
    // Spawn Life (Min, Max), Turns (Min, Max), then the curve's row 4.
    ASSERT_GE( before.Slots.size(), 5u );
    EXPECT_EQ( before.Slots[4].SlotKind, VFX::VFXParamSlot::Kind::Curve );
    EXPECT_NE( before.ShaderText.find( "VFX_CurveSample( 4u, VFX_NormalizedAge( p.Age, p.Lifetime ), 3u ).xyz" ),
               std::string::npos )
         << before.ShaderText;
    EXPECT_NE( before.Layout.Find( "Age" ), nullptr );

    // New key values, same program; and Value -> Curve is a structural change that moves it.
    auto& updateCurve = system.Emitters[0].Stack.ParticleUpdate[0].Inputs[0].Curve;
    if ( !updateCurve.has_value() )
        FAIL() << "the update input carries no curve";
    updateCurve->at( 2 ).at( 1 ).Value                                                = -5.0f;
    const auto after                                                                  = Compile( system );
    EXPECT_EQ( after.ShaderText, before.ShaderText );
    EXPECT_EQ( after.Key, before.Key );
    EXPECT_NE( Compile( Sparks() ).Key, before.Key );

    // The row says where the table is; reading the table at the end of life gives the new last key.
    const auto atlas = VFX::BuildCurveAtlas( system );
    ASSERT_TRUE( atlas.IsSuccess() ) << atlas.GetError();
    ASSERT_EQ( atlas.GetValue().Entries.size(), 1u );
    const auto& entry = atlas.GetValue().Entries[0].second;
    const auto  rows  = VFX::BuildEmitterParams( after, system, 0, atlas.GetValue() );
    ASSERT_TRUE( rows.IsSuccess() ) << rows.GetError();
    EXPECT_EQ( rows.GetValue()[4], VFX::CurveParamRow( entry ) );
    EXPECT_FLOAT_EQ( VFX::SampleCurveLUT( atlas.GetValue().Floats, entry, 1.0f ).z, -5.0f );

    // An atlas built from another system has no table for this input: an error, not a zero row.
    const auto other = VFX::BuildCurveAtlas( Sparks() );
    ASSERT_TRUE( other.IsSuccess() );
    const auto stale = VFX::BuildEmitterParams( after, system, 0, other.GetValue() );
    ASSERT_FALSE( stale.IsSuccess() );
    EXPECT_NE( stale.GetError().find( "curve atlas" ), std::string::npos ) << stale.GetError();
}

// The compiled fragment inside a host compute program that defines the contract's six storage functions, with
// and without a curve input.
TEST( VFXStackCompile, TheCompiledStackCompilesInsideAHostProgram )
{
    for ( const auto& system : { Sparks(), CurvedSparks( -1.0f, -2.0f ) } )
    {
        const auto compiled = Compile( system );
        EXPECT_EQ( HostCompileError( compiled ), "" );
    }
}

// VFX-06: every module of the engine library, its inputs given as Values, compiles in a stack — once in the
// spawn group and once in the update group — and the stack compiles inside the host program.
TEST( VFXStackCompile, EveryEngineModuleCompilesInsideAHostProgram )
{
    std::vector<std::filesystem::path> paths;
    for ( const auto& entry : std::filesystem::directory_iterator( VFX::EngineModuleDir() ) )
        if ( entry.path().extension() == ".shader" )
            paths.push_back( entry.path() );
    std::sort( paths.begin(), paths.end() );
    for ( const char* expected : { "ShapePoint", "ShapeSphere", "ShapeBox", "ShapeCone", "AddVelocityInCone",
                                   "InitializeLifetime", "UpdateAge", "Gravity", "SolveForcesAndVelocity", "Drag",
                                   "CurlNoiseForce", "InitializeColor", "ColorOverLife", "InitializeSpriteSize",
                                   "SizeOverLife", "InitializeRotation", "UpdateRotation", "SubUVAnimation" } )
        EXPECT_NE( std::find( paths.begin(), paths.end(),
                              VFX::EngineModuleDir() / std::format( "{}.shader", expected ) ),
                   paths.end() )
             << expected;

    for ( const auto& path : paths )
    {
        const std::ifstream in( path );
        std::ostringstream text;
        text << in.rdbuf();
        const auto module = VFX::ParseParticleModule( text.str(), path.string() );
        ASSERT_TRUE( module.IsSuccess() ) << module.GetError();

        std::vector<S::VFXModuleInput> inputs;
        for ( const auto& d : module.GetValue().Inputs )
            inputs.push_back( ValueInput( d.Name, d.Type, glm::vec4( 1.0f, 0, 0, 0 ) ) );
        const std::string name = std::format( "engine:{}", path.stem().string() );

        S::VFXSystemData  system;
        S::VFXEmitterData emitter;
        emitter.Name                 = path.stem().string();
        emitter.Stack.ParticleSpawn  = { Use( name, inputs ) };
        emitter.Stack.ParticleUpdate = { Use( name, inputs ) };
        system.Emitters.push_back( emitter );

        const auto compiled = VFX::CompileEmitterStack( system, 0, VFX::EngineModuleDir() );
        ASSERT_TRUE( compiled.IsSuccess() ) << name << ": " << compiled.GetError();
        EXPECT_EQ( HostCompileError( compiled.GetValue() ), "" ) << name;
    }
}

// Two calls of one module draw their own numbers: each call's key is its place in the stack with the top bit up,
// so it differs from the other call's and from every input slot (slots count from zero).
TEST( VFXStackCompile, EveryModuleCallHasItsOwnRandomKey )
{
    S::VFXSystemData  system;
    S::VFXEmitterData emitter;
    emitter.Name = "Two spheres";
    const auto sphere =
         Use( "engine:ShapeSphere", { ValueInput( "Radius", S::VFXValueType::Float, glm::vec4( 10.0f, 0, 0, 0 ) ),
                                      ValueInput( "Offset", S::VFXValueType::Vec3, glm::vec4( 0.0f ) ) } );
    emitter.Stack.ParticleSpawn  = { sphere, sphere };
    emitter.Stack.ParticleUpdate = { sphere };
    system.Emitters.push_back( emitter );
    const auto compiled = Compile( system );
    for ( const char* key :
          { "i.VFXModuleKey = 2147483648u;", "i.VFXModuleKey = 2147483664u;", "i.VFXModuleKey = 2147549184u;" } )
        EXPECT_NE( compiled.ShaderText.find( key ), std::string::npos ) << key << "\n" << compiled.ShaderText;
}

// VFX-06b: each force / over-life module writes the attributes the solver and the sprite renderer (VFX-08) read,
// with the types they read them as — a module that drifted to another name or type would compile and feed nobody.
TEST( VFXStackCompile, EveryOverLifeAndForceModuleWritesItsAttributes )
{
    using T = S::VFXValueType;
    struct Row
    {
        const char*                            Module;
        std::vector<std::pair<std::string, T>> Writes;
    };
    const std::vector<Row> rows = {
         { "Drag", { { "PhysicsDrag", T::Float }, { "PhysicsRotationalDrag", T::Float } } },
         { "CurlNoiseForce", { { "PhysicsForce", T::Vec3 }, { "Position", T::Vec3 } } },
         { "InitializeColor", { { "Color", T::Vec4 }, { "InitialColor", T::Vec4 } } },
         { "ColorOverLife", { { "Color", T::Vec4 }, { "InitialColor", T::Vec4 } } },
         { "InitializeSpriteSize", { { "SpriteSize", T::Vec2 }, { "InitialSpriteSize", T::Vec2 } } },
         { "SizeOverLife", { { "SpriteSize", T::Vec2 }, { "InitialSpriteSize", T::Vec2 } } },
         { "InitializeRotation", { { "SpriteRotation", T::Float }, { "SpriteRotationRate", T::Float } } },
         { "UpdateRotation", { { "SpriteRotation", T::Float }, { "SpriteRotationRate", T::Float } } },
         { "SubUVAnimation", { { "SubImageIndex", T::Float }, { "Age", T::Float }, { "Lifetime", T::Float } } },
         { "SolveForcesAndVelocity", { { "PhysicsDrag", T::Float }, { "PhysicsRotationalDrag", T::Float } } } };
    for ( const Row& row : rows )
    {
        const auto         path = VFX::EngineModuleDir() / std::format( "{}.shader", row.Module );
        const std::ifstream in( path );
        std::ostringstream text;
        text << in.rdbuf();
        const auto module = VFX::ParseParticleModule( text.str(), path.string() );
        ASSERT_TRUE( module.IsSuccess() ) << row.Module << ": " << module.GetError();

        std::vector<S::VFXModuleInput> inputs;
        for ( const auto& d : module.GetValue().Inputs )
            inputs.push_back( ValueInput( d.Name, d.Type, glm::vec4( 1.0f ) ) );
        S::VFXSystemData  system;
        S::VFXEmitterData emitter;
        emitter.Name                 = row.Module;
        emitter.Stack.ParticleUpdate = { Use( std::string( "engine:" ) + row.Module, inputs ) };
        system.Emitters.push_back( emitter );
        const auto compiled = Compile( system );
        for ( const auto& [name, type] : row.Writes )
        {
            const auto* attribute = compiled.Layout.Find( name );
            ASSERT_NE( attribute, nullptr ) << row.Module << " does not declare " << name;
            EXPECT_EQ( attribute->FloatCount, S::ComponentCount( type ) ) << row.Module << " " << name;
            EXPECT_NE( compiled.ShaderText.find( std::format( "p.{}", name ) ), std::string::npos )
                 << row.Module << " " << name;
        }
    }
}

// The over-life inputs are curves over the normalised age: Scale of ColorOverLife (vec4), of SizeOverLife (vec2)
// and RateScale of UpdateRotation (float) each read their LUT row, and the stack compiles inside the host program.
TEST( VFXStackCompile, OverLifeScalesAreCurvesOverTheNormalisedAge )
{
    const auto curve = []( std::string name, S::VFXValueType type, std::size_t channels )
    {
        S::VFXModuleInput in;
        in.Name   = std::move( name );
        in.Type   = type;
        in.Source = S::VFXInputSource::Curve;
        in.Curve  = std::vector<std::vector<S::VFXCurveKey>>(
             channels, { S::VFXCurveKey{ 0.0f, 1.0f }, S::VFXCurveKey{ 1.0f, 0.0f } } );
        return in;
    };
    S::VFXSystemData  system;
    S::VFXEmitterData emitter;
    emitter.Name                = "Puffs";
    emitter.Stack.ParticleSpawn = {
         Use( "engine:InitializeLifetime",
              { ValueInput( "Lifetime", S::VFXValueType::Float, glm::vec4( 2.0f ) ) } ),
         Use( "engine:InitializeColor", { ValueInput( "Color", S::VFXValueType::Vec4, glm::vec4( 1.0f ) ) } ),
         Use( "engine:InitializeSpriteSize", { ValueInput( "Size", S::VFXValueType::Vec2, glm::vec4( 20.0f ) ) } ),
         Use( "engine:InitializeRotation",
              { RandomInput( "Rotation", S::VFXValueType::Float, glm::vec4( 0.0f ), glm::vec4( 360.0f ) ),
                ValueInput( "RotationRate", S::VFXValueType::Float, glm::vec4( 90.0f ) ) } ) };
    emitter.Stack.ParticleUpdate = {
         Use( "engine:ColorOverLife", { curve( "Scale", S::VFXValueType::Vec4, 4 ) } ),
         Use( "engine:SizeOverLife", { curve( "Scale", S::VFXValueType::Vec2, 2 ) } ),
         Use( "engine:Drag", { ValueInput( "Drag", S::VFXValueType::Float, glm::vec4( 0.5f ) ),
                               ValueInput( "RotationalDrag", S::VFXValueType::Float, glm::vec4( 0.25f ) ) } ),
         Use( "engine:UpdateRotation", { curve( "RateScale", S::VFXValueType::Float, 1 ) } ),
         Use( "engine:SolveForcesAndVelocity",
              { ValueInput( "SpeedLimit", S::VFXValueType::Float, glm::vec4( 0.0f ) ) } ),
         Use( "engine:UpdateAge", {} ) };
    system.Emitters.push_back( emitter );
    const auto compiled = Compile( system );
    for ( const char* sample :
          { "VFX_NormalizedAge( p.Age, p.Lifetime ), 4u )", "VFX_NormalizedAge( p.Age, p.Lifetime ), 2u ).xy",
            "VFX_NormalizedAge( p.Age, p.Lifetime ), 1u ).x" } )
        EXPECT_NE( compiled.ShaderText.find( sample ), std::string::npos ) << sample << "\n"
                                                                           << compiled.ShaderText;
    EXPECT_EQ( HostCompileError( compiled ), "" );
}

namespace
{
    // The module shaders are read from the engine directory, before gtest parses its flags.
    const Desert::TestSupport::SuiteHost kHost{ { .EngineDir = true } };
} // namespace
