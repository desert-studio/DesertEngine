// VFX-03j / VFX-10. engine:SpawnFromChannel on the compiled-stack path: the CPU half (EmitterUpdate) turns a
// channel entry into spawns of the step (PlanEmitterSteps), the rows the program reads carry the entry's fields
// (ExpandChannelSpawns), and the stack compiler appends the particle half (VFX/Modules/SpawnFromChannel.shader) to
// the spawn group, whose host program binds the rows. DestructionDust.dfx (DST-05) is the shipped user.

#include <gtest/gtest.h>

#include "../../TestSupport/scratch_dir.hpp"

#include <Engine/Assets/Serialization/VFXDataChannel.hpp>
#include <Engine/Assets/Serialization/VFXSystem.hpp>
#include <Engine/VFX/VFXDataChannel.hpp>
#include <Engine/VFX/VFXEmitterSpawn.hpp>
#include <Engine/VFX/VFXSimulationProgram.hpp>
#include <Engine/VFX/VFXStackCompiler.hpp>
#include <Engine/VFX/VFXWorld.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace Desert;
namespace S = Desert::Assets::Serialization;

namespace
{
    std::string ReadText( const std::filesystem::path& path )
    {
        std::ifstream      in( path, std::ios::binary );
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
    }

    // The shipped DestructionDust.dfx, parsed against the project's category register.
    Common::ResultStr<S::VFXSystemData> DestructionDust()
    {
        const std::filesystem::path project = TestSupport::RepositoryRoot() / "Projects" / "Desert";
        auto categories = S::ReadVFXCategories( project / "Config" / S::kVFXCategoriesFileName );
        if ( !categories )
            return Common::MakeError<S::VFXSystemData>( categories.GetError() );
        return S::ParseVFXSystem( ReadText( project / "Content" / "VFX" / "DestructionDust.dfx" ),
                                  categories.GetValue() );
    }
} // namespace

// Red: an entry stops becoming the step's channel spawns, or a row loses the entry's position / its bound flag /
// the per-entry particle count.
TEST( VFXSpawnFromChannel, AChannelEntryBecomesTheStepsChannelSpawnsAndTheirRows )
{
    VFX::VFXDataChannels  channels;
    S::VFXDataChannelData layout;
    layout.Fields = { { "Pos", S::VFXDataChannelFieldType::Position } };
    ASSERT_TRUE( channels.Register( "Impacts", layout ) );
    {
        auto writer = channels.Write( "Impacts", 1 );
        ASSERT_TRUE( writer.IsSuccess() ) << writer.GetError();
        ASSERT_TRUE( writer.GetValue().WritePosition( 0, "Pos", glm::vec3( 300.0f, 0.0f, -50.0f ) ) );
    }
    VFX::VFXSpawnPlan plan;
    plan.Lifecycle.Loop         = S::VFXLoopBehavior::Infinite;
    plan.Lifecycle.LoopDuration = 1.0f;
    VFX::VFXChannelSpawnModule module;
    module.Channel            = "Impacts";
    module.ParticlesPerEntry  = 3;
    module.MaxEntriesPerFrame = 4;
    module.PositionField      = "Pos";
    plan.Channel              = module;

    VFX::EmitterInstance instance;
    VFX::PlanEmitterSteps( instance, plan, 1, 1.0 / 60.0, channels, glm::vec3( 0.0f ) );
    ASSERT_EQ( instance.Steps.size(), 1u );
    EXPECT_EQ( instance.Steps[0].ChannelFirst, 0u );
    EXPECT_EQ( instance.Steps[0].ChannelCount, 3u );

    const std::vector<VFX::VFXChannelSpawnRow> rows = VFX::ExpandChannelSpawns( instance.ChannelSpawns );
    ASSERT_EQ( rows.size(), 3u );
    for ( const VFX::VFXChannelSpawnRow& row : rows )
    {
        EXPECT_EQ( row.Position, glm::vec4( 300.0f, 0.0f, -50.0f, 1.0f ) );
        EXPECT_EQ( row.Direction.w, 0.0f ) << "an unbound direction must leave the velocity alone";
        EXPECT_EQ( row.Scalars, glm::vec4( 0.0f, 1.0f, 0.0f, 0.0f ) )
             << "unbound size scales by 1, no colour/life";
    }
}

// Red: DestructionDust stops compiling as a stack (a module it names is missing), the compiler stops appending
// the particle half of engine:SpawnFromChannel, or the host program stops binding / handing it the rows.
TEST( VFXSpawnFromChannel, DestructionDustCompilesWithTheChannelReadInItsSpawnGroup )
{
    auto system = DestructionDust();
    ASSERT_TRUE( system ) << system.GetError();
    ASSERT_EQ( system.GetValue().Emitters.size(), 3u );
    for ( std::size_t e = 0; e < system.GetValue().Emitters.size(); ++e )
    {
        auto spawn = VFX::CompileSpawnPlan( system.GetValue(), e );
        ASSERT_TRUE( spawn ) << spawn.GetError();
        EXPECT_TRUE( spawn.GetValue().Channel.has_value() ) << "emitter " << e;

        auto compiled = VFX::CompileEmitterStack( system.GetValue(), e, VFX::EngineModuleDir() );
        ASSERT_TRUE( compiled ) << "emitter " << e << ": " << compiled.GetError();
        EXPECT_NE( compiled.GetValue().ShaderText.find( "VFX_ChannelSpawn( sim.Channel - 1u )" ),
                   std::string::npos )
             << "emitter " << e << ": the particle half of engine:SpawnFromChannel is not in the stack";

        auto program = VFX::ComposeSimulationProgram( compiled.GetValue() );
        ASSERT_TRUE( program ) << program.GetError();
        EXPECT_NE( program.GetValue().find( "ReadBuffer(9) ChannelSpawns" ), std::string::npos );
        EXPECT_NE( program.GetValue().find( "u_Steps[step].ChannelFirst + t + 1u" ), std::string::npos );
    }
}

// Red: a stack WITHOUT the module gets the channel read appended anyway (its key would move with no reason).
TEST( VFXSpawnFromChannel, AStackWithoutTheModuleHasNoChannelRead )
{
    auto system = DestructionDust();
    ASSERT_TRUE( system ) << system.GetError();
    S::VFXSystemData plain = system.GetValue();
    plain.Emitters.resize( 1 );
    plain.Emitters[0].Stack.EmitterUpdate.clear();
    auto compiled = VFX::CompileEmitterStack( plain, 0, VFX::EngineModuleDir() );
    ASSERT_TRUE( compiled ) << compiled.GetError();
    EXPECT_EQ( compiled.GetValue().ShaderText.find( "VFX_ChannelSpawn" ), std::string::npos );
}
