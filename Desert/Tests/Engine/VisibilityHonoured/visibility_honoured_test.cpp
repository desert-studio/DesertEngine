// ── WHO HONOURS "Visible", AND WHO DELIBERATELY DOES NOT ────────────────────────────────────────────
//
// THE DEFECT. `VisibilityComponent { bool Visible }` is written by the outliner's eye and by the
// Details "Visible" tick onto ANY entity. Reading it back was open-coded in exactly three collectors
// — mesh, terrain, text — and in nowhere else in the engine. So unticking Visible on the SUN, on the
// SKY, on a fog volume, on a cloud layer, on a point or spot light did nothing whatsoever: the
// collector that builds that part of the frame never asked. The owner reported it as "visible does
// not work for sun and sky and maybe some other components", and "maybe some other components" was
// the whole of the rest of the list.
//
// WHY THIS IS A CENSUS AND NOT A COUNT. A gate that asserts "N systems honour visibility" is
// satisfied by editing N, and it says nothing about WHICH one stopped. This file instead carries one
// NAMED ROW PER SYSTEM with an explicit verdict, and:
//
//   * the row list is checked against the DIRECTORY, so deleting a row is RED (a system in the tree
//     with no verdict) and adding a system is RED until somebody classifies it;
//   * an `Honours` row whose file stops calling the predicate is RED, naming that system;
//   * a `MustNot` / `OwnerDecision` row whose file STARTS reading visibility is RED, naming that
//     system — a verdict is a decision, and quietly reversing it has to be as loud as deleting it;
//   * the totals are DERIVED from the rows, never typed.
//
// WHAT "MUST NOT" PROTECTS. "Everything honours Visible" is the wrong answer. Hiding a wall you can
// still walk into is a different feature from hiding a wall; if PhysicsECSSystem skipped hidden
// bodies, unticking one box in the outliner would silently change the simulation, and the authoring
// flag would have become a gameplay one. The same argument covers the clock, the scripts, the socket
// attachments and the locomotion state machine: none of them draws anything.
//
// THE SOURCE SCAN STRIPS COMMENTS FIRST. Twice in one week a census in this project went red on
// PROSE quoting the very thing it forbids, and the second time it nearly drowned a real finding in
// the same run. The verdicts below are explained in comments inside the systems themselves, so a scan
// that read comments would fire on every one of them.

#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/EntityVisibility.hpp>

#include <entt/entt.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using Desert::ECS::IsHidden;
using Desert::ECS::IsVisible;
using Desert::ECS::VisibilityComponent;

// ---------------------------------------------------------------------------------------------------
// 1. The predicate itself, on a registry built by hand. No Scene, no device.
// ---------------------------------------------------------------------------------------------------

TEST( EntityVisibility, AbsentComponentMeansVisible )
{
    entt::registry registry;
    const auto     entity = registry.create();

    EXPECT_FALSE( IsHidden( registry, entity ) )
         << "most entities never carry the component; treating absence as hidden blacks out every "
            "scene ever authored";
    EXPECT_TRUE( IsVisible( registry, entity ) );
}

TEST( EntityVisibility, TheBoolIsTheAnswer )
{
    entt::registry registry;
    const auto     entity = registry.create();

    registry.emplace<VisibilityComponent>( entity ).Visible = true;
    EXPECT_FALSE( IsHidden( registry, entity ) );

    registry.get<VisibilityComponent>( entity ).Visible = false;
    EXPECT_TRUE( IsHidden( registry, entity ) ) << "the one bit the outliner's eye writes";

    registry.get<VisibilityComponent>( entity ).Visible = true;
    EXPECT_FALSE( IsHidden( registry, entity ) ) << "and it comes back";
}

// A collector asking about an entity that was destroyed between the view and the query must get an
// answer rather than an assert — the sky collector holds candidate handles across an election.
TEST( EntityVisibility, NullAndDestroyedEntitiesAreNotHidden )
{
    entt::registry registry;
    EXPECT_FALSE( IsHidden( registry, entt::null ) );

    const auto entity = registry.create();
    registry.emplace<VisibilityComponent>( entity ).Visible = false;
    registry.destroy( entity );
    EXPECT_FALSE( IsHidden( registry, entity ) ) << "a destroyed entity is nothing, not a hidden thing";
}

// IsVisible is the same rule read the other way, not a second rule. Two spellings that can disagree is
// how a flag ends up honoured by two of the three things it claims to control.
TEST( EntityVisibility, IsVisibleIsExactlyTheNegation )
{
    entt::registry registry;
    for ( int i = 0; i < 3; ++i )
    {
        const auto entity = registry.create();
        if ( i > 0 )
            registry.emplace<VisibilityComponent>( entity ).Visible = ( i == 1 );
        EXPECT_EQ( IsVisible( registry, entity ), !IsHidden( registry, entity ) ) << i;
    }
}

// ---------------------------------------------------------------------------------------------------
// 2. The census
// ---------------------------------------------------------------------------------------------------

namespace
{
    enum class Verdict
    {
        // Builds some part of the picture out of an entity. Hiding the entity must remove that part.
        Honours,
        // Draws nothing. Honouring the flag here would turn an authoring toggle into a behaviour change.
        MustNot,
        // Neither of the above is obviously right and the answer costs the owner something.
        OwnerDecision,
    };

    struct Row
    {
        const char* File;    // relative to Desert/Desert/Source/Engine/ECS/System/
        Verdict     Result;  //
        const char* Because; // one sentence, and the test refuses an empty one
    };

    // ONE ROW PER SYSTEM. The directory is walked below and every file in it must appear here.
    constexpr std::array kSystems = {
         Row{ "AnimationECSSystem.hpp", Verdict::OwnerDecision,
              "an invisible skeleton still costs a full pose evaluation; UE answers this with a "
              "PER-COMPONENT enum (VisibilityBasedAnimTickOption), not a global rule, because "
              "skipping the pose changes what montages, notifies and attached sockets see. It is a "
              "performance question wearing a visibility costume and it needs a knob, not a verdict." },
         Row{ "AttachmentSystem.hpp", Verdict::MustNot,
              "writes a TransformComponent (a weapon following a bone); a hidden weapon that stops "
              "tracking snaps to the wrong place the moment it is shown again." },
         Row{ "AudioECSSystem.hpp", Verdict::MustNot,
              "sound is not a picture. Ambience emitters carry no mesh at all, and hiding one in the "
              "outliner to declutter the scene would silently mute the level." },
         Row{ "HeightFogECSSystem.hpp", Verdict::Honours,
              "emits the frame's fog; the renderer keeps fog state across frames, so a hidden volume "
              "that is still collected stays on screen forever." },
         Row{ "LocomotionSystem.hpp", Verdict::MustNot,
              "picks a clip NAME from a character's speed; hiding a character must not change which "
              "animation it is playing when it comes back." },
         Row{ "MeshECSSystem.hpp", Verdict::Honours,
              "the original three sites (static, instanced, skinned) and the whole reason the flag "
              "appeared to work at all." },
         Row{ "PhysicsECSSystem.hpp", Verdict::MustNot,
              "an invisible wall you still collide with is a DIFFERENT FEATURE from an invisible "
              "wall; skipping a hidden body would make one outliner tick silently change the "
              "simulation, and bodies are created from authored transforms on Play." },
         Row{ "PointLightSystem.hpp", Verdict::Honours,
              "a hidden lamp that keeps lighting the room is the owner's complaint in its smallest "
              "form." },
         Row{ "ScriptSystem.hpp", Verdict::MustNot,
              "runs Lua and owns the frame's input plumbing; a hidden entity whose script stops "
              "running is a gameplay change, and cursor capture would follow the eye icon." },
         Row{ "SkyboxECSSystem.hpp", Verdict::Honours,
              "the sky, the HDR cubemap AND the sun candidate list — the three halves of the report "
              "that opened this task." },
         Row{ "SpotLightSystem.hpp", Verdict::Honours, "same rule and same reason as the point light." },
         Row{ "TerrainECSSystem.hpp", Verdict::Honours,
              "one of the original three sites that made the flag look like it worked at all; a "
              "terrain tile is geometry and hiding it must remove it." },
         Row{ "TextECSSystem.hpp", Verdict::Honours,
              "one of the original three; world text is drawn geometry like any mesh, and the label "
              "of a hidden object has to go with it." },
         Row{ "TimeOfDayECSSystem.hpp", Verdict::MustNot,
              "it is a CLOCK: it advances TimeOfDay and writes the sun's transform. Freezing that on "
              "hide would make unhiding a sun a jump back in time, and the sky collector already "
              "drops the hidden light from its candidates so nothing lit is affected." },
         Row{ "VolumetricCloudECSSystem.hpp", Verdict::Honours,
              "the cloud layer and every sculpted hero body; same stale-state argument as the fog." },
    };

    // ── ONE ROW PER SITE, NOT PER FILE ──────────────────────────────────────────────────────────────
    //
    // A file-level check is satisfied by ONE surviving call. MeshECSSystem walks three different
    // component kinds in three separate lambdas and the sky collector asks three unrelated questions;
    // delete the check from the instanced-mesh walk and a file-level census stays green while every
    // ISM in the project ignores the eye icon. So each place that has to ask is its own named row,
    // anchored on a token from the loop it guards, and BOTH halves are checked: the anchor must still
    // be findable (a refactor that moves the loop is red, not silently green) and the call must appear
    // within reach of it.
    struct Site
    {
        const char* File;
        const char* Name;   // what this particular walk builds
        const char* Anchor; // a token unique to that walk, in code (comments are stripped first)
    };

    constexpr std::array kHonourSites = {
         Site{ "MeshECSSystem.hpp", "static meshes", "StaticMeshComponent& mesh," },
         Site{ "MeshECSSystem.hpp", "instanced static meshes (ISM)", "InstancedStaticMeshComponent& ism" },
         Site{ "MeshECSSystem.hpp", "skinned meshes", "SkinnedMeshComponent& mesh," },
         Site{ "TerrainECSSystem.hpp", "terrain tiles", "view<TerrainComponent, TransformComponent>" },
         Site{ "TextECSSystem.hpp", "world text", "view<TextComponent, TransformComponent>" },
         Site{ "HeightFogECSSystem.hpp", "the fog volume election",
               "view<ECS::ExponentialHeightFogComponent>" },
         Site{ "VolumetricCloudECSSystem.hpp", "the cloud layer election",
               "view<ECS::VolumetricCloudComponent>" },
         Site{ "VolumetricCloudECSSystem.hpp", "sculpted hero bodies",
               "view<ECS::HeroCloudComponent, ECS::TransformComponent>" },
         Site{ "SkyboxECSSystem.hpp", "the Sky Atmosphere election", "view<ECS::SkyAtmosphereComponent>" },
         Site{ "SkyboxECSSystem.hpp", "the HDR cubemap", "view<ECS::SkyboxComponent>" },
         Site{ "SkyboxECSSystem.hpp", "the atmosphere sun candidates",
               "view<ECS::DirectionLightComponent, ECS::TransformComponent>" },
         Site{ "PointLightSystem.hpp", "point lights", "view<PointLightComponent, TransformComponent>" },
         Site{ "SpotLightSystem.hpp", "spot lights", "view<SpotLightComponent, TransformComponent>" },
    };

    // How far past the anchor the check may sit. Generous enough for a comment-free loop header and a
    // couple of locals, tight enough that a check belonging to the NEXT loop cannot satisfy this one.
    constexpr size_t kReach = 700;

    // Not a file in System/, and the single most visible half of the report: the directional light is
    // collected inline in Scene.cpp and goes through no system at all.
    constexpr const char* kDirectionalLightSite = "Desert/Desert/Source/Engine/Core/Scene.cpp";

    // These are the base class and the pure rules, not systems.
    bool IsNotASystem( const std::string& filename )
    {
        return filename == "System.hpp" || filename == "SystemRules.hpp";
    }

    std::string RepositoryRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + "Desert/Desert/Source/Engine/ECS/EntityVisibility.hpp" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    std::string ReadFile( const std::string& path )
    {
        std::ifstream in( path );
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
    }

    // §8.3: a census that fires on prose gets switched off, and takes a real finding with it. Every
    // verdict below is ALSO written as a comment inside the system it describes, so the scan has to
    // read code only. Line comments and block comments go; string literals are left alone because no
    // rule here is about them.
    std::string StripComments( const std::string& source )
    {
        std::string out;
        out.reserve( source.size() );

        for ( size_t i = 0; i < source.size(); )
        {
            if ( source.compare( i, 2, "//" ) == 0 )
            {
                while ( i < source.size() && source[i] != '\n' )
                    ++i;
                continue;
            }
            if ( source.compare( i, 2, "/*" ) == 0 )
            {
                i += 2;
                while ( i + 1 < source.size() && source.compare( i, 2, "*/" ) != 0 )
                    ++i;
                i = std::min( i + 2, source.size() );
                continue;
            }
            out += source[i];
            ++i;
        }
        return out;
    }

    std::string SystemPath( const std::string& root, const char* file )
    {
        return root + "Desert/Desert/Source/Engine/ECS/System/" + file;
    }
} // namespace

// Every system file on disk must have a verdict, and every verdict must name a file that exists.
// DELETING A ROW IS RED here, which is the whole point: a count would have gone quietly from 15 to 14.
TEST( VisibilityCensus, EveryEcsSystemHasAVerdictAndEveryVerdictHasASystem )
{
    const std::string root = RepositoryRoot();
    ASSERT_FALSE( root.empty() ) << "repository root not found from the test's working directory";

    std::set<std::string> onDisk;
    for ( const auto& entry :
          std::filesystem::directory_iterator( root + "Desert/Desert/Source/Engine/ECS/System" ) )
    {
        if ( !entry.is_regular_file() || entry.path().extension() != ".hpp" )
            continue;
        const std::string filename = entry.path().filename().string();
        if ( IsNotASystem( filename ) )
            continue;
        onDisk.insert( filename );
    }
    ASSERT_FALSE( onDisk.empty() ) << "no ECS systems found — the census is scanning the wrong directory";

    std::set<std::string> classified;
    for ( const Row& row : kSystems )
        classified.insert( row.File );

    for ( const std::string& filename : onDisk )
        EXPECT_TRUE( classified.count( filename ) == 1 )
             << filename
             << " is an ECS system with NO visibility verdict. Decide whether it must honour "
                "VisibilityComponent, must not, or needs the owner, and add a row to kSystems saying "
                "which and why.";

    for ( const std::string& filename : classified )
        EXPECT_TRUE( onDisk.count( filename ) == 1 )
             << filename << " has a visibility verdict but no such system exists — the row is a ghost.";

    // DERIVED, never typed: the numbers exist so a shrinkage is visible in the log, and they are a
    // sum of the rows above rather than a constant somebody can edit to make this pass.
    size_t honours = 0, mustNot = 0, ownerDecision = 0;
    for ( const Row& row : kSystems )
    {
        switch ( row.Result )
        {
            case Verdict::Honours:
                ++honours;
                break;
            case Verdict::MustNot:
                ++mustNot;
                break;
            case Verdict::OwnerDecision:
                ++ownerDecision;
                break;
        }
    }
    EXPECT_EQ( honours + mustNot + ownerDecision, kSystems.size() );
    EXPECT_EQ( kSystems.size(), onDisk.size() );

    std::cout << "[ VISIBILITY ] " << onDisk.size() << " ECS systems: " << honours << " honour, " << mustNot
              << " must not, " << ownerDecision << " await an owner decision.\n";
}

// A row is a decision, so it has to carry the reason for it.
TEST( VisibilityCensus, EveryVerdictCarriesItsReason )
{
    for ( const Row& row : kSystems )
    {
        const std::string because = row.Because;
        EXPECT_GE( because.size(), 40u )
             << row.File
             << " has a verdict with no argument behind it. The next person to touch this system reads "
                "this column and nothing else.";
    }
}

// Removing the honour-check from ANY ONE of the thirteen walks reddens HERE, naming both the system
// and the walk. This is the mutation the brief asked for, one site at a time.
TEST( VisibilityCensus, EveryWalkThatMustHonourAsksTheOnePredicate )
{
    const std::string root = RepositoryRoot();
    ASSERT_FALSE( root.empty() );

    for ( const Site& site : kHonourSites )
    {
        const std::string code = StripComments( ReadFile( SystemPath( root, site.File ) ) );
        ASSERT_FALSE( code.empty() ) << site.File << " is empty or unreadable";

        const size_t anchor = code.find( site.Anchor );
        ASSERT_NE( anchor, std::string::npos )
             << site.File << ": the walk over " << site.Name
             << " can no longer be found (anchor '" << site.Anchor
             << "'). A census that cannot find its subject must go RED, not quiet — re-anchor this row "
                "on the code that replaced it.";

        EXPECT_NE( code.find( "ECS::IsHidden(", anchor ), std::string::npos )
             << site.File << ": " << site.Name
             << " no longer asks whether the entity is hidden, so unticking Visible on it does nothing "
                "again. Either restore the check or change the verdict in kSystems and say why.";
        EXPECT_LT( code.find( "ECS::IsHidden(", anchor ), anchor + kReach )
             << site.File << ": " << site.Name
             << " has no visibility check near its own loop — the next one found belongs to a "
                "different walk.";
    }
}

// Every Honours system must own at least one site row, or the check above silently stops covering it.
TEST( VisibilityCensus, EveryHonoursVerdictIsBackedByAtLeastOneSite )
{
    const std::string root = RepositoryRoot();
    ASSERT_FALSE( root.empty() );

    for ( const Row& row : kSystems )
    {
        if ( row.Result != Verdict::Honours )
            continue;

        const bool covered = std::any_of( kHonourSites.begin(), kHonourSites.end(), [&]( const Site& site )
                                          { return std::string( site.File ) == row.File; } );
        EXPECT_TRUE( covered ) << row.File
                               << " is recorded as honouring visibility but no site row anchors WHERE, "
                                  "so deleting its check would not be seen.";

        const std::string code = StripComments( ReadFile( SystemPath( root, row.File ) ) );
        EXPECT_NE( code.find( "Engine/ECS/EntityVisibility.hpp" ), std::string::npos )
             << row.File << " calls the predicate without including the header that declares it.";
    }

    for ( const Site& site : kHonourSites )
    {
        const bool declared = std::any_of( kSystems.begin(), kSystems.end(), [&]( const Row& row )
                                           {
                                               return std::string( row.File ) == site.File &&
                                                      row.Result == Verdict::Honours;
                                           } );
        EXPECT_TRUE( declared ) << site.File << " has a site row for " << site.Name
                                << " but is not recorded as a system that honours visibility.";
    }
}

// The other direction, and it is not symmetry for its own sake: "must not" is a DECISION with an
// argument behind it, and reversing it silently is exactly as bad as deleting the row.
TEST( VisibilityCensus, NoSystemQuietlyReversesAMustNotVerdict )
{
    const std::string root = RepositoryRoot();
    ASSERT_FALSE( root.empty() );

    for ( const Row& row : kSystems )
    {
        if ( row.Result == Verdict::Honours )
            continue;

        const std::string code = StripComments( ReadFile( SystemPath( root, row.File ) ) );
        ASSERT_FALSE( code.empty() ) << row.File << " is empty or unreadable";

        EXPECT_EQ( code.find( "IsHidden" ), std::string::npos )
             << row.File
             << " reads entity visibility, but the census says it must not (or that the owner has not "
                "decided yet). Read the reason in kSystems before changing this.";
        EXPECT_EQ( code.find( "VisibilityComponent" ), std::string::npos )
             << row.File << " reads VisibilityComponent directly against its own recorded verdict.";
    }
}

// ONE SPELLING. The three collectors that did honour the flag each wrote their own has<>/get<> pair,
// and the only reason all three agreed is that they were copy-pasted. A fourth copy is how a rule ends
// up meaning two things.
TEST( VisibilityCensus, NoSystemOpenCodesTheVisibilityCheck )
{
    const std::string root = RepositoryRoot();
    ASSERT_FALSE( root.empty() );

    for ( const auto& entry :
          std::filesystem::directory_iterator( root + "Desert/Desert/Source/Engine/ECS/System" ) )
    {
        if ( !entry.is_regular_file() || entry.path().extension() != ".hpp" )
            continue;

        const std::string filename = entry.path().filename().string();
        const std::string code     = StripComments( ReadFile( entry.path().string() ) );

        EXPECT_EQ( code.find( "VisibilityComponent" ), std::string::npos )
             << filename
             << " names VisibilityComponent in code. The rule is ECS::IsHidden( registry, entity ) in "
                "Engine/ECS/EntityVisibility.hpp — a second spelling of it here is a second rule.";
    }
}

// The sun is not a system. It is collected inline in Scene.cpp, which is why it was missed — a sweep
// of Engine/ECS/System/ finds nothing wrong and the owner's first example stays broken.
TEST( VisibilityCensus, TheDirectionalLightSiteHonoursItToo )
{
    const std::string root = RepositoryRoot();
    ASSERT_FALSE( root.empty() );

    const std::string code = StripComments( ReadFile( root + kDirectionalLightSite ) );
    ASSERT_FALSE( code.empty() ) << kDirectionalLightSite << " is empty or unreadable";

    const size_t collection = code.find( "DirectionLights.push_back" );
    ASSERT_NE( collection, std::string::npos )
         << "the directional-light collection moved out of Scene.cpp; this census has to follow it "
            "rather than go quietly green.";

    // Within the same lambda, not merely somewhere in a 3000-line file.
    const size_t lambda = code.rfind( "dirLightGroup.each", collection );
    ASSERT_NE( lambda, std::string::npos );
    const std::string block = code.substr( lambda, collection - lambda );

    EXPECT_NE( block.find( "ECS::IsHidden(" ), std::string::npos )
         << "the directional light is collected without asking whether its entity is hidden, so "
            "unticking Visible on the SUN leaves the scene lit and shadowed by it — the owner's first "
            "example, and the one no sweep of the ECS systems can see.";
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
