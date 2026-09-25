// Every image binding a shader declares must be given a fallback descriptor before anything real is
// bound to it — with a view type that matches the binding.
//
// This exists because of a hole that opens silently. Reflection learned to tell `sampler3D` and
// `image3D` apart from their 2D counterparts, which means a volume binding now gets its own layout
// entry — but nothing wrote a descriptor into it. A descriptor nobody wrote is undefined memory that
// the shader samples anyway: no crash, no validation message, just wrong pixels. Substituting a 2D
// fallback would be no better, for the same reason the reflection work was done at all.
//
// The device is what cannot be had on this machine, so what is tested is the part that does not need
// one: the ENUMERATION the fallback pass drives itself off. If a binding shows up here with the right
// kind, VulkanMaterialBackend::WriteFallbacks writes it — that loop has no other input.

#include <gtest/gtest.h>

#include <Engine/Graphic/API/Vulkan/VulkanShaderReflection.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanShaderResource.hpp>

#include <shaderc/shaderc.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace Desert::Graphic::API::Vulkan;
using Desert::Core::Formats::ShaderStage;
using ShaderResource::FallbackImageKind;

namespace
{
    std::vector<uint32_t> Compile( const char* source, shaderc_shader_kind kind )
    {
        shaderc::Compiler       compiler;
        shaderc::CompileOptions options;
        options.SetTargetEnvironment( shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_1 );

        const auto result = compiler.CompileGlslToSpv( source, kind, "descriptor_fallbacks_test.glsl", options );
        EXPECT_EQ( result.GetCompilationStatus(), shaderc_compilation_status_success ) << result.GetErrorMessage();
        if ( result.GetCompilationStatus() != shaderc_compilation_status_success )
        {
            return {};
        }
        return { result.begin(), result.end() };
    }

    // The kind CollectImageBindings reported for @p binding, or nullopt if it reported nothing — which
    // is the failure this whole file is about.
    std::optional<FallbackImageKind> KindOf( const std::vector<ShaderResource::FallbackImageBinding>& bindings,
                                             uint32_t                                                 binding )
    {
        const auto it = std::find_if( bindings.begin(), bindings.end(),
                                      [binding]( const ShaderResource::FallbackImageBinding& candidate )
                                      { return candidate.Binding == binding; } );
        if ( it == bindings.end() )
            return std::nullopt;
        return it->Kind;
    }

    // A compute shader declaring one binding of every image kind the engine can reflect. This is the
    // shape of a noise-generation pass: a volume written as `image3D`, a volume sampled as
    // `sampler3D`, alongside the 2D and cube resources that already worked.
    const char* kEveryImageKind = R"(#version 450
layout(binding = 0) uniform sampler2D   u_Weather;
layout(binding = 1) uniform sampler3D   u_ShapeNoise;
layout(binding = 2) uniform samplerCube u_Radiance;
layout(binding = 3, rgba8) writeonly uniform image2D u_OutSlice;
layout(binding = 4, rgba8) writeonly uniform image3D u_OutVolume;

layout(local_size_x = 8, local_size_y = 8, local_size_z = 8) in;
void main()
{
    vec4 c = texture(u_Weather, vec2(0.5))
           + texture(u_ShapeNoise, vec3(0.5))
           + texture(u_Radiance, vec3(0.0, 1.0, 0.0));
    imageStore(u_OutSlice,  ivec2(gl_GlobalInvocationID.xy), c);
    imageStore(u_OutVolume, ivec3(gl_GlobalInvocationID),    c);
}
)";

    ShaderResource::ShaderDescriptorSet ReflectSetZero( const char* source, shaderc_shader_kind kind,
                                                        ShaderStage stage )
    {
        const auto spirv = Compile( source, kind );
        EXPECT_FALSE( spirv.empty() );

        ShaderResource::ReflectionData data;
        const auto                     diagnostics = ShaderReflection::ReflectStage( spirv, stage, data );
        EXPECT_TRUE( diagnostics.empty() ) << ( diagnostics.empty() ? std::string{} : diagnostics.front() );

        return data.ShaderDescriptorSets.at( 0 );
    }
} // namespace

TEST( DescriptorFallbacks, EveryDeclaredImageBindingIsEnumerated )
{
    const auto set0     = ReflectSetZero( kEveryImageKind, shaderc_glsl_compute_shader, ShaderStage::Compute );
    const auto bindings = ShaderResource::CollectImageBindings( set0 );

    // Five declarations in, five bindings out. A bucket the enumeration forgets shows up as a smaller
    // count here, and as an undefined descriptor at runtime.
    ASSERT_EQ( bindings.size(), 5u );
}

// The point of the whole exercise: the volume bindings are enumerated, and they are enumerated as
// VOLUMES. Reporting binding 1 as Sampled2D would compile, run, and quietly hand a `sampler3D` a
// one-texel 2D image.
TEST( DescriptorFallbacks, VolumeBindingsAreEnumeratedAsVolumes )
{
    const auto set0     = ReflectSetZero( kEveryImageKind, shaderc_glsl_compute_shader, ShaderStage::Compute );
    const auto bindings = ShaderResource::CollectImageBindings( set0 );

    ASSERT_TRUE( KindOf( bindings, 1 ).has_value() ) << "sampler3D binding 1 got no fallback at all";
    EXPECT_EQ( *KindOf( bindings, 1 ), FallbackImageKind::Sampled3D );

    ASSERT_TRUE( KindOf( bindings, 4 ).has_value() ) << "image3D binding 4 got no fallback at all";
    EXPECT_EQ( *KindOf( bindings, 4 ), FallbackImageKind::Storage3D );
}

// The kinds that already worked must keep working: this loop is the one the 2D and cube paths run
// through now as well, so a regression here is a regression everywhere.
TEST( DescriptorFallbacks, TwoDimensionalAndCubeBindingsKeepTheirKinds )
{
    const auto set0     = ReflectSetZero( kEveryImageKind, shaderc_glsl_compute_shader, ShaderStage::Compute );
    const auto bindings = ShaderResource::CollectImageBindings( set0 );

    ASSERT_TRUE( KindOf( bindings, 0 ).has_value() );
    EXPECT_EQ( *KindOf( bindings, 0 ), FallbackImageKind::Sampled2D );

    ASSERT_TRUE( KindOf( bindings, 2 ).has_value() );
    EXPECT_EQ( *KindOf( bindings, 2 ), FallbackImageKind::SampledCube );

    ASSERT_TRUE( KindOf( bindings, 3 ).has_value() );
    EXPECT_EQ( *KindOf( bindings, 3 ), FallbackImageKind::Storage2D );
}

// Uniform and storage BUFFERS are seeded from the dummy buffer, not from a fallback image, so they
// must not appear in this list — an image write aimed at a buffer binding is a descriptor-type
// mismatch, which is the one failure in this area that Vulkan does catch, loudly.
TEST( DescriptorFallbacks, BufferBindingsAreNotImageBindings )
{
    const char* kBuffersOnly = R"(#version 450
layout(binding = 0) uniform Params { vec4 u_Bounds; };
layout(std430, binding = 1) buffer Counters { uint u_Counts[]; };
layout(local_size_x = 1) in;
void main() { u_Counts[0] = uint(u_Bounds.x); }
)";

    const auto set0     = ReflectSetZero( kBuffersOnly, shaderc_glsl_compute_shader, ShaderStage::Compute );
    const auto bindings = ShaderResource::CollectImageBindings( set0 );

    EXPECT_TRUE( bindings.empty() );
}

TEST( DescriptorFallbacks, AShaderWithNoImagesEnumeratesNothing )
{
    const ShaderResource::ShaderDescriptorSet empty;
    EXPECT_TRUE( ShaderResource::CollectImageBindings( empty ).empty() );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
