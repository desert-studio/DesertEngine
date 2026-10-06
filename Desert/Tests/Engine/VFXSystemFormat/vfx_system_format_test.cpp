// VFX-02. The `.dfx` file (UE UNiagaraSystem with embedded emitters): what it keeps, what it refuses, its GUID
// across rewrites, and the category being an id of the PROJECT's register file — read from disk, so a
// category added to the file is accepted without a rebuild.

#include <gtest/gtest.h>

#include <Engine/Assets/Serialization/VFXSystem.hpp>

#include <Common/Core/Serialization/GlmReflection.hpp>
#include <Common/Json/Json.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace
{
    using namespace Desert::Assets::Serialization;
    namespace fs = std::filesystem;

    VFXCategoryRegister Register()
    {
        VFXCategoryRegister reg;
        reg.Categories.push_back( { "Fire", "Fire", glm::vec4( 1.0f, 0.4f, 0.1f, 1.0f ) } );
        return reg;
    }

    VFXSystemData Campfire()
    {
        VFXSystemData data;
        data.Category = "Fire";
        data.Tags     = { "campfire", "loop" };
        data.Duration = 2.0f;
        data.Seed     = 1234;
        data.Bounds   = { glm::vec3( -50.0f, 0.0f, -50.0f ), glm::vec3( 50.0f, 300.0f, 50.0f ) };
        data.UserParams.push_back( { "Intensity", VFXValueType::Float, glm::vec4( 1.0f, 0.0f, 0.0f, 0.0f ) } );
        data.LocalModules.push_back( { "flicker", "Flicker", "void Mod_Flicker( inout ParticleCtx p ) {}" } );

        VFXEmitterData flames;
        flames.Name      = "Flames";
        flames.Capacity  = 512;
        flames.Lifecycle = { 0.25f, 2.0f, VFXLoopBehavior::Multiple, 3 };

        VFXModuleUse   spawn{ "engine:SpawnRate", true, {} };
        VFXModuleInput rate{ "Rate", VFXValueType::Float, VFXInputSource::Binding };
        rate.Binding = "User.Intensity";
        spawn.Inputs.push_back( rate );
        flames.Stack.EmitterUpdate.push_back( spawn );

        VFXModuleUse   init{ "engine:InitializeParticle", true, {} };
        VFXModuleInput life{ "Lifetime", VFXValueType::Float, VFXInputSource::Random };
        life.Random = VFXRandomRange{ glm::vec4( 0.5f, 0, 0, 0 ), glm::vec4( 1.5f, 0, 0, 0 ) };
        init.Inputs.push_back( life );
        VFXModuleInput color{ "Color", VFXValueType::Vec4, VFXInputSource::Value };
        color.Value = glm::vec4( 1.0f, 0.5f, 0.1f, 1.0f );
        init.Inputs.push_back( color );
        flames.Stack.ParticleSpawn.push_back( init );

        VFXModuleUse                   scale{ "engine:ScaleSize", true, {} };
        VFXModuleInput                 curve{ "Scale", VFXValueType::Vec2, VFXInputSource::Curve };
        const std::vector<VFXCurveKey> channel = {
             { 0.0f, 1.0f, 0.0f, 0.0f, Desert::Animation::KeyInterp::Cubic, Desert::Animation::TangentMode::User },
             { 1.0f, 0.0f, -2.0f, -2.0f, Desert::Animation::KeyInterp::Linear,
               Desert::Animation::TangentMode::Break } };
        curve.Curve = std::vector<std::vector<VFXCurveKey>>{ channel, channel };
        scale.Inputs.push_back( curve );
        flames.Stack.ParticleUpdate.push_back( scale );
        flames.Stack.ParticleUpdate.push_back( { "local:flicker", false, {} } );
        flames.Renderers.push_back( { VFXRendererKind::Sprite, true } );
        data.Emitters.push_back( flames );
        return data;
    }

    std::string Refusal( const VFXSystemData& data )
    {
        const auto parsed = ParseVFXSystem( WriteVFXSystem( data ), Register() );
        return parsed ? std::string( "<accepted>" ) : parsed.GetError();
    }
} // namespace

TEST( VFXSystemFormat, RoundTripKeepsEveryField )
{
    auto parsed = ParseVFXSystem( WriteVFXSystem( Campfire() ), Register() );
    ASSERT_TRUE( parsed ) << parsed.GetError();
    VFXSystemData read = parsed.GetValue();
    ASSERT_TRUE( read.Header.has_value() );
    EXPECT_EQ( read.Header->Kind, "VFXSystem" );
    EXPECT_FALSE( read.Header->Guid.empty() );
    read.Header.reset();
    EXPECT_EQ( read, Campfire() );
}

TEST( VFXSystemFormat, GuidIsKeptWhenTheFileIsWrittenAgain )
{
    auto first = ParseVFXSystem( WriteVFXSystem( Campfire() ), Register() );
    ASSERT_TRUE( first ) << first.GetError();
    VFXSystemData edited = first.GetValue();
    edited.Seed          = 99;
    auto again           = ParseVFXSystem( WriteVFXSystem( edited ), Register() );
    ASSERT_TRUE( again ) << again.GetError();
    EXPECT_EQ( again.GetValue().Header->Guid, first.GetValue().Header->Guid );
    EXPECT_EQ( again.GetValue().Seed, 99u );
}

TEST( VFXSystemFormat, RefusesABrokenFile )
{
    EXPECT_FALSE( ParseVFXSystem( "", Register() ) );
    EXPECT_FALSE( ParseVFXSystem( "{\"Header\":", Register() ) );
    EXPECT_FALSE( ParseVFXSystem( Common::Json::Write( Campfire() ), Register() ) ) << "no header";
    const std::string text = WriteVFXSystem( Campfire() );
    EXPECT_FALSE( ParseVFXSystem( text.substr( 0, text.size() / 2 ), Register() ) ) << "truncated";
}

TEST( VFXSystemFormat, RefusesWhatNoStageCouldHonour )
{
    VFXSystemData d = Campfire();
    d.Emitters[0].Stack.EmitterUpdate[0].Inputs[0].Binding = "User.Missing";
    EXPECT_NE( Refusal( d ), "<accepted>" );

    d = Campfire();
    d.Emitters[0].Stack.ParticleUpdate[1].Module = "local:nowhere";
    EXPECT_NE( Refusal( d ), "<accepted>" );

    d = Campfire();
    d.Emitters[0].Stack.ParticleSpawn[0].Inputs[1].Curve = std::vector<std::vector<VFXCurveKey>>{};
    EXPECT_NE( Refusal( d ), "<accepted>" ) << "two sources on one input";

    d = Campfire();
    d.Emitters[0].Stack.ParticleUpdate[0].Inputs[0].Curve->pop_back();
    EXPECT_NE( Refusal( d ), "<accepted>" ) << "a Vec2 curve needs two channels";

    d = Campfire();
    d.Emitters.push_back( d.Emitters[0] );
    EXPECT_NE( Refusal( d ), "<accepted>" ) << "two emitters of one name";

    d = Campfire();
    d.Emitters[0].Capacity = 0;
    EXPECT_NE( Refusal( d ), "<accepted>" );
}

TEST( VFXSystemFormat, AnUnknownCategoryIsRefusedByName )
{
    VFXSystemData d       = Campfire();
    d.Category            = "Smoke";
    const std::string why = Refusal( d );
    EXPECT_NE( why.find( "Smoke" ), std::string::npos ) << why;
}

TEST( VFXSystemFormat, ACategoryAddedToTheRegisterFileIsAcceptedWithoutARebuild )
{
    const fs::path dir = fs::temp_directory_path() / "desert_vfx02_categories";
    fs::remove_all( dir );
    fs::create_directories( dir );
    const fs::path file = dir / kVFXCategoriesFileName;

    EXPECT_FALSE( ReadVFXCategories( file ) ) << "a missing register is an error, not an empty one";

    VFXSystemData d        = Campfire();
    d.Category             = "Smoke";
    const std::string text = WriteVFXSystem( d );

    {
        std::ofstream out( file, std::ios::binary );
        out << R"({"Categories":[{"Id":"Fire","DisplayName":"Fire","Color":[1,0.4,0.1,1]}]})";
    }
    auto before = ReadVFXCategories( file );
    ASSERT_TRUE( before ) << before.GetError();
    EXPECT_FALSE( ParseVFXSystem( text, before.GetValue() ) );

    {
        std::ofstream out( file, std::ios::binary );
        out << R"({"Categories":[{"Id":"Fire","DisplayName":"Fire","Color":[1,0.4,0.1,1]},)"
               R"({"Id":"Smoke","DisplayName":"Smoke","Color":[0.5,0.5,0.5,1]}]})";
    }
    auto after = ReadVFXCategories( file );
    ASSERT_TRUE( after ) << after.GetError();
    auto parsed = ParseVFXSystem( text, after.GetValue() );
    EXPECT_TRUE( parsed ) << parsed.GetError();
    fs::remove_all( dir );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
