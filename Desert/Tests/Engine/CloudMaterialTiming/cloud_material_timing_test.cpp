// WHEN A CLOUD PARAMETER'S EDIT BECOMES VISIBLE — declared in the schema, MEASURED here.
//
// THE FACT THIS SUITE IS ABOUT. Twenty of the volumetric cloud material's thirty-five parameters are
// inputs to a CPU bake of a 256x32x256 volume over several thousand cloud bodies. Moving one of them
// re-runs that bake — 3.3 s to 14.1 s measured on the development machine (task O8) — and the sky goes on
// showing the PREVIOUS volume until the new one lands. The other fifteen answer in the frame that is drawn
// next: fourteen are read per sample by the march, and the fifteenth is the authored Medium, which is
// COMPILED INTO it rather than read by it and still lands in the next frame (O1-E). The owner reported the
// layer as "not updating" twice, and both times nothing was broken: what was missing was any way to tell
// the two kinds of knob apart.
//
// The Material Editor now says which is which, in the heading of every parameter group and in every row's
// tooltip, from ShaderParam::Timing — the `Timing(Immediate)` / `Timing(Rebake)` attribute of the shader's
// own Properties block. A window that makes that promise needs the promise checked, because a promise
// asserted in prose and not in code is the defect shape this tree has paid for nine times.
//
// SO NOTHING HERE IS A LIST OF NAMES. Each parameter is PERTURBED — through the same by-name setter the
// renderer's own material resolve uses, to an endpoint of the parameter's own declared Range — and the
// question "did the volume have to be built again" is put to Assets::CloudProceduralParamsEqual, which is
// the renderer's OWN rebake decision, unchanged and uncopied. The declared timing and the measured one are
// then required to agree, in both directions:
//
//   * Timing(Immediate) on a parameter that DOES force a rebake is a knob that looks free and stalls the
//     editor for seconds;
//   * Timing(Rebake) on one that does NOT is a knob that looks expensive and is instant, which teaches an
//     artist to avoid a control that costs nothing.
//
// Both are red here, named, with the parameter in the message.
//
// WHY THIS REPLACES A TEXT SCAN. Until O1's timing pass, the same claim was pinned by reading
// VolumetricCloudRenderer.cpp as TEXT and looking for `m_Material.<Name>` inside BuildProceduralParams —
// the weakest form of guard in this subsystem, and one that would have gone red for the wrong reason the
// moment that code moved into a header (it did). The bake's material half is now
// Graphic::ApplyCloudMaterialToBakeParams, a free function with no ResourceRegistry behind it, so the
// relation can be EXECUTED instead of grepped.
//
// Pure: reads one shader file, runs the engine's own parser and the engine's own bake-parameter
// application, touches no GPU, no registry and no filesystem beyond that one read.

#include <Engine/Assets/CloudProceduralVolume.hpp>
#include <Engine/Core/ShaderCompiler/DShader/DShaderParser.hpp>
#include <Engine/Graphic/Clouds/CloudMaterialBake.hpp>

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace Assets = Desert::Assets;

using Desert::Core::Formats::ShaderParam;
using Desert::Core::Formats::ShaderParamTiming;
using Desert::Core::Formats::ShaderProgramMeta;
using Desert::Core::Preprocess::DShaderParser;
using Desert::Graphic::ApplyCloudMaterialToBakeParams;
using Desert::Graphic::BuildCloudMaterialValues;
using Desert::Graphic::CloudBakeLayerInputs;
using Desert::Graphic::CloudMaterialValues;
using Desert::Graphic::CloudTypeShape;
using Desert::Graphic::MaterialOverrides;

namespace
{
    // Walks up from the working directory looking for a file only the repository has — the test runner's
    // working directory is not fixed. Same shape as CloudMaterialSchema's RepoRoot, same reason.
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

    // Parsed ONCE for the whole suite: the file on disk is the thing under test.
    const ShaderProgramMeta& Schema()
    {
        static const ShaderProgramMeta meta = []
        {
            const std::string  path = RepoRoot() + "Editor/Resources/Shaders/Programs/Clouds/CloudRaymarch.shader";
            std::ifstream      in( path, std::ios::binary );
            std::ostringstream buffer;
            buffer << in.rdbuf();
            const std::string source = buffer.str();
            EXPECT_FALSE( source.empty() ) << path << " is unreadable";

            auto parsed = DShaderParser::Parse( source );
            EXPECT_TRUE( parsed.IsSuccess() ) << parsed.GetError();
            return parsed.IsSuccess() ? parsed.GetValue().Meta : ShaderProgramMeta{};
        }();
        return meta;
    }

    // THE FIXTURE IS THE SHIPPED LAYER, not a minimal one, because the bake's own comparison SKIPS the five
    // layout placement numbers when no painting is bound — with good reason (an artist dragging Layout
    // Repeats on an unpainted layer would stall the editor for a slider that provably changes nothing).
    // Measuring those five against an unpainted base would therefore report them as instant, and they are
    // not. So the base state used below has a painting in BOTH slots, which is the state in which every one
    // of the thirty-five parameters means something.
    std::shared_ptr<Assets::CloudLayoutData> Painting( uint32_t hash )
    {
        auto data         = std::make_shared<Assets::CloudLayoutData>();
        data->ContentHash = hash;
        return data;
    }

    // A cumulus, written out rather than taken from Assets::CloudTypeDefaultShape() so this suite does not
    // have to compile the cloud-type asset layer for one struct. Nothing below depends on the numbers being
    // the shipped ones — they are a legal shape, and what is asserted is what happens when they MOVE.
    CloudTypeShape Cumulus()
    {
        CloudTypeShape shape{};
        shape.BaseAltitudeKm      = 1.8f;
        shape.TopAltitudeKm       = 3.4f;
        shape.EdgeTopFraction     = 0.35f;
        shape.BaseRampFraction    = 0.25f;
        shape.Profile             = Desert::Graphic::CloudProfileFromTaper( 0.45f );
        shape.AnvilAltitudeKm     = 0.0f;
        shape.AnvilThicknessKm    = 0.0f;
        shape.AnvilStrength       = 0.0f;
        shape.DetailCharacter     = 1.0f;
        shape.DetailFactor        = 1.0f;
        shape.DensityFactor       = 1.0f;
        shape.ExtinctionFactor    = 1.0f;
        shape.PlacementScale      = 1.0f;
        shape.PlacementAnisotropy = 1.0f;
        return shape;
    }

    // Two species, so that a perturbation of the SECOND cloud type slot has somewhere to land. One species
    // would make CloudType2..4 unmeasurable and the suite would pass by having nothing to look at.
    struct Layer
    {
        CloudTypeShape       Shapes[Desert::Graphic::kCloudSpeciesSlots];
        uint32_t             SpeciesCount = 2;
        CloudBakeLayerInputs Inputs;
    };

    Layer ShippedLayer()
    {
        Layer layer;
        for ( uint32_t slot = 0; slot < Desert::Graphic::kCloudSpeciesSlots; ++slot )
            layer.Shapes[slot] = Cumulus();
        // The two live species must not be byte-identical, or a perturbation of one is indistinguishable
        // from a perturbation of the other and the "which slot" half of the claim goes untested.
        layer.Shapes[1].PlacementScale *= 1.7f;
        return layer;
    }

    Assets::CloudProceduralFieldParams Bake( const CloudMaterialValues& look, const Layer& layer,
                                             uint32_t patternHash, uint32_t maskHash )
    {
        Assets::CloudProceduralFieldParams params;
        // The two SOURCES are the caller's in the renderer too — resolving a handle needs a service — so the
        // fixture fills them exactly where VolumetricCloudRenderer::BuildProceduralParams fills them.
        params.PatternSource = patternHash != 0u ? Painting( patternHash ) : nullptr;
        params.MaskSource    = maskHash != 0u ? Painting( maskHash ) : nullptr;
        ApplyCloudMaterialToBakeParams( look, layer.Inputs, layer.Shapes, layer.SpeciesCount, params );
        return params;
    }

    // The schema's defaults, exactly as the renderer resolves them for a material that overrides nothing.
    CloudMaterialValues Defaults()
    {
        return BuildCloudMaterialValues( &Schema(), MaterialOverrides{} );
    }

    // A VALUE FOR THIS PARAMETER THAT IS DEFINITELY NOT ITS DEFAULT, derived from the parameter's own
    // declared Range and never typed out per name. The endpoint FURTHEST from the default is taken, because
    // several of these numbers are floored against the cloud lattice on the way into the bake (the weather
    // patch tile is floored at three cells) and the nearer endpoint can legitimately land back on the
    // default after that floor — which would read as "this knob changes nothing" and be a false green.
    //
    // A parameter with no Range gets default + 1, which is outside every clamp in this schema.
    glm::vec4 Perturbed( const ShaderParam& p )
    {
        glm::vec4 value = p.Default;
        if ( !p.Min.has_value() || !p.Max.has_value() )
        {
            for ( int c = 0; c < 4; ++c )
                value[c] = p.Default[c] + 1.0f;
            return value;
        }

        // `farthest` AND NOT `far`: windef.h #defines `far` away, so MSVC would read the type with no
        // variable after it and report a syntax error nowhere near the cause. Named by
        // Desert/Tests/Common/ReservedIdentifiers, which caught this one before it reached a Windows job.
        const float lo       = *p.Min;
        const float hi       = *p.Max;
        const float here     = p.Default.x;
        const float farthest = ( here - lo ) >= ( hi - here ) ? lo : hi;
        for ( int c = 0; c < 4; ++c )
            value[c] = farthest;
        return value;
    }

    // Does moving THIS parameter make the renderer re-bake? Answered by the renderer's own decision
    // function over the renderer's own bake-parameter application, with one parameter changed.
    //
    // The three families exist because the three kinds of parameter reach the bake by three different
    // routes, and pretending otherwise would leave six of the thirty-five untested:
    //
    //   VALUES        through the same by-name setter BuildCloudMaterialValues uses for a `.demat` override;
    //   CloudType1..4 through the resolved SHAPES, because the renderer resolves the four handles in
    //                 ResolveSpecies and hands the bake shapes rather than handles;
    //   Layout slots  through PatternSource / MaskSource, because a handle only becomes a painting by way of
    //                 the layout service — binding one where there was none is the perturbation.
    bool ForcesRebake( const ShaderParam& p )
    {
        const Layer               layer = ShippedLayer();
        const CloudMaterialValues base  = Defaults();

        // Painted on BOTH slots in the base state — see Painting()'s note.
        const Assets::CloudProceduralFieldParams before = Bake( base, layer, 0xA1u, 0xB2u );

        if ( p.Name.rfind( "CloudType", 0 ) == 0 )
        {
            Layer moved = layer;
            // The FIRST two slots are the two live species; a perturbation of slot 3 or 4 has to be
            // expressed as the layer GROWING a species, which is exactly what an artist filling an empty
            // slot does — and it is the same rebake either way.
            const uint32_t slot = static_cast<uint32_t>( p.Name.back() - '1' );
            if ( slot < moved.SpeciesCount )
                moved.Shapes[slot].TopAltitudeKm += 0.75f;
            else
                ++moved.SpeciesCount;
            return !Assets::CloudProceduralParamsEqual( before, Bake( base, moved, 0xA1u, 0xB2u ) );
        }

        // A PAINTING WHERE THERE WAS A DIFFERENT ONE. Compared by content hash by the bake's own decision,
        // so two distinct hashes is the whole perturbation; swapping only one slot at a time is what makes
        // the two slots separately answerable.
        if ( p.Name == "LayoutPattern" )
            return !Assets::CloudProceduralParamsEqual( before, Bake( base, layer, 0xC3u, 0xB2u ) );
        if ( p.Name == "LayoutMask" )
            return !Assets::CloudProceduralParamsEqual( before, Bake( base, layer, 0xA1u, 0xD4u ) );

        // THE AUTHORED MEDIUM, and its perturbation is a handle rather than a number. It is measured the
        // same way as everything else — the renderer's own bake-parameter application, then the
        // renderer's own comparison — and the answer it gives is the interesting one: a medium is a body
        // of GPU code compiled into the march, so no amount of authoring it can move a single input of a
        // CPU bake that has already run. That is what makes Timing(Immediate) on this slot a measured
        // fact rather than a hopeful label.
        if ( p.Name == "Medium" )
        {
            CloudMaterialValues moved = base;
            moved.Medium              = Assets::AssetHandle( 0xE5E5E5E5ull );
            EXPECT_NE( 0, std::memcmp( &moved, &base, sizeof( CloudMaterialValues ) ) )
                 << "the Medium perturbation changed nothing, so its answer means nothing";
            return !Assets::CloudProceduralParamsEqual( before, Bake( moved, layer, 0xA1u, 0xB2u ) );
        }

        CloudMaterialValues moved = base;
        Desert::Graphic::Detail::ApplyCloudOverride( moved, p.Name, Perturbed( p ) );
        EXPECT_NE( 0, std::memcmp( &moved, &base, sizeof( CloudMaterialValues ) ) )
             << p.Name
             << ": the perturbation did not change the material at all, so whatever this test "
                "reports about it means nothing. Either the by-name setter does not know the name "
                "(the CloudMaterialSchema census would be red too) or the Range's far endpoint IS "
                "the default.";
        return !Assets::CloudProceduralParamsEqual( before, Bake( moved, layer, 0xA1u, 0xB2u ) );
    }
} // namespace

// ── 1. NOTHING MAY GO UNCLASSIFIED ─────────────────────────────────────────────────────────────────────
//
// A parameter with no Timing draws no sentence, which is honest — and for THIS material it is also a hole
// in the one thing the window is being asked to tell an artist. The Volume domain is the domain whose
// material is half bake, so the requirement is stated for the domain rather than for every shader in the
// tree: a Surface material has no bake to be surprised by.
TEST( CloudMaterialTiming, EveryParameterOfTheVolumeMaterialSaysWhenItsEditIsVisible )
{
    ASSERT_FALSE( Schema().Params.empty() ) << "the schema parsed empty; every test below would be vacuous";
    EXPECT_EQ( Schema().Domain, Desert::Core::Formats::ShaderDomain::Volume );

    for ( const ShaderParam& p : Schema().Params )
    {
        EXPECT_NE( p.Timing, ShaderParamTiming::Unspecified )
             << p.Name
             << " declares no Timing, so the Material Editor can say nothing about whether moving "
                "it costs a frame or fourteen seconds. Add Timing(Immediate) or Timing(Rebake) to "
                "its Properties line; this suite then checks the one you chose against the bake.";
    }
}

// ── 2. THE RELATION ────────────────────────────────────────────────────────────────────────────────────
//
// The declared timing IS the measured one, parameter by parameter, in both directions. This is the whole
// suite; everything above and below it is scaffolding or census.
TEST( CloudMaterialTiming, TheDeclaredTimingIsTheOneTheBakeActuallyHas )
{
    uint32_t rebake    = 0;
    uint32_t immediate = 0;

    for ( const ShaderParam& p : Schema().Params )
    {
        const bool measured = ForcesRebake( p );
        const bool declared = p.Timing == ShaderParamTiming::Rebake;

        EXPECT_EQ( measured, declared )
             << p.Name << " declares Timing(" << Desert::Core::Formats::ShaderParamTimingName( p.Timing )
             << ") and the bake says it " << ( measured ? "DOES" : "does NOT" )
             << " have to be built again when this value moves. The Material Editor prints the declared "
                "answer beside the control, so the two disagreeing means the window is telling an artist "
                "the wrong thing about what this knob costs.";

        measured ? ++rebake : ++immediate;
    }

    // PRINTED AND NOT PINNED, and the two literals that used to stand here (20 and 15) are the reason.
    //
    // The hazard they guarded is real: the loop above is vacuously green over an empty parameter list,
    // which is exactly how a census stops counting anything without going red. The guard for that is the
    // assertion below — the schema parsed, and it parsed to something.
    //
    // What the literals ADDED was a gate satisfied by editing a number. Adding a parameter reddened this
    // line, and the cheapest way to make it green again was to type 21 — no name, no reason, nothing said
    // about the parameter itself. That is the shape this project has paid for repeatedly, and the split is
    // pinned properly in two places that name every member: CloudMaterialSchema asserts the schema and the
    // C++ mirror hold the same properties BY NAME, and CloudControlCensus demands a measured frame movement
    // for each of them. Neither can be satisfied by arithmetic.
    ASSERT_FALSE( Schema().Params.empty() )
         << "the cloud material schema parsed to nothing, so the loop above asserted nothing at all";
    std::printf( "[CloudMaterialTiming] %u of %u parameters rebuild the cloud volume; %u reach the march in "
                 "the same frame\n",
                 rebake, rebake + immediate, immediate );
}

// ── 3. THE HEADING MAY SPEAK FOR ITS GROUP ─────────────────────────────────────────────────────────────
//
// The Material Editor prints ONE phrase per parameter group, folded from the members. That is only honest
// while a group's members agree, and today they do: the schema's first four categories are the bake's and
// the last two the march's. Nobody wrote that property down, which is precisely why it is asserted — the
// day a knob is added to "Weather" that the bake does not read, the heading becomes a lie and the panel
// must fall back to per-row marks. This goes red then, naming the category.
TEST( CloudMaterialTiming, EveryCategoryIsWhollyBakeOrWhollyMarch )
{
    std::set<std::string> mixed;
    std::set<std::string> categories;

    for ( const ShaderParam& p : Schema().Params )
    {
        categories.insert( p.Category );
        for ( const ShaderParam& other : Schema().Params )
            if ( other.Category == p.Category && other.Timing != p.Timing )
                mixed.insert( p.Category );
    }

    for ( const std::string& category : mixed )
    {
        ADD_FAILURE() << "the '" << category
                      << "' category mixes Immediate and Rebake parameters, so the Material Editor's "
                         "heading for it cannot state one cost for the group. That is a legal state and the "
                         "panel draws \"mixed - see each row\" for it, but it has never happened before and "
                         "is worth a decision rather than a silent change of what the window says.";
    }

    // NAMED rather than counted, because the claim is about WHICH categories: a count would pass on the
    // wrong six the day one is renamed.
    EXPECT_EQ( categories, ( std::set<std::string>{ "Cloud Types", "Weather", "Placement", "Layout", "Detail",
                                                    "Lighting", "Medium" } ) );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
