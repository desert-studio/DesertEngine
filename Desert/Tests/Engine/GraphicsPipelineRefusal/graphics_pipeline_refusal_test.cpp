// "A GRAPHICS PIPELINE CANNOT BE BUILT FROM A SPEC THAT CANNOT PRODUCE ONE" — AS A RULE THAT CAN GO RED,
// PLUS THE SOURCE FACTS THAT KEEP THE RULE WIRED IN, PLUS A REGISTER OF THE FORM ELSEWHERE.
//
// WHAT WAS WRONG. `GraphicsPipeline::Create` returned a bare `std::shared_ptr` built by `make_shared`,
// which cannot be null. All thirty-five of its call sites were therefore unusable: twenty-eight had no
// check at all, and seven carried an `if ( !pipeline )` branch that COULD NOT RUN. The same held for
// `PipelineCache::GetOrCreate`, which additionally CACHED AND RETURNED PIPELINES THAT HAD NOT BEEN
// BUILT — its `Create(); if ( pipeline ) Invalidate();` stored whatever came back, and an unbuilt
// pipeline is a live non-null pointer whose VkPipeline is null.
//
// Three separate crashes hid behind that, each of which reads at the call site as a handled failure:
//   * a NULL shader (four renderers never checked ShaderService::GetByName) reached
//     VulkanPipeline::CreatePipelineLayout and was dereferenced — the backend's guard read
//     `Shader && !IsCompiled()`, so null sailed through the very check written to stop it;
//   * a shader that never compiled has an EMPTY stage list, and vkCreateGraphicsPipelines answers
//     stageCount = 0 with a validation storm;
//   * a missing framebuffer THREW std::runtime_error from five calls deep, and nothing in this engine
//     catches, so the refusal was std::terminate.
//
// WHY THE RULE IS NOT INSIDE Create(). `GraphicsPipeline::Create` lives in a translation unit that
// constructs the Vulkan leaf, so nothing that links it can run on a machine with no device — a gate
// written over Create could never be made to fire, and a gate that cannot go red is decoration. So the
// decision is `Desert::Graphic::CheckGraphicsPipelineSpecification`, header-only and device-free, and
// this suite calls it with stubs. The source-reading tests below are what stop the rule from being a
// function nobody calls.
//
// MUTATION RECORD (run before this file was committed, and the reason it is trusted) — see the report
// for the transcript.
//   * delete the `!spec.Shader->IsCompiled()` arm of the rule
//        -> ARefusedShaderIsRefused, TheMessageNamesTheShaderAndThePipeline red.
//   * delete the `!spec.Framebuffer` arm
//        -> AMissingFramebufferIsRefusedRatherThanThrown red.
//   * drop NO_DISCARD from the declaration of GraphicsPipeline::Create
//        -> TheRuleIsObeyed red.
//   * discard the result of GetOrCreate at one call site (MeshRenderer's wireframe variant)
//        -> NoCallSiteCanIgnoreTheRefusal red.
//   * put `pipeline->Invalidate()` back into PipelineCache::GetOrCreate and cache the raw pointer
//        -> TheCacheStoresOutcomesNotPipelines red.

#include "../SettingConsumers/setting_consumers_reader.hpp"

#include <Engine/Graphic/Pipeline.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    // ------------------------------------------------------------------------------------------------
    // The rule, exercised directly.
    // ------------------------------------------------------------------------------------------------

    /// A Shader that exists and answers ONE question honestly — whether it compiled. Everything else is
    /// empty, because the rule under test is not allowed to depend on anything else: if a future edit
    /// makes it read a reflection list or a file path, this stub stops compiling and the reviewer is
    /// asked why a device-free decision grew a dependency.
    class StubShader final : public Desert::Graphic::Shader
    {
    public:
        StubShader( std::string name, bool compiled ) : m_Name( std::move( name ) ), m_Compiled( compiled )
        {
        }

        Common::BoolResultStr Reload() override
        {
            return Common::MakeError( "StubShader does not compile anything" );
        }

        const std::string GetName() const override
        {
            return m_Name;
        }

        const std::vector<Desert::ShaderResources::ShaderLayout::UniformBuffer>
        GetUniformBufferModels() const override
        {
            return {};
        }

        const std::vector<Desert::ShaderResources::ShaderLayout::StorageBuffer>
        GetStorageBufferModels() const override
        {
            return {};
        }

        const std::vector<Desert::ShaderResources::ShaderLayout::ImageCubeSampler>
        GetUniformImageCubeModels() const override
        {
            return {};
        }

        const std::vector<Desert::ShaderResources::ShaderLayout::Image2DSampler>
        GetUniformImage2DModels() const override
        {
            return {};
        }

        const Desert::Graphic::ShaderVariant& GetVariant() const override
        {
            return m_Variant;
        }

        const Common::Filepath& GetFilepath() const override
        {
            return m_Path;
        }

        const Desert::Core::Formats::ShaderProgramMeta& GetProgramMeta() const override
        {
            return m_Meta;
        }

        bool IsCompiled() const override
        {
            return m_Compiled;
        }

    private:
        std::string                              m_Name;
        bool                                     m_Compiled = false;
        Desert::Graphic::ShaderVariant           m_Variant;
        Common::Filepath                         m_Path;
        Desert::Core::Formats::ShaderProgramMeta m_Meta;
    };

    /// A Framebuffer that exists and answers nothing. The rule may only ask whether it is THERE — a
    /// graphics pipeline is built against its target's render pass, and asking that question needs a
    /// device. If a future edit makes the device-free rule read a size or an attachment, this stub
    /// starts returning zeroes into a decision, which is the reviewer's cue.
    class StubFramebuffer final : public Desert::Graphic::Framebuffer
    {
    public:
        Common::BoolResultStr Invalidate() override
        {
            return Common::MakeError( "StubFramebuffer has nothing to build" );
        }

        Common::BoolResultStr Release() override
        {
            return Common::MakeError( "StubFramebuffer has nothing to release" );
        }

        const Desert::Graphic::FramebufferSpecification GetSpecification() const override
        {
            return m_Spec;
        }

        Common::BoolResultStr Resize( uint32_t, uint32_t ) override
        {
            return Common::MakeError( "StubFramebuffer has nothing to resize" );
        }

        uint32_t GetFramebufferWidth() const override
        {
            return 0;
        }

        uint32_t GetFramebufferHeight() const override
        {
            return 0;
        }

        uint32_t GetColorAttachmentCount() const override
        {
            return 0;
        }

        uint32_t GetDepthAttachmentCount() const override
        {
            return 0;
        }

        const std::shared_ptr<Desert::Graphic::Image2D>& GetColorAttachmentImage( uint32_t ) const override
        {
            return m_NoImage;
        }

        const std::shared_ptr<Desert::Graphic::Image2D>& GetDepthAttachmentImage() const override
        {
            return m_NoImage;
        }

    private:
        Desert::Graphic::FramebufferSpecification m_Spec;
        std::shared_ptr<Desert::Graphic::Image2D> m_NoImage;
    };

    // ------------------------------------------------------------------------------------------------
    // Source reading, for the facts that keep the rule wired in.
    // ------------------------------------------------------------------------------------------------

    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + "Desert/Desert/Source/Engine/Graphic/Pipeline.hpp" );
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

    /// Every engine/editor/runtime source, comments and string literals blanked (Д33's shared reader), so
    /// a rule below cannot be satisfied or broken by prose. This file's own tree is excluded: it names
    /// every symbol it is about, and a census that counted itself would be worthless.
    std::vector<std::pair<std::string, std::string>> EngineSources( const std::string& root )
    {
        std::vector<std::pair<std::string, std::string>> out;
        for ( const char* subtree : { "Desert/Desert/Source", "Editor/Source", "Runtime/Source" } )
        {
            const fs::path base = fs::path( root ) / subtree;
            if ( !fs::exists( base ) )
                continue;
            for ( const auto& entry : fs::recursive_directory_iterator( base ) )
            {
                if ( !entry.is_regular_file() )
                    continue;
                const std::string ext = entry.path().extension().string();
                if ( ext != ".cpp" && ext != ".hpp" )
                    continue;
                out.emplace_back(
                     entry.path().generic_string(),
                     Desert::Tests::ConsumerText::StripCommentsAndLiterals( ReadAll( entry.path() ) ) );
            }
        }
        return out;
    }

    /// Walk back from @p at over everything that merely QUALIFIES the call — namespaces (`Graphic::`),
    /// member access (`m_SceneRenderer->`, `.`) and the empty argument list of an intermediate accessor
    /// (`GetPipelineCache()`) — so that the character we then read is the one deciding what happens to
    /// the RESULT and not a colon or an arrow inside the callee's own name.
    ///
    /// This is the half Г21's version did not need and Г22 does: half of the graphics call sites spell
    /// `Graphic::GraphicsPipeline::Create`, and every cache site goes through two member accesses. A
    /// walker that stops at the first non-space reports all of them and is therefore useless in both
    /// directions — it cannot pass, so it can never fail for a real reason either.
    std::size_t SkipQualification( const std::string& text, std::size_t at )
    {
        for ( ;; )
        {
            while ( at > 0 && std::isspace( static_cast<unsigned char>( text[at - 1] ) ) )
                --at;
            if ( at == 0 )
                return at;

            const char c = text[at - 1];
            if ( std::isalnum( static_cast<unsigned char>( c ) ) || c == '_' )
            {
                // Consume the identifier WHOLE, and stop in front of `return`: it is a keyword, not a
                // qualifier, and eating it would hide the one form where handing the result upward IS
                // the handling. (It did: SMAARenderer's helper returns the Result to its callers.)
                std::size_t start = at;
                while ( start > 0 && ( std::isalnum( static_cast<unsigned char>( text[start - 1] ) ) ||
                                       text[start - 1] == '_' ) )
                    --start;
                if ( text.compare( start, at - start, "return" ) == 0 )
                    return at;
                at = start;
                continue;
            }
            if ( c == ':' || c == '.' )
            {
                --at;
                continue;
            }
            if ( c == '>' && at >= 2 && text[at - 2] == '-' )
            {
                at -= 2;
                continue;
            }
            if ( c == ')' )
            {
                int         depth = 0;
                std::size_t k     = at - 1;
                for ( ;; )
                {
                    if ( text[k] == ')' )
                        ++depth;
                    else if ( text[k] == '(' && --depth == 0 )
                        break;
                    if ( k == 0 )
                        return at; // unbalanced after stripping — stop rather than guess
                    --k;
                }
                at = k;
                continue;
            }
            return at;
        }
    }

    /// Every place @p callee is named, judged by the character that decides what happens to its result.
    /// `= f(...)` binds it to a name (and the type is a ResultStr, so the value can only be reached
    /// through GetValue()); `( f(...)` is a guard or an argument; `return f(...)` hands the decision to
    /// the caller. A bare `;` or `{` before it means the statement stands alone — the result is
    /// discarded, which is the shape that let a refusal go unread for the life of the engine.
    std::vector<std::string> DiscardedResults( const std::vector<std::pair<std::string, std::string>>& sources,
                                               const std::string& callee, const std::string& definedIn )
    {
        std::vector<std::string> offenders;
        for ( const auto& [file, text] : sources )
        {
            if ( !definedIn.empty() && file.find( definedIn ) != std::string::npos )
                continue;

            for ( std::size_t at = text.find( callee ); at != std::string::npos; at = text.find( callee, at + 1 ) )
            {
                const std::size_t back = SkipQualification( text, at );
                if ( back == 0 )
                    continue;

                const char before = text[back - 1];
                if ( before == '=' || before == '(' || before == ',' )
                    continue;

                // `return f(...);` — the caller decides, which is a use and not a discard.
                if ( back >= 6 && text.compare( back - 6, 6, "return" ) == 0 )
                    continue;

                offenders.push_back( file + ": " + callee + " result is discarded" );
            }
        }
        return offenders;
    }

    std::string Join( const std::vector<std::string>& lines )
    {
        std::string report;
        for ( const auto& line : lines )
            report += "  " + line + "\n";
        return report;
    }
} // namespace

// ----------------------------------------------------------------------------------------------------
// 1. The rule itself — three doors, each of which was a crash.
// ----------------------------------------------------------------------------------------------------

TEST( GraphicsPipelineRefusal, ARefusedShaderIsRefused )
{
    const auto broken = std::make_shared<StubShader>( "StaticMeshPBR", false );
    const auto target = std::make_shared<StubFramebuffer>();

    const auto answer = Desert::Graphic::CheckGraphicsPipelineSpecification(
         { .Shader = broken, .Framebuffer = target, .DebugName = "StaticMeshGeometry" } );

    EXPECT_FALSE( answer.IsSuccess() ) << "a shader with no compiled stages must not reach "
                                          "vkCreateGraphicsPipelines: stageCount is 0 there, and the "
                                          "editor filled the log with validation errors and vanished.";
}

TEST( GraphicsPipelineRefusal, ABuildableSpecIsAccepted )
{
    const auto good   = std::make_shared<StubShader>( "StaticMeshPBR", true );
    const auto target = std::make_shared<StubFramebuffer>();

    const auto answer = Desert::Graphic::CheckGraphicsPipelineSpecification(
         { .Shader = good, .Framebuffer = target, .DebugName = "StaticMeshGeometry" } );

    // The other half of the gate, and it is not ceremony: a rule that refuses everything would pass
    // every test above while deleting every draw in the engine.
    EXPECT_TRUE( answer.IsSuccess() ) << answer.GetError();
}

TEST( GraphicsPipelineRefusal, AMissingShaderIsRefusedAndNotDereferenced )
{
    // THIS IS THE ONE THAT WAS LIVE. TonemapRenderer, FXAARenderer, SMAARenderer and SkyboxRenderer each
    // put an unchecked ShaderService::GetByName straight into a spec, and the backend's own guard read
    // `Shader && !IsCompiled()` — so a NULL shader passed the check written to stop exactly this and was
    // dereferenced inside CreatePipelineLayout. The empty pipeline name is deliberate: a refusal with no
    // name at all still has to be readable.
    const auto answer = Desert::Graphic::CheckGraphicsPipelineSpecification(
         { .Framebuffer = std::make_shared<StubFramebuffer>() } );

    ASSERT_FALSE( answer.IsSuccess() );
    EXPECT_NE( answer.GetError().find( "<unnamed>" ), std::string::npos ) << answer.GetError();
}

TEST( GraphicsPipelineRefusal, AMissingFramebufferIsRefusedRatherThanThrown )
{
    // `throw std::runtime_error( "Framebuffer is required for pipeline creation" )` stood inside
    // VulkanPipeline::CreateGraphicsPipeline. Nothing in this engine catches, so it was std::terminate
    // wearing an error message — and it fired five calls deep, after a VkPipelineLayout had already
    // been created and was then leaked by the stack unwind.
    const auto good = std::make_shared<StubShader>( "SceneComposite", true );

    const auto answer =
         Desert::Graphic::CheckGraphicsPipelineSpecification( { .Shader = good, .DebugName = "SceneTonemap" } );

    ASSERT_FALSE( answer.IsSuccess() );
    EXPECT_NE( answer.GetError().find( "SceneTonemap" ), std::string::npos ) << answer.GetError();
}

TEST( GraphicsPipelineRefusal, TheMessageNamesTheShaderAndThePipeline )
{
    // A refusal that does not say WHICH shader sends the reader through forty .shader files. Both names
    // are needed and they are different things: the pipeline is what the renderer calls it, the shader
    // is what the artist edits.
    const auto broken = std::make_shared<StubShader>( "ProceduralSky", false );
    const auto target = std::make_shared<StubFramebuffer>();

    const auto answer = Desert::Graphic::CheckGraphicsPipelineSpecification(
         { .Shader = broken, .Framebuffer = target, .DebugName = "SkyboxPass" } );

    ASSERT_FALSE( answer.IsSuccess() );
    const std::string error = answer.GetError();
    EXPECT_NE( error.find( "ProceduralSky" ), std::string::npos ) << error;
    EXPECT_NE( error.find( "SkyboxPass" ), std::string::npos ) << error;
}

// ----------------------------------------------------------------------------------------------------
// 2. The rule is what the two factories obey, and their answer cannot be ignored.
// ----------------------------------------------------------------------------------------------------

TEST( GraphicsPipelineRefusal, TheRuleIsObeyed )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "repository root not found from the working directory";

    const std::string header = Desert::Tests::ConsumerText::StripCommentsAndLiterals(
         ReadAll( fs::path( root ) / "Desert/Desert/Source/Engine/Graphic/Pipeline.hpp" ) );
    const std::string source = Desert::Tests::ConsumerText::StripCommentsAndLiterals(
         ReadAll( fs::path( root ) / "Desert/Desert/Source/Engine/Graphic/Pipeline.cpp" ) );
    const std::string cache = Desert::Tests::ConsumerText::StripCommentsAndLiterals(
         ReadAll( fs::path( root ) / "Desert/Desert/Source/Engine/Graphic/PipelineCache.hpp" ) );

    ASSERT_FALSE( header.empty() );
    ASSERT_FALSE( source.empty() );
    ASSERT_FALSE( cache.empty() );

    // The declaration must be able to express a refusal AND must make discarding it a diagnostic. Either
    // half alone is the state this task found: a factory that could only ever succeed.
    EXPECT_NE( header.find( "NO_DISCARD static Common::ResultStr<std::shared_ptr<GraphicsPipeline>>" ),
               std::string::npos )
         << "GraphicsPipeline::Create must be NO_DISCARD and return a ResultStr — it used to return a "
            "bare shared_ptr that make_shared can never leave null, so every one of its thirty-five call "
            "sites was either unchecked or carried an `if ( !pipeline )` branch that COULD NOT RUN.";

    EXPECT_NE( cache.find( "NO_DISCARD Common::ResultStr<std::shared_ptr<GraphicsPipeline>>" ), std::string::npos )
         << "PipelineCache::GetOrCreate must be NO_DISCARD and return a ResultStr, for the same reason "
            "and with one more: it also handed back pipelines that had not been built.";

    // And both definitions must actually ask the rule rather than re-deriving it.
    EXPECT_NE( source.find( "CheckGraphicsPipelineSpecification( spec )" ), std::string::npos )
         << "GraphicsPipeline::Create must obey CheckGraphicsPipelineSpecification — a rule with no "
            "caller is the shape of defect this suite exists for.";
    EXPECT_NE( cache.find( "CheckGraphicsPipelineSpecification( spec )" ), std::string::npos )
         << "PipelineCache::GetOrCreate must obey the same rule BEFORE it touches its map: a malformed "
            "spec has a degenerate key (null Shader, null Framebuffer) and would collide with the next "
            "caller's malformed spec, serving it somebody else's reason.";
}

TEST( GraphicsPipelineRefusal, NoCallSiteCanIgnoreTheRefusal )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const auto sources = EngineSources( root );

    std::vector<std::string> offenders =
         DiscardedResults( sources, "GraphicsPipeline::Create", "Engine/Graphic/Pipeline.cpp" );

    // The cache's own answer is exactly as ignorable, and it is the one that used to be ignored the most:
    // seven of its ten call sites tested a branch that could not run.
    for ( auto& line : DiscardedResults( sources, "GetPipelineCache().GetOrCreate", {} ) )
        offenders.push_back( std::move( line ) );

    EXPECT_TRUE( offenders.empty() ) << Join( offenders );
}

TEST( GraphicsPipelineRefusal, TheCacheStoresOutcomesNotPipelines )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::string cache = Desert::Tests::ConsumerText::StripCommentsAndLiterals(
         ReadAll( fs::path( root ) / "Desert/Desert/Source/Engine/Graphic/PipelineCache.hpp" ) );
    ASSERT_FALSE( cache.empty() );

    // THE SECOND DEFECT, WHICH THE RETURN TYPE ALONE DOES NOT CLOSE. `Create(); if ( pipeline )
    // Invalidate();` cached whatever Create returned, built or not — so a pipeline whose VkPipeline is
    // null lived in the map for the life of the renderer and was drawn through every frame. The map's
    // value type IS the guarantee: an entry is an outcome, and an outcome that succeeded holds a
    // pipeline that Create has already seen built.
    EXPECT_NE( cache.find( "std::unordered_map<Key, Common::ResultStr<std::shared_ptr<GraphicsPipeline>>, "
                           "KeyHash> m_Cache" ),
               std::string::npos )
         << "PipelineCache must store the OUTCOME per key, not a pipeline that may never have been built.";

    // And it must not build one itself: Create hands back a BUILT object, and a second Invalidate() here
    // would be the two-step idiom growing back.
    EXPECT_EQ( cache.find( "->Invalidate()" ), std::string::npos )
         << "PipelineCache must not call Invalidate: GraphicsPipeline::Create returns a pipeline that is "
            "already built, and the two-step whose first half could not report anything is what Г22 "
            "removed.";
}

// ----------------------------------------------------------------------------------------------------
// 3. THE FORM ELSEWHERE — a register, not a count, and not a fix.
// ----------------------------------------------------------------------------------------------------

TEST( GraphicsPipelineRefusal, EveryBareSharedPtrFactoryIsOnTheRegister )
{
    // THE SAME SHAPE EXISTS TWENTY-THREE MORE TIMES AND THIS TASK DID NOT FIX THEM. A `static
    // std::shared_ptr<X> Create(...)` whose body is a `switch ( RendererAPI::GetAPIType() )` over
    // make_shared cannot return null on the Vulkan arm, so `if ( !x )` at its call sites is decoration —
    // which is precisely what Г21 measured on the compute side (16 of 16 call sites unusable) and Г22 on
    // the graphics side (35 of 35).
    //
    // NOT ALL OF THEM ARE DEFECTS, AND CLASSIFYING THEM IS A SEPARATE TASK'S WORK: MaterialPBR::Create,
    // for one, genuinely returns nullptr and its callers' checks genuinely fire. What this register does
    // is make a TWENTY-FOURTH impossible to add without a reviewer seeing it, and record the two that
    // have been converted so the direction cannot silently reverse.
    //
    // It pins NAMES, not a number: a gate pinning a count can be satisfied by editing the count.
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    static const std::vector<std::string> kRegister = {
         "Engine/Core/Device.hpp",
         "Engine/Core/Window.hpp",
         "Engine/Geometry/MeshFactory.hpp",
         "Engine/Geometry/PrimitiveMeshFactory.hpp",
         "Engine/Graphic/API/Vulkan/VulkanDevice.hpp",
         "Engine/Graphic/Framebuffer.hpp",
         "Engine/Graphic/Image.hpp",
         "Engine/Graphic/IndexBuffer.hpp",
         "Engine/Graphic/Materials/Mesh/PBR/MaterialPBR.hpp",
         "Engine/Graphic/RenderPass.hpp",
         "Engine/Graphic/RendererContext.hpp",
         "Engine/Graphic/Shader.hpp",
         "Engine/Graphic/SwapChain.hpp",
         "Engine/Graphic/VertexBuffer.hpp",
         "Engine/ShaderResources/ShaderResourcesManager.hpp",
         "Engine/ShaderResources/StorageBuffer.hpp",
         "Engine/ShaderResources/UniformBuffer.hpp",
         "Engine/ShaderResources/UniformImage2D.hpp",
         "Engine/ShaderResources/UniformImageCube.hpp",
         "Editor/ImGuiIntegration/ImGuiLayer.hpp",
    };

    const std::string needle = "static std::shared_ptr<";

    std::vector<std::string> unregistered;
    for ( const auto& [file, text] : EngineSources( root ) )
    {
        bool declaresBareFactory = false;
        for ( std::size_t at = text.find( needle ); at != std::string::npos; at = text.find( needle, at + 1 ) )
        {
            const std::size_t close = text.find( '>', at );
            if ( close == std::string::npos )
                continue;
            // `static std::shared_ptr<T> Create(` — the factory shape, not any static member.
            if ( text.compare( close + 1, 8, " Create(" ) == 0 )
                declaresBareFactory = true;
        }
        if ( !declaresBareFactory )
            continue;

        const std::size_t sourceAt = file.find( "/Source/" );
        const std::string relative = sourceAt == std::string::npos ? file : file.substr( sourceAt + 8 );
        if ( std::find( kRegister.begin(), kRegister.end(), relative ) == kRegister.end() )
            unregistered.push_back( relative );
    }

    EXPECT_TRUE( unregistered.empty() )
         << "a factory returning a bare std::shared_ptr cannot express a refusal, and every call site "
            "that tests its result is testing a branch that cannot run. Convert it to "
            "Common::ResultStr (see GraphicsPipeline::Create), or add it to the register in this test "
            "with a reason:\n"
         << Join( unregistered );

    // The two that were converted must STAY converted: a row reappearing here is a regression, and this
    // is the half of a register that a plain allow-list forgets.
    for ( const auto& row : kRegister )
    {
        EXPECT_NE( row, "Engine/Graphic/Pipeline.hpp" )
             << "Pipeline.hpp is on the register again — GraphicsPipeline::Create or "
                "ComputePipeline::Create has gone back to a bare shared_ptr.";
    }
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
