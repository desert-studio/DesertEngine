// The DECISIONS the gameplay systems make, tested without a Scene.
//
// AttachmentSystem and the light systems hold a `Core::Scene*`, and Scene drags in the renderer — so
// asking "where does a socketed entity end up?" would otherwise mean linking the graphics stack
// and standing up a device. The rules live in Engine/ECS/System/SystemRules.hpp; the systems only fetch
// the arguments. Same split that made the shadow cascades testable.

#include <Engine/ECS/System/SystemRules.hpp>

#include <Common/Core/Units.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <vector>

#include <algorithm>
#include <array>
#include <vector>

using Desert::ECS::Rules::AtmosphereSunDirection;
using Desert::ECS::Rules::DecomposeTransform;
using Desert::ECS::Rules::DirectionalLightTravel;
using Desert::ECS::Rules::FallbackAtmosphereSunDirection;
using Desert::ECS::Rules::IsSunDirectionValid;
using Desert::ECS::Rules::MeshShadowCaster;
using Desert::ECS::Rules::RouteMeshShadowCaster;
using Desert::ECS::Rules::SelectAtmosphereSun;
using Desert::ECS::Rules::SocketLocalTransform;
using Desert::ECS::Rules::SunCandidate;

namespace Units = Common::Units;

// ---------------------------------------------------------------------------------------------------
// Sockets: bone -> attached entity transform
// ---------------------------------------------------------------------------------------------------

namespace
{
    // Where the socket ends up in WORLD space (the rule returns a parent-local transform).
    glm::vec3 WorldPositionOf( const glm::mat4& local, const glm::mat4& parentWorld = glm::mat4( 1.0f ) )
    {
        return glm::vec3( parentWorld * local * glm::vec4( 0, 0, 0, 1 ) );
    }
} // namespace

TEST( SocketRules, PutsTheEntityOnTheBone )
{
    // Character at x = 5 m; the hand bone is 1.5 m up and 0.3 m out in model space.
    const glm::mat4 targetWorld =
         glm::translate( glm::mat4( 1.0f ), glm::vec3( Units::Metres( 5.0f ), 0.0f, 0.0f ) );
    const glm::mat4 boneModel =
         glm::translate( glm::mat4( 1.0f ), glm::vec3( Units::Metres( 0.3f ), Units::Metres( 1.5f ), 0.0f ) );

    const glm::mat4 local =
         SocketLocalTransform( targetWorld, boneModel, glm::vec3( 0.0f ), glm::vec3( 0.0f ), glm::vec3( 1.0f ) );
    const glm::vec3 world = WorldPositionOf( local );

    EXPECT_NEAR( world.x, Units::Metres( 5.3f ), 0.01f );
    EXPECT_NEAR( world.y, Units::Metres( 1.5f ), 0.01f );
    EXPECT_NEAR( world.z, 0.0f, 0.01f );
}

TEST( SocketRules, GripOffsetIsAppliedInBoneSpace )
{
    // Bone rotated 90° about +Y: an offset along +X must come out along -Z in world space.
    const glm::mat4 targetWorld( 1.0f );
    const glm::mat4 boneModel = glm::rotate( glm::mat4( 1.0f ), glm::radians( 90.0f ), glm::vec3( 0, 1, 0 ) );

    const glm::mat4 local =
         SocketLocalTransform( targetWorld, boneModel, glm::vec3( Units::Metres( 1.0f ), 0.0f, 0.0f ),
                               glm::vec3( 0.0f ), glm::vec3( 1.0f ) );
    const glm::vec3 world = WorldPositionOf( local );

    EXPECT_NEAR( world.x, 0.0f, 0.5f );
    EXPECT_NEAR( world.z, -Units::Metres( 1.0f ), 0.5f );
}

// A parented weapon must be brought back into its parent's space — otherwise it doubles the parent's
// motion and drifts off into the distance as the parent moves.
TEST( SocketRules, ParentSpaceIsRemovedFromTheResult )
{
    const glm::mat4 parentWorld =
         glm::translate( glm::mat4( 1.0f ), glm::vec3( Units::Metres( 10.0f ), 0.0f, 0.0f ) );
    const glm::mat4 targetWorld =
         glm::translate( glm::mat4( 1.0f ), glm::vec3( Units::Metres( 2.0f ), 0.0f, 0.0f ) );
    const glm::mat4 boneModel( 1.0f );

    const glm::mat4 local = SocketLocalTransform( targetWorld, boneModel, glm::vec3( 0.0f ), glm::vec3( 0.0f ),
                                                  glm::vec3( 1.0f ), parentWorld );

    // Local sits 8 m BEHIND the parent, so that parent * local lands on the bone at 2 m.
    EXPECT_NEAR( local[3].x, Units::Metres( -8.0f ), 0.01f );
    EXPECT_NEAR( WorldPositionOf( local, parentWorld ).x, Units::Metres( 2.0f ), 0.01f );
}

TEST( SocketRules, DecomposeRoundTripsTranslationScaleAndRotation )
{
    const glm::vec3 translation( Units::Metres( 1.0f ), Units::Metres( 2.0f ), Units::Metres( -3.0f ) );
    const glm::vec3 scale( 2.0f, 2.0f, 2.0f );
    const float     yaw = glm::radians( 35.0f );

    const glm::mat4 m = glm::translate( glm::mat4( 1.0f ), translation ) *
                        glm::rotate( glm::mat4( 1.0f ), yaw, glm::vec3( 0, 1, 0 ) ) *
                        glm::scale( glm::mat4( 1.0f ), scale );

    const auto out = DecomposeTransform( m );
    EXPECT_NEAR( out.Translation.x, translation.x, 0.01f );
    EXPECT_NEAR( out.Translation.y, translation.y, 0.01f );
    EXPECT_NEAR( out.Translation.z, translation.z, 0.01f );
    EXPECT_NEAR( out.Scale.x, scale.x, 0.01f );
    // Rotation comes back in RADIANS, like TransformComponent stores it — a degree here would be the
    // whole bug of "the weapon is rotated 57 times too far".
    EXPECT_NEAR( out.Rotation.y, yaw, 0.01f );
}

// ---------------------------------------------------------------------------------------------------
// Which directional light is THE SUN
//
// Six rules, one test each. Before this, "the sun" was whichever directional light the registry happened
// to visit first — and the sky and the lighting used different iteration orders, so they could disagree
// about it with nothing said anywhere.
// ---------------------------------------------------------------------------------------------------

namespace
{
    SunCandidate Sun( uint64_t id, bool marked, int index = 0, bool valid = true )
    {
        return SunCandidate{ .Id = id, .Marked = marked, .Index = index, .DirectionValid = valid };
    }
} // namespace

// Rule 1: a degenerate Translation is not a direction — normalizing it yields NaN and the sun points
// nowhere, so such a light is not a candidate at all.
TEST( AtmosphereSunRules, DegenerateDirectionsAreIgnoredEvenWhenMarked )
{
    const std::array<SunCandidate, 2> candidates{ Sun( 1, /*marked=*/true, 0, /*valid=*/false ),
                                                  Sun( 9, /*marked=*/false, 0, /*valid=*/true ) };

    const auto selection = SelectAtmosphereSun( candidates, 0 );
    ASSERT_TRUE( selection.Chosen.has_value() );
    EXPECT_EQ( candidates[*selection.Chosen].Id, 9u ) << "the marked one had no usable direction";
    EXPECT_TRUE( selection.Fallback );
}

// Rule 2: a marked light at the wanted index beats an unmarked one, whatever the ids say.
TEST( AtmosphereSunRules, MarkedBeatsUnmarkedRegardlessOfId )
{
    const std::array<SunCandidate, 2> candidates{ Sun( 1, /*marked=*/false ), Sun( 500, /*marked=*/true ) };

    const auto selection = SelectAtmosphereSun( candidates, 0 );
    ASSERT_TRUE( selection.Chosen.has_value() );
    EXPECT_EQ( candidates[*selection.Chosen].Id, 500u );
    EXPECT_FALSE( selection.Fallback );
    EXPECT_TRUE( selection.Collisions.empty() );
}

// Rule 3: several marked at the same index -> lowest id, deterministically, and every loser is named.
TEST( AtmosphereSunRules, TieIsBrokenByLowestIdAndIsOrderIndependent )
{
    const std::array<SunCandidate, 3> ascending{ Sun( 7, true ), Sun( 3, true ), Sun( 11, true ) };
    const std::array<SunCandidate, 3> shuffled{ Sun( 11, true ), Sun( 7, true ), Sun( 3, true ) };

    const auto a = SelectAtmosphereSun( ascending, 0 );
    const auto b = SelectAtmosphereSun( shuffled, 0 );

    ASSERT_TRUE( a.Chosen.has_value() );
    ASSERT_TRUE( b.Chosen.has_value() );
    EXPECT_EQ( ascending[*a.Chosen].Id, 3u );
    EXPECT_EQ( shuffled[*b.Chosen].Id, 3u ) << "the same scene in a different order picks the same sun";

    // Both losers are reported so the caller can name every colliding entity, not just the count.
    EXPECT_EQ( a.Collisions.size(), 2u );
    std::vector<uint64_t> collided;
    for ( const size_t i : a.Collisions )
        collided.push_back( ascending[i].Id );
    std::sort( collided.begin(), collided.end() );
    EXPECT_EQ( collided, ( std::vector<uint64_t>{ 7u, 11u } ) );
}

// Rule 4: marked at an index the engine cannot render is TREATED AS UNMARKED, and reported. The field is
// authorable, but v1 renders exactly one directional light, so index 1 confers nothing.
//
// "Treated as unmarked" is the whole content of the rule, and it has two halves that must both hold:
// the light loses its priority, but it does NOT lose its candidacy — it still competes in the lowest-id
// fallback like any other unmarked light.
TEST( AtmosphereSunRules, MarkedAtAnotherIndexLosesItsPriorityButNotItsCandidacy )
{
    // Half one: a light marked at index 1 does NOT outrank a light properly marked at index 0, even
    // though its id is lower. This is the half that would break if the index were ignored.
    const std::array<SunCandidate, 2> contested{ Sun( 4, /*marked=*/true, /*index=*/1 ),
                                                 Sun( 8, /*marked=*/true, /*index=*/0 ) };

    const auto winner = SelectAtmosphereSun( contested, 0 );
    ASSERT_TRUE( winner.Chosen.has_value() );
    EXPECT_EQ( contested[*winner.Chosen].Id, 8u ) << "index 0 is the only index that drives the sky";
    EXPECT_FALSE( winner.Fallback );
    ASSERT_EQ( winner.WrongIndex.size(), 1u );
    EXPECT_EQ( contested[winner.WrongIndex.front()].Id, 4u ) << "and the demotion is reported, not silent";

    // Half two: with no properly-marked light present it is an ordinary unmarked candidate, so the plain
    // lowest-id fallback applies and its low id wins. Demoted is not disqualified.
    const std::array<SunCandidate, 2> uncontested{ Sun( 4, /*marked=*/true, /*index=*/1 ),
                                                   Sun( 8, /*marked=*/false, /*index=*/0 ) };

    const auto fallback = SelectAtmosphereSun( uncontested, 0 );
    ASSERT_TRUE( fallback.Chosen.has_value() );
    EXPECT_EQ( uncontested[*fallback.Chosen].Id, 4u );
    EXPECT_TRUE( fallback.Fallback );
    EXPECT_EQ( fallback.WrongIndex.size(), 1u );
}

// Rule 5: nobody ticked the box -> the lowest-id valid light still drives the sky, and says so. The sky
// must never go missing because of an unticked checkbox.
TEST( AtmosphereSunRules, FallsBackToTheLowestIdWhenNothingIsMarked )
{
    const std::array<SunCandidate, 3> candidates{ Sun( 30, false ), Sun( 10, false ), Sun( 20, false ) };

    const auto selection = SelectAtmosphereSun( candidates, 0 );
    ASSERT_TRUE( selection.Chosen.has_value() );
    EXPECT_EQ( candidates[*selection.Chosen].Id, 10u );
    EXPECT_TRUE( selection.Fallback );
}

// Rule 6: nothing usable at all -> no selection; the caller uses the documented fallback direction.
TEST( AtmosphereSunRules, NoValidCandidateSelectsNothing )
{
    const std::array<SunCandidate, 2> candidates{ Sun( 1, true, 0, /*valid=*/false ),
                                                  Sun( 2, false, 0, /*valid=*/false ) };

    EXPECT_FALSE( SelectAtmosphereSun( candidates, 0 ).Chosen.has_value() );
    EXPECT_FALSE( SelectAtmosphereSun( std::span<const SunCandidate>{}, 0 ).Chosen.has_value() );
}

// ---------------------------------------------------------------------------------------------------
// The ONE negation
// ---------------------------------------------------------------------------------------------------

TEST( AtmosphereSunRules, TowardSunIsTheOppositeOfTheTravelDirection )
{
    // The corrected value the shipped scenes now carry: the light travels DOWN, so the sun is UP.
    const glm::vec3 travel( -0.3508783f, -0.9022585f, -0.2506274f );
    const glm::vec3 toward = AtmosphereSunDirection( travel );

    EXPECT_NEAR( glm::length( toward ), 1.0f, 1e-5f );
    EXPECT_GT( toward.y, 0.0f ) << "a sun above the horizon";
    EXPECT_NEAR( glm::degrees( std::asin( toward.y ) ), 64.5f, 0.1f );

    // Applying it twice returns the travel direction — which is exactly what a second, "compensating"
    // negation elsewhere in the engine would undo.
    const glm::vec3 back = -toward;
    EXPECT_NEAR( back.x, glm::normalize( travel ).x, 1e-5f );
    EXPECT_NEAR( back.y, glm::normalize( travel ).y, 1e-5f );
}

TEST( AtmosphereSunRules, DirectionValidityUsesOneEpsilon )
{
    EXPECT_FALSE( IsSunDirectionValid( glm::vec3( 0.0f ) ) );
    EXPECT_FALSE( IsSunDirectionValid( glm::vec3( 1e-5f, 0.0f, 0.0f ) ) );
    EXPECT_TRUE( IsSunDirectionValid( glm::vec3( 0.0f, -1.0f, 0.0f ) ) );

    // The documented no-light fallback points ABOVE the horizon, so an empty scene is lit, not black.
    EXPECT_NEAR( glm::length( FallbackAtmosphereSunDirection() ), 1.0f, 1e-5f );
    EXPECT_GT( FallbackAtmosphereSunDirection().y, 0.0f );
}

// Г18 — AND THE TEST ABOVE IS WHY THIS ONE HAD TO BE WRITTEN.
//
// It is called "uses ONE epsilon" and it asks exactly one consumer. The comment on
// kSunDirectionEpsilon likewise says "two different epsilons for this existed in the engine; this is
// the one". Both were describing an intention: `Engine/Core/Scene.cpp`, which collects the directional
// lights the RENDERER shades with, went on open-coding `glm::length(rawDir) > 0.001f` — ten times this
// epsilon — while the sky asked IsSunDirectionValid. A name and a comment each asserted a guarantee the
// tree did not honour, and neither could fail.
//
// The band between the two thresholds is where they disagreed, and it is a real state a scene can be
// in: a Translation of 5e-4 is a sun to the atmosphere and no light at all to the deferred composite,
// at the same instant, in the same frame.
TEST( AtmosphereSunRules, TheLightingGateAndTheSkyGateAreTheSameGate )
{
    // Every value in the old disagreement band. Each is VALID: the renderer used to drop all of them
    // while the sky lit the dome from them.
    for ( const float length : { 1.1e-4f, 5e-4f, 9.9e-4f } )
    {
        const glm::vec3 travel( 0.0f, -length, 0.0f );
        EXPECT_TRUE( IsSunDirectionValid( travel ) ) << length;
        EXPECT_TRUE( DirectionalLightTravel( travel ).has_value() )
             << length << " is a sun to the sky; it must be a light to the renderer too";
    }

    // And the two answer NO together, so the agreement is not one-sided.
    for ( const float length : { 0.0f, 1e-6f, 1e-5f } )
    {
        const glm::vec3 travel( 0.0f, -length, 0.0f );
        EXPECT_FALSE( IsSunDirectionValid( travel ) ) << length;
        EXPECT_FALSE( DirectionalLightTravel( travel ).has_value() ) << length;
    }
}

// THE CENSUS, because the two tests above are true BY CONSTRUCTION and therefore cannot catch the thing
// that actually happened: somebody writing another copy of the threshold somewhere else.
//
// Three sites open-coded it. `Engine/Core/Scene.cpp` — the collector that decides which directional
// lights the RENDERER shades with — compared against 0.001f, ten times kSunDirectionEpsilon, so a light
// whose Translation measured between the two was a sun to the atmosphere and no light at all to the
// deferred composite. The Details panel's sun dial and the Sky Atmosphere widget each carried their own
// 1e-4f, and the dial also spelled the negation as `-travel / length` inside a subsystem whose rule
// calls itself "the engine's ONE negation" and claims every one of them goes through it. Not one of
// those could fail, and the test above this one was ALREADY called "uses ONE epsilon".
//
// So the invariant is checked against the SOURCE. In any file that deals with directional lights, a
// length taken of a light's TRANSLATION — directly, or through a local assigned from one, which is
// exactly how Scene.cpp spelled it (`const glm::vec3& rawDir = transform.Translation;`) — must reach
// the shared gate by name within two lines either way. Tracking the local is what makes this catch the
// violation that actually shipped rather than only the ones that were easy to see.
TEST( AtmosphereSunRules, NoFileOpenCodesTheSunDirectionThreshold )
{
    const std::string root = []
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + "Desert/Desert/Source/Engine/ECS/System/SystemRules.hpp" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return std::string{};
    }();
    ASSERT_FALSE( root.empty() ) << "repository root not found from the test's working directory";

    // `name = <something>.Translation` — the local that a length is then taken of.
    const std::regex bindsTranslation( R"((\w+)\s*=\s*[A-Za-z_:.>()-]*\.Translation)" );
    const std::regex measuresTranslation( R"(glm::length\s*\(\s*[A-Za-z_:.>()-]*\.Translation)" );
    const std::regex reachesTheGate( "kSunDirectionEpsilon|IsSunDirectionValid|DirectionalLightTravel" );

    std::vector<std::string> offenders;
    for ( const std::string& tree : { "Desert/Desert/Source", "Editor/Source", "Runtime" } )
    {
        const std::filesystem::path base = root + tree;
        if ( !std::filesystem::exists( base ) )
            continue;
        for ( const auto& entry : std::filesystem::recursive_directory_iterator( base ) )
        {
            if ( !entry.is_regular_file() )
                continue;
            const auto ext = entry.path().extension().string();
            if ( ext != ".cpp" && ext != ".hpp" )
                continue;
            if ( entry.path().filename() == "SystemRules.hpp" )
                continue; // the gate itself

            std::vector<std::string> lines;
            {
                std::ifstream file( entry.path() );
                for ( std::string line; std::getline( file, line ); )
                    lines.push_back( line );
            }

            // Only files that deal with directional lights. A length taken of some other entity's
            // Translation is nobody's sun, and flagging it would train the census away within a week.
            bool aboutDirectionalLights = false;
            for ( const auto& line : lines )
                if ( line.find( "DirectionLightComponent" ) != std::string::npos ||
                     line.find( "AtmosphereSunLight" ) != std::string::npos )
                    aboutDirectionalLights = true;
            if ( !aboutDirectionalLights )
                continue;

            // Locals bound to a Translation, and where. Kept for a short window: a name reused far
            // later in a long file is a different variable for our purposes.
            std::vector<std::pair<std::string, size_t>> bound;
            for ( size_t i = 0; i < lines.size(); ++i )
            {
                std::smatch bind;
                if ( std::regex_search( lines[i], bind, bindsTranslation ) )
                    bound.push_back( { bind[1].str(), i } );

                bool measuresALightDirection = std::regex_search( lines[i], measuresTranslation );
                if ( !measuresALightDirection )
                {
                    for ( const auto& [name, at] : bound )
                    {
                        if ( i < at || i > at + 8 )
                            continue;
                        if ( std::regex_search( lines[i],
                                                std::regex( "glm::length\\s*\\(\\s*" + name + "\\s*\\)" ) ) )
                            measuresALightDirection = true;
                    }
                }
                if ( !measuresALightDirection )
                    continue;

                const size_t from    = i > 2 ? i - 2 : 0;
                bool         reached = false;
                for ( size_t j = from; j < lines.size() && j <= i + 2; ++j )
                    if ( std::regex_search( lines[j], reachesTheGate ) )
                        reached = true;
                if ( !reached )
                    offenders.push_back( entry.path().generic_string() + ":" + std::to_string( i + 1 ) + "  " +
                                         lines[i] );
            }
        }
    }

    std::string listed;
    for ( const auto& o : offenders )
        listed += "\n  " + o;
    EXPECT_TRUE( offenders.empty() )
         << "these measure a directional light's direction without reaching ECS::Rules — a sun must be "
            "judged usable in exactly one place, or the sky and the renderer disagree about whether a "
            "scene has one:"
         << listed;
}

// A caller cannot normalize a vector the same call has just declared unusable — which is the whole
// reason this returns an optional and not a bool beside a separate normalize.
TEST( AtmosphereSunRules, TheTravelDirectionIsUnitLengthOrAbsent )
{
    const auto travel = DirectionalLightTravel( glm::vec3( 0.6f, -1.0f, 0.2f ) );
    ASSERT_TRUE( travel.has_value() );
    EXPECT_NEAR( glm::length( *travel ), 1.0f, 1e-6f );

    // CB_Sun as CornellDemo.desce actually stores it: already unit length on disk, so the normalize is
    // a no-op there and the two agree exactly.
    const auto shipped = DirectionalLightTravel( glm::vec3( 0.5071f, -0.8452f, 0.1690f ) );
    ASSERT_TRUE( shipped.has_value() );
    EXPECT_NEAR( shipped->x, 0.5071f, 1e-4f );
    EXPECT_NEAR( shipped->y, -0.8452f, 1e-4f );

    EXPECT_FALSE( DirectionalLightTravel( glm::vec3( 0.0f ) ).has_value() );
}

// ---------------------------------------------------------------------------------------------------
// Which draw carries the shadow caster
// ---------------------------------------------------------------------------------------------------
//
// The shadow pass draws a mesh WHOLE (no submesh mask — depth is material-independent), so the caster
// belongs to the entity and exactly one of its draws may carry it. These tests are the double-caster
// guard: a mesh split across a custom slot and a lit slot must appear in a cascade ONCE.

TEST( MeshShadowCasterRules, CastShadowsOffMeansNobodyCasts )
{
    // Every shape of entity, all silent when the flag is off — including the ones that have no lit
    // draw at all, which is exactly where a "well, SOMEBODY should cast" fallback would creep in.
    EXPECT_EQ( RouteMeshShadowCaster( false, false, 0, true ), MeshShadowCaster::None );
    EXPECT_EQ( RouteMeshShadowCaster( false, true, 0, false ), MeshShadowCaster::None );
    EXPECT_EQ( RouteMeshShadowCaster( false, false, 3, false ), MeshShadowCaster::None );
    EXPECT_EQ( RouteMeshShadowCaster( false, false, 2, true ), MeshShadowCaster::None );
}

TEST( MeshShadowCasterRules, PlainSurfaceMeshStillCastsFromItsSurfaceDraw )
{
    // The pre-existing behaviour, pinned: introducing generic casters must not move the caster of an
    // ordinary mesh, or every all-lit scene in the corpus shifts under us.
    EXPECT_EQ( RouteMeshShadowCaster( true, false, 0, true ), MeshShadowCaster::SurfaceDraw );
}

TEST( MeshShadowCasterRules, ShaderOverrideCastsBecauseItReplacedTheSurfaceDraw )
{
    // A MaterialComponent naming a non-lit shader takes the WHOLE entity off the lit path
    // (MeshECSSystem returns early), so before this rule such a mesh cast no shadow at all — the defect.
    EXPECT_EQ( RouteMeshShadowCaster( true, true, 0, false ), MeshShadowCaster::ShaderOverride );

    // And it stays the caster even if the other flags are set: the override draw is the entity's only
    // draw, so nothing else could carry it.
    EXPECT_EQ( RouteMeshShadowCaster( true, true, 4, true ), MeshShadowCaster::ShaderOverride );
}

TEST( MeshShadowCasterRules, MixedMeshCastsOnceFromTheSurfaceDrawNotFromBoth )
{
    // THE double-caster case: submesh 0 on a custom slot material, submesh 1 still lit. Both draws
    // exist and either could rasterize the mesh into the cascade — the rule picks one, and it picks
    // the one that was already casting.
    EXPECT_EQ( RouteMeshShadowCaster( true, false, 1, true ), MeshShadowCaster::SurfaceDraw );
    EXPECT_EQ( RouteMeshShadowCaster( true, false, 7, true ), MeshShadowCaster::SurfaceDraw );
}

TEST( MeshShadowCasterRules, AllSlotsCustomFallsToTheFirstSlotDrawOnly )
{
    // Every submesh went custom, so no lit draw was emitted and the slot draws are all there is.
    // ONE of them casts — the first — however many there are.
    EXPECT_EQ( RouteMeshShadowCaster( true, false, 1, false ), MeshShadowCaster::FirstSlotDraw );
    EXPECT_EQ( RouteMeshShadowCaster( true, false, 5, false ), MeshShadowCaster::FirstSlotDraw );
}

TEST( MeshShadowCasterRules, NoDrawAtAllCastsNothing )
{
    // Nothing was emitted for this entity (a mesh with no submeshes and no materials). There is no
    // draw to hang a caster on, and inventing one would mean recording a draw the frame never makes.
    EXPECT_EQ( RouteMeshShadowCaster( true, false, 0, false ), MeshShadowCaster::None );
}

// The property the whole rule exists for, stated as a count rather than as four separate equalities:
// over every combination of the inputs, the number of draws that carry a caster is never more than one.
TEST( MeshShadowCasterRules, NeverMoreThanOneCasterForAnyEntityShape )
{
    for ( int castShadows = 0; castShadows <= 1; ++castShadows )
        for ( int shaderOverride = 0; shaderOverride <= 1; ++shaderOverride )
            for ( size_t slotDraws = 0; slotDraws <= 4; ++slotDraws )
                for ( int surfaceEmitted = 0; surfaceEmitted <= 1; ++surfaceEmitted )
                {
                    const MeshShadowCaster route = RouteMeshShadowCaster( castShadows != 0, shaderOverride != 0,
                                                                          slotDraws, surfaceEmitted != 0 );

                    // Count the draws that would set CastShadows on their render data.
                    int casters = 0;
                    casters += ( route == MeshShadowCaster::SurfaceDraw ) ? 1 : 0;
                    casters += ( route == MeshShadowCaster::ShaderOverride ) ? 1 : 0;
                    casters += ( route == MeshShadowCaster::FirstSlotDraw ) ? 1 : 0;

                    EXPECT_LE( casters, 1 ) << "cast=" << castShadows << " override=" << shaderOverride
                                            << " slots=" << slotDraws << " standard=" << surfaceEmitted;

                    // And a caster is never routed to a draw that was not emitted.
                    if ( route == MeshShadowCaster::SurfaceDraw )
                        EXPECT_TRUE( surfaceEmitted != 0 );
                    if ( route == MeshShadowCaster::ShaderOverride )
                        EXPECT_TRUE( shaderOverride != 0 );
                    if ( route == MeshShadowCaster::FirstSlotDraw )
                        EXPECT_GT( slotDraws, 0u );
                }
}
