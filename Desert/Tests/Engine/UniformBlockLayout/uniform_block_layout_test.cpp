// GBUF1 step 4b — EVERY UNIFORM BLOCK THE G-BUFFER RECONSTRUCTION CHANGED HAS ITS C++ TWIN, OFFSET FOR OFFSET.
//
// World position is rebuilt from the depth (Common/ReconstructPosition.glslh), so SSR's trace, the SSR/GI resolve,
// SSAO, the GI gather and the deferred composite each gained u_InvJitteredViewProjection. Every block uploaded
// whole from a C++ struct (SetRawData / a graph buffer) is checked member for member: the GLSL names in order,
// each one's std140 offset == offsetof() of the C++ member that fills it, the block size == sizeof (the parser:
// TestSupport/std140_block.hpp).
// Mutations: a member reordered on either side (the names or the offsets move), a member inserted / dropped on one
// side (the count), the C++ struct padded (the size).
//
// DeferredUB is different: MaterialDeferredLighting fills it FIELD BY FIELD through its MPROPERTYs, each by its
// shader name at the reflected offset (UploadRegisteredProperties + FlushFieldFilledUniformBuffers), so member
// order on either side is free by design; the census there is the name + type set (mutation: a member renamed,
// dropped or retyped on one side -> red).
#include <Engine/Graphic/Materials/Deferred/MaterialGIResolve.hpp>
#include <Engine/Graphic/Materials/Deferred/MaterialSSAO.hpp>
#include <Engine/Graphic/Materials/Deferred/MaterialSSR.hpp>
#include <Engine/Graphic/Systems/Scene/Deferred/SSRRenderer.hpp>

#include "../../TestSupport/scratch_dir.hpp"
#include "../../TestSupport/std140_block.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <regex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace UniformBlockLayoutTest
{
    namespace Std140 = Desert::TestSupport::Std140;

    struct Twin
    {
        const char* GlslName;
        size_t      CppOffset;
    };

    std::filesystem::path DeferredProgram( const char* file )
    {
        return Desert::TestSupport::RepositoryRoot() / "Editor" / "Resources" / "Shaders" / "Programs" /
               "Deferred" / file;
    }

    void ExpectTwin( const char* shaderFile, const char* blockName, const std::vector<Twin>& cpp,
                     const size_t cppSize )
    {
        SCOPED_TRACE( std::string( shaderFile ) + " " + blockName );
        const Std140::Block block =
             Std140::ParseBlock( Std140::ReadText( DeferredProgram( shaderFile ) ), blockName );
        ASSERT_EQ( block.Members.size(), cpp.size() ) << "the GLSL block and its C++ twin differ in member count";
        for ( size_t i = 0; i < cpp.size(); ++i )
        {
            EXPECT_EQ( block.Members[i].Name, cpp[i].GlslName ) << "member " << i;
            EXPECT_EQ( block.Members[i].Offset, cpp[i].CppOffset ) << cpp[i].GlslName;
        }
        EXPECT_EQ( block.Size, cppSize ) << "sizeof the C++ twin != the std140 block size";
    }
} // namespace UniformBlockLayoutTest

using namespace UniformBlockLayoutTest;

TEST( UniformBlockLayout, SSRTraceUBIsTheTraceUniforms )
{
    using T = Desert::Graphic::System::SSRRenderer::TraceUniforms;
    ExpectTwin( "SSR.shader", "SSRTraceUB",
                { { "u_InvJitteredViewProjection", offsetof( T, InvJitteredViewProjection ) } }, sizeof( T ) );
    EXPECT_EQ( sizeof( T ), 64u );
}

TEST( UniformBlockLayout, SSRResolveUBIsTheResolveMaterialsStructInBothVariants )
{
    using T                      = Desert::Graphic::MaterialSSRResolve::SSRResolveUBData;
    const std::vector<Twin> twin = {
         { "u_PrevViewProj", offsetof( T, PrevViewProj ) },
         { "u_InvJitteredViewProjection", offsetof( T, InvJitteredViewProjection ) },
         { "u_Params", offsetof( T, Params ) },
    };
    // SSRResolve.shader is GI's temporal pass (GIResolveRenderer::RecordTemporal), SSRResolveTiled.shader SSR's.
    ExpectTwin( "SSRResolve.shader", "SSRResolveUB", twin, sizeof( T ) );
    ExpectTwin( "SSRResolveTiled.shader", "SSRResolveUB", twin, sizeof( T ) );
    EXPECT_EQ( sizeof( T ), 144u );
}

TEST( UniformBlockLayout, SSAOUBIsTheSSAOMaterialsStruct )
{
    using T = Desert::Graphic::MaterialSSAO::SSAOUBData;
    ExpectTwin( "SSAO.shader", "SSAOUB",
                {
                     { "u_ViewProj", offsetof( T, ViewProj ) },
                     { "u_InvJitteredViewProjection", offsetof( T, InvJitteredViewProjection ) },
                     { "u_CameraPos", offsetof( T, CameraPos ) },
                     { "u_SSAOParams", offsetof( T, Params ) },
                },
                sizeof( T ) );
    EXPECT_EQ( sizeof( T ), 160u );
}

TEST( UniformBlockLayout, GIResolveUBIsTheGatherMaterialsStruct )
{
    using T = Desert::Graphic::MaterialGIResolve::GIResolveUBData;
    ExpectTwin( "GIResolve.shader", "GIResolveUB",
                {
                     { "u_RSMViewProj", offsetof( T, RSMViewProj ) },
                     { "u_InvRSMViewProj", offsetof( T, InvRSMViewProj ) },
                     { "u_InvJitteredViewProjection", offsetof( T, InvJitteredViewProjection ) },
                     { "u_SunColor", offsetof( T, SunColor ) },
                     { "u_Params", offsetof( T, Params ) },
                },
                sizeof( T ) );
    EXPECT_EQ( sizeof( T ), 224u );
}

TEST( UniformBlockLayout, DeferredUBHasExactlyTheDeferredLightingMaterialsProperties )
{
    const Std140::Block block =
         Std140::ParseBlock( Std140::ReadText( DeferredProgram( "DeferredLighting.shader" ) ), "DeferredUB" );
    std::set<std::pair<std::string, std::string>> glsl;
    for ( const Std140::Member& member : block.Members )
        glsl.insert( { member.Name, member.Type } );

    const std::string header = Std140::StripComments(
         Std140::ReadText( Desert::TestSupport::RepositoryRoot() / "Desert" / "Desert" / "Source" / "Engine" /
                           "Graphic" / "Materials" / "Deferred" / "MaterialDeferredLighting.hpp" ) );
    const std::regex property( R"re(MPROPERTY\(\s*(?:glm::)?(\w+)\s*,\s*\w+\s*,\s*"(\w+)")re" );
    std::set<std::pair<std::string, std::string>> cpp;
    for ( auto it = std::sregex_iterator( header.begin(), header.end(), property ); it != std::sregex_iterator();
          ++it )
        cpp.insert( { ( *it )[2], ( *it )[1] } ); // glm::vec4 / glm::mat4 spell the GLSL type

    EXPECT_FALSE( cpp.empty() ) << "no MPROPERTY parsed from MaterialDeferredLighting.hpp";
    EXPECT_EQ( glsl, cpp )
         << "DeferredUB (DeferredLighting.shader) and MaterialDeferredLighting's MPROPERTYs differ";
    EXPECT_EQ( glsl.count( { "u_InvJitteredViewProjection", "mat4" } ), 1u );
}
