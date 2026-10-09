// VFX-03b. The VFXComponent: an entity plays a `.dfx` VFX system. Pinned here:
//
//   - VFXData::System is stored as a {Guid, Path} reference and comes back through the GUID
//     (ReflectionSerializer IsStoredByGuid) - stored as a bare path it would come back empty;
//   - VFXWorld plans one instance per (entity, enabled emitter) of the played system, seeded
//     MakeEmitterSeed(system seed, uuid, emitter index), with that emitter's own spawn plan;
//   - not activated, without a system lookup, or with a system the lookup does not know, nothing is planned.

#include <gtest/gtest.h>

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Json/Document.hpp>
#include <Common/Json/Json.hpp>

#include <Engine/ECS/Components.hpp>
#include <Engine/Reflection/ReflectionRegistry.hpp>
#include <Engine/Reflection/ReflectionSerializer.hpp>
#include <Engine/VFX/VFXEmitterSpawn.hpp>
#include <Engine/VFX/VFXRandom.hpp>
#include <Engine/VFX/VFXWorld.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace
{
    namespace S   = Desert::Assets::Serialization;
    namespace VFX = Desert::VFX;
    namespace ECS = Desert::ECS;
    namespace R   = Desert::Reflection;

    constexpr std::uint64_t kSystemHandle = 0x5A17C0DE00000042ull;
    const char*             kSystemGuid   = "a1b2c3d4e5f60718293a4b5c6d7e8f90";
    const std::string       kSystemPath   = "VFX/FX_Sparks.dfx";

    std::uint64_t GuidHandle()
    {
        const auto parsed = Common::Content::AssetGuidFromText( kSystemGuid );
        EXPECT_TRUE( parsed.IsSuccess() );
        return static_cast<std::uint64_t>( Common::Content::HandleForGuid( parsed.GetValue() ) );
    }

    // Knows the one system by GUID only: FromPath answers nothing, so a reference that arrives as a bare path
    // cannot come back.
    R::AssetResolver GuidOnlyResolver()
    {
        R::AssetResolver r;
        r.ToPath = []( std::uint64_t handle, const std::string& type ) -> std::string
        { return type == "VFXSystemAsset" && handle == kSystemHandle ? kSystemPath : std::string(); };
        r.FromPath = []( const std::string&, const std::string& ) -> std::uint64_t { return 0ull; };
        r.ToGuid   = []( std::uint64_t handle, const std::string& type ) -> std::string
        { return type == "VFXSystemAsset" && handle == kSystemHandle ? kSystemGuid : std::string(); };
        r.FromGuid = []( std::uint64_t guid, const std::string& type ) -> std::uint64_t
        { return type == "VFXSystemAsset" && guid == GuidHandle() ? kSystemHandle : 0ull; };
        return r;
    }

    S::VFXModuleUse Rate( float perSecond )
    {
        S::VFXModuleInput in;
        in.Name   = "SpawnRate";
        in.Type   = S::VFXValueType::Float;
        in.Source = S::VFXInputSource::Value;
        in.Value  = glm::vec4( perSecond, 0, 0, 0 );
        return { std::string( VFX::kVFXSpawnRateModule ), true, { in } };
    }

    // Two emitters of different rates, so planning one with the other's plan is seen.
    S::VFXSystemData TwoEmitterSystem()
    {
        S::VFXSystemData system;
        system.Seed = 77;
        for ( const float rate : { 120.0f, 600.0f } )
        {
            S::VFXEmitterData emitter;
            emitter.Name                = rate < 200.0f ? "Embers" : "Sparks";
            emitter.Stack.EmitterUpdate = { Rate( rate ) };
            system.Emitters.push_back( emitter );
        }
        return system;
    }

    struct Played
    {
        entt::registry registry;
        std::uint64_t  Uuid = 0;

        explicit Played( bool autoActivate )
        {
            const auto entity     = registry.create();
            Uuid                  = registry.emplace<ECS::UUIDComponent>( entity ).UUID;
            auto& vfx             = registry.emplace<ECS::VFXComponent>( entity );
            vfx.Data.System       = Desert::Assets::AssetHandle( kSystemHandle );
            vfx.Data.AutoActivate = autoActivate;
        }
    };

    constexpr double kTick = 0.1; // several fixed steps
} // namespace

TEST( VFXComponent, TheSystemIsStoredByGuidAndComesBackThroughIt )
{
    const R::TypeInfo* type = R::ReflectionRegistry::Get().Find( "VFXData" );
    ASSERT_NE( type, nullptr ) << "VFXData is not reflected, so no scene can carry a VFXComponent";

    ECS::VFXData written;
    written.System       = Desert::Assets::AssetHandle( kSystemHandle );
    written.AutoActivate = false;

    const R::AssetResolver resolver = GuidOnlyResolver();
    const auto             object   = R::SerializeReflected( *type, &written, &resolver );

    const auto stored = object.get( "System" );
    ASSERT_TRUE( stored.has_value() ) << "VFXData wrote no System field";
    const Common::Json::Node ref = Common::Json::Root( stored.value() );
    ASSERT_EQ( ref.GetKind(), Common::Json::Kind::Object )
         << "the system must be written as {Guid, Path} (IsStoredByGuid \"VFXSystemAsset\"), not a bare path";

    const std::string text   = Common::Json::Write( object );
    const auto        parsed = Common::Json::Read<Common::Json::Object>( text );
    ASSERT_TRUE( parsed.IsSuccess() ) << text;

    ECS::VFXData         read;
    Common::Json::Issues issues;
    R::DeserializeReflected( *type, &read, Common::Json::Root( Common::Json::Value( parsed.GetValue() ) ), issues,
                             &resolver );
    for ( const auto& issue : issues )
        ADD_FAILURE() << Common::Json::Describe( issue );
    EXPECT_EQ( static_cast<std::uint64_t>( read.System ), kSystemHandle ) << text;
    EXPECT_FALSE( read.AutoActivate ) << text;
}

TEST( VFXComponent, TheWorldPlansEachEmitterOfThePlayedSystem )
{
    const S::VFXSystemData system = TwoEmitterSystem();
    Played                 played( /*autoActivate=*/true );

    VFX::VFXWorld world;
    world.SetSystemLookup( [&]( Desert::Assets::AssetHandle h ) -> const S::VFXSystemData*
                           { return static_cast<std::uint64_t>( h ) == kSystemHandle ? &system : nullptr; } );
    world.Tick( played.registry, kTick );
    const auto& plan = world.GetPlan();
    ASSERT_GT( plan.StepCount, 0u );

    for ( std::uint32_t i = 0; i < 2; ++i )
    {
        SCOPED_TRACE( "emitter " + std::to_string( i ) );
        const VFX::EmitterInstance* instance = world.FindSystemEmitter( played.Uuid, i );
        ASSERT_NE( instance, nullptr ) << "an activated VFXComponent planned no instance for this emitter";
        EXPECT_EQ( instance->Seed, VFX::MakeEmitterSeed( system.Seed, played.Uuid, i ) );

        // The same emitter planned alone from its own spawn plan: the world must agree step for step.
        const auto own = VFX::CompileSpawnPlan( system, i );
        ASSERT_TRUE( own.IsSuccess() );
        VFX::EmitterInstance reference;
        VFX::VFXDataChannels noChannels;
        VFX::PlanEmitterSteps( reference, own.GetValue(), plan.StepCount,
                               world.GetClock().GetSettings().StepSeconds, noChannels, glm::vec3( 0.0f ) );
        ASSERT_EQ( instance->Steps.size(), reference.Steps.size() );
        std::uint64_t born = 0;
        for ( std::size_t s = 0; s < reference.Steps.size(); ++s )
        {
            EXPECT_EQ( instance->Steps[s].Budget, reference.Steps[s].Budget ) << "step " << s;
            born += instance->Steps[s].Budget;
        }
        EXPECT_GT( born, 0u );
    }
    EXPECT_EQ( world.FindSystemEmitter( played.Uuid, 2 ), nullptr );
    EXPECT_EQ( world.FindEmitter( played.Uuid ), nullptr )
         << "a VFXComponent's emitter answered as the entity's ParticleEmitterComponent instance";
}

TEST( VFXComponent, NothingIsPlannedWhenNotActivatedOrTheSystemIsUnknown )
{
    const S::VFXSystemData system = TwoEmitterSystem();
    const auto             lookup = [&]( Desert::Assets::AssetHandle h ) -> const S::VFXSystemData*
    { return static_cast<std::uint64_t>( h ) == kSystemHandle ? &system : nullptr; };

    {
        SCOPED_TRACE( "AutoActivate = false" );
        Played        played( /*autoActivate=*/false );
        VFX::VFXWorld world;
        world.SetSystemLookup( lookup );
        world.Tick( played.registry, kTick );
        EXPECT_EQ( world.FindSystemEmitter( played.Uuid, 0 ), nullptr );
        EXPECT_EQ( world.FindSystemEmitter( played.Uuid, 1 ), nullptr );
    }
    {
        SCOPED_TRACE( "no system lookup" );
        Played        played( /*autoActivate=*/true );
        VFX::VFXWorld world;
        world.Tick( played.registry, kTick );
        world.Tick( played.registry, kTick ); // reported once, still nothing
        EXPECT_EQ( world.FindSystemEmitter( played.Uuid, 0 ), nullptr );
    }
    {
        SCOPED_TRACE( "a system the lookup does not know" );
        Played        played( /*autoActivate=*/true );
        VFX::VFXWorld world;
        world.SetSystemLookup( []( Desert::Assets::AssetHandle ) -> const S::VFXSystemData* { return nullptr; } );
        world.Tick( played.registry, kTick );
        EXPECT_EQ( world.FindSystemEmitter( played.Uuid, 0 ), nullptr );
    }
    {
        SCOPED_TRACE( "deactivated after playing: the instances are dropped" );
        Played        played( /*autoActivate=*/true );
        VFX::VFXWorld world;
        world.SetSystemLookup( lookup );
        world.Tick( played.registry, kTick );
        ASSERT_NE( world.FindSystemEmitter( played.Uuid, 0 ), nullptr );
        played.registry.view<ECS::VFXComponent>().each( []( ECS::VFXComponent& vfx )
                                                        { vfx.Data.AutoActivate = false; } );
        world.Tick( played.registry, kTick );
        EXPECT_EQ( world.FindSystemEmitter( played.Uuid, 0 ), nullptr );
    }
}
