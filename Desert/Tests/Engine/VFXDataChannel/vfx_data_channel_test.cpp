// VFX-10. The scene's VFX data channels: the `.dfxch` asset, the writer, engine:SpawnFromChannel as the CPU reads
// it (CompileSpawnPlan), and the world's per-tick gather into spawn requests (PlanEmitterSteps / VFXWorld::Tick).

#include <gtest/gtest.h>

#include <Engine/Assets/Serialization/VFXDataChannel.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/VFXDataChannelAsset.hpp>

#include "../../TestSupport/scratch_dir.hpp"

#include <Common/Core/Constants.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <Engine/Assets/Serialization/VFXSystem.hpp>
#include <Engine/VFX/VFXDataChannel.hpp>
#include <Engine/VFX/VFXEmitterSpawn.hpp>
#include <Engine/VFX/VFXWorld.hpp>

#include <entt/entt.hpp>

#include <string>
#include <vector>

using namespace Desert;
namespace S = Desert::Assets::Serialization;

namespace
{
    S::VFXDataChannelData ImpactChannel()
    {
        S::VFXDataChannelData data;
        data.Fields = { { "Pos", S::VFXDataChannelFieldType::Position },
                        { "Dir", S::VFXDataChannelFieldType::Direction },
                        { "Tint", S::VFXDataChannelFieldType::Color },
                        { "Strength", S::VFXDataChannelFieldType::Float },
                        { "Team", S::VFXDataChannelFieldType::Int } };
        return data;
    }

    // Writes one Impacts entry at @p cm with @p strength.
    void WriteImpact( VFX::VFXDataChannels& channels, const glm::vec3& cm, float strength )
    {
        auto writer = channels.Write( "Impacts", 1 );
        ASSERT_TRUE( writer.IsSuccess() ) << writer.GetError();
        VFX::VFXDataChannelWriter w = writer.ExtractValue();
        ASSERT_TRUE( w.WritePosition( 0, "Pos", cm ) );
        ASSERT_TRUE( w.WriteDirection( 0, "Dir", glm::vec3( 0.0f, 0.0f, 1.0f ) ) );
        ASSERT_TRUE( w.WriteFloat( 0, "Strength", strength ) );
        ASSERT_TRUE( w.WriteInt( 0, "Team", 2 ) );
    }

    VFX::VFXSpawnPlan ChannelPlan( const VFX::VFXChannelSpawnModule& module )
    {
        VFX::VFXSpawnPlan plan;
        plan.Lifecycle.Loop         = S::VFXLoopBehavior::Infinite;
        plan.Lifecycle.LoopDuration = 1.0f;
        plan.Channel                = module;
        return plan;
    }

    VFX::VFXChannelSpawnModule Module( std::uint32_t perEntry, std::uint32_t maxEntries )
    {
        VFX::VFXChannelSpawnModule m;
        m.Channel            = "Impacts";
        m.ParticlesPerEntry  = perEntry;
        m.MaxEntriesPerFrame = maxEntries;
        m.PositionField      = "Pos";
        m.DirectionField     = "Dir";
        return m;
    }

    constexpr double kStep = 1.0 / 60.0;

    S::VFXModuleInput Value( const char* name, S::VFXValueType type, float v )
    {
        S::VFXModuleInput in;
        in.Name   = name;
        in.Type   = type;
        in.Source = S::VFXInputSource::Value;
        in.Value  = glm::vec4( v, 0.0f, 0.0f, 0.0f );
        return in;
    }

    S::VFXModuleInput Bound( const char* name, S::VFXValueType type, const char* binding )
    {
        S::VFXModuleInput in;
        in.Name    = name;
        in.Type    = type;
        in.Source  = S::VFXInputSource::Binding;
        in.Binding = binding;
        return in;
    }
} // namespace

TEST( VFXDataChannel, WrittenEntriesBecomeSpawnRequestsWithTheirPayload )
{
    VFX::VFXDataChannels channels;
    ASSERT_TRUE( channels.Register( "Impacts", ImpactChannel() ) );
    WriteImpact( channels, glm::vec3( 100.0f, 0.0f, 0.0f ), 1.0f );
    WriteImpact( channels, glm::vec3( 0.0f, 200.0f, 0.0f ), 1.0f );

    VFX::EmitterInstance instance;
    VFX::PlanEmitterSteps( instance, ChannelPlan( Module( 4, 16 ) ), 2, kStep, channels, glm::vec3( 0.0f ) );

    ASSERT_EQ( instance.ChannelSpawns.size(), 2u );
    EXPECT_EQ( instance.ChannelSpawns[0].Count, 4u );
    EXPECT_TRUE( instance.ChannelSpawns[0].HasPosition );
    EXPECT_EQ( instance.ChannelSpawns[0].Position, glm::vec3( 100.0f, 0.0f, 0.0f ) );
    EXPECT_EQ( instance.ChannelSpawns[1].Position, glm::vec3( 0.0f, 200.0f, 0.0f ) );
    EXPECT_EQ( instance.ChannelSpawns[1].Direction, glm::vec3( 0.0f, 0.0f, 1.0f ) );

    // All channel particles join the first step, ids reserved for them: the second step's ids follow.
    ASSERT_EQ( instance.Steps.size(), 2u );
    EXPECT_EQ( instance.Steps[0].ChannelFirst, 0u );
    EXPECT_EQ( instance.Steps[0].ChannelCount, 8u );
    EXPECT_EQ( instance.Steps[0].Budget, 8u );
    EXPECT_EQ( instance.Steps[1].ChannelCount, 0u );
    EXPECT_EQ( instance.Steps[1].IdBase, 8u );
    EXPECT_EQ( instance.ChannelReport.Read, 2u );
    EXPECT_EQ( instance.ChannelReport.Spawned, 2u );
}

TEST( VFXDataChannel, TheDistanceFilterAndTheFieldPredicateRefuseEntries )
{
    VFX::VFXDataChannels channels;
    ASSERT_TRUE( channels.Register( "Impacts", ImpactChannel() ) );
    WriteImpact( channels, glm::vec3( 50.0f, 0.0f, 0.0f ), 3.0f );  // near, strong: spawns
    WriteImpact( channels, glm::vec3( 500.0f, 0.0f, 0.0f ), 3.0f ); // 500 cm away: refused by distance
    WriteImpact( channels, glm::vec3( 0.0f, 50.0f, 0.0f ), 0.5f );  // near, weak: refused by the predicate

    VFX::VFXChannelSpawnModule m = Module( 1, 16 );
    m.MaxDistance                = 100.0f;
    m.FilterField                = "Strength";
    m.FilterOp                   = VFX::VFXChannelFilterOp::Greater;
    m.FilterValue                = 1.0f;
    const auto batch             = VFX::GatherChannelSpawns( m, channels.Find( "Impacts" ), glm::vec3( 0.0f ) );

    ASSERT_EQ( batch.Requests.size(), 1u );
    EXPECT_EQ( batch.Requests[0].Entry, 0u );
    EXPECT_EQ( batch.Report.Filtered, 2u );
    EXPECT_EQ( batch.Report.Overflow, 0u );

    // An Int field takes the predicate too; a Color field cannot be one.
    m.FilterField = "Team";
    m.FilterOp    = VFX::VFXChannelFilterOp::Equal;
    m.FilterValue = 2.0f;
    m.MaxDistance = 0.0f;
    EXPECT_EQ( VFX::GatherChannelSpawns( m, channels.Find( "Impacts" ), glm::vec3( 0.0f ) ).Requests.size(), 3u );
    m.FilterField = "Tint";
    EXPECT_FALSE( VFX::BindChannelSpawn( m, channels.Find( "Impacts" )->Layout() ) );
}

TEST( VFXDataChannel, ThePerFrameLimitCountsTheOverflow )
{
    VFX::VFXDataChannels channels;
    ASSERT_TRUE( channels.Register( "Impacts", ImpactChannel() ) );
    for ( int i = 0; i < 5; ++i )
        WriteImpact( channels, glm::vec3( static_cast<float>( i ), 0.0f, 0.0f ), 1.0f );

    VFX::EmitterInstance instance;
    const auto           plan = ChannelPlan( Module( 3, 2 ) );
    VFX::PlanEmitterSteps( instance, plan, 1, kStep, channels, glm::vec3( 0.0f ) );
    EXPECT_EQ( instance.ChannelSpawns.size(), 2u );
    EXPECT_EQ( instance.Steps[0].ChannelCount, 6u );
    EXPECT_EQ( instance.ChannelReport.Spawned, 2u );
    EXPECT_EQ( instance.ChannelReport.Overflow, 3u );
    EXPECT_EQ( instance.ChannelOverflowTotal, 3u );

    // A tick that runs no step spawns nothing and counts every passing entry.
    VFX::PlanEmitterSteps( instance, plan, 0, kStep, channels, glm::vec3( 0.0f ) );
    EXPECT_TRUE( instance.ChannelSpawns.empty() );
    EXPECT_EQ( instance.ChannelReport.Overflow, 5u );
    EXPECT_EQ( instance.ChannelOverflowTotal, 8u );
}

TEST( VFXDataChannel, TheWorldTickClearsTheEntries )
{
    VFX::VFXWorld  world;
    entt::registry registry;
    ASSERT_TRUE( world.GetDataChannels().Register( "Impacts", ImpactChannel() ) );
    WriteImpact( world.GetDataChannels(), glm::vec3( 0.0f ), 1.0f );
    ASSERT_EQ( world.GetDataChannels().Find( "Impacts" )->EntryCount(), 1u );

    world.Tick( registry, kStep );
    EXPECT_EQ( world.GetDataChannels().Find( "Impacts" )->EntryCount(), 0u ) << "an entry outlived its frame";
    EXPECT_NE( world.GetDataChannels().Find( "Impacts" ), nullptr ) << "the tick forgot the channel itself";

    // Another layout under the same name is refused while entries are pending.
    WriteImpact( world.GetDataChannels(), glm::vec3( 0.0f ), 1.0f );
    S::VFXDataChannelData other;
    other.Fields = { { "Only", S::VFXDataChannelFieldType::Float } };
    EXPECT_FALSE( world.GetDataChannels().Register( "Impacts", other ) );
}

TEST( VFXDataChannel, TheWriterRefusesWhatTheLayoutDoesNotHold )
{
    VFX::VFXDataChannels channels;
    ASSERT_TRUE( channels.Register( "Impacts", ImpactChannel() ) );
    EXPECT_FALSE( channels.Write( "Nope", 1 ).IsSuccess() );
    auto writer = channels.Write( "Impacts", 1 );
    ASSERT_TRUE( writer.IsSuccess() );
    VFX::VFXDataChannelWriter w = writer.ExtractValue();
    EXPECT_FALSE( w.WriteFloat( 0, "Pos", 1.0f ) ) << "a Position field written as Float";
    EXPECT_FALSE( w.WriteFloat( 0, "Missing", 1.0f ) );
    EXPECT_FALSE( w.WriteFloat( 1, "Strength", 1.0f ) ) << "outside the writer's window";
    EXPECT_FALSE( w.WriteInt( 0, "Team", 1 << 25 ) ) << "an Int the payload cannot hold exactly";
    EXPECT_TRUE( w.WriteColor( 0, "Tint", glm::vec4( 1.0f, 0.5f, 0.25f, 1.0f ) ) );
    const auto entry = channels.Find( "Impacts" )->Entry( 0 );
    EXPECT_EQ( entry[6], 1.0f ); // Tint follows Pos (3) and Dir (3)
    EXPECT_EQ( entry[9], 1.0f );
}

TEST( VFXDataChannel, TheAssetRoundTripsAndRefusesABadLayout )
{
    const S::VFXDataChannelData data = ImpactChannel();
    const std::string           text = S::WriteVFXDataChannel( data );
    auto                        back = S::ParseVFXDataChannel( text );
    ASSERT_TRUE( back.IsSuccess() ) << back.GetError();
    EXPECT_EQ( back.GetValue().Fields, data.Fields );
    EXPECT_EQ( S::WriteVFXDataChannel( back.GetValue() ), text ) << "the canonical text moved on a round trip";

    S::VFXDataChannelData repeated = data;
    repeated.Fields.push_back( { "Pos", S::VFXDataChannelFieldType::Float } );
    EXPECT_FALSE( S::ValidateVFXDataChannelData( repeated ) );
    EXPECT_FALSE( S::ValidateVFXDataChannelData( S::VFXDataChannelData{} ) );
    EXPECT_FALSE( S::ParseVFXDataChannel( R"({"Fields":[{"Name":"A","Type":"Float"}]})" ).IsSuccess() )
         << "a file without a header was read";
}

TEST( VFXDataChannel, CompileSpawnPlanReadsSpawnFromChannel )
{
    S::VFXSystemData  system;
    S::VFXEmitterData emitter;
    emitter.Name                   = "Sparks";
    emitter.Lifecycle.LoopDuration = 1.0f;
    S::VFXModuleUse use;
    use.Module = std::string( VFX::kVFXSpawnFromChannelModule );
    use.Inputs = { Bound( "Channel", S::VFXValueType::Int, "DataChannel.Impacts" ),
                   Value( "ParticlesPerEntry", S::VFXValueType::Int, 4.0f ),
                   Value( "MaxEntriesPerFrame", S::VFXValueType::Int, 8.0f ),
                   Value( "MaxDistance", S::VFXValueType::Float, 300.0f ),
                   Bound( "Position", S::VFXValueType::Vec3, "DataChannel.Impacts.Pos" ),
                   Bound( "Filter", S::VFXValueType::Float, "DataChannel.Impacts.Strength" ),
                   Value( "FilterOp", S::VFXValueType::Int, 3.0f ),
                   Value( "FilterValue", S::VFXValueType::Float, 0.5f ) };
    emitter.Stack.EmitterUpdate.push_back( use );
    system.Emitters.push_back( emitter );

    auto plan = VFX::CompileSpawnPlan( system, 0 );
    ASSERT_TRUE( plan.IsSuccess() ) << plan.GetError();
    ASSERT_TRUE( plan.GetValue().Channel.has_value() );
    const VFX::VFXChannelSpawnModule& m = *plan.GetValue().Channel;
    EXPECT_EQ( m.Channel, "Impacts" );
    EXPECT_EQ( m.ParticlesPerEntry, 4u );
    EXPECT_EQ( m.MaxEntriesPerFrame, 8u );
    EXPECT_EQ( m.MaxDistance, 300.0f );
    EXPECT_EQ( m.PositionField, "Pos" );
    EXPECT_TRUE( m.DirectionField.empty() );
    EXPECT_EQ( m.FilterField, "Strength" );
    EXPECT_EQ( m.FilterOp, VFX::VFXChannelFilterOp::GreaterEqual );

    // A field of another channel, and a filter bound without its operator, are refused.
    system.Emitters[0].Stack.EmitterUpdate[0].Inputs[4] =
         Bound( "Position", S::VFXValueType::Vec3, "DataChannel.Other.Pos" );
    EXPECT_FALSE( VFX::CompileSpawnPlan( system, 0 ).IsSuccess() );
    system.Emitters[0].Stack.EmitterUpdate[0].Inputs[4] =
         Bound( "Position", S::VFXValueType::Vec3, "DataChannel.Impacts.Pos" );
    system.Emitters[0].Stack.EmitterUpdate[0].Inputs.erase(
         system.Emitters[0].Stack.EmitterUpdate[0].Inputs.begin() + 6 );
    EXPECT_FALSE( VFX::CompileSpawnPlan( system, 0 ).IsSuccess() );
}

// VFX-10b: the Color, Float and Int payload reach the particle. Color -> the particle's Tint, a Float or Int field
// -> its lifetime (seconds); the request carries them to ParticleWorldGpu's record (Color, Scalars).
TEST( VFXDataChannel, TheColorAndScalarPayloadReachTheSpawnRequest )
{
    VFX::VFXDataChannels channels;
    ASSERT_TRUE( channels.Register( "Impacts", ImpactChannel() ) );
    {
        auto writer = channels.Write( "Impacts", 1 );
        ASSERT_TRUE( writer.IsSuccess() ) << writer.GetError();
        VFX::VFXDataChannelWriter w = writer.ExtractValue();
        ASSERT_TRUE( w.WritePosition( 0, "Pos", glm::vec3( 10.0f, 0.0f, 0.0f ) ) );
        ASSERT_TRUE( w.WriteColor( 0, "Tint", glm::vec4( 1.0f, 0.5f, 0.25f, 0.75f ) ) );
        ASSERT_TRUE( w.WriteFloat( 0, "Strength", 2.5f ) );
        ASSERT_TRUE( w.WriteInt( 0, "Team", 3 ) );
    }
    VFX::VFXChannelSpawnModule m = Module( 1, 4 );
    m.ColorField                 = "Tint";
    m.LifetimeField              = "Strength";
    auto batch                   = VFX::GatherChannelSpawns( m, channels.Find( "Impacts" ), glm::vec3( 0.0f ) );
    ASSERT_EQ( batch.Requests.size(), 1u );
    EXPECT_TRUE( batch.Requests[0].HasColor );
    EXPECT_EQ( batch.Requests[0].Color, glm::vec4( 1.0f, 0.5f, 0.25f, 0.75f ) );
    EXPECT_TRUE( batch.Requests[0].HasLifetime );
    EXPECT_EQ( batch.Requests[0].Lifetime, 2.5f );

    // An Int field binds the lifetime too (whole seconds, stored exactly).
    m.LifetimeField = "Team";
    batch           = VFX::GatherChannelSpawns( m, channels.Find( "Impacts" ), glm::vec3( 0.0f ) );
    ASSERT_EQ( batch.Requests.size(), 1u );
    EXPECT_EQ( batch.Requests[0].Lifetime, 3.0f );

    // Unbound: no colour, no lifetime - the emitter's own.
    batch = VFX::GatherChannelSpawns( Module( 1, 4 ), channels.Find( "Impacts" ), glm::vec3( 0.0f ) );
    ASSERT_EQ( batch.Requests.size(), 1u );
    EXPECT_FALSE( batch.Requests[0].HasColor );
    EXPECT_FALSE( batch.Requests[0].HasLifetime );

    // A role bound to a field of the wrong type is refused.
    const VFX::VFXDataChannelLayout layout = VFX::VFXDataChannelLayout::From( ImpactChannel() );
    VFX::VFXChannelSpawnModule      bad    = Module( 1, 4 );
    bad.ColorField                         = "Strength";
    EXPECT_FALSE( VFX::BindChannelSpawn( bad, layout ) ) << "a Float field accepted as the colour";
    bad               = Module( 1, 4 );
    bad.LifetimeField = "Tint";
    EXPECT_FALSE( VFX::BindChannelSpawn( bad, layout ) ) << "a Color field accepted as the lifetime";

    // The module's Color (Vec4) and Lifetime (Float) inputs are read from the stack.
    S::VFXSystemData  system;
    S::VFXEmitterData emitter;
    emitter.Name                   = "Sparks";
    emitter.Lifecycle.LoopDuration = 1.0f;
    S::VFXModuleUse use;
    use.Module = std::string( VFX::kVFXSpawnFromChannelModule );
    use.Inputs = { Bound( "Channel", S::VFXValueType::Int, "DataChannel.Impacts" ),
                   Value( "ParticlesPerEntry", S::VFXValueType::Int, 1.0f ),
                   Value( "MaxEntriesPerFrame", S::VFXValueType::Int, 4.0f ),
                   Bound( "Color", S::VFXValueType::Vec4, "DataChannel.Impacts.Tint" ),
                   Bound( "Lifetime", S::VFXValueType::Float, "DataChannel.Impacts.Team" ) };
    emitter.Stack.EmitterUpdate.push_back( use );
    system.Emitters.push_back( emitter );
    auto plan = VFX::CompileSpawnPlan( system, 0 );
    ASSERT_TRUE( plan.IsSuccess() ) << plan.GetError();
    ASSERT_TRUE( plan.GetValue().Channel.has_value() );
    EXPECT_EQ( plan.GetValue().Channel->ColorField, "Tint" );
    EXPECT_EQ( plan.GetValue().Channel->LifetimeField, "Team" );
}

// VFX-10b: a script names a channel by its asset name (as IA_Jump), never by a path; C++ registers the asset (or
// its handle) and the channel is named by the file stem.
TEST( VFXDataChannel, AChannelIsNamedByItsAssetNotByAPath )
{
    const auto path = Assets::VFXDataChannelAsset::PathForName( "Impacts" );
    ASSERT_TRUE( path.IsSuccess() ) << path.GetError();
    EXPECT_EQ( path.GetValue(), Common::Constants::Path::VFX_PATH / "Impacts.dfxch" );
    EXPECT_FALSE( Assets::VFXDataChannelAsset::PathForName( "VFX/Impacts.dfxch" ).IsSuccess() );
    EXPECT_FALSE( Assets::VFXDataChannelAsset::PathForName( "Impacts.dfxch" ).IsSuccess() );
    EXPECT_FALSE( Assets::VFXDataChannelAsset::PathForName( "" ).IsSuccess() );

    namespace fs       = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "VFX10bChannelByAsset";
    fs::remove_all( dir );
    fs::create_directories( dir );
    const fs::path file = dir / "Impacts.dfxch";
    ASSERT_TRUE( Assets::VFXDataChannelAsset::Save( file, ImpactChannel() ) );
    Assets::VFXDataChannelAsset asset( file );
    VFX::VFXDataChannels        channels;
    EXPECT_FALSE( channels.Register( asset ) ) << "an asset not yet loaded was registered";
    ASSERT_TRUE( asset.LoadFromFile() );
    ASSERT_TRUE( channels.Register( asset ) );
    const VFX::VFXDataChannel* channel = channels.Find( "Impacts" );
    ASSERT_NE( channel, nullptr ) << "the channel is not named by the asset's file stem";
    EXPECT_EQ( channel->Layout().Fields, ImpactChannel().Fields );
    fs::remove_all( dir );
}

// VFX-10c: a Float (or Int) field bound to Size scales the particle's base size: the request carries it, an
// unbound size is the emitter's (scale 1), a Color field is refused as a size, and the module's Size input is
// read.
TEST( VFXDataChannel, AFloatFieldBindsTheParticleSize )
{
    VFX::VFXDataChannels channels;
    ASSERT_TRUE( channels.Register( "Impacts", ImpactChannel() ) );
    {
        auto writer = channels.Write( "Impacts", 1 );
        ASSERT_TRUE( writer.IsSuccess() ) << writer.GetError();
        VFX::VFXDataChannelWriter w = writer.ExtractValue();
        ASSERT_TRUE( w.WriteFloat( 0, "Strength", 2.5f ) );
        ASSERT_TRUE( w.WriteInt( 0, "Team", 3 ) );
    }
    VFX::VFXChannelSpawnModule m = Module( 1, 4 );
    m.SizeField                  = "Strength";
    auto batch                   = VFX::GatherChannelSpawns( m, channels.Find( "Impacts" ), glm::vec3( 0.0f ) );
    ASSERT_EQ( batch.Requests.size(), 1u );
    EXPECT_TRUE( batch.Requests[0].HasSize );
    EXPECT_EQ( batch.Requests[0].Size, 2.5f );
    m.SizeField = "Team";
    batch       = VFX::GatherChannelSpawns( m, channels.Find( "Impacts" ), glm::vec3( 0.0f ) );
    ASSERT_EQ( batch.Requests.size(), 1u );
    EXPECT_EQ( batch.Requests[0].Size, 3.0f ) << "an Int field does not bind the size";

    batch = VFX::GatherChannelSpawns( Module( 1, 4 ), channels.Find( "Impacts" ), glm::vec3( 0.0f ) );
    ASSERT_EQ( batch.Requests.size(), 1u );
    EXPECT_FALSE( batch.Requests[0].HasSize );
    EXPECT_EQ( batch.Requests[0].Size, 1.0f ) << "an unbound size must be the emitter's (scale 1)";

    VFX::VFXChannelSpawnModule bad = Module( 1, 4 );
    bad.SizeField                  = "Tint";
    EXPECT_FALSE( VFX::BindChannelSpawn( bad, VFX::VFXDataChannelLayout::From( ImpactChannel() ) ) )
         << "a Color field accepted as the size";

    S::VFXSystemData  system;
    S::VFXEmitterData emitter;
    emitter.Name                   = "Sparks";
    emitter.Lifecycle.LoopDuration = 1.0f;
    S::VFXModuleUse use;
    use.Module = std::string( VFX::kVFXSpawnFromChannelModule );
    use.Inputs = { Bound( "Channel", S::VFXValueType::Int, "DataChannel.Impacts" ),
                   Value( "ParticlesPerEntry", S::VFXValueType::Int, 1.0f ),
                   Value( "MaxEntriesPerFrame", S::VFXValueType::Int, 4.0f ),
                   Bound( "Size", S::VFXValueType::Float, "DataChannel.Impacts.Strength" ) };
    emitter.Stack.EmitterUpdate.push_back( use );
    system.Emitters.push_back( emitter );
    auto plan = VFX::CompileSpawnPlan( system, 0 );
    ASSERT_TRUE( plan.IsSuccess() ) << plan.GetError();
    ASSERT_TRUE( plan.GetValue().Channel.has_value() );
    EXPECT_EQ( plan.GetValue().Channel->SizeField, "Strength" );
}

// VFX-10c: what Lua VFX.useChannel calls - the NAME is resolved by the AssetManager (a held channel asset in any
// folder, matched by its stem); an unknown name, a path, or two held assets sharing a name are refused by name.
TEST( VFXDataChannel, AScriptNameIsResolvedThroughTheAssetManager )
{
    namespace fs       = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "VFX10cUseChannel";
    fs::remove_all( dir );
    fs::create_directories( dir / "Sub" );
    fs::create_directories( dir / "Other" );
    const fs::path file = dir / "Sub" / "VFX10cSparks.dfxch";
    ASSERT_TRUE( Assets::VFXDataChannelAsset::Save( file, ImpactChannel() ) );

    Assets::AssetManager assets;
    VFX::VFXDataChannels channels;
    EXPECT_FALSE( channels.Use( "VFX10cSparks", assets ) ) << "a name the manager does not hold was resolved";
    ASSERT_TRUE( assets.CreateAsset<Assets::VFXDataChannelAsset>( file ) );
    const auto used = channels.Use( "VFX10cSparks", assets );
    ASSERT_TRUE( used ) << used.GetError();
    const VFX::VFXDataChannel* channel = channels.Find( "VFX10cSparks" );
    ASSERT_NE( channel, nullptr ) << "the held asset (in a subfolder) was not the channel the name resolved to";
    EXPECT_EQ( channel->Layout().Fields, ImpactChannel().Fields );

    const auto unknown = channels.Use( "VFX10cNoSuchChannel", assets );
    ASSERT_FALSE( unknown );
    EXPECT_NE( unknown.GetError().find( "VFX10cNoSuchChannel" ), std::string::npos )
         << "an unknown channel is not refused by its name: " << unknown.GetError();
    EXPECT_FALSE( channels.Use( "Sub/VFX10cSparks", assets ) ) << "a path accepted as a channel name";

    const fs::path twin = dir / "Other" / "VFX10cSparks.dfxch";
    ASSERT_TRUE( Assets::VFXDataChannelAsset::Save( twin, ImpactChannel() ) );
    ASSERT_TRUE( assets.CreateAsset<Assets::VFXDataChannelAsset>( twin ) );
    EXPECT_FALSE( channels.Use( "VFX10cSparks", assets ) ) << "two channel assets with one name: one was picked";
    fs::remove_all( dir );
}

// VFX-10c census: Lua VFX.useChannel (the Luau C function UseChannel, registered as VFX.useChannel) goes through
// VFXDataChannels::Use (the AssetManager), never reads the file itself - no asset constructed, no LoadFromFile, no
// PathForName, no stream in its body.
TEST( VFXDataChannel, LuaUseChannelDoesNotReadTheFileItself )
{
    std::ifstream in( TestSupport::RepositoryRoot() / "Desert" / "Desert" / "Source" / "Engine" / "Scripting" /
                      "VFXBindings.cpp" );
    ASSERT_TRUE( in ) << "VFXBindings.cpp not found under the repository root";
    std::stringstream text;
    text << in.rdbuf();
    const std::string source = text.str();
    const auto        begin  = source.find( "int UseChannel( lua_State* L )" );
    const auto        end    = source.find( "int WriteChannel( lua_State* L )" );
    ASSERT_NE( begin, std::string::npos );
    ASSERT_NE( end, std::string::npos );
    ASSERT_LT( begin, end );
    const std::string body = source.substr( begin, end - begin );
    for ( const char* forbidden :
          { "LoadFromFile", "PathForName", "VFXDataChannelAsset", "ifstream", "std::filesystem", "Load(" } )
        EXPECT_EQ( body.find( forbidden ), std::string::npos )
             << "VFX.useChannel reads the channel itself ('" << forbidden << "') instead of the AssetManager";
    EXPECT_NE( body.find( ".Use( name, *host.Assets )" ), std::string::npos )
         << "VFX.useChannel no longer resolves the name through VFXDataChannels::Use and the AssetManager";
}
