#pragma once

// SPIR-V reflection, separated from VulkanShader so it can run with NO VkDevice.
//
// The separation is not cosmetic. Image resources used to be sorted into descriptor buckets by a
// substring of their VARIABLE NAME ("Env"/"Cube" meant a cube, anything else meant 2D), so a
// `sampler3D` was registered as a 2D combined image sampler and a `writeonly image3D` as a 2D storage
// image. Neither fails: the descriptor type is right, only the view type is wrong, so the shader
// simply reads garbage. That class of bug is invisible without a device — unless the classification
// itself can be exercised without one, which is what this translation unit exists for. Everything
// here is a free function over a SPIR-V binary; the unit test in Desert/Tests/Engine/ShaderReflection
// compiles GLSL, runs ReflectStage and asserts the buckets, on a machine with no Vulkan at all.

#include <Engine/Core/Formats/MaterialLayout.hpp>
#include <Engine/Core/ShaderCompiler/ShaderMapCache.hpp>
#include <Engine/Core/Formats/Shader.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanShaderResource.hpp>
#include <Engine/Graphic/VertexBuffer.hpp>

#include <spirv_cross/spirv_glsl.hpp>

#include <optional>
#include <string>
#include <vector>

namespace Desert::Graphic::API::Vulkan::ShaderReflection
{
    // The image bindings this engine can actually build a descriptor for. Anything a shader can
    // declare but the engine cannot bind (1D, arrayed, multisampled, buffer, subpass input, or an
    // array OF descriptors) is Unsupported — reported by name, never guessed into a bucket.
    enum class ImageKind
    {
        Image2D,
        Image3D,
        ImageCube,
        Unsupported
    };

    // Pure. `type` is the type of a `sampled_images` / `storage_images` resource — pass the type of
    // resource.base_type_id, which is the image type itself even when the resource is an array.
    ImageKind ClassifyImage( const spirv_cross::SPIRType& type );

    // The GLSL spelling of what SPIR-V actually declared ("sampler3D", "image2DArray", ...), so a
    // rejection names the thing the author wrote instead of a numeric dimension.
    std::string DescribeImageType( const spirv_cross::SPIRType& type );

    // Reflects ONE stage's SPIR-V into `data`, merging with what earlier stages already contributed
    // (a resource shared by two stages keeps one entry whose ShaderStage mask gains this stage).
    //
    // Returns one message per resource it REFUSED to register, each naming the resource and its type.
    // A refused resource gets no descriptor-layout binding, so the pipeline it belongs to is invalid
    // by construction — which is the point: the caller reports it and fails, rather than binding
    // something of the wrong shape and rendering nonsense.
    [[nodiscard]] std::vector<std::string> ReflectStage( const std::vector<uint32_t>&    spirv,
                                                         Core::Formats::ShaderStage      stage,
                                                         ShaderResource::ReflectionData& data );

    // What ONE stage's SPIR-V says about the material layout: the `Materials` row (binding, stride and
    // every member of one row, padding included), the combined image samplers (name -> binding) and the
    // push block (size and every member). Pure. Core::Formats::ReconcileMaterialLayout holds these to the
    // template's layout and to each other — the witnesses behind Core/Formats/MaterialLayout.hpp.
    Core::Formats::ReflectedMaterialStage ReflectMaterialStage( const std::vector<uint32_t>& spirv,
                                                                Core::Formats::ShaderStage   stage );

    // ONE CELL'S LAYOUT, as a program load takes it: BuildMaterialLayout from the metadata, every stage
    // reflected (ReflectMaterialStage) and held to it by ReconcileMaterialLayout. Non-empty Errors = the
    // cell does not load; each names `templateName`/`cellName`. Pure, so a test refuses a cell the way
    // VulkanShader::BuildFromSpirv does, with no device.
    struct ReconciledCellLayout
    {
        Core::Formats::MaterialLayout Layout;
        std::vector<std::string>      Errors;
    };
    ReconciledCellLayout ReconcileCellLayout( const Core::Formats::ShaderProgramMeta&  meta,
                                              const std::vector<Core::ShaderMapStage>& stages,
                                              std::string_view templateName, std::string_view cellName );

    /**
     * The descriptor-set layout bindings one reflected set turns into, sorted by binding number.
     *
     * Pure, and deliberately here rather than inside VulkanShader: this is the SHAPE a pipeline layout
     * and a descriptor set have to agree on, and when they disagree the validation layer reports it as
     * a count ("has 8 total descriptors, but ... has 9"). A device-free function is one a test can
     * evaluate for a real shader on a machine with no Vulkan, which is the only way that count is
     * checkable here at all.
     */
    std::vector<VkDescriptorSetLayoutBinding>
    BuildLayoutBindings( const ShaderResource::ShaderDescriptorSet& set );

    /** Total descriptors across @p bindings — the number the validation layer compares. */
    uint32_t CountDescriptors( const std::vector<VkDescriptorSetLayoutBinding>& bindings );

    /**
     * The input locations ONE vertex stage's SPIR-V declares, sorted and unique. Built-ins
     * (gl_VertexIndex, gl_InstanceIndex) are not locations and are not listed; a matrix or an array
     * input occupies one location per column and per element. ReflectStage stores this for the vertex
     * stage in ReflectionData::VertexInputLocations.
     */
    std::vector<uint32_t> ReflectVertexInputLocations( const std::vector<uint32_t>& spirv );

    /** The Vulkan format one vertex attribute of @p type is read as; VK_FORMAT_UNDEFINED = no mapping. */
    VkFormat VertexAttributeFormat( ShaderDataType type );

    /**
     * A graphics pipeline's vertex input: the vertex layout INTERSECTED with what the vertex stage reads.
     *
     * The layout (MeshVertexLayout) describes every stream a mesh may carry — binding 0 always, binding 1
     * (colour, UV1) when it has streams. A shader that does not read a location must not be handed an
     * attribute for it: that is the validation layer's "Vertex attribute at location N not consumed by
     * vertex shader", once per pipeline (shadow, silhouette, glass, wireframe did not read 7/8). So an
     * attribute is kept only for a location the stage declares, and binding 1 is kept only when at least
     * one of its attributes is — RenderMesh binds a buffer there only then (HasBinding(1)). Binding 0 is
     * always described: every draw binds the mesh's vertex buffer there, and a binding no attribute reads
     * is legal and silent.
     *
     * The other direction is an error, not a filter: a location the stage reads that the layout does not
     * feed is undefined input, named in Errors. So is an element type with no Vulkan format. Pure — the
     * ShaderReflection suite evaluates it for compiled GLSL on a machine with no Vulkan.
     */
    struct VertexInputState
    {
        std::vector<VkVertexInputBindingDescription>   Bindings;
        std::vector<VkVertexInputAttributeDescription> Attributes;
        std::vector<std::string>                       Errors;

        [[nodiscard]] bool HasBinding( uint32_t binding ) const;
        [[nodiscard]] bool HasLocation( uint32_t location ) const;
    };
    VertexInputState BuildVertexInput( const VertexBufferLayout&    layout,
                                       const std::vector<uint32_t>& consumedLocations );

    /**
     * Why a pipeline with this vertex input must not be built, or nothing when it may (UE: a shader reading an
     * attribute its vertex factory does not provide fails the shader map; it is not drawn with garbage input).
     * One message carrying every reason, so the refusal is logged once. Pure, like BuildVertexInput.
     */
    [[nodiscard]] std::optional<std::string> VertexInputRefusal( const VertexInputState& state );

} // namespace Desert::Graphic::API::Vulkan::ShaderReflection
