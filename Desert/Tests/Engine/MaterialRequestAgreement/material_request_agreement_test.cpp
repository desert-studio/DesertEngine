// Г18 — THE RELATION UNDER A FIND-OR-CREATE, AND THE CENSUS THAT MAKES IT CHEAP:
//
//     What the builder asks a demo material to be, and what the .demat on disk says it is, must agree.
//
// The defect is not a wrong line. `Editor::MaterialAssetUtils`' find-or-create looked a material up by
// NAME and, on a hit, returned that material's handle without ever comparing it to the parameters the
// caller had just passed. Both halves are individually correct — reuse is deliberate, so that a user's
// edits to a demo material survive a restart, and the caller's values are a true statement of what the
// material should be — and a unit test of either passes. What had no owner was the AGREEMENT.
//
// What it cost, measured: `Editor/Resources/Assets/Materials/CB_Red.demat` sat in the repository
// carrying MetallicFactor 1.0 and RoughnessFactor 0.0 — a chrome mirror — against a call site asking
// for roughness 0.9 and no metalness. A conductor has no diffuse lobe, so the Cornell box's left wall
// rendered essentially black: 0.010 mean sRGB luminance against the mirror-image right wall's 0.563,
// with every geometric input to the two walls equal to four decimals. It could not self-correct on any
// launch, because the function never looked. Finding it took a render, six knockouts and an
// instrumented G-buffer frame. Finding the NEXT one now takes this file.
//
// Two claims, and they are different:
//
//   * the reporting is right — Engine/Assets/MaterialParamDiff.hpp names every disagreeing parameter
//     with BOTH values, distinguishes "the file says something else" from "the file is silent", and
//     stays quiet when they agree;
//   * the corpus is clean — every material in Editor/Core/DemoMaterials.hpp agrees with the .demat that
//     was generated from it. This is the one that would have caught CB_Red, and it needs no editor, no
//     GPU and no human looking at a picture.
//
// The table is the SOURCE of those files, not a mirror of them (see DemoMaterials.hpp), so this is a
// generated-from relation and not the "two copies that must not drift" shape — which is why the failure
// message tells you to fix the FILE.

#include <Common/Json/Document.hpp>
#include <gtest/gtest.h>

#include <Editor/Core/DemoMaterials.hpp>
#include <Engine/Assets/MaterialParamDiff.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace Desert::Assets;
using namespace Desert::Editor::MaterialAssetUtils;

namespace
{
    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + "Desert/Desert/Source/Engine/Core/SceneSettings.hpp" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    std::string ReadAll( const std::string& path )
    {
        std::ifstream     file( path );
        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }

    // The .demat as data. Read generically rather than through MaterialData's own rfl schema so the
    // suite links only Common — the question is about two lists of named vec4s and needs no engine.
    MaterialData LoadMaterialFile( const std::string& path )
    {
        MaterialData data;
        const auto   parsed = Common::Json::Parse( ReadAll( path ) );
        EXPECT_TRUE( parsed.IsSuccess() ) << path;
        if ( !parsed.IsSuccess() )
            return data;
        const Common::Json::Node root = Common::Json::Root( parsed.GetValue() );
        EXPECT_EQ( root.GetKind(), Common::Json::Kind::Object ) << path;

        const auto params = root.Find( "Params" );
        if ( !params.has_value() )
            return data;

        params->ForEachElement(
             [&]( std::size_t, const Common::Json::Node& entry )
             {
                 const auto name  = entry.Find( "Name" );
                 const auto value = entry.Find( "Value" );
                 if ( !name.has_value() || !value.has_value() )
                     return;
                 const auto nameText = name->AsString();
                 if ( !nameText )
                     return;

                 std::vector<float> components;
                 value->ForEachElement(
                      [&]( std::size_t, const Common::Json::Node& component )
                      {
                          const auto d = component.AsNumber();
                          components.push_back( d ? static_cast<float>( d.GetValue() ) : 0.0f );
                      } );
                 if ( components.size() < 4 )
                     return;

                 data.SetParam( nameText.GetValue(),
                                glm::vec4( components[0], components[1], components[2], components[3] ) );
             } );
        return data;
    }

    MaterialData MaterialWith( std::initializer_list<std::pair<const char*, glm::vec4>> params )
    {
        MaterialData data;
        for ( const auto& [name, value] : params )
            data.SetParam( name, value );
        return data;
    }
} // namespace

// ── The reporting itself ────────────────────────────────────────────────────────────────────────

TEST( MaterialRequestAgreement, AMaterialThatSaysWhatWasAskedForProducesNoDivergence )
{
    const std::vector<MaterialParamRequest> want  = { { "AlbedoColor", { 0.85f, 0.1f, 0.1f, 1.0f } },
                                                      { "RoughnessFactor", { 0.9f, 0.0f, 0.0f, 0.0f } } };
    const MaterialData                      found = MaterialWith(
         { { "AlbedoColor", { 0.85f, 0.1f, 0.1f, 1.0f } }, { "RoughnessFactor", { 0.9f, 0.0f, 0.0f, 0.0f } } } );

    EXPECT_TRUE( DiffRequestedParams( want, found ).empty() );
}

// THE EXACT CASE THAT SHIPPED. This is CB_Red as it sat in the repository against CB_Red as its author
// described it, and the assertion is that the engine now says so, with both values, rather than
// returning a handle and nothing else.
TEST( MaterialRequestAgreement, TheChromeMirrorIsReportedWithBothValues )
{
    const std::vector<MaterialParamRequest> want = { { "AlbedoColor", { 0.85f, 0.1f, 0.1f, 1.0f } },
                                                     { "RoughnessFactor", { 0.9f, 0.0f, 0.0f, 0.0f } } };
    const MaterialData shipped                   = MaterialWith( { { "AlbedoColor", { 0.85f, 0.1f, 0.1f, 1.0f } },
                                                                   { "RoughnessFactor", { 0.0f, 0.0f, 0.0f, 0.0f } },
                                                                   { "MetallicFactor", { 1.0f, 0.0f, 0.0f, 0.0f } } } );

    const auto divergences = DiffRequestedParams( want, shipped );
    ASSERT_EQ( divergences.size(), 1u ) << "only RoughnessFactor was asked about AND disagrees";
    EXPECT_EQ( divergences[0].Name, "RoughnessFactor" );
    EXPECT_FALSE( divergences[0].Absent );
    EXPECT_FLOAT_EQ( divergences[0].Requested.x, 0.9f );
    EXPECT_FLOAT_EQ( divergences[0].Found.x, 0.0f );

    const std::string message = DescribeDivergences( divergences );
    EXPECT_NE( message.find( "RoughnessFactor" ), std::string::npos );
    EXPECT_NE( message.find( "0.900000" ), std::string::npos ) << message;
    EXPECT_NE( message.find( "0.000000" ), std::string::npos ) << message;
}

// A parameter the file is SILENT about is a different finding from one it contradicts: silence resolves
// to the shader's declared default, which is a live answer and not a missing one.
TEST( MaterialRequestAgreement, ASilentParameterIsReportedAsAbsentRatherThanAsZero )
{
    const std::vector<MaterialParamRequest> want  = { { "RoughnessFactor", { 0.9f, 0.0f, 0.0f, 0.0f } } };
    const MaterialData                      found = MaterialWith( { { "AlbedoColor", { 1, 1, 1, 1 } } } );

    const auto divergences = DiffRequestedParams( want, found );
    ASSERT_EQ( divergences.size(), 1u );
    EXPECT_TRUE( divergences[0].Absent );
    EXPECT_NE( DescribeDivergences( divergences ).find( "does not state it" ), std::string::npos );
}

// The comparison is ONE-DIRECTIONAL by design, and that is worth pinning: a material somebody extended
// in the editor carries params no builder ever mentioned, and those are not disagreements. Without this
// the warning would fire on every hand-edited demo material and be trained away within a day.
TEST( MaterialRequestAgreement, ParametersTheCallerNeverAskedAboutAreNotDivergences )
{
    const std::vector<MaterialParamRequest> want  = { { "AlbedoColor", { 1, 1, 1, 1 } } };
    const MaterialData                      found = MaterialWith(
         { { "AlbedoColor", { 1, 1, 1, 1 } }, { "EmissiveIntensity", { 3.0f, 0.0f, 0.0f, 0.0f } } } );

    EXPECT_TRUE( DiffRequestedParams( want, found ).empty() );
}

// ── The corpus ──────────────────────────────────────────────────────────────────────────────────

// THE ONE THAT WOULD HAVE CAUGHT IT. Every demo material's shipped .demat, against the table it was
// generated from. No editor, no GPU, no picture.
TEST( MaterialRequestAgreement, EveryShippedDemoMaterialStillSaysWhatItsAuthorAsked )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "repository root not found from the test's working directory";

    for ( const auto& demo : CornellDemoMaterials() )
    {
        const std::string path = root + "Editor/Resources/Assets/Materials/" + std::string( demo.Name ) + ".demat";
        ASSERT_TRUE( std::filesystem::exists( path ) )
             << path << " is in the demo material table but not in the repository";

        const MaterialData onDisk = LoadMaterialFile( path );
        ASSERT_FALSE( onDisk.Params.empty() ) << path << " parsed to no parameters at all";
        const auto divergences = DiffRequestedParams( demo.Params, onDisk );
        EXPECT_TRUE( divergences.empty() )
             << path << " has drifted from Editor/Core/DemoMaterials.hpp, which is what generated it: "
             << DescribeDivergences( divergences )
             << ". Fix the FILE — the table is the source, and the builder cannot rewrite an existing "
                "material by design.";
    }
}

// The lookup must not answer a typo with an empty parameter list, because an empty list is a legal
// request and the caller could not tell the two apart — the same shape as the defect above, one level
// down. Cheap to state, and it is the reason FindDemoMaterial returns a pointer.
TEST( MaterialRequestAgreement, AnUnknownDemoMaterialIsRefusedRatherThanAnsweredEmpty )
{
    EXPECT_EQ( FindDemoMaterial( "CB_NoSuchMaterial" ), nullptr );
    ASSERT_NE( FindDemoMaterial( "CB_Red" ), nullptr );
    EXPECT_FALSE( FindDemoMaterial( "CB_Red" )->empty() );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
