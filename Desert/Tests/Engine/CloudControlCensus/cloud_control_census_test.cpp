// EVERY AUTHORED CLOUD CONTROL MOVES THE FRAME, OR IT DOES NOT EXIST — DEV_CONTRACT §1.3's FIFTH LINK,
// as a register with one row per control instead of a document with a total in it.
//
// WHY THIS SUITE EXISTS, AND WHY IT IS NOT A SECOND COPY OF THE FOUR CENSUSES BESIDE IT.
//
// Four suites already pin the first four links of "component -> serialization -> editor UI -> GPU":
//
//   SettingConsumers        every reflected field names the file that READS it
//   CloudMaterialSchema     every shipped material property has a C++ mirror that consumes it
//   CloudMediumConsumers    every slot of the packed GPU block is fetched, or has a row saying why not
//   ShaderGraphVolumeDomain every Immediate material property is readable by a medium graph, or excused
//
// None of them can see the fifth link. A value can travel from the panel to the GPU perfectly and change
// nothing on screen, and that is precisely the failure the contract calls "a TODO wearing a feature's
// clothes". Task Р11 measured that link for the whole subsystem on 2026-08-31 — and wrote the result
// into Docs/Clouds/CONTROL_CENSUS.md, which is a REPORT and not a gate:
//
//   * Docs/ was taken out of version control on 2026-09-04, so the census is not in a fresh clone at all;
//   * no test, header or script anywhere in the tree names it;
//   * its headline is a TABLE OF COUNTS — 97 values, 91 live, 6 unmeasured — and a gate that pins a
//     NUMBER is satisfied by editing the number. It is the exact shape the project already paid for
//     twice, and the reason `kCloudUnreadSlots` next door is a row per slot with the count derived.
//
// What that cost is measurable rather than hypothetical. Between Р11 and today the population MOVED —
// O1 took thirty-three look values off the component and into a material's Properties block, and O1-E
// added a slot that hands the medium itself to a shader graph. FIVE controls appeared that the census
// has never seen (Material, PerSampleAtmosphereTransmittance, VolumeResolution, Medium, and the single
// CloudLayout slot splitting into LayoutPattern and LayoutMask), the population it counts as "52
// component properties" no longer exists, and NOTHING WENT RED, because nothing was pinned.
//
// SO THIS IS THE REGISTER. One row per authored control, carrying what its whole authored range does to
// the frame, the condition it needs to do it, and WHO measured that and on which tree. The membership is
// DERIVED — from the two reflected types and from the shipped shader's own Properties block — so a
// control added tomorrow is red here, by name, before it can reach a panel unmeasured. The counts are
// printed and never asserted, because a count is the thing a person adjusts instead of measuring.
//
// THE THRESHOLD IS Р11'S OWN, AND IT IS OPERATIVE HERE RATHER THAN PROSE: a control is LIVE when its
// whole authored range moves at least one ray by max >= 4/255. Four is where it sits because 1/255 is
// what a fresh worktree's first render produces with nothing changed, and 2-3/255 is what one to five
// HUNDRED clicks of the gentlest live control produce. A row is required to state a figure that clears
// it, so "this knob is dead" cannot be recorded here — a dead control is removed, not registered.
//
// Pure: reads the reflection table and one shader file as text. No GPU, no registry, no scene.

#include <Engine/Core/ShaderCompiler/DShader/DShaderParser.hpp>
#include <Engine/Core/ShaderCompiler/ShaderGraphMedium.hpp>
#include <Engine/Reflection/ReflectionRegistry.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <array>
#include <string_view>
#include <vector>

using Desert::Core::Formats::ShaderParam;
using Desert::Core::Formats::ShaderProgramMeta;
using Desert::Core::Preprocess::DShaderParser;
using Desert::Reflection::ReflectionRegistry;
using Desert::Reflection::TypeInfo;

namespace
{
    /// One authored control and what it does to the picture.
    ///
    /// `Movement` is the largest single-channel difference between the two ENDS of the control's own
    /// authored range, over the whole frame, at the ray where it is largest — the instrument is
    /// Tools/ImageDiff over a `--shot` capture, which is what every figure in this subsystem's history
    /// is measured with. `Condition` is what had to be true for the sweep to mean anything and is empty
    /// when nothing had to be; a layout strength measured against a layout that carries no mask reads
    /// 0/255 and the reading is correct, which is why the condition is part of the row and not a
    /// footnote. `Measured` names the task, the date and the tree, because a figure whose provenance is
    /// missing cannot be told from a figure nobody ever took.
    struct Control
    {
        const char* Name;
        const char* Movement;
        const char* Condition;
        const char* Measured;
    };

    // Provenance strings, spelled once. O1-D re-measured on the merged tree; every other figure is
    // Р11's own, on the tree it names, and is left as measured rather than adjusted.
    constexpr const char* kO1D = "O1-D 2026-09-09, dev@2b504842, Clouds_Protocol 715x784, floor 0 bytes";
    constexpr const char* kP11 = "Р11 2026-08-31, dev@a2631ce0, Clouds_Protocol 1280x766, floor 0 bytes";

    // О11 measured on SIL_Lenticular rather than Clouds_Protocol, and the departure is the point: the
    // control is about ONE species' band against the cell it is placed on, and the protocol scene's
    // congestus already stands at 2.20 km with a 3.0 km cell. The floor is three repeats of the same
    // command in the same tree, and the first render of the worktree was discarded.
    constexpr const char* kO11 =
         "О11 2026-09-10, dev@7e46bda9, SIL_Lenticular 715x784, floor 0 bytes of 560 560 over 3 runs";

    // ── THE COMPONENT: what a cloud LAYER is in the world ────────────────────────────────────────────
    //
    // Tracing budgets, pass routing and world integration. Everything about how the clouds LOOK left for
    // the material in O1, which is why this table is twenty-two rows and Р11's was fifty-two.
    constexpr Control kComponentControls[] = {
         { "Enabled", "max 154/255 over 63.0% at AZ270 EL85", "", kP11 },

         // O1's one addition, and the census had never seen it: the slot that carries the whole look.
         // Its range is null-versus-authored, and null is not a degenerate end — it is the shipped
         // defaults, byte-for-byte the component defaults the moved fields used to carry.
         { "Material", "max 149/255 over 47.8% at AZ270 EL85 (authored <-> empty)", "", kO1D },

         { "PlanetRadius", "max 122/255 over 83.3% at AZ000 EL25", "", kP11 },

         // О11's one addition, and it is the first control in this table whose subject is WHERE the deck
         // is rather than how it is traced or how it looks. Measured on the scene it was asked for —
         // SIL_Lenticular, a single-species sky whose 2.4 km cells sit at a 2.60 km base and therefore
         // subtend 49.6 degrees inside a 45-degree frame, which is the owner's "I am inside a mountain"
         // reported on 2026-09-10. At the far end of the range the same cells sit at 14.60 km and subtend
         // 9.4 degrees.
         { "LayerAltitudeOffset", "max 141/255 over 99.9% at AZ135 EL45 (0 -> 12 km)", "", kO11 },
         { "MaxViewDistance", "max 146/255 over 46.2% at AZ270 EL85", "", kP11 },
         { "TracingStartMaxDistance", "max 89/255 over 14.5% at AZ000 EL25",
           "grazing rays: flat from EL 0.5 to EL 25 and gone above it, which is what a guard on ray ENTRY "
           "distance should do",
           kP11 },
         { "TracingStartDistance", "max 154/255 over 63.0% at AZ270 EL85", "", kP11 },
         { "RegionSize", "max 131/255 over 72.6% at AZ270 EL85", "", kP11 },

         // Two fields, ONE setting: the pair returns fade-OFF unless End is strictly past Start, so
         // either swept alone at the shipped camera reads 0/255 and the reading is correct.
         { "NearFadeStartDistance", "max 145/255 over 91.9% at AZ135 EL45",
           "the other half of the pair set to turn the fade on, and the camera inside the deck", kP11 },
         { "NearFadeEndDistance", "max 145/255 over 91.9% at AZ135 EL45", "same pair, Start = 0", kP11 },

         // RE-MEASURED BY O1-D IN THE SHIPPED CONFIGURATION. Р11 swept this with the volume's own
         // strength at 0.5 and read 11/255; Р12 moved the shipped strength to 1.0 hours later and Р11
         // flagged its own row as measuring a sky the repository no longer has. This figure is the
         // flag's answer: off-versus-on at strength 1.0, which is what ships.
         { "SkyOcclusionVolume", "max 18/255 over 85.6% at AZ000 EL25",
           "AmbientOcclusionStrength at its shipped 1.0 — at 0 the layer applies neither occluder and the flag "
           "chooses between two things nobody reads",
           kO1D },

         // NEW SINCE THE CENSUS, and its own tooltip predicts the shape of the answer: the effect is
         // largest at a low sun and vanishes at noon. Both ends were taken, because a control measured
         // only where it is weakest is how a live one gets called dead.
         { "PerSampleAtmosphereTransmittance", "max 24/255 over 87.5% at AZ000 EL25 (5/255 at noon)",
           "the Physical Atmosphere sky model, and a low sun — measured with the sun at EL 8; Clouds_Protocol's "
           "own noon sun is the weakest condition it has",
           kO1D },

         { "LightMarchDistance", "max 146/255 over 66.3% at AZ270 EL85", "", kP11 },
         { "LightMarchSamples", "max 178/255 over 81.9% at AZ270 EL85", "", kP11 },
         { "AerialPerspectiveStartDistance", "max 8/255 over 64.8% at AZ000 EL25",
           "a non-zero Fade Distance — the shader only reads Start when Fade is non-zero, and Fade's own default "
           "is 0",
           kP11 },
         { "AerialPerspectiveFadeDistance", "max 29/255 over 84.6% at AZ000 EL25", "", kP11 },

         // The two the sky rays cannot see at all: they shade the world UNDER the layer, and
         // Clouds_Protocol's only ground is a 40-metre pad against cloud shadow cells 3 km across.
         { "CastShadows", "max 179/255 over 100% looking down",
           "Clouds_ShadowsOnGround, whose pad is ten times larger — on Clouds_Protocol both shadow rows read "
           "0/255 and a sky-only sweep would call two working features dead",
           kP11 },
         { "ShadowStrength", "max 179/255 over 100% looking down",
           "same scene; strength 0 skips the pass entirely, exactly as if Cast Shadows were off", kP11 },

         { "MaxSteps", "max 154/255 over 63.0% at AZ270 EL85",
           "its RANGE, 8 to 512 — the top octave alone (256 to 512) is worth 2-8/255, which is what a cost "
           "ceiling should look like",
           kP11 },
         { "StopTransmittance", "max 50/255 over 99.2% at AZ135 EL45", "", kP11 },

         // NEW SINCE THE CENSUS. Its tooltip argues the cost is the BAKE's and not the frame's, which is
         // true of the cost and says nothing about the picture: halving the side moves the zenith by 109
         // levels. It is a quality knob that is also visible, and both halves belong in the row.
         { "VolumeResolution", "max 109/255 over 55.9% at AZ270 EL85 (256 -> 128)", "", kO1D },

         { "WindDirection", "max 154/255 over 87.1% at AZ270 EL85", "", kP11 },
         { "WindSpeed", "max 161/255 over 88.6% at AZ270 EL85", "", kP11 },
    };

    // ── THE HERO CLOUD: the only route a sculpted `.dcmv` has into a scene ───────────────────────────
    //
    // Measured on Clouds_HeroCloud with the rays aimed at the body rather than taken from the dome —
    // one hero cloud occupies about a tenth of the frame, which is why every extent here is small and
    // why the dome's own rays would have found nothing.
    constexpr Control kHeroControls[] = {
         { "Enabled", "max 126/255 over 10.9% at the body", "Clouds_HeroCloud, rays aimed at the body", kP11 },
         { "Volume", "max 124/255 over 35.6% at the body", "same, congestus <-> cumulonimbus recipe", kP11 },
         { "Strength", "max 126/255 over 10.9% at the body",
           "same — identical to Enabled because strength 0 removes the body, which is what off means", kP11 },
         { "SuppressProceduralField", "max 86/255 over 2.4% at the body", "same", kP11 },
         { "DetailFactor", "max 121/255 over 11.7% at the body", "same", kP11 },
         { "DensityFactor", "max 132/255 over 10.0% at the body", "same", kP11 },
         { "ExtinctionFactor", "max 132/255 over 10.0% at the body", "same", kP11 },
    };

    // ── THE MATERIAL: what a cloud LOOKS like ────────────────────────────────────────────────────────
    //
    // The Properties block of CloudRaymarch.shader. Thirty-three of these were component fields when Р11
    // measured them; the move copied their ranges and defaults verbatim, so those figures still describe
    // the same travel of the same value under a new home, and the row says which tree took them.
    constexpr Control kMaterialControls[] = {
         { "CloudType1", "max 153/255 over 88.3% at AZ270 EL85", "congestus <-> cirrus", kP11 },
         { "CloudType2", "max 114/255 over 90.6% at AZ270 EL85", "empty <-> cirrus", kP11 },
         { "CloudType3", "max 114/255 over 90.6% at AZ270 EL85", "empty <-> cirrus", kP11 },
         { "CloudType4", "max 114/255 over 90.6% at AZ270 EL85", "empty <-> cirrus", kP11 },

         { "Coverage", "max 155/255 over 77.4% at AZ270 EL85", "", kP11 },
         { "CoverageContrast", "max 154/255 over 74.6% at AZ270 EL85", "", kP11 },
         { "WeatherTileSize", "max 171/255 over 92.6% at AZ270 EL85", "", kP11 },
         { "Seed", "max 147/255 over 81.9% at AZ270 EL85", "", kP11 },

         { "PlacementDensity", "max 146/255 over 99.7% at AZ135 EL45", "", kP11 },
         { "PlacementScatter", "max 134/255 over 99.0% at AZ000 EL25", "", kP11 },
         { "PlacementSizeVariety", "max 153/255 over 83.9% at AZ270 EL85", "", kP11 },
         { "PatchTileSize", "max 154/255 over 50.3% at AZ270 EL85",
           "the low end is silently raised to three lattice cells, which is a stated rule and not a silent clip",
           kP11 },
         { "PatchStrength", "max 154/255 over 60.3% at AZ270 EL85", "", kP11 },

         // THE ONE CENSUS ROW THAT SPLIT IN TWO. Р11 measured a single CloudLayout slot; the material
         // carries the pattern and the mask separately, which is what lets one painting place the clouds
         // and a different one clear a region. Each is measured against the layout that can carry it —
         // Layout_Stripe has a pattern and no mask, Layout_LetterD has both.
         { "LayoutPattern", "max 149/255 over 62.4% at AZ270 EL85 (empty -> Layout_Stripe)", "", kO1D },
         { "LayoutMask", "max 149/255 over 47.8% at AZ270 EL85 (empty -> Layout_LetterD)", "", kO1D },

         { "LayoutPatternStrength", "max 154/255 over 71.0% at AZ270 EL85",
           "a layout with a PATTERN bound — against Layout_LetterD, whose mask empties the sky at full strength, "
           "both ends render the same cloudless frame",
           kP11 },
         { "LayoutMaskStrength", "max 152/255 over 37.6% at AZ270 EL85",
           "a layout with a MASK in it — Layout_Stripe carries none and the component's own tooltip says so",
           kP11 },
         { "LayoutRepeats", "max 152/255 over 43.3% at AZ270 EL85", "a pattern bound", kP11 },
         { "LayoutRotation", "max 151/255 over 32.8% at AZ270 EL85", "a pattern bound", kP11 },
         { "LayoutOffset", "max 154/255 over 78.0% at AZ270 EL85", "a pattern bound", kP11 },

         // RE-MEASURED BY O1-D, because two prior measurements disagreed in the record and the
         // disagreement was about the STATISTIC rather than the frame: Р11 quoted 81/255 as a maximum,
         // the integrator's correction in CloudField.glslh quoted 0.97/255 as a whole-frame MEAN, and
         // both reproduce here. What the control changes is a high-frequency texture confined to the
         // silhouettes, so a mean over half a million pixels drowns exactly the thing being judged.
         { "DetailTileSize", "max 73/255 over 46.0% at AZ270 EL85, whole-frame mean 0.81/255 at AZ000 EL25", "",
           kO1D },

         { "DetailStrength", "max 151/255 over 84.8% at AZ270 EL85", "", kP11 },
         { "DensityScale", "max 158/255 over 63.6% at AZ270 EL85", "", kP11 },
         { "ExtinctionScale", "max 147/255 over 66.0% at AZ270 EL85", "", kP11 },

         { "ScatteringAlbedo", "max 237/255 over 100% at AZ135 EL45",
           "the largest single movement in the subsystem", kP11 },
         { "PhaseG", "max 98/255 over 95.2% at AZ135 EL45", "", kP11 },
         { "PhaseGBackward", "max 71/255 over 97.2% at AZ135 EL45", "", kP11 },
         { "PhaseBlend", "max 85/255 over 95.4% at AZ135 EL45", "", kP11 },

         // RE-MEASURED BY O1-D IN THE SHIPPED CONFIGURATION, and this is the second of the two rows Р11
         // flagged against itself. Its 53/255 was taken with SkyOcclusionVolume OFF, so the strength was
         // driving the PROFILE term — the sample's own depth in its own body — and since Р12 it drives
         // the VOLUME, which is a different quantity with a different geometry. The two are not a larger
         // and a smaller version of one another.
         { "AmbientOcclusionStrength", "max 32/255 over 100% at AZ135 EL45",
           "the sky-occlusion volume ON, as it has shipped since Р12", kO1D },

         { "MultiScatterOctaves", "max 81/255 over 100% at AZ135 EL45", "", kP11 },
         { "MultiScatterContribution", "max 124/255 over 100% at AZ135 EL45", "", kP11 },
         { "MultiScatterOcclusion", "max 125/255 over 58.7% at AZ000 EL25", "", kP11 },
         { "MultiScatterEccentricity", "max 119/255 over 100% at AZ135 EL45", "", kP11 },

         // O1-E's addition, and the one control in this table that is a body of CODE rather than a
         // number. Both ends were taken and the NEGATIVE one is half the result: a Volume graph with
         // nothing wired into its output compiles to the five shipped defaults and renders BYTE-FOR-BYTE
         // the frame that no graph renders, on all three rays. That is what makes the positive figure a
         // measurement of the graph rather than of the recompile.
         { "Medium",
           "max 72/255 over 100% at AZ135 EL45 (empty -> Default Density x 0.5); an identity graph is 0/255 on "
           "all three rays",
           "", kO1D },

         { "AmbientScale", "max 102/255 over 85.5% at AZ000 EL25", "", kP11 },
    };

    // Р11's own derivation, and the line this suite enforces. A control is LIVE when its whole authored
    // range moves at least one ray by this much; 1/255 is what a fresh worktree's first render produces
    // with nothing changed at all, and 2-3/255 is what one to five hundred clicks of the gentlest live
    // control produce.
    constexpr int kLiveThreshold = 4;

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

    // The shipped cloud material's schema, parsed by the engine's own parser from the file the engine
    // actually loads — never a list of names typed here, which would be the mirror this suite exists to
    // refuse.
    const ShaderProgramMeta& CloudSchema()
    {
        static const ShaderProgramMeta meta = []
        {
            const std::string  path = RepoRoot() + "Engine/Content/Shaders/Programs/Clouds/CloudRaymarch.shader";
            std::ifstream      in( path, std::ios::binary );
            std::ostringstream buffer;
            buffer << in.rdbuf();
            const std::string source = buffer.str();
            EXPECT_FALSE( source.empty() ) << path << " is unreadable, so this census counted nothing";

            auto parsed = DShaderParser::Parse( source );
            EXPECT_TRUE( parsed.IsSuccess() ) << parsed.GetError();
            return parsed.IsSuccess() ? parsed.GetValue().Meta : ShaderProgramMeta{};
        }();
        return meta;
    }

    const TypeInfo& Type( const char* name )
    {
        const TypeInfo* info = ReflectionRegistry::Get().Find( name );
        EXPECT_NE( info, nullptr ) << name << " is not a reflected type";
        static const TypeInfo empty{};
        return info ? *info : empty;
    }

    // The two directions of "the register and the tree hold the same set of names", said once so a third
    // population added later cannot be checked half as hard as the first two.
    void CheckCoversExactly( const std::string& population, const std::vector<std::string>& tree,
                             const Control* rows, std::size_t count )
    {
        for ( const std::string& name : tree )
        {
            const std::ptrdiff_t hits =
                 std::count_if( rows, rows + count, [&name]( const Control& c ) { return name == c.Name; } );
            EXPECT_EQ( hits, 1 )
                 << population << "::" << name
                 << " is an authored cloud control with no census row (or with more than one). Sweep its "
                    "whole authored range, at three rays that differ in BOTH azimuth and elevation, and "
                    "add the row — DEV_CONTRACT §1.3 says a parameter is wired only as far as a VISIBLE "
                    "EFFECT, and this is the only place that claim is written down.";
        }

        for ( const Control* r = rows; r != rows + count; ++r )
        {
            const bool known = std::find( tree.begin(), tree.end(), r->Name ) != tree.end();
            EXPECT_TRUE( known ) << population << "::" << r->Name
                                 << " has a census row and is not a control of this population — a stale "
                                    "row outliving the knob it described.";
        }
    }

    // The leading "max N/255" of a movement string. -1 when the row does not state one, which is itself
    // a failure: a row that describes its effect in prose cannot be compared against the threshold.
    int MaxLevelsOf( std::string_view movement )
    {
        const std::size_t at = movement.find( "max " );
        if ( at == std::string_view::npos )
            return -1;

        const std::string tail( movement.substr( at + 4 ) );
        char*             end   = nullptr;
        const long        value = std::strtol( tail.c_str(), &end, 10 );
        if ( end == tail.c_str() )
            return -1;

        // The unit has to be there. "max 8 km" is a range, not a movement, and reading its 8 as levels of
        // 255 would certify a row that never measured the frame at all.
        if ( std::string_view( end ).rfind( "/255", 0 ) != 0 )
            return -1;

        return static_cast<int>( value );
    }

    struct Population
    {
        const char*    Name;
        const Control* Rows;
        std::size_t    Count;
    };

    // ── AUTHORED CLOUD CONTROLS THIS CENSUS CANNOT COVER, WITH THE REASON ───────────────────────────
    //
    // Added by О1-G-2, which introduced the first authored cloud control whose MEMBERSHIP is not
    // derivable. Every population above is derived — from a reflected type or from a shipped shader's
    // Properties block — and that is what makes a new knob red here before it can reach a panel
    // unmeasured. A property of an authored medium has no such source: its name was invented by whoever
    // drew a `.dgraph` that the repository need not contain, and what it DOES is decided by the graph, so
    // there is no range for this register to sweep and no figure for it to hold.
    //
    // The row is here rather than absent because a silent gap is indistinguishable from an oversight —
    // the exact failure that let five controls appear between Р11 and O1 with nothing going red. What the
    // row buys is that the reason is written where the next person counting controls will read it.
    //
    // std::array AND NOT A C ARRAY, deliberately: this register is DESIGNED to be able to empty — the day
    // medium properties acquire a derivable population, its rows go away — and a zero-length C array is a
    // GNU extension clang takes and MSVC rejects (C2466), which has reached `dev` twice.
    struct OutOfScope
    {
        const char* Population;
        const char* Reason;
    };

    constexpr std::array<OutOfScope, 2> kOutOfScope = {
         { { "a Volume medium's own value properties",
             "the name is the graph author's and the effect is the graph's: there is no shipped range to "
             "sweep and no figure a register could hold. What IS pinned about them is that they cannot be "
             "read as a shipped property (Desert/Tests/Engine/CloudMaterialSchema) and that a property no "
             "medium function reads is never emitted at all "
             "(Desert/Tests/Editor/ShaderGraphCompiler), which is this census's own rule -- a control that "
             "moves nothing does not exist -- enforced where it CAN be" },
           { "a Volume medium's own image properties",
             "same, and one more: what an image does to the frame is a property of the IMAGE as much as of "
             "the graph, so even a graph in the repository would not fix the figure. The slot is proved "
             "always bound in all four consumers instead (Desert/Tests/Engine/ShaderCacheKey), which is the "
             "half that can be wrong silently" } } };

#define CENSUS_ROWS( rows ) ( rows ), std::size( rows )

    constexpr Population kPopulations[] = {
         { "VolumetricCloudData", CENSUS_ROWS( kComponentControls ) },
         { "HeroCloudData", CENSUS_ROWS( kHeroControls ) },
         { "CloudRaymarch.shader", CENSUS_ROWS( kMaterialControls ) },
    };
} // namespace

// ═══════════════════════════════════════════════════════════════════════════════════════════════════════
// MEMBERSHIP — the register and the tree hold the same controls
// ═══════════════════════════════════════════════════════════════════════════════════════════════════════

TEST( CloudControlCensus, EveryReflectedCloudFieldHasACensusRow )
{
    const TypeInfo&          type = Type( "VolumetricCloudData" );
    std::vector<std::string> fields;
    for ( const auto& f : type.Fields )
        fields.push_back( f.Name );
    ASSERT_FALSE( fields.empty() ) << "the cloud component is not reflected, so this census counted nothing";

    CheckCoversExactly( "VolumetricCloudData", fields, CENSUS_ROWS( kComponentControls ) );
}

TEST( CloudControlCensus, EveryHeroCloudFieldHasACensusRow )
{
    const TypeInfo&          type = Type( "HeroCloudData" );
    std::vector<std::string> fields;
    for ( const auto& f : type.Fields )
        fields.push_back( f.Name );
    ASSERT_FALSE( fields.empty() ) << "the hero cloud component is not reflected";

    CheckCoversExactly( "HeroCloudData", fields, CENSUS_ROWS( kHeroControls ) );
}

TEST( CloudControlCensus, EveryShippedMaterialPropertyHasACensusRow )
{
    std::vector<std::string> properties;
    for ( const ShaderParam& p : CloudSchema().Params )
        properties.push_back( p.Name );
    ASSERT_FALSE( properties.empty() ) << "the cloud material schema parsed empty";

    CheckCoversExactly( "CloudRaymarch.shader", properties, CENSUS_ROWS( kMaterialControls ) );
}

// ═══════════════════════════════════════════════════════════════════════════════════════════════════════
// CONTENT — every row says what it does, and clears the bar
// ═══════════════════════════════════════════════════════════════════════════════════════════════════════

TEST( CloudControlCensus, EveryRowStatesAMeasuredMovementThatClearsTheThreshold )
{
    int rows = 0;
    for ( const Population& p : kPopulations )
    {
        for ( const Control* r = p.Rows; r != p.Rows + p.Count; ++r )
        {
            ++rows;
            SCOPED_TRACE( std::string( p.Name ) + "::" + r->Name );

            ASSERT_NE( r->Movement, nullptr );
            ASSERT_NE( r->Measured, nullptr );
            ASSERT_NE( r->Condition, nullptr ) << "empty is allowed; null is a row somebody forgot to finish";

            EXPECT_STRNE( r->Measured, "" )
                 << r->Name
                 << " states a movement and does not say who measured it or on what. A figure with no "
                    "provenance cannot be told from a figure nobody ever took, and this subsystem has "
                    "already carried two such figures for six tasks.";

            const int levels = MaxLevelsOf( r->Movement );
            EXPECT_GE( levels, kLiveThreshold )
                 << r->Name << " moves the frame by " << levels
                 << "/255 over its whole authored range, which does not clear the census's live "
                    "threshold of "
                 << kLiveThreshold
                 << "/255. A control that moves nothing is a TODO wearing a feature's clothes "
                    "(DEV_CONTRACT §1.3): fix it or delete it. It may NOT be recorded here — this "
                    "register holds live controls, and a dead one has no row to hide behind.";
        }
    }

    // Derived, printed, never asserted: the number is what a person adjusts instead of measuring.
    std::printf( "[CloudControlCensus] %d authored cloud controls, every one of them measured\n", rows );
}

TEST( CloudControlCensus, NoControlIsRegisteredTwiceAcrossPopulations )
{
    // A value that lives in the component AND in the material is one setting with two sources of truth,
    // which is what O1 moved thirty-three fields to stop. The two names would drift and the frame would
    // follow whichever the renderer read last.
    std::set<std::string> component;
    for ( const Control& c : kComponentControls )
        component.insert( c.Name );

    for ( const Control& c : kMaterialControls )
        EXPECT_EQ( component.count( c.Name ), 0u )
             << c.Name
             << " is authored on the component AND on the material. One value, two homes: the panel "
                "shows two knobs, and the picture follows whichever the renderer resolved last.";
}

TEST( CloudControlCensus, EveryPopulationOutsideThisCensusSaysWhyAndCannotBeADerivableOne )
{
    // A ROW HERE IS AN EXEMPTION, so it has to carry its reason and it has to be about something this
    // census genuinely cannot derive. Both halves are checked, because an exemption whose reason is empty
    // is unreadable in a month and an exemption for something derivable is a knob somebody skipped.
    for ( const OutOfScope& row : kOutOfScope )
    {
        ASSERT_NE( row.Population, nullptr );
        ASSERT_NE( row.Reason, nullptr );
        EXPECT_STRNE( row.Population, "" );
        EXPECT_GT( std::string_view( row.Reason ).size(), 40u )
             << row.Population << " is excused from the census by a sentence too short to be a reason.";
    }

    // AND THE EXEMPTION IS ONLY ABOUT KEYS THIS CENSUS'S OWN POPULATIONS CANNOT CONTAIN. A medium's
    // properties live in the same `.demat` map as the shipped ones and are told apart by a prefix no GLSL
    // identifier may carry; if a shipped property ever COULD be spelled as a medium key, the exemption
    // above would be excusing a control this census is supposed to hold.
    for ( const ShaderParam& p : CloudSchema().Params )
        EXPECT_FALSE( Desert::Core::IsCloudMediumOverrideKey( p.Name ) )
             << p.Name
             << " is a shipped cloud material property that reads as an authored medium's key, so the "
                "out-of-scope rows above would be excusing it from its own census row.";

    std::printf( "[CloudControlCensus] %zu population(s) explicitly out of scope\n", kOutOfScope.size() );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
