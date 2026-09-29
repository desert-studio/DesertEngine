// "A COMPUTE PIPELINE CANNOT BE BUILT FROM A SHADER THAT DID NOT COMPILE" — AS A RULE THAT CAN GO RED,
// PLUS THE THREE SOURCE FACTS THAT KEEP THE RULE WIRED IN.
//
// WHAT WAS WRONG. `VulkanPipelineCompute::Invalidate` read
// `VulkanShader::GetPipelineShaderStageCreateInfos()[0]` — of the vector a FAILED first compile leaves
// EMPTY. Measured on the live editor with the validation layer on, both configurations, one rigged
// binding collision in an authored cloud medium: before, exit 139 and 23 validation errors about a
// descriptor pool with `descriptorSetCount == 0`; after, exit 0 and twelve legible lines naming the
// resources. The cause an artist can reach is a typo in a shader graph.
//
// WHY THE RULE IS NOT INSIDE Create(). `ComputePipeline::Create` lives in a translation unit that
// constructs the Vulkan leaf, so nothing that links it can run on a machine with no device — a gate
// written over Create could never be made to fire, and a gate that cannot go red is decoration. So the
// decision is `Desert::Graphic::CheckComputePipelineSpecification`, header-only and device-free, and
// this suite calls it with a stub Shader. TheRuleIsObeyed/NoCallSiteCanIgnore/TheStageListIsNeverIndexed
// below are what stop the rule from being a function nobody calls.
//
// MUTATION RECORD (run before this file was committed, and the reason it is trusted):
//   * delete the `!spec.Shader->IsCompiled()` arm of CheckComputePipelineSpecification
//        -> ARefusedShaderIsRefused, TheMessageNamesTheShaderAndThePipeline red.
//   * change ComputePipeline::Create back to `std::shared_ptr<ComputePipeline>`
//        -> TheRuleIsObeyed red (and the engine no longer compiles, which is the point of the return type).
//   * restore `GetPipelineShaderStageCreateInfos()[0]` in VulkanPipelineCompute::Invalidate
//        -> TheStageListIsNeverIndexed red.

#include "../SettingConsumers/setting_consumers_reader.hpp"

#include <Engine/Graphic/Pipeline.hpp>

#include <gtest/gtest.h>

#include <Engine/Core/Formats/MaterialLayout.hpp>

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
        const Desert::Core::Formats::MaterialLayout& GetMaterialLayout() const override
        {
            return m_Layout;
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
        Desert::Core::Formats::MaterialLayout m_Layout;
    };

    // ------------------------------------------------------------------------------------------------
    // Source reading, for the three facts that keep the rule wired in.
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

    std::size_t CountOccurrences( const std::string& haystack, const std::string& needle )
    {
        std::size_t count = 0;
        for ( std::size_t at = haystack.find( needle ); at != std::string::npos;
              at             = haystack.find( needle, at + needle.size() ) )
        {
            ++count;
        }
        return count;
    }
} // namespace

// ----------------------------------------------------------------------------------------------------
// 1. The rule itself.
// ----------------------------------------------------------------------------------------------------

TEST( ComputePipelineRefusal, ARefusedShaderIsRefused )
{
    const auto broken = std::make_shared<StubShader>( "CloudMarch", false );

    const auto answer =
         Desert::Graphic::CheckComputePipelineSpecification( { .Shader = broken, .DebugName = "CloudMarch" } );

    EXPECT_FALSE( answer.IsSuccess() ) << "a shader with no compiled stages must not reach "
                                          "vkCreateComputePipelines: that read stage list [0] of an "
                                          "empty vector and took the process down.";
}

TEST( ComputePipelineRefusal, ACompiledShaderIsAccepted )
{
    const auto good = std::make_shared<StubShader>( "CloudMarch", true );

    const auto answer =
         Desert::Graphic::CheckComputePipelineSpecification( { .Shader = good, .DebugName = "CloudMarch" } );

    // The other half of the gate, and it is not ceremony: a rule that refuses everything would pass the
    // test above while deleting every compute pass in the engine.
    EXPECT_TRUE( answer.IsSuccess() ) << answer.GetError();
}

TEST( ComputePipelineRefusal, AMissingShaderIsRefusedAndNotVerified )
{
    // `DESERT_VERIFY( spec.Shader != nullptr )` stood at the top of Create. That is not a refusal — it
    // logs and takes the process down, which is the outcome the caller is being handed a Result to
    // avoid. The empty pipeline name is deliberate: the message still has to be readable.
    const auto answer = Desert::Graphic::CheckComputePipelineSpecification( { .Shader = nullptr } );

    ASSERT_FALSE( answer.IsSuccess() );
    EXPECT_NE( answer.GetError().find( "<unnamed>" ), std::string::npos ) << answer.GetError();
}

TEST( ComputePipelineRefusal, TheMessageNamesTheShaderAndThePipeline )
{
    // A refusal that does not say WHICH shader sends the reader through forty .shader files. Both names
    // are needed and they are different things: the pipeline is what the renderer calls it, the shader
    // is what the artist edits.
    const auto broken = std::make_shared<StubShader>( "SkyTransmittanceLut", false );

    const auto answer = Desert::Graphic::CheckComputePipelineSpecification(
         { .Shader = broken, .DebugName = "TransmittancePass" } );

    ASSERT_FALSE( answer.IsSuccess() );
    const std::string error = answer.GetError();
    EXPECT_NE( error.find( "SkyTransmittanceLut" ), std::string::npos ) << error;
    EXPECT_NE( error.find( "TransmittancePass" ), std::string::npos ) << error;
}

// ----------------------------------------------------------------------------------------------------
// 2. The rule is what Create obeys, and its answer cannot be ignored.
// ----------------------------------------------------------------------------------------------------

TEST( ComputePipelineRefusal, TheRuleIsObeyed )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "repository root not found from the working directory";

    const std::string header = Desert::Tests::ConsumerText::StripCommentsAndLiterals(
         ReadAll( fs::path( root ) / "Desert/Desert/Source/Engine/Graphic/Pipeline.hpp" ) );
    const std::string source = Desert::Tests::ConsumerText::StripCommentsAndLiterals(
         ReadAll( fs::path( root ) / "Desert/Desert/Source/Engine/Graphic/Pipeline.cpp" ) );

    ASSERT_FALSE( header.empty() );
    ASSERT_FALSE( source.empty() );

    // The declaration must be able to express a refusal AND must make discarding it a diagnostic. Either
    // half alone is the state this task found: a factory that could only ever succeed.
    EXPECT_NE( header.find( "NO_DISCARD static Common::ResultStr<std::shared_ptr<ComputePipeline>>" ),
               std::string::npos )
         << "ComputePipeline::Create must be NO_DISCARD and return a ResultStr — it used to return a bare "
            "shared_ptr that make_shared can never leave null, so every one of its call sites was either "
            "unchecked or carried an `if ( !pipeline )` branch that COULD NOT RUN.";

    // And the definition must actually ask the rule rather than re-deriving it.
    EXPECT_NE( source.find( "CheckComputePipelineSpecification( spec )" ), std::string::npos )
         << "ComputePipeline::Create must obey CheckComputePipelineSpecification — a rule with no caller "
            "is the shape of defect this suite exists for.";
}

TEST( ComputePipelineRefusal, NoCallSiteCanIgnoreTheRefusal )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    std::vector<std::string> offenders;
    for ( const auto& [file, text] : EngineSources( root ) )
    {
        // Pipeline.cpp is where Create is DEFINED; the name occurs there as a definition, not a call.
        if ( file.find( "Engine/Graphic/Pipeline.cpp" ) != std::string::npos )
            continue;

        for ( std::size_t at = text.find( "ComputePipeline::Create" ); at != std::string::npos;
              at             = text.find( "ComputePipeline::Create", at + 1 ) )
        {
            // Walk back over whitespace to the character that decides what happens to the result.
            std::size_t back = at;
            while ( back > 0 && std::isspace( static_cast<unsigned char>( text[back - 1] ) ) )
                --back;
            if ( back == 0 )
                continue;
            const char before = text[back - 1];

            // `= Create(...)` binds it to a name (and the type is a ResultStr, so the value can only be
            // reached through GetValue()); `( Create(...)` is `if ( !x = ...` style guarding or an
            // argument. A bare `;` or `{` before it means the statement stands alone: the result is
            // discarded, which is the shape that let a refusal go unread for the life of the engine.
            if ( before != '=' && before != '(' && before != ',' )
                offenders.push_back( file + ": ComputePipeline::Create result is discarded" );
        }
    }

    EXPECT_TRUE( offenders.empty() ) << [&]
    {
        std::string report;
        for ( const auto& line : offenders )
            report += line + "\n";
        return report;
    }();
}

// ----------------------------------------------------------------------------------------------------
// 3. The FORM of the defect, everywhere — not the one line that happened to be found.
// ----------------------------------------------------------------------------------------------------

TEST( ComputePipelineRefusal, TheStageListIsNeverIndexed )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    // A shader's stage list may be asked its size, handed to Vulkan whole, or searched by stage bit
    // (VulkanShader::GetComputeStage). It may NOT be subscripted or have front()/back() taken, because
    // the vector is empty for every shader that never compiled and nothing at the call site says so.
    // This is the exact form of the crash Г21 closed, pinned as a form rather than as a line.
    const std::vector<std::string> forbidden = { "GetPipelineShaderStageCreateInfos()[",
                                                 "GetPipelineShaderStageCreateInfos().front(",
                                                 "GetPipelineShaderStageCreateInfos().back(" };

    std::vector<std::string> offenders;
    for ( const auto& [file, text] : EngineSources( root ) )
    {
        for ( const auto& form : forbidden )
        {
            if ( CountOccurrences( text, form ) > 0 )
                offenders.push_back( file + ": " + form );
        }
    }

    EXPECT_TRUE( offenders.empty() ) << [&]
    {
        std::string report = "a shader that never compiled carries NO stages; ask GetComputeStage() "
                             "instead:\n";
        for ( const auto& line : offenders )
            report += "  " + line + "\n";
        return report;
    }();
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
