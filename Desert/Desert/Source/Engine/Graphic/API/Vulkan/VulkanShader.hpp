#pragma once

#include <Engine/Graphic/Shader.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanDescriptorSetLayout.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanShaderResource.hpp>

#include <Engine/Core/ShaderCompiler/ShaderMapCache.hpp>

#include <vulkan/vulkan.h>

#include <atomic>
#include <memory>

namespace Desert::Graphic::API::Vulkan
{
    struct RdgSlotTable; // VulkanRdgPassBindings.hpp

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

        /**
         * ONE COMPILED PROGRAM, IMMUTABLE ONCE PUBLISHED: the modules, the stage infos that name them, the
         * reflection, the slot table and the set layouts built from it, the metadata and the material layout.
         *
         * A reload builds a NEW record and publishes it; it never writes into the one readers see. Render
         * workers record frame N-1 while the game thread hot-reloads in frame N (OVERLAP-ON-7), and every
         * accessor below hands out a reference into the record - so the record a reader may hold is parked
         * when it is replaced and destroyed only once every frame in flight has retired
         * (Graphic::RetireReplacedShaderPrograms; UE: a shader map swapped on the game thread, the old one
         * released by deferred deletion). The modules die with their record and with nothing else.
         */
        struct CompiledProgram
        {
            std::vector<VkPipelineShaderStageCreateInfo> StageInfos;
            std::vector<VkShaderModule>                  Modules;
            Core::Formats::ShaderProgramMeta             Meta;
            Core::Formats::MaterialLayout                MaterialLayout;
            ShaderResource::ReflectionData               Reflection;
            // Built from Reflection in the same record, so the two never describe different modules.
            std::shared_ptr<const RdgSlotTable> RdgSlots;
            std::vector<DescriptorSetLayoutRef> SetLayouts; // indexed by set

            CompiledProgram()                                    = default;
            CompiledProgram( const CompiledProgram& )            = delete;
            CompiledProgram& operator=( const CompiledProgram& ) = delete;
            ~CompiledProgram(); // destroys Modules
        };

    public:
        VulkanShader( const Assets::Asset<Assets::ShaderAsset>& asset, const ShaderVariant& variant,
                      const std::string& passName = {} );
        // Shader::CreateFromShaderMap: the CPU half already built (@p built), only the device half runs here.
        VulkanShader( const Assets::Asset<Assets::ShaderAsset>& asset, const ShaderVariant& variant,
                      const std::string& passName, const Core::ShaderMap& built );
        ~VulkanShader();

        virtual Common::BoolResultStr Reload() override;
        virtual const std::string     GetName() const override
        {
            return m_ShaderName;
        }

        virtual const std::vector<ShaderResources::ShaderLayout::UniformBuffer>
        GetUniformBufferModels() const override; // flattened per call; read once per ShaderResourcesManager
        virtual const std::vector<ShaderResources::ShaderLayout::StorageBuffer>
        GetStorageBufferModels() const override; // flattened per call; read once per ShaderResourcesManager
        virtual const std::vector<ShaderResources::ShaderLayout::ImageCubeSampler> GetUniformImageCubeModels() const override;
        virtual const std::vector<ShaderResources::ShaderLayout::Image2DSampler>   GetUniformImage2DModels() const override;
        [[nodiscard]] bool SamplesTexture( std::string_view name ) const override;

        virtual const Common::Filepath& GetFilepath() const override
        {
            return m_ShaderPath;
        }

        virtual const Core::Formats::ShaderProgramMeta& GetProgramMeta() const override
        {
            return Current().Meta;
        }

        [[nodiscard]] const Core::Formats::MaterialLayout& GetMaterialLayout() const override
        {
            return Current().MaterialLayout;
        }

        const std::vector<VkPipelineShaderStageCreateInfo>& GetPipelineShaderStageCreateInfos() const
        {
            return Current().StageInfos;
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
            for ( const auto& stage : Current().StageInfos )
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
            for ( const auto& stage : Current().StageInfos )
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
            return !Current().StageInfos.empty();
        }
        [[nodiscard]] std::string_view GetDeviceRefusal() const override
        {
            return m_DeviceRefusal;
        }
        [[nodiscard]] std::string_view GetCompileError() const override
        {
            return m_CompileError;
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
            const auto& layouts = Current().SetLayouts;
            return set < layouts.size() ? layouts[set] : nullptr;
        }

        auto GetDescriptorSetLayoutCount() const
        {
            return Current().SetLayouts.size();
        }

        const std::vector<DescriptorSetLayoutRef>& GetAllDescriptorSetLayouts() const
        {
            return Current().SetLayouts;
        }

        const ShaderResource::ReflectionData& GetReflectionData() const
        {
            return Current().Reflection;
        }

        // RDG-PARALLEL-6: the resource slots of GetReflectionData(), built with it (never null once reflected):
        // pass bindings resolve by an index lookup, not by walking the reflection per dispatch.
        const std::shared_ptr<const RdgSlotTable>& GetRdgSlots() const
        {
            return Current().RdgSlots;
        }

        const auto& GetShaderDescriptorSets() const
        {
            return Current().Reflection.ShaderDescriptorSets;
        }

        const auto& GetShaderPushConstant() const
        {
            return Current().Reflection.PushConstantRanges;
        }

        // The input locations the vertex stage declares (ShaderReflection::ReflectVertexInputLocations).
        [[nodiscard]] const std::vector<uint32_t>& GetVertexInputLocations() const
        {
            return Current().Reflection.VertexInputLocations;
        }

        // The vertex stage's inputs by name and type (ShaderReflection::ReflectVertexInputs).
        [[nodiscard]] const std::vector<ShaderResource::ReflectionData::VertexInputDeclaration>&
        GetVertexInputs() const
        {
            return Current().Reflection.VertexInputs;
        }

        // The colour locations the fragment stage writes (ShaderReflection::ReflectFragmentOutputLocations).
        [[nodiscard]] const std::vector<uint32_t>& GetFragmentOutputLocations() const
        {
            return Current().Reflection.FragmentOutputLocations;
        }

        auto& GetVulkanDescriptorSetInfo() const
        {
            return m_DescriptorSetInfo;
        }

        // RETURNS THE MEMBER, and the member exists because of what `return {};` did here. That built a
        // temporary, bound the returned reference to it, and destroyed it before the caller could look —
        // every read of this getter was a read of freed memory.
        virtual const ShaderVariant& GetVariant() const override
        {
            return m_Variant;
        }

    private:
        [[nodiscard]] const CompiledProgram& Current() const noexcept
        {
            return *m_Current.load( std::memory_order_acquire );
        }
        // Swap @p next in for readers; the record it replaces is parked until the frames in flight retire.
        void Publish( std::shared_ptr<const CompiledProgram> next ) noexcept;
        [[nodiscard]] static const std::shared_ptr<const CompiledProgram>& NeverCompiled() noexcept;

        // Fails when a stage declares an image resource the engine cannot bind: reflection refuses to
        // register it, so the descriptor layout would silently lack the binding. Better to lose the
        // shader with a named reason than to bind something of the wrong shape.
        Common::BoolResultStr Reflect( VkShaderStageFlagBits flag, const std::vector<uint32_t>& spirvBinary,
                                       ShaderResource::ReflectionData& into );
        // Builds into @p layouts, never into the shader: it runs before the compile is committed, so a stage
        // over a per-stage limit leaves the shader uncompiled (IsCompiled) and no pipeline is ever built on it.
        Common::BoolResultStr CreateDescriptorsLayout( const ShaderResource::ReflectionData& reflection,
                                                       std::vector<DescriptorSetLayoutRef>&  layouts ) const;

        Common::BoolResultStr BuildFromSpirv( const std::vector<Core::ShaderMapStage>& stages,
                                              const Core::Formats::ShaderProgramMeta&  meta );
        // Everything Reload does after Core::BuildShaderMap answered: the empty-map refusal, the metadata, the
        // device half, the code generation. Shared by Reload and the prebuilt-map constructor - one path.
        Common::BoolResultStr AdoptShaderMap( const Core::ShaderMap& built );

    private:
        const std::weak_ptr<Assets::ShaderAsset> m_ShaderAsset;

    private:
        std::filesystem::path                        m_ShaderPath;
        std::string                                  m_ShaderName;
        std::string                                  m_PassName; // empty = default program
        // Set by BuildFromSpirv when a stage needs a capability this device lacks (GetDeviceRefusal).
        std::string m_DeviceRefusal;
        // The last Reload's failure, or empty after one that succeeded (GetCompileError).
        std::string m_CompileError;

        // STORED **AND** HONOURED, which is the half its predecessor never had: this reaches
        // CompileProgram below, and through it the includer and the SPIR-V cache key. A variant that
        // were only stored would be a knob that reports itself and changes nothing — exactly what the
        // ShaderDefines member it replaces was.
        ShaderVariant m_Variant;

        // The published record. m_Owned is touched only by the reloading thread (construction, Reload);
        // readers on any thread go through m_Current, which is never null (an empty record before the first
        // successful compile - IsCompiled false, no stage, no layout).
        std::shared_ptr<const CompiledProgram> m_Owned;
        std::atomic<const CompiledProgram*>    m_Current{ nullptr };

        DescriptorSetInfo m_DescriptorSetInfo;
    };

} // namespace Desert::Graphic::API::Vulkan