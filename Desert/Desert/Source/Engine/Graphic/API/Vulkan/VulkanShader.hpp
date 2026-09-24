#pragma once

#include <Engine/Graphic/Shader.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanDescriptorSetLayout.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanShaderResource.hpp>

#include <Engine/Core/ShaderCompiler/ShaderMapCache.hpp>

#include <vulkan/vulkan.h>

namespace Desert::Graphic::API::Vulkan
{
    enum class WriteDescriptorType
    {
        Uniform = 0,
        Sampler2D,
        SamplerCube,
        StorageImage
    };

    class VulkanShader final : public Shader
    {
    public:
        struct DescriptorSetInfo
        {
            std::vector<VkDescriptorPool>             Pool;
            std::vector<std::vector<VkDescriptorSet>> DescriptorSets; // frame -> set
        };

    public:
        VulkanShader( const Assets::Asset<Assets::ShaderAsset>& asset, const ShaderVariant& variant,
                      const std::string& passName = {} );
        ~VulkanShader();

        virtual Common::BoolResultStr Reload() override;
        virtual const std::string     GetName() const override
        {
            return m_ShaderName;
        }

        virtual const std::vector<ShaderResources::ShaderLayout::UniformBuffer>
        GetUniformBufferModels() const override; // don't use it often! TODO: cache
        virtual const std::vector<ShaderResources::ShaderLayout::StorageBuffer>
        GetStorageBufferModels() const override; // don't use it often! TODO: cache
        virtual const std::vector<ShaderResources::ShaderLayout::ImageCubeSampler> GetUniformImageCubeModels() const override;
        virtual const std::vector<ShaderResources::ShaderLayout::Image2DSampler>   GetUniformImage2DModels() const override;

        virtual const Common::Filepath& GetFilepath() const override
        {
            return m_ShaderPath;
        }

        virtual const Core::Formats::ShaderProgramMeta& GetProgramMeta() const override
        {
            return m_ProgramMeta;
        }

        const std::vector<VkPipelineShaderStageCreateInfo>& GetPipelineShaderStageCreateInfos() const
        {
            return m_PipelineShaderStageCreateInfos;
        }

        /**
         * This program's COMPUTE stage, or nullptr when it has none.
         *
         * A POINTER BECAUSE "NONE" IS A STATE THIS OBJECT REALLY REACHES, and the line this replaces was
         * `GetPipelineShaderStageCreateInfos()[0]` in VulkanPipelineCompute::Invalidate — a subscript of
         * a vector that a failed first compile leaves EMPTY. It killed the editor with SIGSEGV after a
         * validation storm about a descriptor pool of size zero, for no worse a cause than a typo in a
         * shader graph.
         *
         * It also answers a question `[0]` silently got wrong even when the vector was full: the first
         * stage of a GRAPHICS program is a vertex stage, and feeding that to vkCreateComputePipelines is
         * a different failure with the same shape. Searching by stage bit is what makes "this name is
         * not a compute program" a refusal instead of undefined behaviour.
         */
        [[nodiscard]] const VkPipelineShaderStageCreateInfo* GetComputeStage() const
        {
            for ( const auto& stage : m_PipelineShaderStageCreateInfos )
            {
                if ( stage.stage == VK_SHADER_STAGE_COMPUTE_BIT )
                    return &stage;
            }
            return nullptr;
        }

        /**
         * This program's VERTEX stage, or nullptr when it has none — the mirror of GetComputeStage, and
         * it exists because the mistake is symmetric.
         *
         * A graphics pipeline built from a program whose only stage is a COMPUTE stage is handed to
         * vkCreateGraphicsPipelines with a stage bit that call does not accept. Nothing about the
         * spec says so: both sides are a Shader that compiled, and both call sites reach their
         * program BY NAME through ShaderService::GetByName, where one typo swaps a compute program for
         * a graphics one. Asking for the stage that a graphics pipeline is REQUIRED to have (Vulkan
         * spec: a graphics pipeline without mesh shading must include a vertex stage) is what turns
         * that into a refusal instead of a driver-dependent failure.
         */
        [[nodiscard]] const VkPipelineShaderStageCreateInfo* GetVertexStage() const
        {
            for ( const auto& stage : m_PipelineShaderStageCreateInfos )
            {
                if ( stage.stage == VK_SHADER_STAGE_VERTEX_BIT )
                    return &stage;
            }
            return nullptr;
        }

        // No stages means CompileProgram never succeeded — it is transactional, so a shader that has ever
        // compiled keeps its modules even if a later recompile fails. Reading the stage list rather than
        // a separate bool keeps this from becoming a second piece of state that can disagree with the
        // first: the list IS what a pipeline would be built from.
        [[nodiscard]] virtual bool IsCompiled() const override
        {
            return !m_PipelineShaderStageCreateInfos.empty();
        }
        /**
         * The layout for @p set, as a STRONG reference.
         *
         * Every caller is expected to keep it for as long as it keeps whatever it builds from it — a
         * pipeline layout, a descriptor pool, an allocated set. A recompile replaces this shader's
         * references; it does not reach into objects that are still standing on the old ones. See
         * VulkanDescriptorSetLayout.hpp for the failure this arrangement exists to make impossible.
         */
        DescriptorSetLayoutRef GetDescriptorSetLayout( uint32_t set ) const
        {
            return set < m_DescriptorSetLayouts.size() ? m_DescriptorSetLayouts[set] : nullptr;
        }

        auto GetDescriptorSetLayoutCount() const
        {
            return m_DescriptorSetLayouts.size();
        }

        const std::vector<DescriptorSetLayoutRef>& GetAllDescriptorSetLayouts() const
        {
            return m_DescriptorSetLayouts;
        }

        /**
         * Bumped by every successful recompile.
         *
         * What it is for: an object built from this shader records the generation it was built at, and
         * a later mismatch means "you are running code this shader no longer contains". That is a
         * legitimate state — a hot reload cannot reach a pipeline the renderer built and owns — but it
         * is never a silent one.
         */
        uint32_t GetReloadGeneration() const
        {
            return m_ReloadGeneration;
        }

        auto& GetShaderDescriptorSets()
        {
            return m_ReflectionData.ShaderDescriptorSets;
        }

        auto& GetShaderPushConstant()
        {
            return m_ReflectionData.PushConstantRanges;
        }

        auto& GetVulkanDescriptorSetInfo() const
        {
            return m_DescriptorSetInfo;
        }

        // RETURNS THE MEMBER, and the member exists because of what `return {};` did here. That built a
        // temporary, bound the returned reference to it, and destroyed it before the caller could look —
        // every read of this getter was a read of freed memory. ShaderLibraryPanel does that read on
        // every frame a shader node is expanded, then iterates the "vector" it got back.
        virtual const ShaderVariant& GetVariant() const override
        {
            return m_Variant;
        }

    private:
        // Fails when a stage declares an image resource the engine cannot bind: reflection refuses to
        // register it, so the descriptor layout would silently lack the binding. Better to lose the
        // shader with a named reason than to bind something of the wrong shape.
        Common::BoolResultStr Reflect( VkShaderStageFlagBits flag, const std::vector<uint32_t>& spirvBinary,
                                       ShaderResource::ReflectionData& into );
        Common::BoolResultStr CreateDescriptorsLayout();

        Common::BoolResultStr BuildFromSpirv( const std::vector<Core::ShaderMapStage>& stages );

    private:
        const std::weak_ptr<Assets::ShaderAsset> m_ShaderAsset;

    private:
        std::vector<VkPipelineShaderStageCreateInfo> m_PipelineShaderStageCreateInfos;
        std::vector<VkShaderModule>                  m_ShaderModules;
        std::filesystem::path                        m_ShaderPath;
        std::string                                  m_ShaderName;
        std::string                                  m_PassName; // empty = default program

        // STORED **AND** HONOURED, which is the half its predecessor never had: this reaches
        // CompileProgram below, and through it the includer and the SPIR-V cache key. A variant that
        // were only stored would be a knob that reports itself and changes nothing — exactly what the
        // ShaderDefines member it replaces was.
        ShaderVariant m_Variant;

        Core::Formats::ShaderProgramMeta             m_ProgramMeta;

        ShaderResource::ReflectionData      m_ReflectionData;
        std::vector<DescriptorSetLayoutRef> m_DescriptorSetLayouts; // indexed by set

        // Monotonic; 0 means "never compiled". Never reset, so a comparison against a recorded value
        // stays meaningful for the life of the process.
        uint32_t m_ReloadGeneration = 0;

        DescriptorSetInfo m_DescriptorSetInfo;
    };

} // namespace Desert::Graphic::API::Vulkan