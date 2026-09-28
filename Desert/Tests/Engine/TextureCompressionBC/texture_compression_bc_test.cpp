// THE ENABLE OF textureCompressionBC, PINNED WHERE IT ACTUALLY LIVES.
//
// This suite exists because the defect it guards has already happened once, in the quietest possible
// shape. The feature stood written down as
//
//     // .textureCompressionBC = vkFeatures10_.features.textureCompressionBC, // enable if supported
//
// in a vendored third-party Vulkan helper copy (since deleted), and that line read exactly like a switch somebody
// merely forgot to flip. It was not: that whole file was commented out, and the device the engine
// actually creates is VulkanLogicalDevice::CreateDevice in VulkanDevice.cpp, which before this suite's
// change never mentioned the feature in any form. A comment is not the code.
//
// WHY A SOURCE CENSUS AND NOT A RUNTIME TEST. The question is "does the device we create ask for this
// feature", and no test binary in this tree creates a Vulkan device. Worse, a runtime test on THIS
// machine could not fail even if it did: measured 2026-09-23 with a probe that creates an 8x8
// BC7_UNORM_BLOCK image, uploads four hand-built mode-6 blocks through a staging buffer and texelFetches
// all 64 texels from a compute shader, MoltenVK 1.1.357 on an Apple M1 Pro returns every texel bit-exact
// WHETHER OR NOT the feature was enabled, and validation layer 1.4.350.1 emits nothing in either run
// (the same run proved the layer live by a deliberate anisotropy error, which it did report). The
// absence of the enable is undetectable at run time here and enforceable on other drivers, which is
// precisely the combination a source census is for.
//
// THREE ROWS ARE PINNED, each a named statement rather than a count.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    // Same shape as the other censuses: the runner's working directory is not fixed, so walk up until a
    // file only this repository has appears. Deliberately not shared -- each census probes a different
    // sentinel, and a parameterised version would say less than the three lines it replaced.
    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + "Desert/Desert/Source/Engine/Graphic/API/Vulkan/VulkanDevice.cpp" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    std::string ReadAll( const fs::path& path )
    {
        std::ifstream      in( path, std::ios::binary );
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    // Strips // and /* */ comments so a mention inside a comment can never satisfy a row here. That is
    // not a detail: the mention this suite was written about WAS a comment, and a census that counted it
    // would have reported the hole as filled.
    std::string StripComments( const std::string& src )
    {
        std::string out;
        out.reserve( src.size() );
        enum class State
        {
            Code,
            Line,
            Block,
            String,
            Char
        };
        State state = State::Code;
        for ( size_t i = 0; i < src.size(); ++i )
        {
            const char c    = src[i];
            const char next = ( i + 1 < src.size() ) ? src[i + 1] : '\0';
            switch ( state )
            {
                case State::Code:
                    if ( c == '/' && next == '/' )
                    {
                        state = State::Line;
                        ++i;
                    }
                    else if ( c == '/' && next == '*' )
                    {
                        state = State::Block;
                        ++i;
                    }
                    else
                    {
                        if ( c == '"' )
                            state = State::String;
                        else if ( c == '\'' )
                            state = State::Char;
                        out.push_back( c );
                    }
                    break;
                case State::Line:
                    if ( c == '\n' )
                    {
                        state = State::Code;
                        out.push_back( c );
                    }
                    break;
                case State::Block:
                    if ( c == '*' && next == '/' )
                    {
                        state = State::Code;
                        ++i;
                    }
                    else if ( c == '\n' )
                    {
                        out.push_back( c );
                    }
                    break;
                case State::String:
                case State::Char:
                    if ( c == '\\' )
                    {
                        out.push_back( c );
                        if ( i + 1 < src.size() )
                            out.push_back( src[++i] );
                        break;
                    }
                    if ( ( state == State::String && c == '"' ) || ( state == State::Char && c == '\'' ) )
                        state = State::Code;
                    out.push_back( c );
                    break;
            }
        }
        return out;
    }

    std::string Collapse( const std::string& src )
    {
        std::string out;
        out.reserve( src.size() );
        bool space = false;
        for ( const char c : src )
        {
            if ( c == ' ' || c == '\t' || c == '\n' || c == '\r' )
            {
                space = true;
                continue;
            }
            if ( space && !out.empty() )
                out.push_back( ' ' );
            space = false;
            out.push_back( c );
        }
        return out;
    }

    const char* kDevicePath = "Desert/Desert/Source/Engine/Graphic/API/Vulkan/VulkanDevice.cpp";
    const char* kCapsPath   = "Desert/Desert/Source/Engine/Core/Device.hpp";
    const char* kProbePath  = "Desert/Desert/Source/Engine/Graphic/API/Vulkan/DeviceCapsProbe.cpp";
    const char* kTablePath  = "Desert/Desert/Source/Engine/Graphic/API/Vulkan/DeviceCaps.cpp";
} // namespace

// ROW 1: the feature is a row of the DeviceCaps table, OPTIONAL and core 1.0. Required would refuse a
// device without BC (every Apple GPU before M-series); absent would mean it is never asked for.
TEST( TextureCompressionBC, TheFeatureIsAnOptionalCore10RowOfDeviceCaps )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "repository root not found from the working directory";

    const std::string table = Collapse( StripComments( ReadAll( fs::path( root ) / kTablePath ) ) );
    EXPECT_NE( table.find( "{ C::TextureCompressionBC, \"textureCompressionBC\", N::Optional, V10, {} }" ),
               std::string::npos )
         << "DeviceCaps.cpp must carry textureCompressionBC as an optional Vulkan 1.0 row";
}

// ROW 2: the probe ASKS FOR the feature, in code, and enables it on the object the device is built
// from (DeviceCapsCensus pins that the VkDevice is built from that same object). enable_*_if_present
// both asks and enables: a device without BC is not asked for it, so vkCreateDevice cannot fail with
// VK_ERROR_FEATURE_NOT_PRESENT over it.
TEST( TextureCompressionBC, TheProbeEnablesTheFeatureWhenPresent )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::string probe = Collapse( StripComments( ReadAll( fs::path( root ) / kProbePath ) ) );
    EXPECT_NE( probe.find( "case Capability::TextureCompressionBC: return EnableCore10( device, "
                           "&VkPhysicalDeviceFeatures::textureCompressionBC );" ),
               std::string::npos )
         << "DeviceCapsProbe must map Capability::TextureCompressionBC to the textureCompressionBC feature. "
            "Without it every BC1..BC7 image the texture pipeline creates is outside the spec, and on "
            "MoltenVK nothing warns -- this census is what stands between here and a strict Windows driver.";
    EXPECT_NE( probe.find( "return device.enable_features_if_present( wanted );" ), std::string::npos )
         << "EnableCore10 must ask-and-enable through vk-bootstrap";
}

// ROW 3: what the renderer branches on is published FROM DeviceCaps, so the flag and the enabled
// feature cannot disagree.
TEST( TextureCompressionBC, TheCapabilityIsPublishedFromDeviceCaps )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::string device = Collapse( StripComments( ReadAll( fs::path( root ) / kDevicePath ) ) );
    const std::string caps   = Collapse( StripComments( ReadAll( fs::path( root ) / kCapsPath ) ) );

    EXPECT_NE( caps.find( "bool SupportsTextureCompressionBC" ), std::string::npos )
         << "DeviceCapabilities must carry SupportsTextureCompressionBC; the encoder work downstream "
            "branches on it.";
    EXPECT_NE( device.find( "m_Capabilities.SupportsTextureCompressionBC = m_DeviceCaps.Has( "
                            "Capability::TextureCompressionBC );" ),
               std::string::npos )
         << "SupportsTextureCompressionBC must be read from DeviceCaps, the record of what was enabled";
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
