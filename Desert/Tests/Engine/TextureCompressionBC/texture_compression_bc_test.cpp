// THE ENABLE OF textureCompressionBC, PINNED WHERE IT ACTUALLY LIVES.
//
// This suite exists because the defect it guards has already happened once, in the quietest possible
// shape. The feature stood written down as
//
//     // .textureCompressionBC = vkFeatures10_.features.textureCompressionBC, // enable if supported
//
// in Graphic/API/Vulkan/VulkanUtils/lightweightvk/VulkanClasses.cpp, and that line reads exactly like a
// switch somebody merely forgot to flip. It is not. THAT WHOLE FILE IS COMMENTED OUT -- all 8093 lines
// of it, zero of them code -- and no header outside the vendored `lightweightvk` directory is included
// by anything in the engine. Uncommenting those two lines would change nothing at all, because the
// device that the engine actually creates is VulkanLogicalDevice::CreateDevice in VulkanDevice.cpp, and
// before this suite's change it never mentioned the feature in any form. A comment is not the code.
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
// FOUR ROWS ARE PINNED, each a named statement rather than a count.

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
    const char* kVendored   = "Desert/Desert/Source/Engine/Graphic/API/Vulkan/VulkanUtils/lightweightvk";
} // namespace

// ROW 1: the logical device ASKS FOR the feature, in code, not in a comment.
TEST( TextureCompressionBC, DeviceCreationAsksForTheFeature )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "repository root not found from the working directory";

    const std::string code = Collapse( StripComments( ReadAll( fs::path( root ) / kDevicePath ) ) );
    EXPECT_NE( code.find( "deviceFeatures.textureCompressionBC = VK_TRUE" ), std::string::npos )
         << "VulkanLogicalDevice::CreateDevice must set deviceFeatures.textureCompressionBC = VK_TRUE "
            "before vkCreateDevice. Without it every BC1..BC7 image the texture pipeline creates is "
            "outside the spec. It will not fail on MoltenVK -- measured, it does not even warn -- so "
            "this census is the only thing standing between here and a Windows driver that enforces it.";
}

// ROW 2: the enable sits INSIDE CreateDevice and BEFORE the vkCreateDevice call it has to reach. A
// correct assignment placed after the call, or in a different function, satisfies row 1 and does
// nothing -- pEnabledFeatures is read at the call, and never again.
TEST( TextureCompressionBC, TheEnableReachesTheCreateCall )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::string src = StripComments( ReadAll( fs::path( root ) / kDevicePath ) );

    const size_t fn = src.find( "VulkanLogicalDevice::CreateDevice" );
    ASSERT_NE( fn, std::string::npos ) << "VulkanLogicalDevice::CreateDevice no longer exists under that "
                                          "name; this census needs re-aiming, not deleting.";

    const size_t create = src.find( "vkCreateDevice(", fn );
    ASSERT_NE( create, std::string::npos ) << "no vkCreateDevice call after CreateDevice's definition";

    const size_t enable = src.find( "textureCompressionBC", fn );
    ASSERT_NE( enable, std::string::npos ) << "CreateDevice does not mention textureCompressionBC at all";
    EXPECT_LT( enable, create ) << "the textureCompressionBC enable is written AFTER the vkCreateDevice "
                                   "call it is supposed to feed. VkDeviceCreateInfo::pEnabledFeatures is "
                                   "read once, at the call; an assignment past it is dead.";
}

// ROW 3: the enable is GATED on support that was actually read from the physical device. Asking for a
// feature the device does not have makes vkCreateDevice fail outright with
// VK_ERROR_FEATURE_NOT_PRESENT, which turns a missing texture format into a dead engine.
TEST( TextureCompressionBC, SupportIsReadBeforeItIsAskedFor )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::string device = Collapse( StripComments( ReadAll( fs::path( root ) / kDevicePath ) ) );
    const std::string caps   = Collapse( StripComments( ReadAll( fs::path( root ) / kCapsPath ) ) );

    EXPECT_NE( caps.find( "bool SupportsTextureCompressionBC" ), std::string::npos )
         << "DeviceCapabilities must carry SupportsTextureCompressionBC; the encoder work downstream "
            "branches on it, and it is the only place the read is published.";

    EXPECT_NE( device.find( "m_Capabilities.SupportsTextureCompressionBC = deviceFeatures."
                            "textureCompressionBC == VK_TRUE" ),
               std::string::npos )
         << "the physical device's textureCompressionBC must be read into the capability before anything "
            "asks for it";

    EXPECT_NE( device.find( "if ( m_PhysicalDevice->m_Capabilities.SupportsTextureCompressionBC )" ),
               std::string::npos )
         << "the enable must be gated on the capability. An ungated request for an unsupported feature "
            "does not degrade -- vkCreateDevice returns VK_ERROR_FEATURE_NOT_PRESENT and the engine has "
            "no device at all.";
}

// ROW 4: the enable does NOT move back into vendored lightweightvk.
//
// That tree is carried inside Desert/ rather than under ThirdParty/, which makes it look editable, and
// the original mention of this feature lives there. It is dead: VulkanClasses.cpp contains no code, and
// nothing outside the directory includes any of its headers. If a future change puts the enable back
// there it will be invisible for the same reason it was invisible the first time, so this row asserts
// the absence of live code mentioning the feature inside it. A comment there is fine -- it is what the
// file is made of -- so comments are stripped before looking.
TEST( TextureCompressionBC, VendoredLightweightVkHoldsNoLiveEnable )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const fs::path dir = fs::path( root ) / kVendored;
    ASSERT_TRUE( fs::exists( dir ) ) << "vendored lightweightvk directory moved; re-aim this row";

    std::vector<std::string> offenders;
    for ( const auto& entry : fs::directory_iterator( dir ) )
    {
        if ( !entry.is_regular_file() )
            continue;
        const std::string ext = entry.path().extension().string();
        if ( ext != ".cpp" && ext != ".hpp" && ext != ".h" )
            continue;
        const std::string code = StripComments( ReadAll( entry.path() ) );
        if ( code.find( "textureCompressionBC" ) != std::string::npos )
            offenders.push_back( entry.path().filename().string() );
    }

    EXPECT_TRUE( offenders.empty() )
         << "live code in the vendored lightweightvk tree now mentions textureCompressionBC ("
         << ( offenders.empty() ? std::string{} : offenders.front() )
         << "). The engine does not create its device there -- VulkanDevice.cpp does -- so an enable in "
            "that tree is a change that cannot reach a running frame.";
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
