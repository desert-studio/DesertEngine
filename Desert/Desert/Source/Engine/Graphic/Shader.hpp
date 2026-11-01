#pragma once

#include <string_view>
#include <Engine/Graphic/RendererTypes.hpp>
#include <Engine/Core/Formats/Shader.hpp>
#include <Engine/Core/Formats/ShaderProgramMeta.hpp>
#include <Engine/Core/Formats/MaterialLayout.hpp>
#include <Engine/Core/ShaderCompiler/ShaderVariant.hpp>

#include <Engine/ShaderResources/ShaderReflectionTypes.hpp>

#include <Engine/Assets/Shader/ShaderAsset.hpp>
#include <Engine/Graphic/ResourceLedger.hpp>

#include <atomic>
#include <cstdint>

namespace Desert::Core
{
    struct ShaderMap;
}

namespace Desert::Graphic
{
    // THE COMPILE-TIME AXIS OF A PROGRAM, and it used to be `ShaderDefines` — a vector of name/value
    // pairs that Shader::Create accepted, VulkanShader stored, and NOTHING ever gave to the compiler.
    // Every caller passed `{}`, so the knob could not be observed to do nothing; its own header said so
    // in a comment ("these defines still do not reach CompileProgram"). It is deleted rather than
    // wired up, because the axis O1 needs is not a preprocessor symbol: the cloud medium is a BODY OF
    // CODE, and four shipped programs have to receive it through one include whose name never changes
    // (Docs/Clouds/O1_DESIGN.md §10.3). Core::ShaderVariant is that axis, and it reaches the compiler
    // AND the cache key.
    using ShaderVariant = Core::ShaderVariant;

    class Shader
    {
    public:
        // The ledger row — see Engine/Graphic/ResourceLedger.hpp. One row per Shader object, which on the
        // Vulkan backend is N `VkShaderModule` plus its descriptor set layouts and pools.
        // DEVICE-OWNED AT CREATION (RT-LEDGER-4): a shader is shared by every view that draws with it and
        // outlives any of them, so a view scope open around a lazy build must not make it that view's row
        // (closing the view would report it as a leak). The service that loaded it re-claims it for its asset.
        Shader() : m_Accounting( ResourceOwnership::Take( ResourceKind::Shader ) )
        {
            m_Accounting.Claim( ResourceOwner::Device );
        }

        virtual ~Shader() = default;

        void ClaimOwnership( const ResourceOwner owner, const Common::AssetHandle asset = Common::AssetHandle{} )
        {
            m_Accounting.Claim( owner, asset );
        }

        virtual Common::BoolResultStr Reload()                                                                 = 0;

        // THE VERSION OF THE CODE THIS OBJECT CARRIES (UE: a recompile bumps the shader map, and every PSO built
        // from the old one is stale). Moves on every successful Reload; a pipeline records the value it was
        // built against (IPipeline::RecordShaderCodeGeneration) and RebuildPipelinesBehindTheirShader rebuilds
        // exactly the ones that fell behind — renderer-owned or cached, graphics or compute, one rule for all.
        [[nodiscard]] uint64_t GetCodeGeneration() const
        {
            return m_CodeGeneration.load( std::memory_order_acquire );
        }
        virtual const std::string     GetName() const                                                          = 0;
        virtual const std::vector<ShaderResources::ShaderLayout::UniformBuffer> GetUniformBufferModels() const = 0;
        virtual const std::vector<ShaderResources::ShaderLayout::StorageBuffer> GetStorageBufferModels() const = 0;
        virtual const std::vector<ShaderResources::ShaderLayout::ImageCubeSampler>
        GetUniformImageCubeModels() const = 0;
        virtual const std::vector<ShaderResources::ShaderLayout::Image2DSampler>
                                        GetUniformImage2DModels() const = 0;
        // Whether a stage reads a separate texture (texture2D / texture2DArray / textureCube ..., outside the
        // bindless set) of this name: what a pass binds a scene/view input to (the shadow cascades, the scene
        // depth).
        [[nodiscard]] virtual bool SamplesTexture( std::string_view name ) const = 0;
        // The substitution this program was COMPILED under — the one thing about a Shader object that
        // its name and its file do not say, and the thing two objects of the same name differ by.
        virtual const ShaderVariant&    GetVariant() const              = 0;
        virtual const Common::Filepath& GetFilepath() const             = 0;

        // Data-driven material metadata parsed from the .shader's `#pragma param` / `#pragma state`.
        virtual const Core::Formats::ShaderProgramMeta& GetProgramMeta() const = 0;

        // THE layout every material filling this program writes through (Graphic/Materials/MaterialBinder.hpp):
        // row params and textures from the template, push fields read off the compiled stages. A program
        // whose stages disagree with it or with each other never loads, so what is here is what the GPU reads.
        [[nodiscard]] virtual const Core::Formats::MaterialLayout& GetMaterialLayout() const = 0;

        // False when this shader has never compiled successfully, i.e. it carries no stages at all.
        //
        // A failed RECOMPILE is already safe — CompileProgram builds into locals and keeps the previous
        // modules — but a shader whose FIRST compile fails is still constructed, still registered under
        // its name, and still handed out by ShaderService. Building a pipeline from it produces
        // `stageCount = 0`, which Vulkan answers with a validation storm and a crash before the frame is
        // presented. That is an artist's typo in a shader graph, so the engine has to survive it: callers
        // ask this and skip, and the pipeline builder refuses as the last line of defence.
        [[nodiscard]] virtual bool IsCompiled() const = 0;
        // Why THIS DEVICE cannot run the program, or empty. A program that needs a device feature the device
        // lacks (inline ray tracing on a device without ray query, MoltenVK) is not built there at all: no module
        // and no set layout is created, IsCompiled() is false, and this names the feature and the device. It is a
        // refusal for the device, not a compile error: the same source builds on a device that has the feature.
        [[nodiscard]] virtual std::string_view GetDeviceRefusal() const
        {
            return {};
        }
        // Why the program has no compiled stages, or empty: the last build's failure as the build reported it
        // (a preprocess, compile, binding, reflection or set-layout refusal, naming the program). Kept on the
        // object because the constructor cannot return it, and a shader whose first build failed is still
        // registered and handed out by name: whoever registers it says WHY it will not draw (ShaderService).
        [[nodiscard]] virtual std::string_view GetCompileError() const
        {
            return {};
        }

        // Inline ON PURPOSE: ShaderCompiler (a device-free TU that offline cooks and tests link)
        // needs only this name mapping — defined in Shader.cpp it dragged Shader::Create and with it
        // the whole Vulkan backend into every such link.
        static std::string GetStringShaderStage( const Core::Formats::ShaderStage stage )
        {
            switch ( stage )
            {
                case Core::Formats::ShaderStage::Fragment:
                    return "Fragment";
                case Core::Formats::ShaderStage::Vertex:
                    return "Vertex";
                case Core::Formats::ShaderStage::Compute:
                    return "Compute";
                case Core::Formats::ShaderStage::TessControl:
                    return "TessControl";
                case Core::Formats::ShaderStage::TessEvaluation:
                    return "TessEvaluation";
                case Core::Formats::ShaderStage::RayGen:
                    return "RayGen";
                case Core::Formats::ShaderStage::AnyHit:
                    return "AnyHit";
                case Core::Formats::ShaderStage::ClosestHit:
                    return "ClosestHit";
                case Core::Formats::ShaderStage::Miss:
                    return "Miss";
                // `None` is the enum's zero and names no stage; the fallthrough below is its answer, and
                // writing the case is what makes a NEW stage a compiler error here instead of "Unknown".
                case Core::Formats::ShaderStage::None:
                    break;
            }
            return "Unknown";
        }
        // passName selects a `Pass "Name"` block of a DSL multi-pass shader; empty = the default
        // program. Pass shaders are named "<Shader>/<Pass>".
        static std::shared_ptr<Shader> Create( const Assets::Asset<Assets::ShaderAsset>& asset,
                                               const ShaderVariant&                      variant  = {},
                                               const std::string&                        passName = {} );
        // THE DEVICE HALF ALONE (VFX-PRESETS2-4): a program whose shader map (the CPU half, Core::BuildShaderMap) was
        // built elsewhere - on a job, off the main thread - gets its modules and set layouts here, on the thread
        // that owns the device. @p built must be the map of exactly (@p asset, @p variant, @p passName); a later
        // Reload compiles from the asset as any other program does. Refusals as Create: IsCompiled() false and
        // GetCompileError() says why.
        static std::shared_ptr<Shader> CreateFromShaderMap( const Assets::Asset<Assets::ShaderAsset>& asset,
                                                            const ShaderVariant&                      variant,
                                                            const std::string&                        passName,
                                                            const Core::ShaderMap&                    built );

    protected:
        // Called by the backend when a Reload has replaced the code — and only then: a failed recompile keeps the
        // previous modules, so the pipelines built from them are not behind.
        void BumpCodeGeneration()
        {
            m_CodeGeneration.fetch_add( 1, std::memory_order_acq_rel );
        }

    private:
        ResourceOwnership     m_Accounting;
        std::atomic<uint64_t> m_CodeGeneration{ 0 };
    };

} // namespace Desert::Graphic