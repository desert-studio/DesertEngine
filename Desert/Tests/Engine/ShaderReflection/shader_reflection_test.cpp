// CLD-11 — SPIR-V reflection must classify image resources by their declared TYPE.
//
// The rule this replaced looked at the variable's NAME: a resource whose name contained "Env" or
// "Cube" was filed as a cube sampler, anything else as a 2D one, and every storage image as a 2D
// storage image. Nothing about that fails loudly — the descriptor TYPE it produces is right, only the
// view type is wrong — so a `sampler3D` bound as 2D just returns nonsense and the render is merely
// "off". These tests are the regression guard: the resource names below are deliberately chosen so
// that name matching gets every single one of them wrong.

#include <gtest/gtest.h>

#include <Engine/Graphic/API/Vulkan/VulkanShaderReflection.hpp>
#include <Engine/Graphic/Materials/Mesh/MeshVertexLayout.hpp>

#include <shaderc/shaderc.hpp>

#include <string>
#include <vector>

using namespace Desert::Graphic::API::Vulkan;
using Desert::Core::Formats::ShaderStage;

namespace
{
    std::vector<uint32_t> Compile( const char* source, shaderc_shader_kind kind )
    {
        shaderc::Compiler       compiler;
        shaderc::CompileOptions options;
        // Same target as Core::ShaderCompiler::CompileGLSLToSPIRV, so the SPIR-V under test is shaped
        // like the SPIR-V the engine actually reflects.
        options.SetTargetEnvironment( shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_1 );

        const auto result = compiler.CompileGlslToSpv( source, kind, "shader_reflection_test.glsl", options );
        EXPECT_EQ( result.GetCompilationStatus(), shaderc_compilation_status_success ) << result.GetErrorMessage();
        if ( result.GetCompilationStatus() != shaderc_compilation_status_success )
        {
            return {};
        }
        return { result.begin(), result.end() };
    }

    std::string FirstOr( const std::vector<std::string>& messages, const char* fallback )
    {
        return messages.empty() ? fallback : messages.front();
    }

    // All five image declarations at once, which is exactly what CLD-11 asks to be shown apart.
    // u_EnvNoise is a sampler2D and u_Radiance a samplerCube: under name matching the first would be
    // filed as a cube and the second as 2D, so this shader alone fails if the old rule comes back.
    const char* kAllFiveImageKinds = R"(#version 450
layout(binding = 0) uniform sampler2D   u_EnvNoise;
layout(binding = 1) uniform sampler3D   u_ShapeNoise;
layout(binding = 2) uniform samplerCube u_Radiance;
layout(binding = 3, rgba8) writeonly uniform image2D u_OutSlice;
layout(binding = 4, rgba8) writeonly uniform image3D u_OutVolume;

layout(binding = 5) uniform Params { vec4 u_Bounds; };
layout(push_constant) uniform Push { vec4 u_Extent; };

layout(local_size_x = 8, local_size_y = 8, local_size_z = 8) in;
void main()
{
    vec4 c = texture(u_EnvNoise, vec2(0.5))
           + texture(u_ShapeNoise, vec3(0.5))
           + texture(u_Radiance, vec3(0.0, 1.0, 0.0))
           + u_Bounds + u_Extent;
    imageStore(u_OutSlice,  ivec2(gl_GlobalInvocationID.xy), c);
    imageStore(u_OutVolume, ivec3(gl_GlobalInvocationID),    c);
}
)";
} // namespace

TEST( ShaderReflection, FiveImageKindsLandInFiveDistinctBuckets )
{
    const auto spirv = Compile( kAllFiveImageKinds, shaderc_glsl_compute_shader );
    ASSERT_FALSE( spirv.empty() );

    ShaderResource::ReflectionData data;
    const auto diagnostics = ShaderReflection::ReflectStage( spirv, ShaderStage::Compute, data );
    ASSERT_TRUE( diagnostics.empty() ) << FirstOr( diagnostics, "" );

    ASSERT_EQ( data.ShaderDescriptorSets.size(), 1u );
    const auto& set0 = data.ShaderDescriptorSets.at( 0 );

    ASSERT_EQ( set0.Image2DSamplers.size(), 1u );
    ASSERT_EQ( set0.Image3DSamplers.size(), 1u );
    ASSERT_EQ( set0.ImageCubeSamplers.size(), 1u );
    ASSERT_EQ( set0.StorageImage2DSamplers.size(), 1u );
    ASSERT_EQ( set0.StorageImage3DSamplers.size(), 1u );

    EXPECT_EQ( set0.Image2DSamplers.at( 0 ).Name, "u_EnvNoise" );
    EXPECT_EQ( set0.Image3DSamplers.at( 1 ).Name, "u_ShapeNoise" );
    EXPECT_EQ( set0.ImageCubeSamplers.at( 2 ).Name, "u_Radiance" );
    EXPECT_EQ( set0.StorageImage2DSamplers.at( 3 ).Name, "u_OutSlice" );
    EXPECT_EQ( set0.StorageImage3DSamplers.at( 4 ).Name, "u_OutVolume" );

    EXPECT_EQ( set0.Image3DSamplers.at( 1 ).BindingPoint, 1u );
    EXPECT_EQ( set0.StorageImage3DSamplers.at( 4 ).BindingPoint, 4u );
}

// The reflection also carries buffers and push constants; it moved translation unit wholesale when it
// was made device-free, so assert it still brings them along.
TEST( ShaderReflection, KeepsUniformBuffersAndPushConstants )
{
    const auto spirv = Compile( kAllFiveImageKinds, shaderc_glsl_compute_shader );
    ASSERT_FALSE( spirv.empty() );

    ShaderResource::ReflectionData data;
    const auto diagnostics = ShaderReflection::ReflectStage( spirv, ShaderStage::Compute, data );
    ASSERT_TRUE( diagnostics.empty() ) << FirstOr( diagnostics, "" );

    const auto& set0 = data.ShaderDescriptorSets.at( 0 );
    ASSERT_EQ( set0.UniformBuffers.size(), 1u );

    const auto& ub = set0.UniformBuffers.at( 5 );
    EXPECT_EQ( ub.Name, "Params" );
    EXPECT_EQ( ub.Size, 16u );
    EXPECT_EQ( ub.ShaderStage, ShaderStage::Compute );
    ASSERT_EQ( ub.Fields.size(), 1u );
    EXPECT_EQ( ub.Fields[0].Name, "u_Bounds" );
    EXPECT_EQ( ub.Fields[0].Offset, 0u );

    ASSERT_TRUE( data.PushConstantRanges.has_value() );
    EXPECT_EQ( data.PushConstantRanges->Size, 16u );
    EXPECT_EQ( data.PushConstantRanges->ShaderStage, ShaderStage::Compute );
}

// The sharpest form of the regression guard: names that say the opposite of the truth. `u_EnvCubeMap`
// contains BOTH substrings the old rule keyed on and is a plain sampler2D; `u_Density` contains
// neither and is a cube. Restore name matching and both of these flip.
TEST( ShaderReflection, ClassificationIgnoresResourceNames )
{
    const char* kMisleadingNames = R"(#version 450
layout(binding = 0) uniform sampler2D   u_EnvCubeMap;
layout(binding = 1) uniform samplerCube u_Density;
layout(location = 0) out vec4 o_Color;
void main()
{
    o_Color = texture(u_EnvCubeMap, vec2(0.5)) + texture(u_Density, vec3(0.0, 1.0, 0.0));
}
)";

    const auto spirv = Compile( kMisleadingNames, shaderc_glsl_fragment_shader );
    ASSERT_FALSE( spirv.empty() );

    ShaderResource::ReflectionData data;
    const auto diagnostics = ShaderReflection::ReflectStage( spirv, ShaderStage::Fragment, data );
    ASSERT_TRUE( diagnostics.empty() ) << FirstOr( diagnostics, "" );

    const auto& set0 = data.ShaderDescriptorSets.at( 0 );
    ASSERT_EQ( set0.Image2DSamplers.size(), 1u );
    ASSERT_EQ( set0.ImageCubeSamplers.size(), 1u );
    EXPECT_EQ( set0.Image2DSamplers.at( 0 ).Name, "u_EnvCubeMap" );
    EXPECT_EQ( set0.ImageCubeSamplers.at( 1 ).Name, "u_Density" );
}

// A stage that declares an image the engine cannot bind must be reported by name and type, and must
// end up in NO bucket. Filing it under its base dimension is what the old rule effectively did.
TEST( ShaderReflection, RefusesArrayedImagesInsteadOfGuessing )
{
    const char* kArrayed = R"(#version 450
layout(binding = 0) uniform sampler2DArray u_Slices;
layout(location = 0) out vec4 o_Color;
void main() { o_Color = texture(u_Slices, vec3(0.5)); }
)";

    const auto spirv = Compile( kArrayed, shaderc_glsl_fragment_shader );
    ASSERT_FALSE( spirv.empty() );

    ShaderResource::ReflectionData data;
    const auto diagnostics = ShaderReflection::ReflectStage( spirv, ShaderStage::Fragment, data );

    ASSERT_EQ( diagnostics.size(), 1u );
    EXPECT_NE( diagnostics.front().find( "u_Slices" ), std::string::npos ) << diagnostics.front();
    EXPECT_NE( diagnostics.front().find( "sampler2DArray" ), std::string::npos ) << diagnostics.front();

    for ( const auto& [set, descriptorSet] : data.ShaderDescriptorSets )
    {
        EXPECT_TRUE( descriptorSet.Image2DSamplers.empty() ) << "set " << set;
        EXPECT_TRUE( descriptorSet.Image3DSamplers.empty() ) << "set " << set;
        EXPECT_TRUE( descriptorSet.ImageCubeSamplers.empty() ) << "set " << set;
    }
}

// An ARRAY of samplers is a different thing from an arrayed image, and equally unbindable here: every
// descriptor-set layout the engine builds hardcodes descriptorCount = 1.
TEST( ShaderReflection, RefusesArraysOfDescriptors )
{
    const char* kDescriptorArray = R"(#version 450
layout(binding = 0) uniform sampler2D u_Textures[4];
layout(location = 0) out vec4 o_Color;
void main() { o_Color = texture(u_Textures[1], vec2(0.5)); }
)";

    const auto spirv = Compile( kDescriptorArray, shaderc_glsl_fragment_shader );
    ASSERT_FALSE( spirv.empty() );

    ShaderResource::ReflectionData data;
    const auto diagnostics = ShaderReflection::ReflectStage( spirv, ShaderStage::Fragment, data );

    ASSERT_EQ( diagnostics.size(), 1u );
    EXPECT_NE( diagnostics.front().find( "u_Textures" ), std::string::npos ) << diagnostics.front();

    // The only resource in the shader was refused, so no set was ever touched.
    EXPECT_TRUE( data.ShaderDescriptorSets.empty() );
}

// Storage 2D and storage cube deliberately share one bucket — a storage binding is written from a mip
// view that already carries its own view type. Asserted so the sharing stays a decision, not a
// leftover: the IBL compute chain (PanoramaToCubemap, DiffuseIrradiance, PrefilterEnvMap) depends on
// an `imageCube` output continuing to land exactly where it lands today.
TEST( ShaderReflection, StorageCubeSharesTheStorage2DBucket )
{
    const char* kCubeOutput = R"(#version 450
layout(set = 0, binding = 0) uniform samplerCube inputTexture;
layout(set = 0, binding = 1, rgba32f) restrict writeonly uniform imageCube outputTexture;
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
void main()
{
    imageStore(outputTexture, ivec3(gl_GlobalInvocationID),
               texture(inputTexture, vec3(0.0, 1.0, 0.0)));
}
)";

    const auto spirv = Compile( kCubeOutput, shaderc_glsl_compute_shader );
    ASSERT_FALSE( spirv.empty() );

    ShaderResource::ReflectionData data;
    const auto diagnostics = ShaderReflection::ReflectStage( spirv, ShaderStage::Compute, data );
    ASSERT_TRUE( diagnostics.empty() ) << FirstOr( diagnostics, "" );

    const auto& set0 = data.ShaderDescriptorSets.at( 0 );
    ASSERT_EQ( set0.StorageImage2DSamplers.size(), 1u );
    EXPECT_EQ( set0.StorageImage2DSamplers.at( 1 ).Name, "outputTexture" );
    EXPECT_TRUE( set0.StorageImage3DSamplers.empty() );

    // ...while the sampled cube DOES move: `inputTexture` names neither "Env" nor "Cube", so the old
    // rule filed this real samplerCube as a 2D sampler. This is PrefilterEnvMap.shader, verbatim.
    ASSERT_EQ( set0.ImageCubeSamplers.size(), 1u );
    EXPECT_EQ( set0.ImageCubeSamplers.at( 0 ).Name, "inputTexture" );
    EXPECT_TRUE( set0.Image2DSamplers.empty() );
}

TEST( ShaderReflection, MergesStageMasksAcrossStages )
{
    const char* kVertex   = R"(#version 450
layout(binding = 0) uniform sampler2D u_Shared;
void main() { gl_Position = texture(u_Shared, vec2(0.5)); }
)";
    const char* kFragment = R"(#version 450
layout(binding = 0) uniform sampler2D u_Shared;
layout(location = 0) out vec4 o_Color;
void main() { o_Color = texture(u_Shared, vec2(0.5)); }
)";

    ShaderResource::ReflectionData data;
    ASSERT_TRUE( ShaderReflection::ReflectStage( Compile( kVertex, shaderc_glsl_vertex_shader ),
                                                 ShaderStage::Vertex, data )
                      .empty() );
    ASSERT_TRUE( ShaderReflection::ReflectStage( Compile( kFragment, shaderc_glsl_fragment_shader ),
                                                 ShaderStage::Fragment, data )
                      .empty() );

    const auto& set0 = data.ShaderDescriptorSets.at( 0 );
    ASSERT_EQ( set0.Image2DSamplers.size(), 1u ); // one descriptor, not one per stage

    const uint32_t stages = (uint32_t)set0.Image2DSamplers.at( 0 ).ShaderStage;
    EXPECT_TRUE( stages & (uint32_t)ShaderStage::Vertex );
    EXPECT_TRUE( stages & (uint32_t)ShaderStage::Fragment );
}

TEST( ShaderReflection, ClassifiesDimensionsDirectly )
{
    const auto spirv = Compile( kAllFiveImageKinds, shaderc_glsl_compute_shader );
    ASSERT_FALSE( spirv.empty() );

    spirv_cross::CompilerGLSL compiler( spirv );
    const auto                resources = compiler.get_shader_resources();

    for ( const auto& resource : resources.sampled_images )
    {
        const auto& type      = compiler.get_type( resource.base_type_id );
        const auto  kind      = ShaderReflection::ClassifyImage( type );
        const auto  described = ShaderReflection::DescribeImageType( type );

        if ( resource.name == "u_ShapeNoise" )
        {
            EXPECT_EQ( kind, ShaderReflection::ImageKind::Image3D );
            EXPECT_EQ( described, "sampler3D" );
        }
        else if ( resource.name == "u_Radiance" )
        {
            EXPECT_EQ( kind, ShaderReflection::ImageKind::ImageCube );
            EXPECT_EQ( described, "samplerCube" );
        }
        else
        {
            EXPECT_EQ( kind, ShaderReflection::ImageKind::Image2D );
            EXPECT_EQ( described, "sampler2D" );
        }
    }

    for ( const auto& resource : resources.storage_images )
    {
        const auto& type = compiler.get_type( resource.base_type_id );
        EXPECT_EQ( ShaderReflection::DescribeImageType( type ),
                   resource.name == "u_OutVolume" ? "image3D" : "image2D" );
    }
}

// ─── One descriptor slot holds one resource ───────────────────────────────────────────────────────
//
// Г17. The engine has an automatic binding allocator in its shader DSL (DShaderParser's
// TranslateLayoutSugar): `Uniform Name {}` with no number takes the lowest slot not already taken. It
// learns which slots are taken by scanning ONE TEXT for `binding = <digits>` — and that is a
// recognizer, not a census. It cannot see a binding spelled as a MACRO (three shipped headers do
// exactly that, e.g. Common/CloudAuthored.glslh's `binding = CLOUD_AUTHORED_BUFFER_BINDING`) and it
// cannot see one declared in an INCLUDED file at all, because every `.glslh` is translated in its own
// separate call. So it can hand out an occupied number, and nothing downstream used to notice:
//
//   * glslang compiles two resources on one Binding with no diagnostic — measured with
//     `glslc -Werror --target-env=vulkan1.1` on 2026-09-08, both Binding decorations present in the
//     disassembly;
//   * the reflection's buckets are keyed BY BINDING, so the second resource simply overwrote the
//     first and the descriptor layout that came out was complete, plausible, and short one resource.
//
// THE RELATION, and it is the point of these three tests: the number of descriptor-bound resources the
// SPIR-V declares must equal the number of slots the layout claims. Both sides look right on their own
// — the module is valid, the layout is well-formed — and they disagree by exactly the resource that
// was lost. Asserting it here is what makes the loss impossible to spell, whatever syntax produced it.
namespace
{
    // How many resources in the module consume a descriptor slot — the left-hand side of the relation,
    // read from the SPIR-V rather than from the reflection under test.
    size_t DescriptorBoundResourceCount( const std::vector<uint32_t>& spirv )
    {
        spirv_cross::CompilerGLSL compiler( spirv );
        const auto                resources = compiler.get_shader_resources();
        return resources.uniform_buffers.size() + resources.sampled_images.size() +
               resources.storage_buffers.size() + resources.storage_images.size();
    }

    size_t LayoutSlotCount( const ShaderResource::ReflectionData& data )
    {
        size_t total = 0;
        for ( const auto& [set, descriptorSet] : data.ShaderDescriptorSets )
            total += ShaderReflection::BuildLayoutBindings( descriptorSet ).size();
        return total;
    }
} // namespace

// Two resources of the SAME kind on one binding: the pure form of the loss, because both land in one
// bucket and the map keeps whichever was written last.
TEST( ShaderReflection, RefusesTwoStorageBuffersOnOneBinding )
{
    // Written the way the shipped headers write it, so the test fails for the reason the tree can
    // actually produce: an explicit binding the DSL's digit scan cannot read, and a second resource on
    // the number that scan therefore believes is free.
    const char* kMacroThenAuto = R"(#version 450
#define CLOUD_AUTHORED_BUFFER_BINDING 0
layout(std430, binding = CLOUD_AUTHORED_BUFFER_BINDING) readonly buffer Authored { vec4 a[]; };
layout(std430, binding = 0) buffer Output { vec4 o[]; };
layout(local_size_x = 8) in;
void main() { o[0] = a[0]; }
)";

    const auto spirv = Compile( kMacroThenAuto, shaderc_glsl_compute_shader );
    ASSERT_FALSE( spirv.empty() ) << "the compiler refused it, so this test is no longer about silence";
    ASSERT_EQ( DescriptorBoundResourceCount( spirv ), 2u );

    ShaderResource::ReflectionData data;
    const auto diagnostics = ShaderReflection::ReflectStage( spirv, ShaderStage::Compute, data );

    ASSERT_EQ( diagnostics.size(), 1u ) << FirstOr( diagnostics, "no diagnostic at all" );
    EXPECT_NE( diagnostics.front().find( "Authored" ), std::string::npos ) << diagnostics.front();
    EXPECT_NE( diagnostics.front().find( "Output" ), std::string::npos ) << diagnostics.front();
    EXPECT_NE( diagnostics.front().find( "binding 0" ), std::string::npos ) << diagnostics.front();

    // And the reason the refusal has to happen HERE: the layout on its own cannot say anything is
    // wrong. Two declared resources, one slot — the shape a reviewer of either side would approve.
    EXPECT_EQ( LayoutSlotCount( data ), 1u )
         << "a same-bucket collision no longer loses a resource; if that changed, this test is asserting"
            " the wrong half of the relation";
}

// Two resources of DIFFERENT kinds on one binding. Nothing is lost this time — and it is worse: the
// layout keeps both, with the same binding number, which is not a VkDescriptorSetLayout Vulkan will
// accept. The relation holds and the result is still invalid, so the count alone is not the guard.
TEST( ShaderReflection, RefusesAUniformBufferAndASamplerOnOneBinding )
{
    const char* kCrossBucket = R"(#version 450
#define FOG_PARAMS_BINDING 3
layout(binding = FOG_PARAMS_BINDING) uniform FogParams { vec4 u_Fog; };
layout(binding = 3) uniform sampler2D u_Depth;
layout(location = 0) out vec4 o_Color;
void main() { o_Color = u_Fog + texture(u_Depth, vec2(0.5)); }
)";

    const auto spirv = Compile( kCrossBucket, shaderc_glsl_fragment_shader );
    ASSERT_FALSE( spirv.empty() );

    ShaderResource::ReflectionData data;
    const auto diagnostics = ShaderReflection::ReflectStage( spirv, ShaderStage::Fragment, data );

    ASSERT_EQ( diagnostics.size(), 1u ) << FirstOr( diagnostics, "no diagnostic at all" );
    EXPECT_NE( diagnostics.front().find( "FogParams" ), std::string::npos ) << diagnostics.front();
    EXPECT_NE( diagnostics.front().find( "u_Depth" ), std::string::npos ) << diagnostics.front();
    EXPECT_NE( diagnostics.front().find( "binding 3" ), std::string::npos ) << diagnostics.front();

    const auto bindings = ShaderReflection::BuildLayoutBindings( data.ShaderDescriptorSets.at( 0 ) );
    ASSERT_EQ( bindings.size(), 2u );
    EXPECT_EQ( bindings[0].binding, bindings[1].binding )
         << "two descriptors on one binding number — vkCreateDescriptorSetLayout rejects this, which is"
            " why the refusal above must happen before it is ever built";
}

// The other direction, and the one a naive check gets wrong: a binding number is only claimed WITHIN
// its set, and a resource shared by two stages is one resource seen twice, not two.
TEST( ShaderReflection, ADescriptorSetAndAStageBoundaryAreNotCollisions )
{
    const char* kTwoSets     = R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D u_Albedo;
layout(set = 1, binding = 0) uniform sampler2D u_Normal;
layout(location = 0) out vec4 o_Color;
void main() { o_Color = texture(u_Albedo, vec2(0.5)) + texture(u_Normal, vec2(0.5)); }
)";
    const char* kVertexShare = R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D u_Albedo;
void main() { gl_Position = texture(u_Albedo, vec2(0.5)); }
)";

    ShaderResource::ReflectionData data;
    const auto twoSets = ShaderReflection::ReflectStage( Compile( kTwoSets, shaderc_glsl_fragment_shader ),
                                                         ShaderStage::Fragment, data );
    EXPECT_TRUE( twoSets.empty() ) << FirstOr( twoSets, "" );

    // The same set-0 binding-0 sampler again, from a second stage: the merge path, which must stay silent.
    const auto shared = ShaderReflection::ReflectStage( Compile( kVertexShare, shaderc_glsl_vertex_shader ),
                                                        ShaderStage::Vertex, data );
    EXPECT_TRUE( shared.empty() ) << FirstOr( shared, "" );

    EXPECT_EQ( LayoutSlotCount( data ), 2u );
}

// The same loss across a STAGE boundary, which the two tests above cannot reach: each stage is valid on
// its own and each is reflected by its own call, so the collision exists only in the merged data — and
// the merge is exactly what a pipeline layout is built from.
TEST( ShaderReflection, RefusesTwoStagesNamingDifferentResourcesOnOneSlot )
{
    const char* kVertex   = R"(#version 450
layout(binding = 0) uniform Transform { mat4 u_ViewProjection; };
void main() { gl_Position = u_ViewProjection[0]; }
)";
    const char* kFragment = R"(#version 450
layout(binding = 0) uniform sampler2D u_Albedo;
layout(location = 0) out vec4 o_Color;
void main() { o_Color = texture(u_Albedo, vec2(0.5)); }
)";

    ShaderResource::ReflectionData data;
    const auto first = ShaderReflection::ReflectStage( Compile( kVertex, shaderc_glsl_vertex_shader ),
                                                       ShaderStage::Vertex, data );
    ASSERT_TRUE( first.empty() ) << FirstOr( first, "" );

    const auto second = ShaderReflection::ReflectStage( Compile( kFragment, shaderc_glsl_fragment_shader ),
                                                        ShaderStage::Fragment, data );
    ASSERT_EQ( second.size(), 1u ) << FirstOr( second, "no diagnostic at all" );
    EXPECT_NE( second.front().find( "Transform" ), std::string::npos ) << second.front();
    EXPECT_NE( second.front().find( "u_Albedo" ), std::string::npos ) << second.front();
}

// VERTEX INPUT = LAYOUT ∩ WHAT THE VERTEX STAGE READS. The static mesh layout feeds 0..4 on binding 0 and
// colour/UV1 at 7/8 on binding 1; a shadow-like stage reads position only, a surface stage reads 7 and 8.
// Before this, every mesh pipeline described 7/8 whatever its shader read, and the validation layer said
// "Vertex attribute at location 7/8 not consumed by vertex shader" for shadow, silhouette, glass, wireframe.
namespace
{
    const char* kPositionOnlyVertex = R"(#version 450
layout(location = 0) in vec3 a_Position;
void main() { gl_Position = vec4(a_Position, 1.0); }
)";
    const char* kStreamsVertex      = R"(#version 450
layout(location = 0) in vec3 a_Position;
layout(location = 4) in vec2 a_TextureCoord;
layout(location = 7) in vec4 a_Color;
layout(location = 8) in vec2 a_TexCoord1;
layout(location = 0) out vec4 v_Color;
void main() { v_Color = a_Color + vec4(a_TexCoord1 + a_TextureCoord, 0.0, 0.0); gl_Position = vec4(a_Position, 1.0); }
)";
} // namespace

TEST( ShaderReflection, VertexStageRecordsItsInputLocations )
{
    ShaderResource::ReflectionData data;
    ASSERT_TRUE( ShaderReflection::ReflectStage( Compile( kStreamsVertex, shaderc_glsl_vertex_shader ),
                                                 ShaderStage::Vertex, data )
                      .empty() );
    EXPECT_EQ( data.VertexInputLocations, ( std::vector<uint32_t>{ 0, 4, 7, 8 } ) );
}

TEST( ShaderReflection, VertexInputDropsStreamsTheStageDoesNotRead )
{
    const auto layout = Desert::Graphic::MeshVertexLayout( Desert::Graphic::MeshVertexPath::Static );
    const auto state =
         ShaderReflection::BuildVertexInput( layout, ShaderReflection::ReflectVertexInputLocations( Compile(
                                                          kPositionOnlyVertex, shaderc_glsl_vertex_shader ) ) );

    EXPECT_TRUE( state.Errors.empty() ) << state.Errors.front();
    ASSERT_EQ( state.Attributes.size(), 1u );
    EXPECT_EQ( state.Attributes[0].location, 0u );
    EXPECT_FALSE( state.HasLocation( 7 ) );
    EXPECT_FALSE( state.HasLocation( 8 ) );
    EXPECT_TRUE( state.HasBinding( 0 ) ); // the draw binds the mesh there regardless
    EXPECT_FALSE( state.HasBinding( 1 ) );
}

TEST( ShaderReflection, VertexInputKeepsStreamsTheStageReads )
{
    const auto layout = Desert::Graphic::MeshVertexLayout( Desert::Graphic::MeshVertexPath::Static );
    const auto state  = ShaderReflection::BuildVertexInput(
         layout,
         ShaderReflection::ReflectVertexInputLocations( Compile( kStreamsVertex, shaderc_glsl_vertex_shader ) ) );

    EXPECT_TRUE( state.Errors.empty() ) << state.Errors.front();
    ASSERT_EQ( state.Attributes.size(), 4u );
    EXPECT_TRUE( state.HasLocation( 4 ) );
    EXPECT_FALSE( state.HasLocation( 1 ) );
    ASSERT_TRUE( state.HasBinding( 1 ) );
    for ( const auto& attribute : state.Attributes )
    {
        EXPECT_EQ( attribute.binding,
                   attribute.location >= Desert::Graphic::kMeshVertexStreamFirstLocation ? 1u : 0u )
             << "location " << attribute.location;
    }
    for ( const auto& binding : state.Bindings )
    {
        EXPECT_EQ( binding.stride, binding.binding == 1 ? layout.GetStreamStride() : layout.GetStride() );
    }
}

TEST( ShaderReflection, VertexInputNamesALocationTheLayoutDoesNotFeed )
{
    const char* kReadsFive = R"(#version 450
layout(location = 5) in vec4 a_Unfed;
void main() { gl_Position = a_Unfed; }
)";
    const auto  layout     = Desert::Graphic::MeshVertexLayout( Desert::Graphic::MeshVertexPath::Static );
    const auto  state      = ShaderReflection::BuildVertexInput(
         layout,
         ShaderReflection::ReflectVertexInputLocations( Compile( kReadsFive, shaderc_glsl_vertex_shader ) ) );

    ASSERT_EQ( state.Errors.size(), 1u );
    EXPECT_NE( state.Errors.front().find( "location 5" ), std::string::npos ) << state.Errors.front();
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
