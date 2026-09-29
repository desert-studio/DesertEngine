#pragma once

#include <Engine/Graphic/RendererTypes.hpp>
#include <Engine/Core/Formats/Shader.hpp>
#include <Engine/Core/Formats/ShaderProgramMeta.hpp>
#include <Engine/Core/Formats/MaterialLayout.hpp>
#include <Engine/Core/ShaderCompiler/ShaderVariant.hpp>

#include <Engine/ShaderResources/ShaderReflectionTypes.hpp>

#include <Engine/Assets/Shader/ShaderAsset.hpp>
#include <Engine/Graphic/ResourceLedger.hpp>

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
        Shader() : m_Accounting( ResourceOwnership::Take( ResourceKind::Shader ) )
        {
        }

        virtual ~Shader() = default;

        void ClaimOwnership( const ResourceOwner owner, const Common::AssetHandle asset = Common::AssetHandle{} )
        {
            m_Accounting.Claim( owner, asset );
        }

        virtual Common::BoolResultStr Reload()                                                                 = 0;
        virtual const std::string     GetName() const                                                          = 0;
        virtual const std::vector<ShaderResources::ShaderLayout::UniformBuffer> GetUniformBufferModels() const = 0;
        virtual const std::vector<ShaderResources::ShaderLayout::StorageBuffer> GetStorageBufferModels() const = 0;
        virtual const std::vector<ShaderResources::ShaderLayout::ImageCubeSampler>
        GetUniformImageCubeModels() const = 0;
        virtual const std::vector<ShaderResources::ShaderLayout::Image2DSampler>
                                        GetUniformImage2DModels() const = 0;
        // The substitution this program was COMPILED under — the one thing about a Shader object that
        // its name and its file do not say, and the thing two objects of the same name differ by.
        virtual const ShaderVariant&    GetVariant() const              = 0;
        virtual const Common::Filepath& GetFilepath() const             = 0;

        // Data-driven material metadata parsed from the .shader's `#pragma param` / `#pragma state`.
        virtual const Core::Formats::ShaderProgramMeta& GetProgramMeta() const = 0;

        // THE layout every material filling this program writes through (Graphic/Materials/MaterialBinder.hpp):
        // row params and textures from the template, push fields read off the compiled stages. A program
        // whose stages disagree with it or with each other never loads, so what is here is what the GPU reads.
        virtual const Core::Formats::MaterialLayout& GetMaterialLayout() const = 0;

        // False when this shader has never compiled successfully, i.e. it carries no stages at all.
        //
        // A failed RECOMPILE is already safe — CompileProgram builds into locals and keeps the previous
        // modules — but a shader whose FIRST compile fails is still constructed, still registered under
        // its name, and still handed out by ShaderService. Building a pipeline from it produces
        // `stageCount = 0`, which Vulkan answers with a validation storm and a crash before the frame is
        // presented. That is an artist's typo in a shader graph, so the engine has to survive it: callers
        // ask this and skip, and the pipeline builder refuses as the last line of defence.
        [[nodiscard]] virtual bool IsCompiled() const = 0;

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

    private:
        ResourceOwnership m_Accounting;
    };

} // namespace Desert::Graphic