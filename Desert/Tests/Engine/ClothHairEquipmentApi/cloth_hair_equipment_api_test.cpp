// CLO0: IS THE CLOTH / HAIR / MODULAR-CHARACTER API USABLE? Proven the only way an interface can be before
// its implementation exists — by implementing it with fakes and driving it the way its callers will.
//
// The fakes are deliberately dumb (explicit Euler, no constraints): what is under test is the SHAPE of the
// seam — who creates what, who destroys what, what a failure says, and that a step is a pure function of
// its inputs — not a solver. CLO1 (Jolt SoftBody), HAIR1 and EQP1 replace them with real backends and must
// keep these tests' promises.
//
// "Putting on the vest" below is plain test code standing in for a SCRIPT: the engine has no equipment,
// slots or equip events (owner, 2026-09-27); it has the four mechanisms of IModularCharacter.
#include <Engine/Animation/Modular/ModularCharacter.hpp>
#include <Engine/Hair/GroomSimulation.hpp>
#include <Engine/Physics/Cloth/ClothingSimulation.hpp>

#include <gtest/gtest.h>

#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace
{
    using namespace Desert;
    using Physics::Cloth::ClothingAsset;
    using Physics::Cloth::ClothingSimulationFactoryRegistry;
    using Physics::Cloth::ClothSimulationOutput;
    using Physics::Cloth::ClothStepContext;
    using Physics::Cloth::ClothTeleportMode;
    using Physics::Cloth::IClothingSimulation;
    using Physics::Cloth::IClothingSimulationFactory;

    struct Counters
    {
        int Created = 0;
        int Live    = 0;
    };

    class FakeClothSimulation final : public IClothingSimulation
    {
    public:
        FakeClothSimulation( const ClothingAsset& asset, Counters& counters )
             : m_Asset( asset ), m_Counters( counters ), m_Positions( asset.Mesh.Positions ),
               m_Velocities( asset.Mesh.Positions.size(), glm::vec3( 0.0f ) )
        {
            ++m_Counters.Created;
            ++m_Counters.Live;
        }
        ~FakeClothSimulation() override
        {
            --m_Counters.Live;
        }

        Common::BoolResultStr Step( const ClothStepContext& context ) override
        {
            if ( !( context.FixedDeltaSeconds > 0.0f ) )
                return Common::MakeError<bool>( "dt " + std::to_string( context.FixedDeltaSeconds ) + " <= 0" );
            if ( context.BoneModelTransforms.size() != m_Asset.UsedBoneNames.size() )
                return Common::MakeError<bool>( "bones " + std::to_string( context.BoneModelTransforms.size() ) +
                                                " != " + std::to_string( m_Asset.UsedBoneNames.size() ) );
            const float     dt    = context.FixedDeltaSeconds;
            const glm::vec3 accel = context.Gravity * m_Asset.Config.GravityScale +
                                    context.WindVelocity * m_Asset.Config.DragCoefficient;
            for ( uint32_t v = 0; v < m_Positions.size(); ++v )
            {
                if ( Physics::Cloth::IsFixedVertex( m_Asset.Maps, v ) )
                    continue;
                m_Velocities[v] = ( m_Velocities[v] + accel * dt ) * ( 1.0f - m_Asset.Config.LinearDamping );
                m_Positions[v] += m_Velocities[v] * dt;
            }
            return Common::MakeSuccess( true );
        }

        [[nodiscard]] ClothSimulationOutput GetOutput() const override
        {
            return { m_Positions, m_Asset.Mesh.Normals };
        }

        void Teleport( const glm::mat4&, ClothTeleportMode mode ) override
        {
            if ( mode == ClothTeleportMode::Reset )
                m_Positions = m_Asset.Mesh.Positions;
            std::fill( m_Velocities.begin(), m_Velocities.end(), glm::vec3( 0.0f ) );
        }

    private:
        ClothingAsset          m_Asset;
        Counters&              m_Counters;
        std::vector<glm::vec3> m_Positions;
        std::vector<glm::vec3> m_Velocities;
    };

    class FakeClothFactory final : public IClothingSimulationFactory
    {
    public:
        explicit FakeClothFactory( Counters& counters ) : m_Counters( counters )
        {
        }

        [[nodiscard]] std::string_view GetName() const override
        {
            return "FakeCloth";
        }

        [[nodiscard]] Common::BoolResultStr SupportsAsset( const ClothingAsset& asset ) const override
        {
            if ( asset.Maps.MaxDistance.size() != asset.Mesh.Positions.size() )
                return Common::MakeError<bool>( "max-distance map does not cover the mesh" );
            return Common::MakeSuccess( true );
        }

        Common::ResultStr<std::unique_ptr<IClothingSimulation>> CreateSimulation( const ClothingAsset& asset,
                                                                                  const glm::mat4& ) override
        {
            if ( auto supported = SupportsAsset( asset ); !supported.IsSuccess() )
                return Common::MakeError<std::unique_ptr<IClothingSimulation>>( supported.GetError() );
            return Common::MakeSuccess(
                 std::unique_ptr<IClothingSimulation>( new FakeClothSimulation( asset, m_Counters ) ) );
        }

    private:
        Counters& m_Counters;
    };

    // A two-vertex strip: vertex 0 pinned to the skin, vertex 1 free to fall.
    ClothingAsset VestCloth()
    {
        ClothingAsset asset;
        asset.Name                  = "VestFlap";
        asset.Mesh.Positions        = { { 0.0f, 150.0f, 0.0f }, { 0.0f, 140.0f, 0.0f } };
        asset.Mesh.Normals          = { { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 1.0f } };
        asset.Maps.MaxDistance      = { 0.0f, 20.0f };
        asset.Maps.BackstopDistance = { 0.0f, 0.0f };
        asset.Maps.BackstopRadius   = { 0.0f, 0.0f };
        asset.UsedBoneNames         = { "root", "spine", "head" };
        asset.Colliders.emplace_back(
             Physics::Cloth::ClothCapsuleCollider{ "spine", {}, { 0, 40, 0 }, 15.0f, 12.0f } );
        return asset;
    }

    // Stands in for EQP1's ECS-backed character. Everything a script can do goes through IModularCharacter.
    class FakeModularCharacter final : public Animation::IModularCharacter
    {
    public:
        FakeModularCharacter( ClothingSimulationFactoryRegistry& registry, std::vector<std::string> leaderBones,
                              uint32_t bodySections )
             : m_Registry( registry ), m_LeaderBones( std::move( leaderBones ) ),
               m_SectionVisible( bodySections, true )
        {
        }

        std::map<uint64_t, uint32_t>      MeshBoneCount; // skinned mesh -> bones in its skeleton
        std::map<uint64_t, ClothingAsset> ClothAssets;

        Common::ResultStr<Animation::FollowerMeshId> AttachFollowerMesh( Common::AssetHandle mesh ) override
        {
            const auto it = MeshBoneCount.find( static_cast<uint64_t>( mesh ) );
            if ( it == MeshBoneCount.end() || it->second != m_LeaderBones.size() )
                return Common::MakeError<Animation::FollowerMeshId>( "skeleton does not match the leader's" );
            const Animation::FollowerMeshId id{ ++m_NextId };
            m_Followers.push_back( id.Value );
            return Common::MakeSuccess( id );
        }

        Common::BoolResultStr DetachFollowerMesh( Animation::FollowerMeshId follower ) override
        {
            const auto it = std::find( m_Followers.begin(), m_Followers.end(), follower.Value );
            if ( it == m_Followers.end() )
                return Common::MakeError<bool>( "unknown follower " + std::to_string( follower.Value ) );
            std::erase_if( m_Cloth, [&]( const auto& c ) { return c.second.Follower == follower.Value; } );
            m_Followers.erase( it );
            return Common::MakeSuccess( true );
        }

        Common::ResultStr<Animation::SocketAttachmentId>
        AttachToSocket( const Animation::SocketAttachmentDesc& desc ) override
        {
            if ( std::find( m_LeaderBones.begin(), m_LeaderBones.end(), desc.BoneName ) == m_LeaderBones.end() )
                return Common::MakeError<Animation::SocketAttachmentId>( "no bone '" + desc.BoneName + "'" );
            const Animation::SocketAttachmentId id{ ++m_NextId };
            Sockets[id.Value] = desc;
            return Common::MakeSuccess( id );
        }

        Common::BoolResultStr DetachFromSocket( Animation::SocketAttachmentId attachment ) override
        {
            if ( Sockets.erase( attachment.Value ) == 0 )
                return Common::MakeError<bool>( "unknown socket attachment" );
            return Common::MakeSuccess( true );
        }

        Common::BoolResultStr SetBodySectionVisible( uint32_t section, bool visible ) override
        {
            if ( section >= m_SectionVisible.size() )
                return Common::MakeError<bool>( "section " + std::to_string( section ) +
                                                " >= " + std::to_string( m_SectionVisible.size() ) );
            m_SectionVisible[section] = visible;
            return Common::MakeSuccess( true );
        }

        Common::ResultStr<Animation::ClothInstanceId> CreateCloth( Animation::FollowerMeshId follower,
                                                                   Common::AssetHandle       clothAsset,
                                                                   std::string_view          backend ) override
        {
            if ( std::find( m_Followers.begin(), m_Followers.end(), follower.Value ) == m_Followers.end() )
                return Common::MakeError<Animation::ClothInstanceId>( "unknown follower" );
            const auto asset = ClothAssets.find( static_cast<uint64_t>( clothAsset ) );
            if ( asset == ClothAssets.end() )
                return Common::MakeError<Animation::ClothInstanceId>( "unknown cloth asset" );
            auto factory = m_Registry.Find( backend );
            if ( !factory.IsSuccess() )
                return Common::MakeError<Animation::ClothInstanceId>( factory.GetError() );
            auto sim = factory.GetValue()->CreateSimulation( asset->second, glm::mat4( 1.0f ) );
            if ( !sim.IsSuccess() )
                return Common::MakeError<Animation::ClothInstanceId>( sim.GetError() );
            const Animation::ClothInstanceId id{ ++m_NextId };
            m_Cloth[id.Value] = { follower.Value, sim.ExtractValue() };
            return Common::MakeSuccess( id );
        }

        Common::BoolResultStr DestroyCloth( Animation::ClothInstanceId cloth ) override
        {
            if ( m_Cloth.erase( cloth.Value ) == 0 )
                return Common::MakeError<bool>( "unknown cloth instance" );
            return Common::MakeSuccess( true );
        }

        Common::BoolResultStr ApplyLeaderPose( const Animation::LeaderPose& pose, float dt ) override
        {
            for ( auto& [id, cloth] : m_Cloth )
            {
                auto stepped = cloth.Simulation->Step(
                     { dt, pose.ComponentToWorld, pose.BoneModelTransforms, pose.Gravity, pose.WindVelocity } );
                if ( !stepped.IsSuccess() )
                    return stepped;
            }
            return Common::MakeSuccess( true );
        }

        [[nodiscard]] bool IsSectionVisible( uint32_t section ) const
        {
            return m_SectionVisible[section];
        }
        [[nodiscard]] size_t FollowerCount() const
        {
            return m_Followers.size();
        }

        std::map<uint32_t, Animation::SocketAttachmentDesc> Sockets;

    private:
        struct Cloth
        {
            uint32_t                             Follower = 0;
            std::unique_ptr<IClothingSimulation> Simulation;
        };

        ClothingSimulationFactoryRegistry& m_Registry;
        std::vector<std::string>           m_LeaderBones;
        std::vector<bool>                  m_SectionVisible;
        std::vector<uint32_t>              m_Followers;
        std::map<uint32_t, Cloth>          m_Cloth;
        uint32_t                           m_NextId = 0;
    };

    constexpr uint64_t k_VestMesh     = 1001;
    constexpr uint64_t k_VestCloth    = 1002;
    constexpr uint64_t k_Helmet       = 1003;
    constexpr uint32_t k_TorsoSection = 1;

    struct Fixture
    {
        Counters                          Count;
        ClothingSimulationFactoryRegistry Registry;
        FakeModularCharacter              Character{ Registry, { "root", "spine", "head" }, 3 };

        Fixture()
        {
            EXPECT_TRUE( Registry.Register( std::make_unique<FakeClothFactory>( Count ) ).IsSuccess() );
            Character.MeshBoneCount[k_VestMesh] = 3;
            Character.ClothAssets[k_VestCloth]  = VestCloth();
        }
    };

    // ---- the "script" ----------------------------------------------------------------------------------
    struct WornVest
    {
        Animation::FollowerMeshId  Mesh;
        Animation::ClothInstanceId Cloth;
    };

    WornVest PutOnVest( Animation::IModularCharacter& character )
    {
        const auto mesh = character.AttachFollowerMesh( Common::AssetHandle( k_VestMesh ) );
        EXPECT_TRUE( mesh.IsSuccess() );
        const auto cloth =
             character.CreateCloth( mesh.GetValue(), Common::AssetHandle( k_VestCloth ), "FakeCloth" );
        EXPECT_TRUE( cloth.IsSuccess() ) << cloth.GetError();
        EXPECT_TRUE( character.SetBodySectionVisible( k_TorsoSection, false ).IsSuccess() );
        return { mesh.GetValue(), cloth.GetValue() };
    }

    void TakeOffVest( Animation::IModularCharacter& character, const WornVest& vest )
    {
        EXPECT_TRUE( character.DestroyCloth( vest.Cloth ).IsSuccess() );
        EXPECT_TRUE( character.DetachFollowerMesh( vest.Mesh ).IsSuccess() );
        EXPECT_TRUE( character.SetBodySectionVisible( k_TorsoSection, true ).IsSuccess() );
    }
} // namespace

TEST( ClothHairEquipmentApi, PuttingOnTheVestCreatesItsClothThroughTheFactory )
{
    Fixture f;
    PutOnVest( f.Character );
    EXPECT_EQ( f.Count.Created, 1 );
    EXPECT_EQ( f.Count.Live, 1 );
    EXPECT_EQ( f.Character.FollowerCount(), 1u );
    EXPECT_FALSE( f.Character.IsSectionVisible( k_TorsoSection ) );
}

TEST( ClothHairEquipmentApi, TakingOffTheVestDestroysItsCloth )
{
    Fixture        f;
    const WornVest vest = PutOnVest( f.Character );
    TakeOffVest( f.Character, vest );
    EXPECT_EQ( f.Count.Live, 0 );
    EXPECT_EQ( f.Character.FollowerCount(), 0u );
    EXPECT_TRUE( f.Character.IsSectionVisible( k_TorsoSection ) );
    // A stale id is an error, not another object.
    EXPECT_FALSE( f.Character.DestroyCloth( vest.Cloth ).IsSuccess() );
}

TEST( ClothHairEquipmentApi, DetachingTheMeshTakesItsClothWithIt )
{
    Fixture        f;
    const WornVest vest = PutOnVest( f.Character );
    EXPECT_TRUE( f.Character.DetachFollowerMesh( vest.Mesh ).IsSuccess() );
    EXPECT_EQ( f.Count.Live, 0 );
}

TEST( ClothHairEquipmentApi, TheHelmetGoesOnTheHeadSocketAndMakesNoCloth )
{
    Fixture                         f;
    Animation::SocketAttachmentDesc helmet{ Common::AssetHandle( k_Helmet ), "head", { 0, 12, 0 } };
    const auto                      attached = f.Character.AttachToSocket( helmet );
    ASSERT_TRUE( attached.IsSuccess() );
    EXPECT_EQ( f.Character.Sockets.at( attached.GetValue().Value ).BoneName, "head" );
    EXPECT_EQ( f.Count.Created, 0 );

    helmet.BoneName = "hat_bone";
    EXPECT_FALSE( f.Character.AttachToSocket( helmet ).IsSuccess() );
    EXPECT_TRUE( f.Character.DetachFromSocket( attached.GetValue() ).IsSuccess() );
}

TEST( ClothHairEquipmentApi, AMissingBackendIsAnErrorThatNamesTheRegisteredOnes )
{
    Fixture    f;
    const auto mesh = f.Character.AttachFollowerMesh( Common::AssetHandle( k_VestMesh ) );
    const auto cloth =
         f.Character.CreateCloth( mesh.GetValue(), Common::AssetHandle( k_VestCloth ), "JoltSoftBody" );
    ASSERT_FALSE( cloth.IsSuccess() );
    EXPECT_NE( cloth.GetError().find( "registered: [FakeCloth]" ), std::string::npos ) << cloth.GetError();
    EXPECT_FALSE( f.Registry.Register( std::make_unique<FakeClothFactory>( f.Count ) ).IsSuccess() );
}

TEST( ClothHairEquipmentApi, AFollowerOnAnotherSkeletonIsRefused )
{
    Fixture f;
    f.Character.MeshBoneCount[k_VestMesh] = 2;
    EXPECT_FALSE( f.Character.AttachFollowerMesh( Common::AssetHandle( k_VestMesh ) ).IsSuccess() );
}

TEST( ClothHairEquipmentApi, AClothStepIsAPureFunctionOfItsInputs )
{
    Counters         count;
    FakeClothFactory factory( count );
    const auto       asset = VestCloth();
    auto             madeA = factory.CreateSimulation( asset, glm::mat4( 1.0f ) );
    auto             madeB = factory.CreateSimulation( asset, glm::mat4( 1.0f ) );
    ASSERT_TRUE( madeA.IsSuccess() && madeB.IsSuccess() );
    auto a = madeA.ExtractValue();
    auto b = madeB.ExtractValue();

    const std::vector<glm::mat4> bones( 3, glm::mat4( 1.0f ) );
    ClothStepContext             context{ 1.0f / 60.0f, glm::mat4( 1.0f ), bones };
    context.WindVelocity = { 300.0f, 0.0f, 0.0f };
    for ( int i = 0; i < 60; ++i )
    {
        ASSERT_TRUE( a->Step( context ).IsSuccess() );
        ASSERT_TRUE( b->Step( context ).IsSuccess() );
    }
    const auto outA = a->GetOutput().Positions;
    const auto outB = b->GetOutput().Positions;
    ASSERT_EQ( outA.size(), outB.size() );
    EXPECT_EQ( std::memcmp( outA.data(), outB.data(), outA.size_bytes() ), 0 );
    EXPECT_EQ( outA[0], asset.Mesh.Positions[0] ) << "a zero max distance is a fixed vertex";
    EXPECT_LT( outA[1].y, asset.Mesh.Positions[1].y ) << "the free vertex fell";

    // A different dt is a different result; a non-positive or bone-mismatched step is refused.
    ClothStepContext half  = context;
    half.FixedDeltaSeconds = 1.0f / 120.0f;
    ASSERT_TRUE( b->Step( half ).IsSuccess() );
    ASSERT_TRUE( a->Step( context ).IsSuccess() );
    EXPECT_NE( a->GetOutput().Positions[1], b->GetOutput().Positions[1] );
    half.FixedDeltaSeconds = 0.0f;
    EXPECT_FALSE( a->Step( half ).IsSuccess() );
    ClothStepContext noBones;
    noBones.FixedDeltaSeconds = 1.0f / 60.0f;
    EXPECT_FALSE( a->Step( noBones ).IsSuccess() );

    a->Teleport( glm::mat4( 1.0f ), ClothTeleportMode::Reset );
    EXPECT_EQ( a->GetOutput().Positions[1], asset.Mesh.Positions[1] );
}

TEST( ClothHairEquipmentApi, ThePoseContractStepsWornClothWithTheLeadersPose )
{
    Fixture f;
    PutOnVest( f.Character );
    const std::vector<glm::mat4> threeBones( 3, glm::mat4( 1.0f ) );
    const std::vector<glm::mat4> twoBones( 2, glm::mat4( 1.0f ) );
    EXPECT_TRUE( f.Character.ApplyLeaderPose( { threeBones }, 1.0f / 60.0f ).IsSuccess() );
    EXPECT_FALSE( f.Character.ApplyLeaderPose( { twoBones }, 1.0f / 60.0f ).IsSuccess() );
}

namespace
{
    // One groom instance, simulated and rendered, as HAIR1's will be.
    class FakeGroom final : public Hair::IGroomSimulation, public Hair::IGroomRenderDataSource
    {
    public:
        explicit FakeGroom( const Hair::GroomAsset& asset ) : m_Asset( asset )
        {
            Reset();
        }

        Common::BoolResultStr Step( const Hair::GroomStepContext& context ) override
        {
            if ( !( context.FixedDeltaSeconds > 0.0f ) )
                return Common::MakeError<bool>( "dt <= 0" );
            for ( auto& p : m_Guides[0] )
                p += context.Gravity * context.FixedDeltaSeconds * context.FixedDeltaSeconds;
            m_Strands = m_Asset.Groups[0].Strands.Points; // interpolation is HAIR1's
            ++m_Revision;
            return Common::MakeSuccess( true );
        }

        [[nodiscard]] std::span<const glm::vec3> GetGuidePositions( uint32_t group ) const override
        {
            return m_Guides[group];
        }

        void Reset() override
        {
            m_Guides  = { m_Asset.Groups[0].Guides.Points };
            m_Strands = m_Asset.Groups[0].Strands.Points;
        }

        [[nodiscard]] Hair::HairRenderFrame GetRenderFrame() const override
        {
            const auto& strands = m_Asset.Groups[0].Strands;
            return { m_Revision,
                     { { m_Strands, strands.PointRadius, strands.CurvePointOffset, strands.CurvePointCount,
                         m_Asset.Groups[0].Material } } };
        }

    private:
        Hair::GroomAsset                    m_Asset;
        std::vector<std::vector<glm::vec3>> m_Guides;
        std::vector<glm::vec3>              m_Strands;
        uint64_t                            m_Revision = 0;
    };

    class FakeGroomFactory final : public Hair::IGroomSimulationFactory
    {
    public:
        [[nodiscard]] std::string_view GetName() const override
        {
            return "FakeGroom";
        }

        Common::ResultStr<std::unique_ptr<Hair::IGroomSimulation>>
        CreateSimulation( const Hair::GroomAsset& asset, const Hair::GroomBinding& binding ) override
        {
            if ( binding.GuideRoots.size() != asset.Groups.size() )
                return Common::MakeError<std::unique_ptr<Hair::IGroomSimulation>>(
                     "binding groups != asset groups" );
            return Common::MakeSuccess( std::unique_ptr<Hair::IGroomSimulation>( new FakeGroom( asset ) ) );
        }
    };
} // namespace

TEST( ClothHairEquipmentApi, TheGroomRenderFrameCarriesTheAssetsCurveLayout )
{
    Hair::HairGroup group;
    group.Name                     = "Scalp";
    group.Strands.Points           = { { 0, 170, 0 }, { 0, 165, 0 }, { 1, 170, 0 }, { 1, 165, 0 } };
    group.Strands.PointRadius      = { 0.01f, 0.01f, 0.01f, 0.01f };
    group.Strands.CurvePointOffset = { 0, 2 };
    group.Strands.CurvePointCount  = { 2, 2 };
    group.Guides.Points            = { { 0.5f, 170, 0 }, { 0.5f, 165, 0 } };
    group.Guides.CurvePointOffset  = { 0 };
    group.Guides.CurvePointCount   = { 2 };
    const Hair::GroomAsset asset{ "Buzzcut", { group } };

    FakeGroomFactory factory;
    EXPECT_FALSE( factory.CreateSimulation( asset, Hair::GroomBinding{} ).IsSuccess() );
    Hair::GroomBinding binding;
    binding.GuideRoots.resize( 1 );
    binding.StrandRoots.resize( 1 );
    auto made = factory.CreateSimulation( asset, binding );
    ASSERT_TRUE( made.IsSuccess() ) << made.GetError();
    auto simulation = made.ExtractValue();
    Hair::GroomStepContext step;
    step.FixedDeltaSeconds = 1.0f / 60.0f;
    ASSERT_TRUE( simulation->Step( step ).IsSuccess() );
    EXPECT_LT( simulation->GetGuidePositions( 0 )[1].y, 165.0f );

    const auto&                 source = dynamic_cast<const Hair::IGroomRenderDataSource&>( *simulation );
    const Hair::HairRenderFrame frame  = source.GetRenderFrame();
    ASSERT_EQ( frame.Groups.size(), 1u );
    EXPECT_EQ( frame.Revision, 1u );
    EXPECT_EQ( frame.Groups[0].Positions.size(), group.Strands.Points.size() );
    EXPECT_EQ( frame.Groups[0].CurvePointCount.size(), 2u );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
