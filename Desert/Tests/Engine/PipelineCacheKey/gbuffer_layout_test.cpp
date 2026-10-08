// GBUF1 step 4a — THE G-BUFFER'S SLOTS ARE ViewTargetFormats' CONSTANTS, IN ORDER, AND NO TARGET HOLDS A POSITION.
//
// World position is rebuilt from the depth (Common/ReconstructPosition.glslh); the RGBA32F GBufferC (16 B/px) is
// gone. Pinned here:
//  (1) SceneRenderer builds the G-buffer framebuffer from exactly kGBufferA, kGBufferB, kGBufferShadingWord,
//      kGBufferEmissive, kGBufferDepth, in that order, and from nothing else.
//      Mutations: a position target re-added (any slot, a ViewTargetFormats constant or an ImageFormat literal),
//      two slots swapped -> red.
//  (2) Slot 2 is the R32_UINT shading word, an integer format (never blended, ColourAttachmentBlendEnables); no
//      G-buffer slot is RGBA32F. Mutation: kGBufferShadingWord back to a float format -> red.
//  (3) Every shader that writes the G-buffer (an output named oGBuffer*) declares location i with the type slot
//  i's
//      format needs (uint for the integer slot, vec4 otherwise) and the slot's name, and declares no output past
//      the colour slots. Mutations: `layout(location = 4) out vec4 oGBufferC` in a G-buffer writer, the shading
//      word declared vec4 -> red.
//  (4) ViewTargetFormats names no position target and RGBA32F only for the jump-flood seeds; the RSM's colour
//  slots
//      (kRSMColourSlots, pinned in PipelineBlendState.TheRSMSlotsAreOneListWithSlotTwoUnused) are the G-buffer's
//      slots with slot 2 unused. Mutations: a kGBufferC / k*WorldPos constant re-added, an RSM slot 2 format ->
//      red.
#include <Engine/Core/Formats/ImageFormat.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>

#include "../../TestSupport/scratch_dir.hpp"

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    namespace F = Desert::Graphic::ViewTargetFormats;
    using Desert::Core::Formats::ImageFormat;

    struct Slot
    {
        const char* Constant;   // the ViewTargetFormats name SceneRenderer spells
        ImageFormat Format;     // its value
        const char* ShaderName; // the G-buffer writers' output at this location (colour slots only)
    };

    // The G-buffer in slot order: four colour slots, then the depth.
    const std::array<Slot, 5> kGBuffer     = { {
         { "kGBufferA", F::kGBufferA, "oGBufferA" },
         { "kGBufferB", F::kGBufferB, "oGBufferB" },
         { "kGBufferShadingWord", F::kGBufferShadingWord, "oGBufferShadingWord" },
         { "kGBufferEmissive", F::kGBufferEmissive, "oGBufferEmissive" },
         { "kGBufferDepth", F::kGBufferDepth, nullptr },
    } };
    constexpr size_t          kColourSlots = 4;

    std::string ReadText( const std::filesystem::path& path )
    {
        std::ifstream file( path, std::ios::binary );
        EXPECT_TRUE( file.good() ) << "cannot read " << path;
        std::ostringstream text;
        text << file.rdbuf();
        return text.str();
    }
} // namespace

TEST( GBufferLayout, TheGBufferIsBuiltFromTheViewTargetFormatsInSlotOrder )
{
    const std::string scene = ReadText( Desert::TestSupport::RepositoryRoot() / "Desert" / "Desert" / "Source" /
                                        "Engine" / "Graphic" / "SceneRenderer.cpp" );
    const size_t      begin = scene.find( "FramebufferSpecification gbufferSpec;" );
    ASSERT_NE( begin, std::string::npos ) << "SceneRenderer.cpp no longer declares gbufferSpec";
    const size_t end = scene.find( "Framebuffer::Create( gbufferSpec )", begin );
    ASSERT_NE( end, std::string::npos ) << "SceneRenderer.cpp no longer creates the G-buffer from gbufferSpec";
    const std::string setup = scene.substr( begin, end - begin );

    std::vector<std::string> spelled;
    const std::regex         slot( R"(emplace_back\(\s*ViewTargetFormats::(\w+)\s*\))" );
    for ( auto it = std::sregex_iterator( setup.begin(), setup.end(), slot ); it != std::sregex_iterator(); ++it )
        spelled.push_back( ( *it )[1] );

    size_t attachments = 0;
    for ( size_t at = setup.find( "emplace_back" ); at != std::string::npos;
          at        = setup.find( "emplace_back", at + 1 ) )
        ++attachments;
    EXPECT_EQ( attachments, spelled.size() ) << "a G-buffer attachment is not a ViewTargetFormats constant";

    std::vector<std::string> expected;
    for ( const Slot& s : kGBuffer )
        expected.push_back( s.Constant );
    EXPECT_EQ( spelled, expected );
}

TEST( GBufferLayout, SlotTwoIsTheIntegerShadingWordAndNoSlotIsAFloat4Position )
{
    EXPECT_EQ( F::kGBufferShadingWord, ImageFormat::R32_UINT );
    EXPECT_TRUE( Desert::Core::Formats::IsIntegerFormat( kGBuffer[2].Format ) );
    for ( size_t i = 0; i < kGBuffer.size(); ++i )
    {
        EXPECT_NE( kGBuffer[i].Format, ImageFormat::RGBA32F ) << kGBuffer[i].Constant;
        if ( i != 2 && i < kColourSlots )
            EXPECT_FALSE( Desert::Core::Formats::IsIntegerFormat( kGBuffer[i].Format ) ) << kGBuffer[i].Constant;
    }
    EXPECT_EQ( F::kGBufferDepth, ImageFormat::DEPTH32F ) << "the reconstruction's precision is the depth's";
}

TEST( GBufferLayout, EveryGBufferWriterDeclaresTheSlotsTypeAtTheSlotsLocation )
{
    const std::filesystem::path shaders =
         Desert::TestSupport::RepositoryRoot() / "Editor" / "Resources" / "Shaders";
    ASSERT_TRUE( std::filesystem::is_directory( shaders ) ) << shaders;

    // `layout( location = N ) out T name` (plain GLSL / .glslh) and `Out(N) T name` (the engine's .shader).
    const std::regex output(
         R"((?:layout\s*\(\s*location\s*=\s*(\d+)\s*\)\s*out|\bOut\(\s*(\d+)\s*\))\s+(\w+)\s+(\w+))" );
    std::set<std::string> writers;
    for ( const auto& entry : std::filesystem::recursive_directory_iterator( shaders ) )
    {
        if ( !entry.is_regular_file() )
            continue;
        const std::string extension = entry.path().extension().string();
        if ( extension != ".shader" && extension != ".glslh" && extension != ".glsl" )
            continue;
        const std::string text = ReadText( entry.path() );
        if ( text.find( "oGBuffer" ) == std::string::npos )
            continue;

        const std::string file = std::filesystem::relative( entry.path(), shaders ).generic_string();
        for ( auto it = std::sregex_iterator( text.begin(), text.end(), output ); it != std::sregex_iterator();
              ++it )
        {
            const std::string location = ( *it )[1].matched ? ( *it )[1].str() : ( *it )[2].str();
            const size_t      index    = std::stoul( location );
            const std::string type     = ( *it )[3];
            const std::string name     = ( *it )[4];
            writers.insert( file );
            // The slot after the framebuffer's own colours is the graph-provided view velocity (TAA1:
            // ViewTargetLayouts.hpp GBufferLayout, RG16F) — the one output a G-buffer writer may declare past
            // them.
            if ( index == kColourSlots )
            {
                EXPECT_EQ( name, "oVelocity" ) << file << " location " << index;
                EXPECT_EQ( type, "vec2" ) << file << ": the velocity output is RG16F";
                continue;
            }
            ASSERT_LT( index, kColourSlots )
                 << file << ": " << name << " at location " << index << " - the G-buffer has " << kColourSlots
                 << " colour slots and the velocity after them";
            EXPECT_EQ( name, kGBuffer[index].ShaderName ) << file << " location " << index;
            const char* wanted =
                 Desert::Core::Formats::IsIntegerFormat( kGBuffer[index].Format ) ? "uint" : "vec4";
            EXPECT_EQ( type, wanted ) << file << ": " << name << " vs " << kGBuffer[index].Constant;
        }
    }
    // The surface cell pass every mesh G-buffer program shares, and the terrain's own program.
    EXPECT_EQ( writers.count( "Mesh/Surface/Pass_GBuffer.glslh" ), 1u );
    EXPECT_EQ( writers.count( "Programs/Terrain/TerrainGBuffer.shader" ), 1u );
}

TEST( GBufferLayout, NoViewTargetIsAPositionAndTheRSMIsTheGBufferWithSlotTwoUnused )
{
    const std::string formats = ReadText( Desert::TestSupport::RepositoryRoot() / "Desert" / "Desert" / "Source" /
                                          "Engine" / "Graphic" / "ViewTargetFormats.hpp" );
    std::set<std::string> float4x32;
    const std::regex      constant( R"(inline\s+constexpr\s+ImageFormat\s+(\w+)\s*=\s*ImageFormat::(\w+))" );
    for ( auto it = std::sregex_iterator( formats.begin(), formats.end(), constant ); it != std::sregex_iterator();
          ++it )
    {
        const std::string name = ( *it )[1];
        EXPECT_EQ( name.find( "Pos" ), std::string::npos )
             << name << ": a position target is reconstructed, not stored";
        EXPECT_NE( name, "kGBufferC" );
        if ( ( *it )[2] == "RGBA32F" )
            float4x32.insert( name );
    }
    EXPECT_EQ( float4x32, std::set<std::string>{ "kJFASeed" } ) << "an RGBA32F view target besides the JFA seeds";

    ASSERT_EQ( F::kRSMColourSlots.size(), kColourSlots );
    for ( size_t i = 0; i < kColourSlots; ++i )
    {
        if ( i == 2 )
            EXPECT_FALSE( F::kRSMColourSlots[i].has_value() ) << "the RSM writes no shading word";
        else
            EXPECT_EQ( F::kRSMColourSlots[i], kGBuffer[i].Format ) << "RSM slot " << i;
    }
}
