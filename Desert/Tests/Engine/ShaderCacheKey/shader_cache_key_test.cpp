// The two things that decided whether a shader edit reaches the GPU — tested without one.
//
// Both are here because a real failure needed both to be understood. A compute shader gained a binding
// while the editor was running; the validation layer then reported, every frame:
//
//     VkDescriptorSetLayout from VkPipelineLayout has 8 total descriptors,
//     but VkDescriptorSetLayout, trying to bind, has 9 total descriptors
//
// Two candidates: a SPIR-V cache key that missed the edit (so one compile saw the old shader), or a
// descriptor set layout destroyed under a live pipeline (so two compiles disagreed). Only the second
// turned out to be true — and the only way to say that here, with no Vulkan, is to be able to compute
// both numbers on the CPU:
//
//   1. THE CACHE KEY — Core::ComputeShaderCacheKey and Core::CollectShaderIncludes, the functions the
//      compiler itself calls. A key that does not move when an included .glslh moves is the worst kind
//      of failure there is: the machine with the stale artifact renders differently from the machine
//      without it, and neither says anything.
//
//   2. THE DESCRIPTOR COUNT — ShaderReflection::BuildLayoutBindings over the REAL shaders, which is the
//      shape VulkanShader turns into a VkDescriptorSetLayout. The number this produces is the number
//      the validation layer compares, so a shader that gains a binding is visible here first.

#include <gtest/gtest.h>

#include <Engine/Core/Formats/MaterialParamRow.hpp>
#include <Engine/Core/ShaderCompiler/DShader/DShaderParser.hpp>
#include <Engine/Core/ShaderCompiler/ShaderCacheKey.hpp>
#include <Engine/Core/ShaderCompiler/ShaderGraphBindings.hpp>
#include <Engine/Core/ShaderCompiler/ShaderGraphMedium.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanShaderReflection.hpp>
#include <Engine/Graphic/Clouds/CloudAuthoredPayload.hpp>
#include <Engine/Graphic/Clouds/CloudEnvironmentBake.hpp>
#include <Engine/Graphic/Clouds/CloudMaterialValues.hpp>
#include <Engine/Graphic/Clouds/CloudShadowPayload.hpp>
#include <Engine/Graphic/Clouds/CloudSkyOcclusionPayload.hpp>
#include <Engine/Graphic/SkyPayload.hpp>
#include <Engine/Graphic/Systems/Scene/Particles/ParticleGpuLayout.hpp>

#include <Common/Core/Constants.hpp>

#include <shaderc/shaderc.hpp>

#include <format>
#include <iostream>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

using Desert::Core::CollectShaderIncludes;
using Desert::Core::ComputeShaderCacheKey;
using Desert::Core::Formats::ShaderStage;
using namespace Desert::Graphic::API::Vulkan;

namespace
{
    // The engine resolves `#include <...>` against Common::Constants::Path::SHADERDIR_PATH, which is
    // relative ("Resources/Shaders/"). The editor runs with its own directory as the working one; the
    // test does the same so the include walk resolves the same files the runtime would.
    struct ShaderRootFixture : ::testing::Test
    {
        static void SetUpTestSuite()
        {
            // The test binary lives in build/Bin/Tests/<config>; the shader root is Editor/Resources.
            std::filesystem::path here = std::filesystem::current_path();
            for ( int up = 0; up < 8 && !std::filesystem::exists( here / "Editor" / "Resources" / "Shaders" );
                  ++up )
                here = here.parent_path();

            s_RepoRoot = here;
            ASSERT_TRUE( std::filesystem::exists( s_RepoRoot / "Editor" / "Resources" / "Shaders" ) )
                 << "could not find Editor/Resources/Shaders above " << std::filesystem::current_path();

            std::filesystem::current_path( s_RepoRoot / "Editor" );
        }

        static std::filesystem::path s_RepoRoot;
    };

    std::filesystem::path ShaderRootFixture::s_RepoRoot;

    std::string ReadFile( const std::filesystem::path& path )
    {
        std::ifstream      in( path, std::ios::binary );
        std::ostringstream out;
        out << in.rdbuf();
        return out.str();
    }

    std::filesystem::path ShaderPath( const char* relative )
    {
        return std::filesystem::path( "Resources/Shaders/Programs" ) / relative;
    }

    // The assembled GLSL of one stage, straight out of the engine's own DSL parser — the same string
    // the compiler hashes and hands to shaderc.
    std::string StageSource( const std::filesystem::path& shaderFile, ShaderStage stage )
    {
        auto parsed = Desert::Core::Preprocess::DShaderParser::Parse( ReadFile( shaderFile ) );
        EXPECT_TRUE( parsed.IsSuccess() ) << shaderFile.string();
        if ( !parsed.IsSuccess() )
            return {};
        const auto it = parsed.GetValue().Stages.find( stage );
        EXPECT_NE( it, parsed.GetValue().Stages.end() ) << shaderFile.string();
        return it == parsed.GetValue().Stages.end() ? std::string{} : it->second;
    }

    // Resolves `#include <...>` exactly as ShaderIncluder does, so the SPIR-V under test is the SPIR-V
    // the engine compiles.
    class Includer final : public shaderc::CompileOptions::IncluderInterface
    {
    public:
        shaderc_include_result* GetInclude( const char* requested, shaderc_include_type type,
                                            const char* requesting, size_t ) override
        {
            const std::filesystem::path full =
                 type == shaderc_include_type_relative
                      ? ( std::filesystem::path( requesting ).parent_path() / requested ).lexically_normal()
                      : ( Common::Constants::Path::SHADERDIR_PATH / requested ).lexically_normal();

            auto* name = new std::string( full.string() );
            auto* body =
                 new std::string( Desert::Core::Preprocess::DShaderParser::TranslateSugar( ReadFile( full ) ) );

            auto* result               = new shaderc_include_result;
            result->source_name        = name->c_str();
            result->source_name_length = name->size();
            result->content            = body->c_str();
            result->content_length     = body->size();
            result->user_data          = new std::pair<std::string*, std::string*>( name, body );
            return result;
        }

        void ReleaseInclude( shaderc_include_result* data ) override
        {
            auto* pair = static_cast<std::pair<std::string*, std::string*>*>( data->user_data );
            delete pair->first;
            delete pair->second;
            delete pair;
            delete data;
        }
    };

    std::vector<uint32_t> CompileStage( const std::string& source, const std::filesystem::path& path,
                                        shaderc_shader_kind kind )
    {
        shaderc::Compiler       compiler;
        shaderc::CompileOptions options;
        options.SetIncluder( std::make_unique<Includer>() );
        options.SetTargetEnvironment( shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_1 );
        options.SetWarningsAsErrors();

        const auto result = compiler.CompileGlslToSpv( source, kind, path.string().c_str(), options );
        EXPECT_EQ( result.GetCompilationStatus(), shaderc_compilation_status_success )
             << path.string() << ": " << result.GetErrorMessage();
        if ( result.GetCompilationStatus() != shaderc_compilation_status_success )
            return {};
        return { result.begin(), result.end() };
    }

    // Reflects one compute shader and returns the layout bindings of set 0 — the contract a pipeline
    // layout and a descriptor set have to agree on.
    std::vector<VkDescriptorSetLayoutBinding> ComputeSetZero( const std::filesystem::path& shaderFile )
    {
        const std::string source = StageSource( shaderFile, ShaderStage::Compute );
        const auto        spirv  = CompileStage( source, shaderFile, shaderc_compute_shader );
        if ( spirv.empty() )
            return {};

        ShaderResource::ReflectionData data;
        const auto diagnostics = ShaderReflection::ReflectStage( spirv, ShaderStage::Compute, data );
        EXPECT_TRUE( diagnostics.empty() ) << ( diagnostics.empty() ? "" : diagnostics.front() );

        const auto it = data.ShaderDescriptorSets.find( 0 );
        return it == data.ShaderDescriptorSets.end() ? std::vector<VkDescriptorSetLayoutBinding>{}
                                                     : ShaderReflection::BuildLayoutBindings( it->second );
    }

    // The same reflection for a FRAGMENT stage. A separate function rather than a stage parameter on the
    // one above, because the two shaderc kinds are the only difference and a bool argument at the call
    // site reads as nothing at all.
    std::vector<VkDescriptorSetLayoutBinding> FragmentSetZero( const std::filesystem::path& shaderFile )
    {
        const std::string source = StageSource( shaderFile, ShaderStage::Fragment );
        const auto        spirv  = CompileStage( source, shaderFile, shaderc_fragment_shader );
        if ( spirv.empty() )
            return {};

        ShaderResource::ReflectionData data;
        const auto diagnostics = ShaderReflection::ReflectStage( spirv, ShaderStage::Fragment, data );
        EXPECT_TRUE( diagnostics.empty() ) << ( diagnostics.empty() ? "" : diagnostics.front() );

        const auto it = data.ShaderDescriptorSets.find( 0 );
        return it == data.ShaderDescriptorSets.end() ? std::vector<VkDescriptorSetLayoutBinding>{}
                                                     : ShaderReflection::BuildLayoutBindings( it->second );
    }

    // Set 0 of a GRAPHICS shader as the PIPELINE LAYOUT sees it: both stages reflected into one
    // ReflectionData, which is what VulkanShader does before it builds the VkDescriptorSetLayout. The
    // per-stage helpers above answer "what does this stage read"; this one answers "what shape must a
    // descriptor set have to be bindable to a pipeline built from this shader", and only the second
    // question can compare two different shaders.
    std::vector<VkDescriptorSetLayoutBinding> GraphicsSetZero( const std::filesystem::path& shaderFile )
    {
        const auto vertexSpirv =
             CompileStage( StageSource( shaderFile, ShaderStage::Vertex ), shaderFile, shaderc_vertex_shader );
        const auto fragmentSpirv =
             CompileStage( StageSource( shaderFile, ShaderStage::Fragment ), shaderFile, shaderc_fragment_shader );
        if ( vertexSpirv.empty() || fragmentSpirv.empty() )
            return {};

        ShaderResource::ReflectionData data;
        auto diagnostics = ShaderReflection::ReflectStage( vertexSpirv, ShaderStage::Vertex, data );
        EXPECT_TRUE( diagnostics.empty() ) << ( diagnostics.empty() ? "" : diagnostics.front() );
        diagnostics = ShaderReflection::ReflectStage( fragmentSpirv, ShaderStage::Fragment, data );
        EXPECT_TRUE( diagnostics.empty() ) << ( diagnostics.empty() ? "" : diagnostics.front() );

        const auto it = data.ShaderDescriptorSets.find( 0 );
        return it == data.ShaderDescriptorSets.end() ? std::vector<VkDescriptorSetLayoutBinding>{}
                                                     : ShaderReflection::BuildLayoutBindings( it->second );
    }

    // Printable "binding:type" list, so a failure names the slot that diverged instead of a count.
    std::string DescribeBindings( const std::vector<VkDescriptorSetLayoutBinding>& bindings )
    {
        std::ostringstream out;
        for ( const auto& b : bindings )
            out << b.binding << ':' << static_cast<int>( b.descriptorType ) << ' ';
        return out.str();
    }

    /**
     * The DECLARED SIZE of a storage block, in bytes, as the compiled SPIR-V says it is.
     *
     * WHY THIS EXISTS BESIDE ComputeSetZero, which already reflects the same module: the descriptor list
     * says a storage buffer is bound at a slot and says NOTHING about what is inside it. A parameter block
     * has two statements — a `struct` in C++ and a `layout(std430)` in GLSL — and every member after a
     * divergence is read from the wrong offset. That does not look like a bug; it looks like the clouds
     * being badly tuned, which is the sentence CloudPayload.hpp opens with.
     *
     * Returns 0 when the block is absent, which every caller treats as a failure rather than as a size.
     */
    uint32_t ComputeStorageBlockBytes( const std::filesystem::path& shaderFile, uint32_t binding )
    {
        const std::string source = StageSource( shaderFile, ShaderStage::Compute );
        const auto        spirv  = CompileStage( source, shaderFile, shaderc_compute_shader );
        if ( spirv.empty() )
            return 0u;

        ShaderResource::ReflectionData data;
        const auto diagnostics = ShaderReflection::ReflectStage( spirv, ShaderStage::Compute, data );
        EXPECT_TRUE( diagnostics.empty() ) << ( diagnostics.empty() ? "" : diagnostics.front() );

        const auto set = data.ShaderDescriptorSets.find( 0 );
        if ( set == data.ShaderDescriptorSets.end() )
            return 0u;

        const auto block = set->second.StorageBuffers.find( binding );
        return block == set->second.StorageBuffers.end() ? 0u : block->second.Size;
    }

    bool HasBinding( const std::vector<VkDescriptorSetLayoutBinding>& bindings, uint32_t binding,
                     VkDescriptorType type )
    {
        for ( const auto& b : bindings )
            if ( b.binding == binding )
                return b.descriptorType == type && b.descriptorCount == 1;
        return false;
    }

    // A temporary .glslh next to the real ones, so an include can be edited without touching the tree.
    struct ScopedHeader
    {
        explicit ScopedHeader( std::string body )
             : Path( std::filesystem::path( "Resources/Shaders/Common" ) / "CacheKeyTestScratch.glslh" )
        {
            Write( std::move( body ) );
        }

        ~ScopedHeader()
        {
            std::error_code ec;
            std::filesystem::remove( Path, ec );
        }

        void Write( std::string body ) const
        {
            std::ofstream out( Path, std::ios::binary | std::ios::trunc );
            out << body;
        }

        std::filesystem::path Path;
    };
} // namespace

// ---- The cache key ----------------------------------------------------------------------------------

TEST_F( ShaderRootFixture, TheKeyIsStableForUnchangedInput )
{
    const std::string source = "#version 450\nvoid main() {}\n";
    const auto        path   = ShaderPath( "Fog/HeightFog.shader" );

    EXPECT_EQ( ComputeShaderCacheKey( ShaderStage::Compute, source, path ),
               ComputeShaderCacheKey( ShaderStage::Compute, source, path ) );
}

TEST_F( ShaderRootFixture, EditingTheStageSourceMovesTheKey )
{
    const auto path = ShaderPath( "Fog/HeightFog.shader" );

    EXPECT_NE( ComputeShaderCacheKey( ShaderStage::Compute, "#version 450\nvoid main() {}\n", path ),
               ComputeShaderCacheKey( ShaderStage::Compute, "#version 450\nvoid main() { }\n", path ) );
}

TEST_F( ShaderRootFixture, TheStageIsPartOfTheKey )
{
    const std::string source = "#version 450\nvoid main() {}\n";
    const auto        path   = ShaderPath( "Fog/HeightFog.shader" );

    // Vertex and fragment SPIR-V from identical text are different binaries, and a key that ignored the
    // stage would serve one for the other.
    EXPECT_NE( ComputeShaderCacheKey( ShaderStage::Vertex, source, path ),
               ComputeShaderCacheKey( ShaderStage::Fragment, source, path ) );
}

TEST_F( ShaderRootFixture, EditingAnIncludedHeaderMovesTheKey )
{
    // THE property the cache rests on. The stage source does not change at all here — only a file it
    // includes does, which is exactly the case a naive key gets wrong.
    ScopedHeader header( "// v1\nconst float kScratch = 1.0f;\n" );

    const std::string source = "#version 450\n#include <Common/CacheKeyTestScratch.glslh>\nvoid main() {}\n";
    const auto        path   = ShaderPath( "Fog/HeightFog.shader" );

    const uint64_t before = ComputeShaderCacheKey( ShaderStage::Compute, source, path );

    header.Write( "// v2\nconst float kScratch = 2.0f;\n" );
    const uint64_t after = ComputeShaderCacheKey( ShaderStage::Compute, source, path );

    EXPECT_NE( before, after );

    // ...and it comes back when the header does: the key is a function of content, not of edit count.
    header.Write( "// v1\nconst float kScratch = 1.0f;\n" );
    EXPECT_EQ( ComputeShaderCacheKey( ShaderStage::Compute, source, path ), before );
}

TEST_F( ShaderRootFixture, AnIncludeThatDoesNotResolveIsNotFatal )
{
    const std::string source = "#version 450\n#include <Common/NoSuchHeaderAnywhere.glslh>\nvoid main() {}\n";
    const auto        path   = ShaderPath( "Fog/HeightFog.shader" );

    EXPECT_TRUE( CollectShaderIncludes( source, path ).empty() );
    EXPECT_NO_THROW( (void)ComputeShaderCacheKey( ShaderStage::Compute, source, path ) );
}

// ---- The compile-time variant -----------------------------------------------------------------------
//
// A ShaderVariant substitutes the BYTES of an include without changing its NAME (Engine/Core/
// ShaderCompiler/ShaderVariant.hpp). Everything on disk therefore stays identical between two variants of
// one program, which is precisely why the key has to carry them: without it the first variant compiled
// wins the cache entry and every other one is silently served its SPIR-V — across restarts, because the
// cache is a directory. The symptom would be "my cloud graph does nothing", which names neither a cache
// nor a key.

TEST_F( ShaderRootFixture, TheVariantSeparatesTwoBodiesUnderOneIncludeName )
{
    // THE MUTATION THIS AXIS EXISTS FOR: same stage, same file, same headers on disk, same include NAME —
    // only the substituted body differs.
    const std::string source = "#version 450\n#include <Generated/CloudMedium.glslh>\nvoid main() {}\n";
    const auto        path   = ShaderPath( "Fog/HeightFog.shader" );

    Desert::Core::ShaderVariant a{ { { "Generated/CloudMedium.glslh", "// medium A\n" } } };
    Desert::Core::ShaderVariant b{ { { "Generated/CloudMedium.glslh", "// medium B\n" } } };

    EXPECT_NE( ComputeShaderCacheKey( ShaderStage::Compute, source, path, a ),
               ComputeShaderCacheKey( ShaderStage::Compute, source, path, b ) )
         << "two different media compiled under one include name produced ONE cache key, so the second "
            "one would be served the first one's SPIR-V.";

    // And it is a function of content, not of how many variants have been seen.
    EXPECT_EQ( ComputeShaderCacheKey( ShaderStage::Compute, source, path, a ),
               ComputeShaderCacheKey(
                    ShaderStage::Compute, source, path,
                    Desert::Core::ShaderVariant{ { { "Generated/CloudMedium.glslh", "// medium A\n" } } } ) );
}

TEST_F( ShaderRootFixture, MovingTextBetweenTwoSubstitutedNamesIsADifferentVariant )
{
    // The hash is order-independent, and the cheap way to get that is to XOR a hash per entry. Hashing
    // the name and the body SEPARATELY would make these two variants equal — the bodies swapped between
    // the names — and they compile to different programs.
    const std::string source = "#version 450\nvoid main() {}\n";
    const auto        path   = ShaderPath( "Fog/HeightFog.shader" );

    const Desert::Core::ShaderVariant straight{
         { { "Generated/A.glslh", "// one\n" }, { "Generated/B.glslh", "// two\n" } } };
    const Desert::Core::ShaderVariant swapped{
         { { "Generated/A.glslh", "// two\n" }, { "Generated/B.glslh", "// one\n" } } };
    const Desert::Core::ShaderVariant reordered{
         { { "Generated/B.glslh", "// two\n" }, { "Generated/A.glslh", "// one\n" } } };

    EXPECT_NE( straight.Hash(), swapped.Hash() );
    EXPECT_EQ( straight.Hash(), reordered.Hash() )
         << "the same substitution assembled in a different order hashed differently, so it would compile "
            "and cache twice and report a miss for ever.";
    EXPECT_NE( ComputeShaderCacheKey( ShaderStage::Compute, source, path, straight ),
               ComputeShaderCacheKey( ShaderStage::Compute, source, path, swapped ) );
}

TEST_F( ShaderRootFixture, TheDefaultVariantLeavesTheKeyExactlyWhereItWas )
{
    // Every SPIR-V artifact already on disk was keyed before this axis existed. A default variant that
    // mixed anything at all would invalidate the whole cache — hundreds of cold compiles on the next
    // launch — for a substitution nobody made.
    const std::string source = "#version 450\nvoid main() {}\n";
    const auto        path   = ShaderPath( "Fog/HeightFog.shader" );

    EXPECT_EQ( Desert::Core::ShaderVariant{}.Hash(), 0u );
    EXPECT_EQ( ComputeShaderCacheKey( ShaderStage::Compute, source, path ),
               ComputeShaderCacheKey( ShaderStage::Compute, source, path, Desert::Core::ShaderVariant{} ) );
}

TEST_F( ShaderRootFixture, ASubstitutingVariantNeverHashesToTheDefaultsValue )
{
    // Zero means "no substitution" everywhere it is read. A variant that substituted something and
    // reported zero would be invisible to the key and to every diagnostic that prints it.
    const Desert::Core::ShaderVariant substituting{ { { "Generated/CloudMedium.glslh", "" } } };
    EXPECT_NE( substituting.Hash(), 0u );
    EXPECT_FALSE( substituting.IsDefault() );
}

TEST_F( ShaderRootFixture, TheClosureFollowsASubstitutedBodyRatherThanTheFileOnDisk )
{
    // A generated medium may include a header of its own. Walking the FILE instead would leave that
    // header out of the key and out of the hot-reload watch list, so editing it would change nothing
    // until a restart — the same staleness this file exists to prevent, arriving through the new door.
    const std::string source = "#version 450\n#include <Generated/CloudMedium.glslh>\nvoid main() {}\n";
    const auto        path   = ShaderPath( "Fog/HeightFog.shader" );

    const Desert::Core::ShaderVariant variant{
         { { "Generated/CloudMedium.glslh", "#include <Common/CameraUB.glslh>\n" } } };

    const auto includes = CollectShaderIncludes( source, path, variant );

    const auto contains = [&includes]( const char* name )
    {
        for ( const auto& include : includes )
            if ( include.filename() == name )
                return true;
        return false;
    };

    EXPECT_TRUE( contains( "CloudMedium.glslh" ) );
    EXPECT_TRUE( contains( "CameraUB.glslh" ) )
         << "the walk read the medium file on disk instead of the bytes actually compiled, so a header "
            "reachable only through the substitution is invisible to the key and to hot reload.";
}

TEST_F( ShaderRootFixture, SubstitutingTheShippedMediumMovesTheKeyOfTheRealCloudMarch )
{
    // Not a synthetic source: the compute stage of the program the camera actually marches, whose include
    // closure reaches Generated/CloudMedium.glslh through Common/CloudField.glslh and names it nowhere.
    const auto path   = ShaderPath( "Clouds/CloudRaymarch.shader" );
    const auto source = StageSource( path, ShaderStage::Compute );
    ASSERT_FALSE( source.empty() );

    const uint64_t shipped = ComputeShaderCacheKey( ShaderStage::Compute, source, path );

    // THE SUBSTITUTION KEEPS THE FILE'S OWN INCLUDE, and it has to for this test to test what it says.
    // A body that included nothing would drop Common/CloudMediumDefault.glslh out of the closure, and the
    // key would then move because a HEADER left the list — which is the assertion three tests above,
    // not this one. MEASURED: with the shorter body, deleting the variant from the key left this test
    // GREEN. With the closure held equal, the variant's hash is the only thing that can separate the two.
    const Desert::Core::ShaderVariant authored{
         { { "Generated/CloudMedium.glslh",
             "#include <Common/CloudMediumDefault.glslh>\n// an authored medium\n" } } };

    EXPECT_EQ( CollectShaderIncludes( source, path ).size(),
               CollectShaderIncludes( source, path, authored ).size() )
         << "the substituted body's include closure differs from the file's, so the assertion below "
            "would pass on the closure rather than on the variant";

    EXPECT_NE( shipped, ComputeShaderCacheKey( ShaderStage::Compute, source, path, authored ) );
}

// ---- The include closure ----------------------------------------------------------------------------

TEST_F( ShaderRootFixture, TheClosureOfTheFogPassListsEveryHeaderItNames )
{
    const auto path     = ShaderPath( "Fog/HeightFog.shader" );
    const auto source   = StageSource( path, ShaderStage::Compute );
    const auto includes = CollectShaderIncludes( source, path );

    const auto contains = [&includes]( const char* name )
    {
        for ( const auto& include : includes )
            if ( include.filename() == name )
                return true;
        return false;
    };

    EXPECT_TRUE( contains( "HeightFog.glslh" ) );
    EXPECT_TRUE( contains( "FogParams.glslh" ) );
    EXPECT_TRUE( contains( "SkyMedium.glslh" ) );
    EXPECT_TRUE( contains( "SkyScattering.glslh" ) );
}

TEST_F( ShaderRootFixture, TheClosureFollowsAHeaderThatIncludesAnother )
{
    // The TRANSITIVE step, which is what makes the walk worth having over a single grep of the stage
    // source: NewShaderGraph names Common/GraphVertex.glslh, and only GraphVertex names
    // Common/CameraUB.glslh. A key that stopped at depth one would not move when CameraUB was edited,
    // and the machine holding the stale SPIR-V would render differently from the one that had none.
    const auto path     = ShaderPath( "Graph/NewShaderGraph.shader" );
    const auto includes = CollectShaderIncludes( StageSource( path, ShaderStage::Vertex ), path );

    const auto contains = [&includes]( const char* name )
    {
        for ( const auto& include : includes )
            if ( include.filename() == name )
                return true;
        return false;
    };

    EXPECT_TRUE( contains( "GraphVertex.glslh" ) );
    EXPECT_TRUE( contains( "CameraUB.glslh" ) );
}

TEST_F( ShaderRootFixture, TheClosureListsEachFileOnce )
{
    const auto path     = ShaderPath( "Fog/HeightFog.shader" );
    const auto includes = CollectShaderIncludes( StageSource( path, ShaderStage::Compute ), path );

    std::vector<std::string> seen;
    for ( const auto& include : includes )
    {
        const std::string key = include.generic_string();
        EXPECT_EQ( std::find( seen.begin(), seen.end(), key ), seen.end() ) << "duplicate: " << key;
        seen.push_back( key );
    }
    EXPECT_FALSE( seen.empty() );
}

// ---- The descriptor count a pipeline layout and a bound set have to agree on ------------------------

TEST_F( ShaderRootFixture, TheFogEvaluationDeclaresFiveDescriptorsInSetZero )
{
    // The regression guard for the failure this test file exists for: a pipeline built before a shader
    // gained a binding and a set allocated after it cannot be bound together. Five is not a magic number
    // — it is counted from the shader's own text by the engine's own reflection, so it moves when the
    // shader does.
    const auto bindings = ComputeSetZero( ShaderPath( "Fog/HeightFog.shader" ) );

    EXPECT_EQ( ShaderReflection::CountDescriptors( bindings ), 5u );

    EXPECT_TRUE( HasBinding( bindings, 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ) );          // the fog it writes
    EXPECT_TRUE( HasBinding( bindings, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ) );         // fog params
    EXPECT_TRUE( HasBinding( bindings, 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) ); // scene depth
    EXPECT_TRUE( HasBinding( bindings, 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) ); // AP volume
    EXPECT_TRUE( HasBinding( bindings, 4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) ); // distant sky light
}

TEST_F( ShaderRootFixture, TheDistantSkyLightDeclaresFourDescriptorsInSetZero )
{
    // A second pass through the same reflection, and the one that would notice the sky's LUT chain
    // gaining or losing an input: the fill reads the cached transmittance and multi-scatter pair and
    // writes exactly one image.
    const auto bindings = ComputeSetZero( ShaderPath( "Sky/SkyDistantLight.shader" ) );

    EXPECT_EQ( ShaderReflection::CountDescriptors( bindings ), 4u );

    EXPECT_TRUE( HasBinding( bindings, 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ) );          // the texel it fills
    EXPECT_TRUE( HasBinding( bindings, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ) );         // sky params
    EXPECT_TRUE( HasBinding( bindings, 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) ); // transmittance LUT
    EXPECT_TRUE( HasBinding( bindings, 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) ); // multi-scatter LUT
}

TEST_F( ShaderRootFixture, TheCloudShadowMapDeclaresNineDescriptorsInSetZero )
{
    // THE PRODUCER OF THE CLOUD SHADOW MAP, and the reason it is pinned here rather than trusted: its
    // inputs are bound by NUMBER and not by reflection (ComputePipeline::SetInput takes the binding
    // as an argument), so the C++ constants in Engine/Graphic/Clouds/CloudShadowPayload.hpp and the
    // numbers in the shader are two statements of one fact. The assertions below are that fact, checked
    // against the compiled SPIR-V rather than against the file's text.
    //
    // The inputs are DELIBERATELY the march's own slot numbers — one vocabulary for one field — which is
    // why 3, 7, 8 and 9 are occupied and 1, 2, 4, 5, 6 are not.
    //
    // SIX SINCE SLOT A, and two of those are the ones a reader would not expect on a SHADOW pass: a hero
    // cloud shades the ground under it because it IS the cloud field, so the shadow march samples the
    // sculpted body through the same instance buffer and the same volume the view march does. Nothing was
    // added to the deferred pass to make that happen.
    //
    // NINE SINCE PHASE NV, and the three that arrived are noise volumes 1, 2 and 3. A layer carries four
    // cloud types and a type names its own volume; binding one of them was the programme's last recorded
    // debt and a dead setting in three slots of four. THE SHADOW PASS TAKES ALL FOUR TOO, and that is the
    // relation worth pinning here rather than the count: a cirrus eroded by the fine volume for the eye
    // and by the default one for the shadow map would be two different clouds in one frame.
    const auto bindings = ComputeSetZero( ShaderPath( "Clouds/CloudShadowMap.shader" ) );

    EXPECT_EQ( ShaderReflection::CountDescriptors( bindings ), 9u );

    for ( std::uint32_t slot = 0; slot < Desert::Graphic::kCloudSpeciesSlots; ++slot )
    {
        EXPECT_TRUE( HasBinding( bindings, Desert::Graphic::kCloudShadowNoiseBindings[slot],
                                 VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) )
             << "the shadow pass does not declare noise volume " << slot;
    }

    EXPECT_TRUE( HasBinding( bindings, Desert::Graphic::kCloudShadowOutputBinding,
                             VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ) ); // the triple it writes
    EXPECT_TRUE( HasBinding( bindings, Desert::Graphic::kCloudShadowParamsBinding,
                             VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ) ); // the cloud parameter block
    EXPECT_TRUE( HasBinding( bindings, Desert::Graphic::kCloudShadowNoiseBinding,
                             VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) ); // the noise volume
    EXPECT_TRUE( HasBinding( bindings, Desert::Graphic::kCloudShadowModellingBinding,
                             VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) ); // the procedural modelling volume
    EXPECT_TRUE( HasBinding( bindings, Desert::Graphic::kCloudShadowAuthoredBinding,
                             VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ) ); // the hero cloud instances
    EXPECT_TRUE( HasBinding( bindings, Desert::Graphic::kCloudShadowAuthoredAtlasBinding,
                             VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) ); // the sculpted body
}

TEST_F( ShaderRootFixture, TheCloudMarchDeclaresFifteenDescriptorsInSetZero )
{
    // THE VIEW MARCH, pinned on the same terms and for the same reason, and it was NOT pinned before slot
    // A landed — which is precisely why it is worth doing now: two of its ten descriptors are new, both
    // are bound by number, and one of them is a `sampler3D` that MUST be bound even when nothing reads
    // it. An unbound sampler is an invalid descriptor set, and this backend answers an invalid set by
    // silently skipping the dispatch: every cloud in the frame would vanish with nothing in the log.
    // That failure has happened in this subsystem before, which is what makes a count a useful assertion.
    //
    // THIRTEEN SINCE PHASE NV. The three that arrived are noise volumes 1, 2 and 3, and they are bound on
    // exactly the terms the note above describes: ALWAYS, even when the layer needs one volume, because
    // Graphic::ResolveCloudNoiseVolumes fills the unused slots with slot 0's image rather than leaving
    // them empty. They cost nothing in memory — Assets::AssetPreloader uploads every `.dcnv` in the
    // project whatever any scene names — and they are what makes a type's own volume reach the frame.
    //
    // FOURTEEN SINCE Р4. The one that arrived is the SKY-LIGHT OCCLUSION VOLUME at binding 13, and it is
    // bound on exactly the terms this note describes: ALWAYS, fallback included, even in the default scene
    // where the layer's flag is off and CloudPush::Frame.x tells the march not to read it.
    //
    // FIFTEEN SINCE Р14. The one that arrived is the ATMOSPHERE'S TRANSMITTANCE LUT at binding 14 — the
    // SKY's texture, read by the cloud march so that the sun's colour can be re-evaluated at each sample's
    // own altitude instead of once at sea level. Bound on the same terms again, and in the shipped scene
    // it is the FALLBACK that is bound, because the field it serves is off by default.
    const auto bindings = ComputeSetZero( ShaderPath( "Clouds/CloudRaymarch.shader" ) );

    EXPECT_EQ( ShaderReflection::CountDescriptors( bindings ), 15u );

    for ( std::uint32_t slot = 0; slot < Desert::Graphic::kCloudSpeciesSlots; ++slot )
    {
        EXPECT_TRUE( HasBinding( bindings, Desert::Graphic::kCloudNoiseBindings[slot],
                                 VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) )
             << "the march does not declare noise volume " << slot;
    }

    // AND THE TWO PASSES USE ONE VOCABULARY FOR ONE FIELD. Stated as an assertion rather than as an alias
    // in a header nobody re-reads: the shadow map binds by number too, and a march that read volume 2 from
    // descriptor 11 while the shadow pass read it from 12 would give a cloud a shadow cut from a different
    // noise, which is not an error anywhere — it is a shadow that does not fit its cloud.
    for ( std::uint32_t slot = 0; slot < Desert::Graphic::kCloudSpeciesSlots; ++slot )
    {
        EXPECT_EQ( Desert::Graphic::kCloudNoiseBindings[slot], Desert::Graphic::kCloudShadowNoiseBindings[slot] );
    }

    EXPECT_TRUE( HasBinding( bindings, Desert::Graphic::kCloudOutputBinding,
                             VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ) ); // the scatter target
    EXPECT_TRUE( HasBinding( bindings, Desert::Graphic::kCloudGuideOutputBinding,
                             VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ) ); // the depth guide beside it
    EXPECT_TRUE( HasBinding( bindings, Desert::Graphic::kCloudParamsBinding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ) );
    EXPECT_TRUE( HasBinding( bindings, Desert::Graphic::kCloudSceneDepthBinding,
                             VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) );
    EXPECT_TRUE(
         HasBinding( bindings, Desert::Graphic::kCloudNoiseBinding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) );
    EXPECT_TRUE( HasBinding( bindings, Desert::Graphic::kCloudDistantSkyLightBinding,
                             VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) );
    EXPECT_TRUE( HasBinding( bindings, Desert::Graphic::kCloudAerialPerspectiveBinding,
                             VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) );
    EXPECT_TRUE( HasBinding( bindings, Desert::Graphic::kCloudModellingBinding,
                             VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) );
    EXPECT_TRUE( HasBinding( bindings, Desert::Graphic::kCloudAuthoredBinding,
                             VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ) ); // the hero cloud instances
    EXPECT_TRUE( HasBinding( bindings, Desert::Graphic::kCloudAuthoredAtlasBinding,
                             VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) ); // the sculpted body
    EXPECT_TRUE( HasBinding( bindings, Desert::Graphic::kCloudSkyOcclusionBinding,
                             VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) ); // the sky-light occlusion volume
    EXPECT_TRUE( HasBinding( bindings, Desert::Graphic::kCloudSunTransmittanceLutBinding,
                             VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) ); // the atmosphere transmittance LUT

    // AND IT MUST NOT COLLIDE WITH ANYTHING ALREADY THERE. Stated as an assertion because the bindings are
    // handed to SetInput verbatim and a duplicate number does not fail anywhere: it lands one resource on
    // top of another, and what the march then samples is whichever of the two the renderer bound last.
    for ( const std::uint32_t taken :
          { Desert::Graphic::kCloudOutputBinding, Desert::Graphic::kCloudParamsBinding,
            Desert::Graphic::kCloudSceneDepthBinding, Desert::Graphic::kCloudNoiseBinding,
            Desert::Graphic::kCloudDistantSkyLightBinding, Desert::Graphic::kCloudAerialPerspectiveBinding,
            Desert::Graphic::kCloudGuideOutputBinding, Desert::Graphic::kCloudModellingBinding,
            Desert::Graphic::kCloudAuthoredBinding, Desert::Graphic::kCloudAuthoredAtlasBinding,
            Desert::Graphic::kCloudNoiseBindings[1], Desert::Graphic::kCloudNoiseBindings[2],
            Desert::Graphic::kCloudNoiseBindings[3], Desert::Graphic::kCloudSkyOcclusionBinding } )
    {
        EXPECT_NE( Desert::Graphic::kCloudSunTransmittanceLutBinding, taken );
    }
}

TEST_F( ShaderRootFixture, TheSkyOcclusionVolumesProducerAndConsumerAgreeAboutTheFieldTheyIntegrate )
{
    // THE RELATION THAT MAKES THE VOLUME MEAN ANYTHING. A column is integrated by one shader and read by
    // another, and what makes the number correct is that BOTH sampled the same field: the same four noise
    // volumes, the same modelling volume, the same hero-cloud atlas and instance list. A producer that
    // eroded its column from a different volume than the eye's would darken clouds the frame does not
    // contain — not an error anywhere, just a shaded side that does not fit its cloud, which is the exact
    // failure shape the shadow map's own binding assertions exist to catch.
    const auto producer = ComputeSetZero( ShaderPath( "Clouds/CloudSkyOcclusionVolume.shader" ) );

    EXPECT_TRUE( HasBinding( producer, Desert::Graphic::kCloudSkyOcclusionOutputBinding,
                             VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ) ); // the volume it writes
    EXPECT_TRUE( HasBinding( producer, Desert::Graphic::kCloudSkyOcclusionParamsBinding,
                             VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ) );
    EXPECT_TRUE( HasBinding( producer, Desert::Graphic::kCloudSkyOcclusionModellingBinding,
                             VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) );
    EXPECT_TRUE( HasBinding( producer, Desert::Graphic::kCloudSkyOcclusionAuthoredBinding,
                             VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ) );
    EXPECT_TRUE( HasBinding( producer, Desert::Graphic::kCloudSkyOcclusionAuthoredAtlasBinding,
                             VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) );

    for ( std::uint32_t slot = 0; slot < Desert::Graphic::kCloudSpeciesSlots; ++slot )
    {
        EXPECT_TRUE( HasBinding( producer, Desert::Graphic::kCloudSkyOcclusionNoiseBindings[slot],
                                 VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) )
             << "the sky-light occlusion producer does not declare noise volume " << slot;

        // ONE VOCABULARY FOR ONE FIELD, asserted rather than left to an alias nobody re-reads.
        EXPECT_EQ( Desert::Graphic::kCloudNoiseBindings[slot],
                   Desert::Graphic::kCloudSkyOcclusionNoiseBindings[slot] );
    }

    // AND IT READS THE SAME PARAMETER BLOCK, byte for byte. It is handed the very buffer the march is
    // handed — one upload, two dispatches — so a block of a different size here would be every field after
    // the divergence read from the wrong offset in a pass whose output is then trusted by the other.
    const uint32_t bytes = ComputeStorageBlockBytes( ShaderPath( "Clouds/CloudSkyOcclusionVolume.shader" ),
                                                     Desert::Graphic::kCloudParamsBinding );
    EXPECT_EQ( bytes, sizeof( Desert::Graphic::CloudGpuPayload ) );
}

TEST_F( ShaderRootFixture, TheCloudParameterBlockIsTheSameNumberOfBytesOnBothSidesOfTheWire )
{
    // THE RELATION THE static_asserts IN CloudPayload.hpp CANNOT REACH. They pin the C++ struct's offsets
    // against themselves, which catches half a move inside one file and nothing at all about the GLSL
    // block that reads those bytes — and the two are edited in different files, in different languages, by
    // whoever adds a parameter. A member added on one side only shifts every member after it, and the
    // symptom is not an error: it is a sky that looks badly tuned.
    //
    // IT IS THE SIZE AND NOT THE OFFSETS, and the difference is what spirv-cross gives for a STORAGE block
    // — VulkanShaderReflection fills member offsets for uniform buffers and only the declared size for
    // storage buffers, and this block is a storage buffer. The size is the weaker statement of the two and
    // it is the one available without changing the engine's reflection, which is another task's file. It
    // catches exactly the failure this phase could have introduced: SpeciesNoise added to one side only.
    //
    // BOTH PASSES, because both include Common/CloudParams.glslh and both are handed the SAME bytes by
    // VolumetricCloudRenderer — the shadow map's block and the march's block are one struct written twice.
    for ( const char* shader : { "Clouds/CloudRaymarch.shader", "Clouds/CloudShadowMap.shader" } )
    {
        const uint32_t bytes =
             ComputeStorageBlockBytes( ShaderPath( shader ), Desert::Graphic::kCloudParamsBinding );

        EXPECT_GT( bytes, 0u ) << shader << " declares no storage block at the cloud parameter binding";
        EXPECT_EQ( bytes, sizeof( Desert::Graphic::CloudGpuPayload ) )
             << shader << " reads a parameter block of " << bytes << " bytes where Graphic::CloudGpuPayload "
             << "is " << sizeof( Desert::Graphic::CloudGpuPayload )
             << " — every member after the divergence is read from the wrong offset";
    }
}

TEST_F( ShaderRootFixture, TheDeferredLightingPassDeclaresTwentyOneDescriptorsInSetZero )
{
    // THE CONSUMER, and the pass this repository shares most widely — every deferred scene draws it, and
    // it is the one file the cloud work was told to touch as little as possible. Pinning its descriptor
    // set is how "as little as possible" becomes checkable: the count moves the day somebody adds a
    // binding to it, whether or not they meant to.
    //
    // Twenty. Slots 0..16 with none free in between: six G-buffer and scene samplers (1, 2, 3, 8, 9, 10),
    // four cascade maps (5, 13, 14, 15), four uniform blocks (0, 4, 7, 12), two light SSBOs (6, 16) and
    // the cloud shadow map (11) — plus the three the ambient needs (17, 18, 19). Was seventeen until
    // 2026-09-03, when this pass stopped inventing its ambient out of a flat constant and started reading
    // the same baked environment the forward mesh shaders read. Twenty-one since 2026-09-24 (ENV1): the
    // sky's look (20, SkyLookUB) is applied where the environment cubes are sampled instead of being
    // baked into them, so the composite reads the rotation and gain the backdrop is drawn with.
    const auto bindings = FragmentSetZero( ShaderPath( "Deferred/DeferredLighting.shader" ) );

    EXPECT_EQ( ShaderReflection::CountDescriptors( bindings ), 21u );
    EXPECT_TRUE( HasBinding( bindings, 20, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ) ); // SkyLookUB

    EXPECT_TRUE( HasBinding( bindings, 11, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) ); // u_CloudShadowMap
    EXPECT_TRUE( HasBinding( bindings, 12, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ) );         // CloudShadowUB

    // THE AMBIENT'S OWN THREE, and the point of the whole change: a deferred composite that declares
    // none of these cannot be reading the sky, whatever its ambient line says it is doing. That was
    // exactly the state the owner saw as black ground under a bright sky.
    EXPECT_TRUE( HasBinding( bindings, 17, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) ); // u_EnvIrradianceTex
    EXPECT_TRUE( HasBinding( bindings, 18, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) ); // u_EnvSpecularTex
    EXPECT_TRUE( HasBinding( bindings, 19, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) ); // u_BRDFLUTTexture

    // And the cascade block it must NOT have disturbed: the same four maps at the same four slots, with
    // ShadowUB where PBR.glsl.frag mirrors it.
    EXPECT_TRUE( HasBinding( bindings, 5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) );
    EXPECT_TRUE( HasBinding( bindings, 13, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) );
    EXPECT_TRUE( HasBinding( bindings, 14, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) );
    EXPECT_TRUE( HasBinding( bindings, 15, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) );
    EXPECT_TRUE( HasBinding( bindings, 7, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ) );
}

TEST_F( ShaderRootFixture, TheGBufferShaderDeclaresOnlyWhatAGBufferWriteActuallyReads )
{
    // WHAT THIS REPLACED, because the change is the interesting part. Until the deferred pass got a
    // material of its own, this file asserted the OPPOSITE relation — that StaticMeshGBuffer's set 0 was
    // byte-identical to StaticMeshPBR's — and StaticMeshGBuffer.shader carried fourteen descriptors it
    // never read, each touched through a `keep` sum multiplied by 1e-20 so SPIR-V reflection would not
    // drop it. That was not decoration: MeshRenderer drew the pass with the (Static x Forward) material,
    // whose sets are allocated from the FORWARD shader's reflection, against a pipeline layout built from
    // THIS shader's, and Vulkan requires the two to be compatible.
    //
    // MaterialService now keys a runtime material by (asset x vertex path x PASS), so the G-buffer pass
    // binds sets allocated from its own shader. The old assertion would now be actively wrong — it would
    // demand that the pass go on declaring four cascade maps it cannot sample and two light SSBOs nothing
    // fills — so it is replaced rather than relaxed.
    //
    // THE RELATION NOW: a pass that shades nothing declares the SURFACE and nothing else. Stated as an
    // exact set, because "fewer than the forward shader" would still pass with one cascade map left
    // behind, and a lighting descriptor in a pass with no lighting is a slot the material has no data for.
    const auto gbuffer = GraphicsSetZero( ShaderPath( "PBR/StaticMeshGBuffer.shader" ) );
    ASSERT_FALSE( gbuffer.empty() );

    EXPECT_EQ( ShaderReflection::CountDescriptors( gbuffer ), 5u )
         << "StaticMeshGBuffer's set 0 is " << DescribeBindings( gbuffer )
         << " — a G-buffer write reads the camera, the material rows and the surface's three maps, and a "
            "sixth descriptor is either a lighting slot that came back or a surface input nobody fills";

    EXPECT_TRUE( HasBinding( gbuffer, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ) );          // CameraUB (vertex)
    EXPECT_TRUE( HasBinding( gbuffer, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ) );          // Materials[]
    EXPECT_TRUE( HasBinding( gbuffer, 11, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) ); // u_AlbedoTexture
    EXPECT_TRUE( HasBinding( gbuffer, 12, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) ); // u_NormalTexture
    EXPECT_TRUE( HasBinding( gbuffer, 18, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) ); // u_OpacityTexture

    // And the two the deferred composite owns, named individually because they are the ones a reader is
    // most likely to put back "so the G-buffer can shade the ambient". It cannot: it writes attributes and
    // Deferred/DeferredLighting.shader shades them, on its own material with its own layout.
    EXPECT_FALSE( HasBinding( gbuffer, 5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) ) // u_ShadowMap0
         << "the G-buffer pass declares a cascade map; it shades nothing and samples none";
    EXPECT_FALSE( HasBinding( gbuffer, 9, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) ) // u_EnvIrradianceTex
         << "the G-buffer pass declares the irradiance cube; the ambient lives in the deferred composite";

    // The forward shader is the control: it still declares everything a lit draw needs, so a G-buffer set
    // this small is the PASS shrinking and not the whole family losing its lighting.
    const auto forward = GraphicsSetZero( ShaderPath( "PBR/StaticMeshPBR.shader" ) );
    ASSERT_FALSE( forward.empty() );
    EXPECT_GT( ShaderReflection::CountDescriptors( forward ), ShaderReflection::CountDescriptors( gbuffer ) );
    EXPECT_TRUE( HasBinding( forward, 5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) );
    EXPECT_TRUE( HasBinding( forward, 9, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) );
}

TEST_F( ShaderRootFixture, TheBindingsComeOutSortedAndCountedTheWayTheLayerCounts )
{
    // The layer reports "N total descriptors", which is the SUM of descriptorCount, not the number of
    // bindings. They agree here only because every binding this engine builds has a count of one — the
    // day that stops being true, this is where it is noticed.
    const auto bindings = ComputeSetZero( ShaderPath( "Fog/HeightFog.shader" ) );

    ASSERT_FALSE( bindings.empty() );
    for ( size_t i = 1; i < bindings.size(); ++i )
        EXPECT_LT( bindings[i - 1].binding, bindings[i].binding );

    EXPECT_EQ( ShaderReflection::CountDescriptors( bindings ), bindings.size() );
}

TEST_F( ShaderRootFixture, TheEnvironmentBakeReadsTheSkyAndTheCloudsOnTwoDIFFERENTDESCRIPTORS )
{
    // THE MINE THIS TEST EXISTS FOR, and it is one line of arithmetic rather than a rendering failure.
    // Graphic::kSkyPayloadBinding is 1, and Common/CloudParams.glslh's CLOUD_PARAMS_BINDING defaults to 1
    // as well. The environment bake is the ONE pass in the engine that reads both blocks, so if the cloud
    // header's number were not overridable the two would land on one descriptor — and the symptom is not
    // an error but one of the blocks reading the other's bytes, which renders as a sky whose clouds are
    // shaped by the atmosphere's ozone density.
    //
    // Both numbers below come from the C++ constants, and the reflection reads the SHADER, so this
    // asserts the two statements of the layout against each other rather than either against a literal.
    ASSERT_NE( Desert::Graphic::kSkyBakeCloudParamsBinding, Desert::Graphic::kSkyPayloadBinding )
         << "the bake would bind the cloud block and the sky block to one descriptor";

    const auto bindings = ComputeSetZero( ShaderPath( "Compute/BakeProceduralSky.shader" ) );

    EXPECT_TRUE( HasBinding( bindings, Desert::Graphic::kSkyPayloadBinding,
                             VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ) ); // the sky payload
    EXPECT_TRUE( HasBinding( bindings, Desert::Graphic::kSkyBakeCloudParamsBinding,
                             VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ) ); // the cloud payload
}

TEST_F( ShaderRootFixture, TheEnvironmentBakeMarchesTheSameFIELDTheScreenMarchDoes )
{
    // THE RELATION, not the count. What makes the baked environment agree with the visible sky is that
    // both marches sample the SAME field: the same four noise volumes, the same modelling volume, the
    // same hero-cloud atlas and instance list, the same sky-light occlusion volume. A bake that erased
    // one of them would light the world with a sky nobody can see — no error anywhere, just an ambient
    // that does not fit the frame, which is the failure shape the shadow map's own binding assertions
    // exist to catch.
    //
    // The NUMBERS differ from the march's on purpose (the bake's set already holds the sky's own four
    // descriptors), so what is asserted is that every resource the march binds has a counterpart here and
    // that the counterpart is the constant C++ hands to SetInput.
    const auto bindings = ComputeSetZero( ShaderPath( "Compute/BakeProceduralSky.shader" ) );

    for ( std::uint32_t slot = 0; slot < Desert::Graphic::kCloudSpeciesSlots; ++slot )
    {
        EXPECT_TRUE( HasBinding( bindings, Desert::Graphic::kSkyBakeCloudNoiseBindings[slot],
                                 VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) )
             << "noise volume " << slot;
    }

    EXPECT_TRUE( HasBinding( bindings, Desert::Graphic::kSkyBakeCloudModellingBinding,
                             VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) );
    EXPECT_TRUE( HasBinding( bindings, Desert::Graphic::kSkyBakeCloudAuthoredAtlasBinding,
                             VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) );
    EXPECT_TRUE( HasBinding( bindings, Desert::Graphic::kSkyBakeCloudAuthoredBinding,
                             VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ) );
    EXPECT_TRUE( HasBinding( bindings, Desert::Graphic::kSkyBakeDistantSkyLightBinding,
                             VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) );
    EXPECT_TRUE( HasBinding( bindings, Desert::Graphic::kSkyBakeCloudSkyOcclusionBinding,
                             VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) );

    // EVERY DESCRIPTOR IS DISTINCT, which is the property the two blocks above nearly broke and which no
    // amount of per-binding checking states: the reflection would happily report one binding satisfying
    // two of the expectations above.
    EXPECT_EQ( ShaderReflection::CountDescriptors( bindings ), bindings.size() );
}

// ---- The shader graph's lit surface against the standard mesh shader --------------------------------

namespace
{
    // Is @p include one of the engine's SHARED SHADING TEXTS — the files whose whole purpose is that
    // every shader that shades a surface compiles the same one?
    //
    // DERIVED, not typed. Everything under Resources/Shaders/Mesh is such a text by construction: that
    // directory holds the ambient (AmbientIBL.glslh), the direct-light BRDF (DirectLighting.glslh), the
    // two punctual light models and the BRDF pieces they share, and nothing else has ever been put there.
    // Common/CloudShadowReceiver.glslh is named on top because it is the same kind of file and lives in
    // Common only under duress — its producer includes Common/CloudShadowMap.glslh and declares
    // u_CloudShadowMap as a writeonly image2D, so the receiver cannot sit beside it.
    //
    // A typed list would drift, and drifting is the whole failure mode: the next shared text somebody
    // extracts must reach the graph too, and nobody will remember to add it here.
    bool IsSharedShadingText( const std::filesystem::path& include )
    {
        if ( include.parent_path().filename() == "Mesh" )
            return true;
        return include.filename() == "CloudShadowReceiver.glslh";
    }

    std::vector<std::filesystem::path> FragmentIncludes( const std::filesystem::path& shaderFile )
    {
        return CollectShaderIncludes( StageSource( shaderFile, ShaderStage::Fragment ), shaderFile );
    }
} // namespace

TEST_F( ShaderRootFixture, ALitGraphSurfaceCompilesEverySharedShadingTextTheMeshShaderDoes )
{
    // THE RELATION Д16 exists to assert, and it is deliberately not "the graph calls AmbientIBL".
    //
    // A shader-graph material is a surface an artist ticked "Lit" on. Until this test, ticking it produced
    // a lighting model the graph COMPILER wrote into the generated file: a flat `vec3( 0.12 )` ambient
    // that read no environment, a Lambert term that did not divide albedo by PI, and no cloud shadow at
    // all — three defects the engine had already fixed, in Р16, Р20 and Р21 respectively, by making each
    // term ONE text. The graph was a fourth copy that nobody had migrated, so all three lived on in it.
    //
    // What is asserted is therefore membership, not behaviour: every shared shading text the standard mesh
    // shader compiles, the lit graph surface compiles as well. That fails the day somebody extracts a new
    // one into Mesh/ and wires it into StaticMeshPBR only — which is exactly how the graph fell behind the
    // first time.
    const auto mesh  = FragmentIncludes( ShaderPath( "PBR/StaticMeshPBR.shader" ) );
    const auto graph = FragmentIncludes( ShaderPath( "Graph/MatLitConst.shader" ) );

    ASSERT_FALSE( mesh.empty() );
    ASSERT_FALSE( graph.empty() );

    const auto compiles = [&graph]( const std::filesystem::path& header )
    {
        for ( const auto& include : graph )
            if ( include.filename() == header.filename() )
                return true;
        return false;
    };

    int shared = 0;
    for ( const auto& include : mesh )
    {
        if ( !IsSharedShadingText( include ) )
            continue;
        ++shared;
        EXPECT_TRUE( compiles( include ) )
             << "StaticMeshPBR shades with " << include.filename().string()
             << " and the lit shader-graph surface does not — a graph material is lit by a model of its "
                "own again";
    }

    // Vacuous success is the failure mode of every membership test: a closure that came back empty, or a
    // Mesh/ that stopped being where the shared texts live, would pass the loop above without checking
    // anything. Seven is what StaticMeshPBR's closure names today — six after Д16, plus
    // Mesh/CascadedShadow.glslh, which is exactly the automatic membership this test was written to give:
    // Д20 extracted ShadowFactor into Mesh/ and the graph was REQUIRED to compile it without a line of
    // this test changing. The assertion is a floor rather than an equality so that adding the next shared
    // text is not, by itself, a broken test.
    EXPECT_GE( shared, 7 ) << "the mesh shader's closure no longer names the shared shading texts";
}

TEST_F( ShaderRootFixture, TheLitGraphSurfaceIsHANDEDTheSceneITSHADESWITH )
{
    // The other half of the same relation: compiling the shared texts is worth nothing if the descriptors
    // they read are not in the set. Each of these is a resource the graph's generated shader could not
    // have received before — MeshRenderer's generic path filled CameraUB, TimeUB and DirectionLightsUB by
    // hand and nothing else, so an environment cube or a cloud shadow map had no route to a data-driven
    // material at all.
    //
    // Numbers, not names, because that is what a descriptor set is; they are the SAME numbers the four
    // mesh shaders use for the same things, which is a property worth keeping even though every material
    // in this engine binds by name.
    const auto bindings = GraphicsSetZero( ShaderPath( "Graph/MatLitConst.shader" ) );
    ASSERT_FALSE( bindings.empty() );

    EXPECT_TRUE( HasBinding( bindings, 8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) );  // u_EnvSpecularTex
    EXPECT_TRUE( HasBinding( bindings, 9, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) );  // u_EnvIrradianceTex
    EXPECT_TRUE( HasBinding( bindings, 10, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) ); // u_BRDFLUTTexture
    EXPECT_TRUE( HasBinding( bindings, 20, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) ); // u_CloudShadowMap
    EXPECT_TRUE( HasBinding( bindings, 21, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ) );         // CloudShadowUB
    EXPECT_TRUE( HasBinding( bindings, 4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ) );          // LightsMetadata
    EXPECT_TRUE( HasBinding( bindings, 6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ) );          // PointLightsUB
    EXPECT_TRUE( HasBinding( bindings, 16, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ) );         // SpotLightsUB
    EXPECT_TRUE( HasBinding( bindings, 14, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ) );         // DirectionLightsUB

    // The five cascade bindings Д20 added. Two of them are NOT at the mesh shaders' numbers and cannot be:
    // 14 and 15 hold DirectionLightsUB and TimeUB in a shader-graph layout, which no mesh shader declares.
    // The NAMES are what the engine binds by, and Tests/Engine/PBRSceneFrame asserts those against the C++
    // writer for every consumer of the shared text; what is pinned here is that the numbers this layout
    // chose are the ones it still has.
    EXPECT_TRUE( HasBinding( bindings, 7, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ) );          // ShadowUB
    EXPECT_TRUE( HasBinding( bindings, 5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) );  // u_ShadowMap0
    EXPECT_TRUE( HasBinding( bindings, 13, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) ); // u_ShadowMap1
    EXPECT_TRUE( HasBinding( bindings, 22, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) ); // u_ShadowMap2
    EXPECT_TRUE( HasBinding( bindings, 23, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) ); // u_ShadowMap3

    // The graph's own textures start at kGraphTextureBinding (24) and count upward, so none of the slots
    // above can be taken by a Properties block however many textures an artist adds. Distinctness is the
    // property that says so, and it is the one a per-slot check cannot state.
    EXPECT_EQ( ShaderReflection::CountDescriptors( bindings ), bindings.size() );
}

// ---- The material row: the C++ packer against the GLSL the compiler actually produced -------------------
//
// Every material in the engine delivers its parameters as a row of a shared `Materials[]` storage buffer,
// and Graphic::DataDrivenMaterial writes that row as a plain `std::vector<glm::vec4>` — parameter i at
// 16*i, no std430 arithmetic anywhere in the C++. That is only true because DShaderParser pads every
// parameter out to a whole 16-byte slot, and "because the generator pads it" is a claim about GLSL layout
// rules made by someone who is not the GLSL compiler.
//
// So it is asked of the compiler. This walks every shipped shader that carries a generated block, compiles
// its fragment stage for real and reads the member offsets out of the SPIR-V. A packer and a shader that
// disagree about one offset is the exact defect shape this repository keeps paying for — each side
// individually correct, nothing checking they agree — and it would surface as a material whose third
// parameter is somebody else's second.
namespace
{
    // Shipped shaders whose fragment stage declares the DSL-generated material row, found by parsing
    // rather than by a list: a list is a second census, and the point of the transport being one is that
    // no census has to be maintained.
    std::vector<std::filesystem::path> ShadersWithAGeneratedMaterialRow()
    {
        std::vector<std::filesystem::path> out;
        const auto                         root = std::filesystem::path( "Resources/Shaders/Programs" );
        if ( !std::filesystem::exists( root ) )
            return out;

        for ( const auto& entry : std::filesystem::recursive_directory_iterator( root ) )
        {
            if ( !entry.is_regular_file() || entry.path().extension() != ".shader" )
                continue;
            auto parsed = Desert::Core::Preprocess::DShaderParser::Parse( ReadFile( entry.path() ) );
            if ( !parsed.IsSuccess() )
                continue;
            const auto it = parsed.GetValue().Stages.find( ShaderStage::Fragment );
            if ( it == parsed.GetValue().Stages.end() )
                continue;
            if ( it->second.find( "#define u_Material u_Materials[" ) != std::string::npos )
                out.push_back( entry.path() );
        }
        std::sort( out.begin(), out.end() );
        return out;
    }
} // namespace

TEST_F( ShaderRootFixture, AMaterialParameterSitsAtSixteenTimesItsIndexInTheCompiledSpirv )
{
    const auto shaders = ShadersWithAGeneratedMaterialRow();
    ASSERT_FALSE( shaders.empty() ) << "no shipped shader carries a generated material row — either the "
                                       "shader root was not found or the transport changed spelling";

    for ( const auto& shaderFile : shaders )
    {
        const auto spirv =
             CompileStage( StageSource( shaderFile, ShaderStage::Fragment ), shaderFile, shaderc_fragment_shader );
        ASSERT_FALSE( spirv.empty() ) << shaderFile.string();

        spirv_cross::CompilerGLSL          compiler( spirv );
        const spirv_cross::ShaderResources resources = compiler.get_shader_resources();

        const spirv_cross::Resource* block = nullptr;
        for ( const auto& buffer : resources.storage_buffers )
        {
            if ( compiler.get_name( buffer.base_type_id ) == Desert::Core::Formats::kMaterialRowBlockName )
                block = &buffer;
        }
        ASSERT_NE( block, nullptr ) << shaderFile.string() << ": no storage block named "
                                    << Desert::Core::Formats::kMaterialRowBlockName;

        // The block holds one member — the runtime array of rows — and the row struct is its element type.
        const spirv_cross::SPIRType& blockType = compiler.get_type( block->base_type_id );
        ASSERT_EQ( blockType.member_types.size(), 1u ) << shaderFile.string();
        const spirv_cross::SPIRType& rowType = compiler.get_type( blockType.member_types[0] );

        const uint32_t stride = compiler.type_struct_member_array_stride( blockType, 0 );
        EXPECT_EQ( stride % Desert::Core::Formats::kMaterialParamSlotSize, 0u )
             << shaderFile.string() << ": a row is " << stride
             << " bytes, which is not a whole number of parameter slots";

        // The DECLARED members alternate value, padding, value, padding... so the value of parameter i is
        // whichever member sits at 16*i. Checking that each 16-byte boundary is the START of a member is
        // the property the packer needs; checking member k's own index is not, because the padding
        // members shift it.
        std::set<uint32_t> memberOffsets;
        for ( uint32_t m = 0; m < rowType.member_types.size(); ++m )
            memberOffsets.insert( compiler.type_struct_member_offset( rowType, m ) );

        const uint32_t slots = stride / Desert::Core::Formats::kMaterialParamSlotSize;
        for ( uint32_t slot = 0; slot < slots; ++slot )
        {
            const uint32_t offset = slot * Desert::Core::Formats::kMaterialParamSlotSize;
            EXPECT_TRUE( memberOffsets.count( offset ) != 0 )
                 << shaderFile.string() << ": nothing starts at byte " << offset << ", so parameter " << slot
                 << " is not where DataDrivenMaterial writes it";
        }

        // And the reverse, which is the half that catches a MISSING pad rather than a wrong one: no member
        // may start anywhere except on a slot boundary or inside its own parameter's padding. Expressed as
        // a count, because a row of N slots has exactly N parameter members and N boundaries.
        EXPECT_EQ( memberOffsets.count( 0u ), 1u ) << shaderFile.string();
        EXPECT_GE( memberOffsets.size(), slots ) << shaderFile.string();
    }
}

// The test above cannot fail for a row that SHRANK, and the shipped set cannot show you why.
//
// It derives `slots` from the compiled stride, so a packer that emits no padding at all produces a
// shorter row, fewer slots, and a loop that checks fewer boundaries — both sides of the comparison
// move together and the assertion passes. Measured, not supposed: with the padding helper forced to
// zero, every shipped shader still reported the same stride it reports now (MatProbe 48, Terrain and
// TextSDF 32, Unlit 16) and only lost a member, because each of them declares at most ONE
// Float/int/bool parameter and GLSL's own alignment re-establishes the boundaries by itself. The
// padding is what separates two SMALL parameters, and no shipped shader has two in a row — so the
// case the padding exists for was the one case nothing compiled.
//
// This test supplies it. The source is authored here rather than added to Resources/ because a
// shader asset nothing renders is a fixture pretending to be content, and because the expected slot
// count then comes from the DECLARATION (three parameters were written, so three slots) instead of
// from the SPIR-V being examined. That is the whole point: with the count fixed by construction, a
// row that shrinks fails on the count before it ever reaches the boundary loop.
TEST_F( ShaderRootFixture, TwoSmallParametersInARowStillLandOnTheirOwnSlots )
{
    // Three parameters, and the first two are the adjacent small pair the shipped set never forms.
    const std::string source = R"(Shader "RowPackingProbe"
{
    Domain Surface

    Properties Binding(1)
    {
        Float  First  ("First")  = 1.0
        Float  Second ("Second") = 2.0
        Color  Third  ("Third")  = (0.1, 0.2, 0.3, 1.0)
    }

    State { Cull Back ZTest LEqual ZWrite On }

    Vertex
    {
        In(0) vec3 a_Position;
        #include <Common/CameraUB.glslh>
        #include <Common/MaterialTransport.glslh>
        void main()
        {
            gl_Position = cameraUB.Projection * cameraUB.View * m_PushConstants.Transform
                        * vec4( a_Position, 1.0 );
        }
    }

    Fragment
    {
        Out(0) vec4 o_Color;
        void main()
        {
            o_Color = u_Material.Third * ( u_Material.First + u_Material.Second );
        }
    }
})";

    constexpr uint32_t kDeclaredParameters = 3; // written above, not read back from the shader

    auto parsed = Desert::Core::Preprocess::DShaderParser::Parse( source );
    ASSERT_TRUE( parsed.IsSuccess() ) << "the probe shader no longer parses: " << parsed.GetError();
    const auto stageIt = parsed.GetValue().Stages.find( ShaderStage::Fragment );
    ASSERT_NE( stageIt, parsed.GetValue().Stages.end() );

    // Any real path works — it only resolves `#include <...>` and names the source in diagnostics.
    const auto spirv =
         CompileStage( stageIt->second, ShaderPath( "Unlit/Unlit.shader" ), shaderc_fragment_shader );
    ASSERT_FALSE( spirv.empty() );

    spirv_cross::CompilerGLSL          compiler( spirv );
    const spirv_cross::ShaderResources resources = compiler.get_shader_resources();

    const spirv_cross::Resource* block = nullptr;
    for ( const auto& buffer : resources.storage_buffers )
    {
        if ( compiler.get_name( buffer.base_type_id ) == Desert::Core::Formats::kMaterialRowBlockName )
            block = &buffer;
    }
    ASSERT_NE( block, nullptr ) << "the probe declares parameters but got no material row";

    const spirv_cross::SPIRType& blockType = compiler.get_type( block->base_type_id );
    ASSERT_EQ( blockType.member_types.size(), 1u );
    const spirv_cross::SPIRType& rowType = compiler.get_type( blockType.member_types[0] );
    const uint32_t               stride  = compiler.type_struct_member_array_stride( blockType, 0 );

    // The count is the assertion the shipped-set test is missing: three declared parameters occupy
    // three whole slots, whatever the compiler would have done with them unpadded.
    EXPECT_EQ( stride, kDeclaredParameters * Desert::Core::Formats::kMaterialParamSlotSize )
         << "a row of " << kDeclaredParameters << " parameters is " << stride
         << " bytes; two adjacent small parameters have been packed into one slot";

    std::set<uint32_t> memberOffsets;
    for ( uint32_t m = 0; m < rowType.member_types.size(); ++m )
        memberOffsets.insert( compiler.type_struct_member_offset( rowType, m ) );

    for ( uint32_t slot = 0; slot < kDeclaredParameters; ++slot )
    {
        const uint32_t offset = slot * Desert::Core::Formats::kMaterialParamSlotSize;
        EXPECT_TRUE( memberOffsets.count( offset ) != 0 )
             << "nothing starts at byte " << offset << ", so parameter " << slot
             << " is not where DataDrivenMaterial writes it";
    }
}

TEST_F( ShaderRootFixture, AnUnlitGraphSurfaceReceivesNoneOfIt )
{
    // The boundary is real and not a matter of degree: an unlit graph shader is a DIFFERENT domain of
    // shading, it declares no lighting resource at all, and the change that gave the lit branch the
    // engine's model must not have quietly given the unlit branch a lighting descriptor it will never
    // read. (The Metallic/Roughness/Occlusion pins are likewise not evaluated for an unlit graph — the
    // nodes behind them would otherwise be emitted into a shader that discards the result.)
    const auto includes = FragmentIncludes( ShaderPath( "Graph/MatConst.shader" ) );
    for ( const auto& include : includes )
        EXPECT_FALSE( IsSharedShadingText( include ) )
             << "an unlit graph surface compiles " << include.filename().string();

    const auto bindings = GraphicsSetZero( ShaderPath( "Graph/MatConst.shader" ) );
    EXPECT_FALSE( HasBinding( bindings, 20, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) );
    EXPECT_FALSE( HasBinding( bindings, 9, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) );
}

// ---- The terrain's per-draw data rides beside the draw, not in the shared block --------------------

TEST_F( ShaderRootFixture, TheTerrainKeepsPerDrawDataOutOfItsSharedUniformBlock )
{
    // The terrain pass records EVERY draw of a frame before the GPU executes any of them, and a
    // material's descriptors are written at most once per frame — so anything per-terrain that lives
    // in the shared TerrainUB is read by the GPU as the LAST terrain's values, for every terrain. That
    // was live: on a two-terrain scene both drew with one Model/size/seed. The fix is the same
    // transport the parameters already use (MaterialParamRow.hpp): per-draw data is a row of
    // `TerrainInstances[]`, named by the push-constant index. This test pins the SPLIT — the relation
    // between what stays shared and what rides per draw — in the compiled SPIR-V of both stages,
    // exactly as VulkanShader merges them into one descriptor set layout.
    const auto path = ShaderPath( "Terrain/Terrain.shader" );

    struct StageToCompile
    {
        ShaderStage         Stage;
        shaderc_shader_kind Kind;
    };
    const StageToCompile stages[] = {
         { ShaderStage::Vertex, shaderc_vertex_shader },
         { ShaderStage::Fragment, shaderc_fragment_shader },
    };

    ShaderResource::ReflectionData data;
    std::vector<uint32_t>          vertexSpirv; // kept for the stride check below
    for ( const auto& [stage, kind] : stages )
    {
        const auto spirv = CompileStage( StageSource( path, stage ), path, kind );
        ASSERT_FALSE( spirv.empty() ) << "stage " << static_cast<int>( stage ) << " did not compile";
        if ( stage == ShaderStage::Vertex )
            vertexSpirv = spirv;
        const auto diagnostics = ShaderReflection::ReflectStage( spirv, stage, data );
        EXPECT_TRUE( diagnostics.empty() ) << ( diagnostics.empty() ? "" : diagnostics.front() );
    }

    const auto setIt = data.ShaderDescriptorSets.find( 0 );
    ASSERT_NE( setIt, data.ShaderDescriptorSets.end() );
    const auto& set = setIt->second;

    // The shared half: TerrainUB holds ONLY what every terrain of a frame agrees on — View +
    // Projection + SunDir + SunColor, 2*64 + 2*16 = 160 bytes. This is a size relation, not a spot
    // value: putting any per-terrain field back (Model was the first to be forgotten here) grows the
    // block past 160 and fails this line before it fails on screen.
    const auto ub = set.UniformBuffers.find( 0 );
    ASSERT_NE( ub, set.UniformBuffers.end() ) << "TerrainUB left binding 0";
    EXPECT_EQ( ub->second.Size, 160u )
         << "TerrainUB is no longer just the shared frame data - a per-draw field moved back in";

    // The per-draw half: TerrainInstances[] at binding 8, one struct per recorded draw.
    const auto instances = set.StorageBuffers.find( 8 );
    ASSERT_NE( instances, set.StorageBuffers.end() ) << "the TerrainInstances row buffer is gone";
    EXPECT_EQ( instances->second.Name, "TerrainInstances" );

    // ...and its stride is the C++ writer's stride. The reflected block Size of a runtime array is 0
    // by definition, so the stride is read straight from the SPIR-V type — the same number
    // TerrainBatch.hpp static_asserts as sizeof(TerrainInstance).
    {
        spirv_cross::Compiler compiler( vertexSpirv );
        const auto            resources = compiler.get_shader_resources();
        bool                  found     = false;
        for ( const auto& resource : resources.storage_buffers )
        {
            if ( compiler.get_decoration( resource.id, spv::DecorationBinding ) != 8 )
                continue;
            const auto& block = compiler.get_type( resource.base_type_id );
            ASSERT_FALSE( block.member_types.empty() );
            const uint32_t stride = compiler.type_struct_member_array_stride( block, 0 );
            EXPECT_EQ( stride, 112u ) << "the GLSL TerrainInstance and the C++ TerrainInstance disagree";
            found = true;
        }
        EXPECT_TRUE( found ) << "the vertex stage no longer reads TerrainInstances";
    }

    // The census, so a binding added or lost anywhere in the two stages is named here first.
    const auto bindings = ShaderReflection::BuildLayoutBindings( set );
    EXPECT_EQ( bindings.size(), 9u ) << DescribeBindings( bindings );
    EXPECT_TRUE( HasBinding( bindings, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ) );         // TerrainUB (shared)
    EXPECT_TRUE( HasBinding( bindings, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ) );         // Materials[] rows
    EXPECT_TRUE( HasBinding( bindings, 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) ); // u_GrassTex
    EXPECT_TRUE( HasBinding( bindings, 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) ); // u_RockTex
    EXPECT_TRUE( HasBinding( bindings, 4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) ); // u_SnowTex
    EXPECT_TRUE( HasBinding( bindings, 6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) ); // u_CloudShadowMap
    EXPECT_TRUE( HasBinding( bindings, 7, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ) );         // CloudShadowUB
    EXPECT_TRUE( HasBinding( bindings, 8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ) );         // TerrainInstances[]
    EXPECT_TRUE( HasBinding( bindings, 9, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) ); // u_Heightmap (R16)
}

// ---- The terrain's three programs (LS-5): one patch, three things written ---------------------------------
//
// Terrain.shader (forward), TerrainGBuffer.shader (deferred) and TerrainShadow.shader (cascade depth) share
// their vertex stage through Programs/Terrain/TerrainVertex.glslh. What must hold between them, and would not be seen on
// screen until it had already been wrong for a while:
//   - the G-buffer program's Properties are the forward program's, param for param: a terrain's .demat names
//     `Terrain`, and the renderer writes the SAME param row into whichever program the render path uses —
//     a block that drifted would read Tint where DetailTiling was written, in Deferred only;
//   - the shadow program binds only what a caster reads — TerrainInstances[] (whose row carries the main
//     camera's LOD) and the heightmap — and no Materials[] row or TerrainUB it is never given.
TEST_F( ShaderRootFixture, TheTerrainProgramsShareOneMaterialAndTheCasterReadsOnlyThePatch )
{
    const auto parse = [&]( const char* file )
    { return Desert::Core::Preprocess::DShaderParser::Parse( ReadFile( ShaderPath( file ) ) ); };
    const auto forward = parse( "Terrain/Terrain.shader" );
    const auto gbuffer = parse( "Terrain/TerrainGBuffer.shader" );
    const auto shadow  = parse( "Terrain/TerrainShadow.shader" );
    ASSERT_TRUE( forward.IsSuccess() && gbuffer.IsSuccess() && shadow.IsSuccess() );

    const auto& fp = forward.GetValue().Meta.Params;
    const auto& gp = gbuffer.GetValue().Meta.Params;
    ASSERT_EQ( fp.size(), gp.size() ) << "TerrainGBuffer's Properties block is no longer Terrain's";
    ASSERT_FALSE( fp.empty() );
    for ( size_t i = 0; i < fp.size(); ++i )
    {
        EXPECT_EQ( fp[i].Name, gp[i].Name ) << i;
        EXPECT_EQ( fp[i].DisplayName, gp[i].DisplayName ) << i;
        EXPECT_EQ( fp[i].Type, gp[i].Type ) << fp[i].Name;
        EXPECT_EQ( fp[i].IsTexture, gp[i].IsTexture ) << fp[i].Name;
        EXPECT_EQ( fp[i].Default, gp[i].Default ) << fp[i].Name;
    }

    // Only the forward program is a user's choice; the other two are the renderer's.
    EXPECT_EQ( forward.GetValue().Meta.Domain, Desert::Core::Formats::ShaderDomain::Terrain );
    EXPECT_NE( gbuffer.GetValue().Meta.Domain, Desert::Core::Formats::ShaderDomain::Terrain );
    EXPECT_NE( shadow.GetValue().Meta.Domain, Desert::Core::Formats::ShaderDomain::Terrain );

    struct StageToCompileTerrain
    {
        ShaderStage         Stage;
        shaderc_shader_kind Kind;
    };
    const StageToCompileTerrain stages[] = {
         { ShaderStage::Vertex, shaderc_vertex_shader },
         { ShaderStage::Fragment, shaderc_fragment_shader },
    };
    const auto reflect = [&]( const char* file )
    {
        const auto                     path = ShaderPath( file );
        ShaderResource::ReflectionData data;
        for ( const auto& [stage, kind] : stages )
        {
            const auto spirv = CompileStage( StageSource( path, stage ), path, kind );
            EXPECT_FALSE( spirv.empty() ) << file << " stage " << static_cast<int>( stage ) << " did not compile";
            if ( !spirv.empty() )
                ShaderReflection::ReflectStage( spirv, stage, data );
        }
        const auto set = data.ShaderDescriptorSets.find( 0 );
        return set == data.ShaderDescriptorSets.end() ? std::vector<VkDescriptorSetLayoutBinding>{}
                                                      : ShaderReflection::BuildLayoutBindings( set->second );
    };

    const auto g = reflect( "Terrain/TerrainGBuffer.shader" );
    EXPECT_TRUE( HasBinding( g, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ) ) << DescribeBindings( g ); // Materials[]
    EXPECT_TRUE( HasBinding( g, 8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ) ) << DescribeBindings( g );
    EXPECT_TRUE( HasBinding( g, 9, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) ) << DescribeBindings( g );

    const auto c = reflect( "Terrain/TerrainShadow.shader" );
    EXPECT_EQ( c.size(), 2u ) << DescribeBindings( c );
    EXPECT_TRUE( HasBinding( c, 8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ) );         // TerrainInstances[]
    EXPECT_TRUE( HasBinding( c, 9, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) ); // u_Heightmap
}

// ---- The particle state: one layout, three statements of it, and the dispatch that divides by a fourth --
//
// The particle subsystem carried the defect shape this file exists for, in its purest form. Two shaders
// declared `struct Particle` INDEPENDENTLY — the simulation that writes the storage and the billboard draw
// that reads it — and ParticleRenderer sized and zeroed that storage from a C++ constant whose entire
// guard was the comment `must match the shader's Particle`. Д26 then gave Age.yzw a meaning in one of the
// two declarations, so the copies had already begun to diverge in MEANING while still agreeing in SIZE.
//
// Г4 removed the mirror where it could: the struct now lives in Common/ParticleState.glslh and both stages
// include it. The C++ stride cannot be folded into GLSL, so it is asserted here instead — against the
// array stride the COMPILER produced for each stage, which is the number the GPU actually indexes by.
//
// The same treatment for the workgroup: LocalSize is declared in the shader, the group count is computed
// in C++, and a disagreement leaves the tail of every emitter unsimulated with nothing in any log.

namespace
{
    // The ARRAY STRIDE of the runtime array inside a storage block, read from the compiled SPIR-V.
    //
    // Not ShaderReflection's block Size, for the reason the terrain test above gives: a block whose only
    // member is a runtime array reflects as size 0 by definition. The stride is the number that matters
    // anyway — it is what `u_Particles[i]` multiplies i by.
    uint32_t StorageArrayStride( const std::filesystem::path& shaderFile, ShaderStage stage,
                                 shaderc_shader_kind kind, uint32_t binding )
    {
        const auto spirv = CompileStage( StageSource( shaderFile, stage ), shaderFile, kind );
        if ( spirv.empty() )
            return 0u;

        spirv_cross::Compiler              compiler( spirv );
        const spirv_cross::ShaderResources resources = compiler.get_shader_resources();
        for ( const auto& resource : resources.storage_buffers )
        {
            if ( compiler.get_decoration( resource.id, spv::DecorationBinding ) != binding )
                continue;
            const spirv_cross::SPIRType& block = compiler.get_type( resource.base_type_id );
            if ( block.member_types.empty() )
                return 0u;
            return compiler.type_struct_member_array_stride( block, 0 );
        }
        return 0u;
    }

    // The declared workgroup size of a compute stage, as the compiled module states it. Zeroes when the
    // stage did not compile, which every caller treats as a failure rather than as a size.
    std::array<uint32_t, 3> ComputeLocalSize( const std::filesystem::path& shaderFile )
    {
        const auto spirv =
             CompileStage( StageSource( shaderFile, ShaderStage::Compute ), shaderFile, shaderc_compute_shader );
        if ( spirv.empty() )
            return { 0u, 0u, 0u };

        spirv_cross::Compiler compiler( spirv );
        return { compiler.get_execution_mode_argument( spv::ExecutionModeLocalSize, 0 ),
                 compiler.get_execution_mode_argument( spv::ExecutionModeLocalSize, 1 ),
                 compiler.get_execution_mode_argument( spv::ExecutionModeLocalSize, 2 ) };
    }

    bool ClosureNames( const std::vector<std::filesystem::path>& includes, const char* filename )
    {
        for ( const auto& include : includes )
            if ( include.filename() == filename )
                return true;
        return false;
    }
} // namespace

TEST_F( ShaderRootFixture, BothParticleStagesIndexTheStateByTheStrideTheEngineAllocates )
{
    // THE RELATION, stated against BOTH readers of the storage because the engine writes only one of the
    // two numbers it needs to be right about. ParticleRenderer allocates MaxParticles * kParticleStride
    // and zeroes exactly that many bytes; the simulation writes element i at i * (its own stride) and the
    // draw reads element i at i * (its own stride). Any one of the three moving alone is a frame in which
    // particles are read from bytes nobody wrote — and the shipped symptom of that is an emitter that
    // looks mistuned, not an error.
    const uint32_t simulate  = StorageArrayStride( ShaderPath( "Particles/ParticleSimulate.shader" ),
                                                   ShaderStage::Compute, shaderc_compute_shader, 0 );
    const uint32_t billboard = StorageArrayStride( ShaderPath( "Particles/ParticleBillboard.shader" ),
                                                   ShaderStage::Vertex, shaderc_vertex_shader, 1 );

    EXPECT_GT( simulate, 0u ) << "ParticleSimulate declares no storage block at slot 0";
    EXPECT_GT( billboard, 0u ) << "ParticleBillboard declares no storage block at slot 1";

    EXPECT_EQ( simulate, Desert::Graphic::System::kParticleStride )
         << "the simulation indexes the state by " << simulate << " bytes and ParticleRenderer allocates "
         << Desert::Graphic::System::kParticleStride << " per particle";
    EXPECT_EQ( billboard, Desert::Graphic::System::kParticleStride )
         << "the billboard draw indexes the state by " << billboard << " bytes and ParticleRenderer allocates "
         << Desert::Graphic::System::kParticleStride << " per particle";

    // ...and therefore each other. Stated separately, because the two equalities above would both hold in
    // a world where the constant was edited to follow ONE of the shaders and this line is what names the
    // pair that actually shares the bytes.
    EXPECT_EQ( simulate, billboard ) << "the two particle stages read one storage with two layouts";
}

TEST_F( ShaderRootFixture, TheParticleStructIsCompiledFromOneTextByBothStages )
{
    // WHY THE EQUALITY ABOVE IS NOT ENOUGH. Two independent declarations that happen to be the same size
    // pass it — which was exactly the state Д26 left behind, one copy having gained a meaning for Age.yzw
    // that the other did not know about. Sizes are what a stride can see; MEANING is not, and the only
    // way to assert it is that there is one text.
    //
    // Membership rather than absence of the word `struct`, because what must hold is that both stages
    // COMPILE the shared header — a second declaration alongside it would not even build.
    const auto simulate = CollectShaderIncludes(
         StageSource( ShaderPath( "Particles/ParticleSimulate.shader" ), ShaderStage::Compute ),
         ShaderPath( "Particles/ParticleSimulate.shader" ) );
    const auto billboard = CollectShaderIncludes(
         StageSource( ShaderPath( "Particles/ParticleBillboard.shader" ), ShaderStage::Vertex ),
         ShaderPath( "Particles/ParticleBillboard.shader" ) );

    EXPECT_TRUE( ClosureNames( simulate, "ParticleState.glslh" ) )
         << "the simulation declares the particle layout itself again";
    EXPECT_TRUE( ClosureNames( billboard, "ParticleState.glslh" ) )
         << "the billboard draw declares the particle layout itself again";
}

TEST_F( ShaderRootFixture, TheParticleDispatchDividesByTheWorkgroupTheShaderDeclares )
{
    // The second of the two numbers, and the one whose failure is the quietest of any in this file: too
    // large a group size in C++ and SimulateInFrame launches too few groups, so the tail of every emitter
    // is never touched by the simulation. Those particles keep their zeroed state — dead, alpha 0, never
    // respawned — so the emitter simply carries fewer particles than it was configured for, forever, with
    // nothing logged and nothing invalid anywhere.
    const auto local = ComputeLocalSize( ShaderPath( "Particles/ParticleSimulate.shader" ) );

    EXPECT_EQ( local[0], Desert::Graphic::System::kParticleLocalSize )
         << "ParticleSimulate runs " << local[0] << " threads per group and SimulateInFrame divides by "
         << Desert::Graphic::System::kParticleLocalSize;

    // One thread per particle over a one-dimensional dispatch: the group count C++ computes is a division
    // of a particle COUNT, which is only the right arithmetic while the other two extents are one.
    EXPECT_EQ( local[1], 1u );
    EXPECT_EQ( local[2], 1u );
}

// ---- The debug-info PROFILE, asserted as a relation --------------------------------------------------
//
// The key fingerprints whether SPIR-V debug info is generated, because the binaries differ. That used
// to be an #ifdef buried in the hash — invisible to the game packager, which cooks on ONE machine for
// a runtime built in a possibly DIFFERENT configuration. The packager now names the target profile
// explicitly (SpirvDebugInfoForConfigName), and these three pins are what keep that name honest.

// A Debug artifact under a Release key (or vice versa) would be served across configs with different
// binaries — the profiles must key apart.
TEST( ShaderCacheKeyProfile, TheTwoProfilesKeyApart )
{
    const std::string src = "#version 450\nvoid main() {}\n";
    EXPECT_NE( Desert::Core::ComputeShaderCacheKeyForProfile( ShaderStage::Vertex, src, "probe", true ),
               Desert::Core::ComputeShaderCacheKeyForProfile( ShaderStage::Vertex, src, "probe", false ) );
}

// The 3-argument overload IS the profile overload at this build's own policy — the runtime asks with
// it, the cook answers with the explicit one, and this equality is why a same-config cook always hits.
TEST( ShaderCacheKeyProfile, TheRuntimeDefaultIsThisBuildsProfile )
{
    const std::string src = "#version 450\nvoid main() {}\n";
    EXPECT_EQ( ComputeShaderCacheKey( ShaderStage::Vertex, src, "probe" ),
               Desert::Core::ComputeShaderCacheKeyForProfile( ShaderStage::Vertex, src, "probe",
                                                              Desert::Core::SpirvDebugInfoThisBuild() ) );
}

// The packager maps its target-config STRING through SpirvDebugInfoForConfigName; the engine compiles
// under DESERT_CONFIG_DEBUG. Both spellings of "which configuration is this" must agree — this test
// runs in both configurations in CI, so each side of the mapping is pinned by a real build of the
// other. (This suite defines DESERT_CONFIG_DEBUG for Debug exactly as the engine's own premake does.)
TEST( ShaderCacheKeyProfile, TheConfigNameMappingMatchesTheBuildThatCarriesIt )
{
#ifdef DESERT_CONFIG_DEBUG
    const char* thisConfig = "Debug";
#else
    const char* thisConfig = "Release";
#endif
    EXPECT_EQ( Desert::Core::SpirvDebugInfoForConfigName( thisConfig ), Desert::Core::SpirvDebugInfoThisBuild() );

    // And the mapping is a real function of its argument, not a constant.
    EXPECT_TRUE( Desert::Core::SpirvDebugInfoForConfigName( "Debug" ) );
    EXPECT_FALSE( Desert::Core::SpirvDebugInfoForConfigName( "Release" ) );
}

// ─── Prose in a comment cannot move a location ────────────────────────────────────────────────────
//
// The DSL's paren-less sugar (`In T x;`, `Out T x;`, `Uniform Name {}`, `Buffer Name {}`) allocates
// the lowest free slot in declaration order, and until Г5 it searched the WHOLE file text — comments
// included. Five of the keywords are ordinary English words, so a sentence beginning "In LOCAL mode"
// consumed a location, and six shipped shaders were doing exactly that.
//
// This is the same relation asserted where it is finally paid: not "the translated text looks right"
// but "the MODULE the GPU is handed declares its inputs at the same locations", read back out of the
// SPIR-V. Text and SPIR-V are two statements of one fact and it is the second one that a vertex
// buffer binds against.
namespace
{
    // The interface variables of a compiled stage, as location -> name. `spirv_cross::Compiler` is
    // already this suite's reflection dependency (VulkanShaderReflection is built on it); the engine's
    // own ReflectStage deliberately does not carry locations, so this reads them directly.
    std::map<uint32_t, std::string> StageLocations( const std::vector<uint32_t>& spirv, bool inputs )
    {
        spirv_cross::Compiler compiler( spirv );
        const auto            resources = compiler.get_shader_resources();

        std::map<uint32_t, std::string> byLocation;
        for ( const auto& res : ( inputs ? resources.stage_inputs : resources.stage_outputs ) )
            byLocation[compiler.get_decoration( res.id, spv::DecorationLocation )] = res.name;
        return byLocation;
    }

    // One shader, twice: written the way an engineer would write it, and with every comment deleted.
    // The comments are the ones that were live in the tree — "In LOCAL mode" is verbatim from
    // Programs/Particles/ParticleSimulate.shader.
    const char* kCommented = R"(
Shader "ProseVsCode"
{
    Vertex
    {
        // Integrate alive particles. In LOCAL mode (u_Counts.w) the integrated state is the offset.
        /* Out of the pool comes exactly one entry, and the Buffer is a Uniform elsewhere. */
        In vec3 a_Position;
        In vec2 a_TexCoord;
        Out vec2 v_UV;
        void main() { gl_Position = vec4( a_Position, 1.0 ); v_UV = a_TexCoord; }
    }
}
)";

    const char* kStripped = R"(
Shader "ProseVsCode"
{
    Vertex
    {
        In vec3 a_Position;
        In vec2 a_TexCoord;
        Out vec2 v_UV;
        void main() { gl_Position = vec4( a_Position, 1.0 ); v_UV = a_TexCoord; }
    }
}
)";

    std::vector<uint32_t> VertexSpirvOf( const char* dsl )
    {
        auto parsed = Desert::Core::Preprocess::DShaderParser::Parse( dsl );
        EXPECT_TRUE( parsed.IsSuccess() ) << ( parsed.IsSuccess() ? "" : parsed.GetError() );
        if ( !parsed.IsSuccess() )
            return {};
        return CompileStage( parsed.GetValue().Stages.at( ShaderStage::Vertex ), "ProseVsCode.shader",
                             shaderc_vertex_shader );
    }
} // namespace

TEST( DShaderCommentsVsSpirv, CommentsDoNotMoveALocationInTheCompiledModule )
{
    const auto withProse = VertexSpirvOf( kCommented );
    const auto withNone  = VertexSpirvOf( kStripped );
    ASSERT_FALSE( withProse.empty() );
    ASSERT_FALSE( withNone.empty() );

    // The relation. Before the fix the prose took locations 0 and 0, so a_Position compiled to
    // location 1, a_TexCoord to 2 and v_UV to 1 — every attribute one past where the vertex layout
    // binds it.
    EXPECT_EQ( StageLocations( withProse, true ), StageLocations( withNone, true ) );
    EXPECT_EQ( StageLocations( withProse, false ), StageLocations( withNone, false ) );

    // And the numbers themselves, so the test still says something if BOTH sides drift together.
    const auto inputs = StageLocations( withNone, true );
    ASSERT_EQ( inputs.size(), 2u );
    EXPECT_EQ( inputs.at( 0 ), "a_Position" );
    EXPECT_EQ( inputs.at( 1 ), "a_TexCoord" );

    // Two modules that differ only in comments are the SAME module: comments do not survive
    // preprocessing, so anything at all in the binary would mean the sugar had written code.
    EXPECT_EQ( withProse, withNone );
}

// ─── No shipped shader claims one descriptor slot twice ───────────────────────────────────────────
//
// Г17. The DSL can allocate binding numbers automatically (`Uniform Name {}` with no parentheses takes
// the lowest free slot), and it decides which slots are free by scanning ONE TEXT for
// `binding = <digits>`. That is a RECOGNIZER, not a census, and it is blind three ways:
//
//   * a binding spelled as a MACRO is not digits — Common/CloudAuthored.glslh, Common/CloudParams.glslh
//     and Common/FogParams.glslh all write theirs that way;
//   * a binding declared in an INCLUDED file is not in the text at all, because ShaderIncluder hands
//     every `.glslh` its own separate translation call;
//   * a binding a SECOND STAGE declares is not in this stage's text either.
//
// Widening the pattern only closes the first of the three, so it is not what this asserts. THE NUMBER IS
// A NUMBER AFTER COMPILATION, whatever spelled it — so the claim is made against the SPIR-V the GPU is
// handed, by the engine's own reflection, over the whole shipped tree. It is the same asked-of-the-
// compiler form as the material-row test above, and it cannot be outrun by a syntax invented tomorrow.
//
// It has to be asked here rather than of glslang, which compiles two resources on one Binding with no
// diagnostic at all — measured with `glslc -Werror --target-env=vulkan1.1` on 2026-09-08, both Binding
// decorations present in the disassembly.
namespace
{
    shaderc_shader_kind KindOf( ShaderStage stage )
    {
        switch ( stage )
        {
            case ShaderStage::Vertex:
                return shaderc_vertex_shader;
            case ShaderStage::Fragment:
                return shaderc_fragment_shader;
            case ShaderStage::Compute:
                return shaderc_compute_shader;
            case ShaderStage::TessControl:
                return shaderc_tess_control_shader;
            case ShaderStage::TessEvaluation:
                return shaderc_tess_evaluation_shader;
            default:
                return shaderc_glsl_infer_from_source;
        }
    }

    std::vector<std::filesystem::path> ShippedShaderFiles()
    {
        std::vector<std::filesystem::path> files;
        const std::filesystem::path        root = "Resources/Shaders";
        if ( std::filesystem::exists( root ) )
            for ( const auto& entry : std::filesystem::recursive_directory_iterator( root ) )
                if ( entry.is_regular_file() && entry.path().extension() == ".shader" )
                    files.push_back( entry.path() );
        std::sort( files.begin(), files.end() );
        return files;
    }
} // namespace

TEST_F( ShaderRootFixture, NoShippedShaderClaimsOneDescriptorSlotTwice )
{
    const auto files = ShippedShaderFiles();
    ASSERT_GE( files.size(), 60u ) << "found " << files.size()
                                   << " shipped shaders under Resources/Shaders — the walk found"
                                      " nothing to examine, so a green result would mean nothing";

    // THERE IS NO EXCEPTION ANY MORE (Г20). This census used to skip one file —
    // Resources/Shaders/Programs/Graph/MatBroken.shader, a shader deliberately kept broken — and skipping
    // it was the smaller half of the cost: `AssetPreloader` compiles every `.shader` under this same root
    // at every editor start, so that fixture printed two errors into every clean log, for ever. An error
    // that is always present distinguishes nothing, and a genuinely broken shader was indistinguishable
    // from it. The fixture now lives in this suite's own Fixtures/ directory, where a TEST reaches it and
    // nobody else does, and the shipped tree is a tree in which EVERY file is meant to compile — which is
    // a stronger claim than this loop could make while it carried a name to skip.
    int passesChecked = 0;

    for ( const auto& file : files )
    {
        const auto parsed = Desert::Core::Preprocess::DShaderParser::Parse( ReadFile( file ) );
        ASSERT_TRUE( parsed.IsSuccess() ) << file.string() << ": " << parsed.GetError();

        // One ReflectionData PER PASS, because one pass is one pipeline layout. Sharing it across
        // passes would invent collisions between shaders that never meet on a device.
        const auto checkPass =
             [&]( const std::string& passName, const std::unordered_map<ShaderStage, std::string>& stages )
        {
            ShaderResource::ReflectionData data;
            for ( const auto& [stage, source] : stages )
            {
                const auto spirv = CompileStage( source, file, KindOf( stage ) );
                if ( spirv.empty() )
                    continue; // CompileStage already reported it

                const auto diagnostics = ShaderReflection::ReflectStage( spirv, stage, data );
                EXPECT_TRUE( diagnostics.empty() )
                     << file.string() << " [pass '" << passName
                     << "']: " << ( diagnostics.empty() ? std::string{} : diagnostics.front() );
            }
            ++passesChecked;
        };

        if ( parsed.GetValue().Passes.empty() )
            checkPass( "", parsed.GetValue().Stages );
        else
            for ( const auto& pass : parsed.GetValue().Passes )
                checkPass( pass.Name, pass.Stages );
    }

    // One pass minimum per file, and no `- 1` for a skipped one any more.
    EXPECT_GE( passesChecked, (int)files.size() );
}

// ─── The broken-shader fixture is still broken ────────────────────────────────────────────────────
//
// Г20. The repository keeps ONE shader that is meant not to compile, because two engine refusals are
// only reachable when such a shader exists: `ShaderService::Register`'s "registered but has no compiled
// stages" (which keeps a failed shader's NAME so a material does not silently fall back to the standard
// one) and the sentence `MaterialEditorPanel::PreviewUnavailableReason` writes for that state.
//
// WHAT THIS REPLACES, and the difference is the whole task. The census above used to name the fixture
// and SKIP it, asserting only that the file existed — a test satisfied by the presence of a file, which
// stays green if somebody repairs the shader and green if the shader never meant anything. Meanwhile the
// fixture sat in the shipped shader tree, so `AssetPreloader` compiled it at every editor start and put
// two errors in every clean log (measured on Desert_Sandbox: exactly two, both this file). The fixture
// is now reached HERE and only here, and it is reached by being COMPILED: repair it and this goes red.
TEST_F( ShaderRootFixture, TheBrokenShaderFixtureStillDoesNotCompile )
{
    const std::filesystem::path fixture =
         s_RepoRoot / "Desert" / "Tests" / "Engine" / "ShaderCacheKey" / "Fixtures" / "MatBroken.shader";
    ASSERT_TRUE( std::filesystem::exists( fixture ) )
         << fixture.string()
         << " is gone. Deleting it does not remove the engine states it stands for - it removes the only "
            "thing that reaches them.";

    // It has to be a well-formed DSL document: the failure being asserted is a GLSL type error, and a
    // fixture that failed to parse would exercise a different refusal while looking like this one.
    const auto parsed = Desert::Core::Preprocess::DShaderParser::Parse( ReadFile( fixture ) );
    ASSERT_TRUE( parsed.IsSuccess() ) << fixture.string() << ": " << parsed.GetError();
    const auto stage = parsed.GetValue().Stages.find( ShaderStage::Fragment );
    ASSERT_NE( stage, parsed.GetValue().Stages.end() ) << "the fixture declares no fragment stage";

    // Compiled the way the engine compiles, not the way this suite usually does: `CompileStage` EXPECTs
    // success, which is exactly wrong here.
    shaderc::Compiler       compiler;
    shaderc::CompileOptions options;
    options.SetIncluder( std::make_unique<Includer>() );
    options.SetTargetEnvironment( shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_1 );
    options.SetWarningsAsErrors();

    const auto result =
         compiler.CompileGlslToSpv( stage->second, shaderc_fragment_shader, fixture.string().c_str(), options );
    EXPECT_NE( result.GetCompilationStatus(), shaderc_compilation_status_success )
         << "the fixture COMPILES. Whoever repaired it also removed the only shader in this repository "
            "that reaches ShaderService's \"registered but has no compiled stages\" refusal and the "
            "Material Editor's message for it. Break it again, or delete both this test and the fixture "
            "and say which refusal is now unreachable.";

    // ...and it fails for the reason it is kept for. Any old error would pass the assertion above — a
    // typo in an include, a renamed header — and then the fixture would still be "broken" while standing
    // for nothing.
    EXPECT_NE( std::string( result.GetErrorMessage() ).find( "cannot convert" ), std::string::npos )
         << "the fixture no longer fails on the vec2 -> vec4 assignment it is kept for:\n"
         << result.GetErrorMessage();
}

// ─── The window a shader graph's own resources live in is EMPTY in the shipped tree ──────────────
//
// О1-G. Core::kGraphOwnedBindingFirst reserves set-0 bindings from 24 upward for the resources a SHADER
// GRAPH declares — the textures a Surface graph's Properties block takes today, and whatever an authored
// cloud medium declares tomorrow. A reservation is worth exactly what enforces it, and nothing enforced
// this one: the claim lived in a comment listing the engine's slots by hand, and the check beside it was
// `EXPECT_GT( kGraphTextureBinding, 23u )` — a number that can be satisfied by editing the number.
//
// The claim asserted here is the one that matters and it names no engine binding: over the whole shipped
// tree, compiled by shaderc and reflected by the engine's own reflection, NOTHING declares a set-0
// binding inside the window. A slot added at 24 tomorrow reddens this whatever spelled it — a literal, a
// macro, an included header or a second stage, the three blindnesses Г17 measured — because after
// compilation a binding is a number.
//
// SET 0 ONLY, because that is the set a graph declares into: the DSL's `TextureBinding(n)` emits
// `layout(binding = n)` with no set qualifier, which is set 0, and the runtime binds a material's
// resources there. A shader that puts something at set 1, binding 24 has not touched the window.
TEST_F( ShaderRootFixture, NoShippedProgramDeclaresABindingInTheGraphsReservedWindow )
{
    const auto files = ShippedShaderFiles();
    ASSERT_GE( files.size(), 60u ) << "found " << files.size()
                                   << " shipped shaders under Resources/Shaders — the walk found nothing"
                                      " to examine, so a green result would mean nothing";

    // Reported on success as well as on failure: the distance between the two is the headroom the next
    // engine binding has, and it is currently ZERO. A number nobody prints is a number nobody notices
    // closing.
    uint32_t    highest = 0;
    std::string highestWhere;
    int         passesChecked = 0;

    for ( const auto& file : files )
    {
        const auto parsed = Desert::Core::Preprocess::DShaderParser::Parse( ReadFile( file ) );
        ASSERT_TRUE( parsed.IsSuccess() ) << file.string() << ": " << parsed.GetError();

        // WHAT THIS FILE IS ENTITLED TO PUT IN THE WINDOW — ITS OWN TEXTURE PROPERTIES, BY NAME.
        //
        // The DSL numbers a Properties block's Texture2D/TextureCube properties upward from the block's
        // base, one at a time, declaring each as `sampler2D <PropertyName>` — and a shader GENERATED from
        // a graph asks for the reserved base. So a graph-generated shader with three textures legitimately
        // occupies the first three slots of the window, and a census that simply forbade the window would
        // go red on the next such file somebody commits. That false alarm's obvious "fix" is to edit the
        // reservation, which is the failure this whole test replaces.
        //
        // BY NAME AND NOT BY COUNT, and the difference is a hole. A count says "N slots of the window are
        // excused", which excuses them in a shader whose textures are at base 2 and never went near the
        // window — Programs/Text/TextSDF.shader would have bought an engine binding at 24 a free pass.
        // The name says WHICH resource is standing there, so the excuse only covers the declaration it
        // was granted for.
        std::set<std::string> ownTextureProperties;
        for ( const auto& param : parsed.GetValue().Meta.Params )
            if ( param.IsTexture && !param.IsAssetRef() )
                ownTextureProperties.insert( param.Name );

        const auto checkPass =
             [&]( const std::string& passName, const std::unordered_map<ShaderStage, std::string>& stages )
        {
            ShaderResource::ReflectionData data;
            for ( const auto& [stage, source] : stages )
            {
                const auto spirv = CompileStage( source, file, KindOf( stage ) );
                if ( spirv.empty() )
                    continue; // CompileStage already reported it

                // Asserted here TOO, and not because the census above forgot to: the reflection's
                // buckets are keyed by binding, so a collision DELETES one of the two resources from the
                // layout this test then walks. A slot inside the reserved window could be the one that
                // disappeared, and this test would report a clean tree over a layout that is short.
                const auto diagnostics = ShaderReflection::ReflectStage( spirv, stage, data );
                EXPECT_TRUE( diagnostics.empty() )
                     << file.string() << " [pass '" << passName
                     << "']: " << ( diagnostics.empty() ? std::string{} : diagnostics.front() );
            }
            ++passesChecked;

            const auto setZero = data.ShaderDescriptorSets.find( 0 );
            if ( setZero == data.ShaderDescriptorSets.end() )
                return;

            // What a Properties texture COULD have become: the parser emits `sampler2D` for Texture2D and
            // `samplerCube` for TextureCube and nothing else, so those are the only two buckets an
            // entitled declaration can be in. A uniform block, a storage buffer, a storage image or a
            // sampler3D inside the window is an engine declaration whatever it is called.
            std::map<uint32_t, std::string> entitledCandidates;
            for ( const auto& [binding, resource] : setZero->second.Image2DSamplers )
                entitledCandidates.emplace( binding, resource.Name );
            for ( const auto& [binding, resource] : setZero->second.ImageCubeSamplers )
                entitledCandidates.emplace( binding, resource.Name );

            for ( const auto& binding : ShaderReflection::BuildLayoutBindings( setZero->second ) )
            {
                if ( binding.binding < Desert::Core::kGraphOwnedBindingFirst )
                {
                    // The headroom below is about the ENGINE's slots, so a graph's own texture must not be
                    // counted into it — it lives in the window by right and would report a headroom of -1
                    // for a tree in which nothing is wrong.
                    if ( binding.binding > highest )
                    {
                        highest      = binding.binding;
                        highestWhere = file.string() + " [" + passName + "]";
                    }
                    continue;
                }

                const auto        candidate = entitledCandidates.find( binding.binding );
                const std::string name = candidate == entitledCandidates.end() ? std::string{} : candidate->second;

                EXPECT_TRUE( !name.empty() && ownTextureProperties.count( name ) == 1 )
                     << file.string() << " [pass '" << passName << "'] declares set 0, binding " << binding.binding
                     << ( name.empty() ? "" : " ('" + name + "')" )
                     << ", which is inside the window reserved for a shader graph's own resources"
                        " (Core::kGraphOwnedBindingFirst = "
                     << Desert::Core::kGraphOwnedBindingFirst
                     << ") and is not one of this file's own Texture2D/TextureCube properties. A graph's"
                        " texture would land on top of it, and GLSL says nothing about two declarations on"
                        " one slot. Move this binding down, or raise the reservation and regenerate every"
                        " .shader whose Properties block spells TextureBinding().";
            }
        };

        if ( parsed.GetValue().Passes.empty() )
            checkPass( "", parsed.GetValue().Stages );
        else
            for ( const auto& pass : parsed.GetValue().Passes )
                checkPass( pass.Name, pass.Stages );
    }

    EXPECT_GE( passesChecked, (int)files.size() );

    // Signed, because the difference is negative exactly when this test has already failed and an
    // unsigned one prints 4294967295 instead of saying so — measured, on the mutation that proved the
    // assertion above can go red.
    const int headroom = (int)Desert::Core::kGraphOwnedBindingFirst - (int)highest - 1;
    std::cout << "[ SHIPPED  ] highest set-0 binding is " << highest << " (" << highestWhere
              << "); the graph's window opens at " << Desert::Core::kGraphOwnedBindingFirst
              << ", so the headroom is " << headroom << " slot(s)\n";
}

// ─── The medium seam, both directions ─────────────────────────────────────────────────────────────
//
// О1-G. An authored cloud medium is BYTES SUBSTITUTED FOR A HEADER NAME inside four shipped programs
// (Docs/Clouds/O1_DESIGN.md §12.2), so its declarations sit beside declarations it never sees. О1-E
// declined to give the Volume domain its own parameters and textures for exactly that reason and said so
// in §12.3: "their bindings would have to be free in all four programs, and a collision between two GLSL
// declarations at one binding is silent".
//
// It is silent in GLSL and it is not silent here, and the two tests below are the two halves of that
// sentence, asked of the REAL programs rather than of a synthetic one:
//
//   * a medium that takes an OCCUPIED slot is refused by name, in every one of the four consumers;
//   * a medium that declares a buffer AND a texture in the reserved window compiles and reflects clean
//     in every one of the four, and the two resources come back at the numbers it asked for.
//
// The second is the one that makes the first worth having. A guard that only ever says no leaves "is
// there anywhere safe to put this?" unanswered — which is the question О1-E's remainder actually turned
// on, and the answer is now a measurement instead of an assumption.
namespace
{
    // The four programs a cloud material's medium is compiled into. Written out because they are a
    // CONTRACT — Generated/CloudMedium.glslh names the same four in its own header, and O1_DESIGN §10.1
    // measured that each of them moves the frame — not because they are hard to find.
    const std::array<const char*, 4> kMediumConsumers = {
         "Clouds/CloudRaymarch.shader", "Clouds/CloudShadowMap.shader", "Clouds/CloudSkyOcclusionVolume.shader",
         "Compute/BakeProceduralSky.shader" };

    // shaderc's includer, resolving one name from a variant and everything else from disk — the same
    // arrangement Core::ShaderIncluder has, narrowed to what a test needs. The suite's own Includer reads
    // only the file system, so a substituted medium would silently compile the shipped one.
    class SubstitutingIncluder final : public shaderc::CompileOptions::IncluderInterface
    {
    public:
        SubstitutingIncluder( std::string name, std::string body )
             : m_Name( std::move( name ) ), m_Body( std::move( body ) )
        {
        }

        shaderc_include_result* GetInclude( const char* requested, shaderc_include_type type,
                                            const char* requesting, size_t ) override
        {
            const bool substituted = ( type == shaderc_include_type_standard && m_Name == requested );

            const std::filesystem::path full =
                 type == shaderc_include_type_relative
                      ? ( std::filesystem::path( requesting ).parent_path() / requested ).lexically_normal()
                      : ( Common::Constants::Path::SHADERDIR_PATH / requested ).lexically_normal();

            auto* name = new std::string( full.string() );
            auto* body = new std::string( Desert::Core::Preprocess::DShaderParser::TranslateSugar(
                 substituted ? m_Body : ReadFile( full ) ) );

            auto* result               = new shaderc_include_result;
            result->source_name        = name->c_str();
            result->source_name_length = name->size();
            result->content            = body->c_str();
            result->content_length     = body->size();
            result->user_data          = new std::pair<std::string*, std::string*>( name, body );
            return result;
        }

        void ReleaseInclude( shaderc_include_result* data ) override
        {
            auto* pair = static_cast<std::pair<std::string*, std::string*>*>( data->user_data );
            delete pair->first;
            delete pair->second;
            delete pair;
            delete data;
        }

    private:
        std::string m_Name;
        std::string m_Body;
    };

    // Compiles one consumer's compute stage with @p mediumBody standing in for the medium include, then
    // reflects it. Returns the reflection's diagnostics and, through @p data, the layout it built.
    //
    // The compilation itself is EXPECTed to succeed: a medium that fails to compile would produce an
    // empty diagnostic list, which reads exactly like a medium that collided with nothing.
    std::vector<std::string> ReflectConsumerWithMedium( const char* consumer, const std::string& mediumBody,
                                                        ShaderResource::ReflectionData& data )
    {
        const auto        path   = ShaderPath( consumer );
        const std::string source = StageSource( path, ShaderStage::Compute );
        EXPECT_FALSE( source.empty() ) << consumer;
        if ( source.empty() )
            return { "no compute stage" };

        shaderc::Compiler       compiler;
        shaderc::CompileOptions options;
        options.SetIncluder(
             std::make_unique<SubstitutingIncluder>( Desert::Graphic::kCloudMediumInclude, mediumBody ) );
        options.SetTargetEnvironment( shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_1 );
        options.SetWarningsAsErrors();

        const auto result =
             compiler.CompileGlslToSpv( source, shaderc_compute_shader, path.string().c_str(), options );
        EXPECT_EQ( result.GetCompilationStatus(), shaderc_compilation_status_success )
             << consumer << ": " << result.GetErrorMessage();
        if ( result.GetCompilationStatus() != shaderc_compilation_status_success )
            return { result.GetErrorMessage() };

        const std::vector<uint32_t> spirv( result.begin(), result.end() );
        return ShaderReflection::ReflectStage( spirv, ShaderStage::Compute, data );
    }

    // A medium body that forwards every contract entry point to the default chain, plus whatever @p extra
    // declares before them. Forwarding rather than inventing keeps the thing under test the DECLARATION:
    // a body that computed something else would also change the picture, and a body that computed nothing
    // would be dead-code-eliminated along with its resource.
    std::string MediumForwardingWith( const std::string& extra, const std::string& densityExpression )
    {
        return "#include <Common/CloudMediumDefault.glslh>\n" + extra +
               "float CloudSampleDensity( CloudFieldParams params, CloudFieldSample field, vec3 positionKm )\n"
               "{ return CloudDefaultDensity( params, field, positionKm ) * ( " +
               densityExpression +
               " ); }\n"
               "float CloudSampleExtinctionFactor( CloudFieldParams params, CloudFieldSample field, vec3 p )\n"
               "{ return CloudDefaultExtinctionFactor( params, field, p ); }\n"
               "vec3 CloudSampleAlbedo( CloudFieldParams params, CloudFieldSample field, vec3 p, vec3 a )\n"
               "{ return CloudDefaultAlbedo( params, field, p, a ); }\n"
               "vec3 CloudSampleEmissive( CloudFieldParams params, CloudFieldSample field, vec3 p )\n"
               "{ return CloudDefaultEmissive( params, field, p ); }\n"
               "float CloudSampleOcclusion( CloudFieldParams params, CloudFieldSample field, vec3 p, float o )\n"
               "{ return CloudDefaultOcclusion( params, field, p, o ); }\n";
    }
} // namespace

TEST_F( ShaderRootFixture, AMediumThatTakesAnOccupiedBindingIsRefusedByNameInEveryConsumer )
{
    // Binding 1 is the cloud parameter block in three of the four consumers and the SKY payload in the
    // fourth, which is why the collision is asserted per consumer rather than once: the four do not have
    // one layout, and a medium is compiled into all four.
    const std::string colliding = MediumForwardingWith(
         "layout( std430, binding = 1 ) readonly buffer AuthoredMediumImpostor { vec4 u_Impostor[4]; };\n",
         "u_Impostor[0].x" );

    for ( const char* consumer : kMediumConsumers )
    {
        ShaderResource::ReflectionData data;
        const auto                     diagnostics = ReflectConsumerWithMedium( consumer, colliding, data );

        ASSERT_FALSE( diagnostics.empty() )
             << consumer
             << " accepted a medium that declares its own buffer on an occupied slot. That is the "
                "silence О1-E's remainder was blocked on: glslang emits both Binding decorations, the "
                "reflection buckets are keyed BY BINDING, and one of the two resources disappears into a "
                "layout that is complete, plausible and wrong.";
        EXPECT_NE( diagnostics.front().find( "AuthoredMediumImpostor" ), std::string::npos )
             << consumer << ": " << diagnostics.front();
        EXPECT_NE( diagnostics.front().find( "binding 1" ), std::string::npos )
             << consumer << ": " << diagnostics.front();
    }
}

TEST_F( ShaderRootFixture, AMediumsWholeCapacityLandsAtTheNumbersTheRUNTIMEBinds )
{
    // О1-G ASKED "IS THERE A SAFE PLACE"; THIS ASKS "IS IT THE PLACE THE C++ WRITES TO". The renderer and
    // the sky bake call SetStorageBuffer/SetInput with an explicit number and never consult reflection —
    // a mismatch lands a resource on a different descriptor rather than on an error — so the numbers here
    // are Core::kCloudMediumParamsBinding and Core::kCloudMediumTextureFirst themselves, not a second
    // arithmetic on kGraphOwnedBindingFirst that could agree today and drift tomorrow.
    //
    // AND IT IS THE WHOLE CAPACITY, not one slot of it. Every image a medium may declare is a descriptor
    // that all four programs carry on every frame of every scene; if the last of them collided with
    // something, only a medium that used all four would find out, in front of the artist who applied it.
    std::string declarations =
         std::format( "layout( std430, binding = {} ) readonly buffer {} {{ vec4 u_MediumParams[{}]; }};\n",
                      Desert::Core::kCloudMediumParamsBinding, Desert::Core::kCloudMediumBlockName,
                      Desert::Core::kCloudMediumMaxValues );
    std::string density = "u_MediumParams[0].x";
    for ( uint32_t i = 0; i < Desert::Core::kCloudMediumMaxTextures; ++i )
    {
        declarations += std::format( "layout( binding = {} ) uniform sampler2D u_MediumTexture{};\n",
                                     Desert::Core::kCloudMediumTextureFirst + i, i );
        density += std::format( " * textureLod( u_MediumTexture{}, positionKm.xz, 0.0f ).r", i );
    }

    const std::string reserved = MediumForwardingWith( declarations, density );

    for ( const char* consumer : kMediumConsumers )
    {
        ShaderResource::ReflectionData data;
        const auto                     diagnostics = ReflectConsumerWithMedium( consumer, reserved, data );

        EXPECT_TRUE( diagnostics.empty() )
             << consumer << ": " << ( diagnostics.empty() ? std::string{} : diagnostics.front() );

        const auto setZero = data.ShaderDescriptorSets.find( 0 );
        ASSERT_NE( setZero, data.ShaderDescriptorSets.end() ) << consumer;
        const auto bindings = ShaderReflection::BuildLayoutBindings( setZero->second );

        EXPECT_TRUE(
             HasBinding( bindings, Desert::Core::kCloudMediumParamsBinding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ) )
             << consumer << " lost the medium's own buffer: " << DescribeBindings( bindings );
        for ( uint32_t i = 0; i < Desert::Core::kCloudMediumMaxTextures; ++i )
            EXPECT_TRUE( HasBinding( bindings, Desert::Core::kCloudMediumTextureFirst + i,
                                     VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) )
                 << consumer << " lost the medium's image " << i << ": " << DescribeBindings( bindings );

        // The other half of "it landed where it asked": nothing else moved to make room. A layout that
        // gained them all AND lost one of its own would satisfy every assertion above.
        EXPECT_EQ( ShaderReflection::CountDescriptors( bindings ), bindings.size() ) << consumer;
    }
}

TEST_F( ShaderRootFixture, AMediumMaySampleTheLayersOwnNoiseVolumeInEveryConsumer )
{
    // O1-H. THE Cloud Noise Volume NODE COSTS NO BINDING AND NO RUNTIME PLUMBING — it reaches the `.dcnv`
    // volumes the scene has ALREADY bound, through the CLOUD_SAMPLE_NOISE macro — and that is a claim
    // about SCOPE, which is what this domain has been wrong about twice: the medium is substituted into
    // the middle of Common/CloudField.glslh, so what is in scope there is a fact about four programs'
    // include order and not about the header the macro is documented in.
    //
    // Both halves of the emitter's output are compiled here: the unwired form, which passes the
    // producer's own int straight to the macro, and the wired form, which goes through CloudNoiseSlotOf.
    // A shipped program that stopped defining the macro before its include of the field would take this
    // red instead of failing in front of the artist who applied the material.
    const std::string sampling = MediumForwardingWith(
         "", "CLOUD_SAMPLE_NOISE( field.NoiseSlot, CloudDefaultNoiseCoordinate( params, positionKm ) ).z * "
             "CLOUD_SAMPLE_NOISE( CloudNoiseSlotOf( CloudGraphSampleAt( params, field, positionKm ).NoiseSlot ),"
             " CloudDefaultNoiseCoordinate( params, positionKm ) ).w" );

    for ( const char* consumer : kMediumConsumers )
    {
        ShaderResource::ReflectionData data;
        const auto                     diagnostics = ReflectConsumerWithMedium( consumer, sampling, data );

        EXPECT_TRUE( diagnostics.empty() )
             << consumer << ": " << ( diagnostics.empty() ? std::string{} : diagnostics.front() );

        // AND IT ADDED NOTHING TO THE LAYOUT. The whole argument for this node over a Medium Texture is
        // that the volumes are already there; if sampling one grew the descriptor set, it would be a
        // fifth declaration of textures the renderer binds itself, on numbers nobody reserved.
        const auto setZero = data.ShaderDescriptorSets.find( 0 );
        ASSERT_NE( setZero, data.ShaderDescriptorSets.end() ) << consumer;
        const auto bindings = ShaderReflection::BuildLayoutBindings( setZero->second );
        EXPECT_FALSE(
             HasBinding( bindings, Desert::Core::kCloudMediumParamsBinding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ) )
             << consumer << " grew the medium's own parameter block for a graph that declares no property";
        for ( uint32_t i = 0; i < Desert::Core::kCloudMediumMaxTextures; ++i )
            EXPECT_FALSE( HasBinding( bindings, Desert::Core::kCloudMediumTextureFirst + i,
                                      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ) )
                 << consumer << " gave a noise fetch a descriptor of its own at slot " << i
                 << "; the volumes it reads are the ones the renderer already binds.";
    }
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
