// TAA1 step 3 — THE CAMERA BLOCK IS THE SHADER VIEW OF THE ViewFrame.
//
// (1) C++ <-> GLSL twin: ShaderProtocols::Camera has, member for member, the std140 offsets of the CameraUB block
//     parsed from Editor/Resources/Shaders/Common/CameraUB.glslh, and sizeof equals the block size (so a writer
//     uploading sizeof never runs past the reflected buffer).
//     Mutations: reordering / dropping a member on either side; removing the explicit tail pads.
// (2) MakeCameraUB is a field-for-field copy of the ViewFrame (jittered vs unjittered not swapped, Prev* from
//     the previous frame). Mutations: writing ViewProjection into JitteredViewProjection; Prev* from the current.
// (3) MakeStillViewFrame (a camera that is not a view: shadow light cameras, an editor preview) has no jitter and
//     no history: Prev* equal the current values, the jittered matrices the unjittered ones.
#include <Engine/Graphic/ShaderProtocols/Camera.hpp>
#include <Engine/Graphic/View/ViewFrame.hpp>

#include "../../TestSupport/scratch_dir.hpp"

#include <gtest/gtest.h>

#include <glm/gtc/matrix_transform.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    using Desert::Graphic::ViewFrame;
    using Desert::Graphic::ShaderProtocols::Camera;

    struct BlockMember
    {
        std::string Name;
        size_t      Offset = 0;
    };

    struct Std140Block
    {
        std::vector<BlockMember> Members;
        size_t                   Size = 0; // rounded up to the block's 16-byte base alignment
    };

    std::string ReadText( const std::filesystem::path& path )
    {
        std::ifstream      file( path, std::ios::binary );
        std::ostringstream text;
        text << file.rdbuf();
        return text.str();
    }

    size_t AlignUp( const size_t value, const size_t alignment )
    {
        return ( value + alignment - 1 ) / alignment * alignment;
    }

    // std140 for the member types the camera block uses. An unknown type fails the test instead of guessing.
    Std140Block ParseCameraBlock( const std::string& text )
    {
        Std140Block block;
        const auto  open = text.find( "CameraUB {" );
        EXPECT_NE( open, std::string::npos ) << "CameraUB block not found";
        if ( open == std::string::npos )
            return block;
        const auto close = text.find( '}', open );
        EXPECT_NE( close, std::string::npos );
        const std::string body = text.substr( open, close - open );

        const std::map<std::string, std::pair<size_t, size_t>> kSizeAlign = {
             { "float", { 4, 4 } },  { "vec2", { 8, 8 } },   { "vec3", { 12, 16 } },
             { "vec4", { 16, 16 } }, { "mat4", { 64, 16 } },
        };
        const std::regex member( R"(\b(float|vec2|vec3|vec4|mat4|[A-Za-z_]\w*)\s+([A-Za-z_]\w*)\s*;)" );
        size_t           offset = 0;
        for ( auto it = std::sregex_iterator( body.begin(), body.end(), member ); it != std::sregex_iterator();
              ++it )
        {
            const std::string type = ( *it )[1];
            const auto        info = kSizeAlign.find( type );
            EXPECT_NE( info, kSizeAlign.end() ) << "unhandled std140 type " << type;
            if ( info == kSizeAlign.end() )
                return block;
            offset = AlignUp( offset, info->second.second );
            block.Members.push_back( { ( *it )[2], offset } );
            offset += info->second.first;
        }
        block.Size = AlignUp( offset, 16 );
        return block;
    }

    ViewFrame DistinctFrame()
    {
        ViewFrame  f;
        const auto m                = []( const float v ) { return glm::mat4( v ); };
        f.Projection                = m( 2.0f );
        f.View                      = m( 3.0f );
        f.JitteredViewProjection    = m( 4.0f );
        f.ViewProjection            = m( 5.0f );
        f.PrevViewProjection        = m( 6.0f );
        f.InvJitteredViewProjection = m( 7.0f );
        f.InvViewProjection         = m( 8.0f );
        f.JitterNdc                 = glm::vec2( 0.25f, -0.25f );
        f.PrevJitterNdc             = glm::vec2( -0.125f, 0.125f );
        f.CameraPosition            = glm::vec3( 1.0f, 2.0f, 3.0f );
        f.PrevCameraPosition        = glm::vec3( 4.0f, 5.0f, 6.0f );
        f.TimeSeconds               = 10.5;
        f.PrevTimeSeconds           = 10.25;
        f.MaterialMipBias           = -0.5f;
        return f;
    }
} // namespace

TEST( CameraUBLayout, CppStructHasTheStd140OffsetsOfTheGlslBlock )
{
    const Std140Block block =
         ParseCameraBlock( ReadText( Desert::TestSupport::RepositoryRoot() / "Editor" / "Resources" / "Shaders" /
                                     "Common" / "CameraUB.glslh" ) );

    const std::vector<BlockMember> cpp = {
         { "Projection", offsetof( Camera, Projection ) },
         { "View", offsetof( Camera, View ) },
         { "JitteredViewProjection", offsetof( Camera, JitteredViewProjection ) },
         { "ViewProjection", offsetof( Camera, ViewProjection ) },
         { "PrevViewProjection", offsetof( Camera, PrevViewProjection ) },
         { "InvJitteredViewProjection", offsetof( Camera, InvJitteredViewProjection ) },
         { "JitterNdc", offsetof( Camera, JitterNdc ) },
         { "PrevJitterNdc", offsetof( Camera, PrevJitterNdc ) },
         { "CameraPos", offsetof( Camera, CameraPos ) },
         { "Time", offsetof( Camera, Time ) },
         { "PrevCameraPos", offsetof( Camera, PrevCameraPos ) },
         { "PrevTime", offsetof( Camera, PrevTime ) },
         { "MaterialMipBias", offsetof( Camera, MaterialMipBias ) },
         { "CameraPad0", offsetof( Camera, Pad0 ) },
         { "CameraPad1", offsetof( Camera, Pad1 ) },
         { "CameraPad2", offsetof( Camera, Pad2 ) },
    };

    ASSERT_EQ( block.Members.size(), cpp.size() )
         << "CameraUB.glslh and ShaderProtocols::Camera differ in members";
    for ( size_t i = 0; i < cpp.size(); ++i )
    {
        EXPECT_EQ( block.Members[i].Name, cpp[i].Name ) << "member " << i;
        EXPECT_EQ( block.Members[i].Offset, cpp[i].Offset ) << cpp[i].Name;
    }
    EXPECT_EQ( block.Size, sizeof( Camera ) );
    EXPECT_EQ( sizeof( Camera ), 448u );
}

TEST( CameraUBLayout, MakeCameraUBCopiesTheViewFrame )
{
    const ViewFrame f  = DistinctFrame();
    const Camera    ub = Desert::Graphic::ShaderProtocols::MakeCameraUB( f );
    EXPECT_EQ( ub.Projection, f.Projection );
    EXPECT_EQ( ub.View, f.View );
    EXPECT_EQ( ub.JitteredViewProjection, f.JitteredViewProjection );
    EXPECT_EQ( ub.ViewProjection, f.ViewProjection );
    EXPECT_EQ( ub.PrevViewProjection, f.PrevViewProjection );
    EXPECT_EQ( ub.InvJitteredViewProjection, f.InvJitteredViewProjection );
    EXPECT_EQ( ub.JitterNdc, f.JitterNdc );
    EXPECT_EQ( ub.PrevJitterNdc, f.PrevJitterNdc );
    EXPECT_EQ( ub.CameraPos, f.CameraPosition );
    EXPECT_EQ( ub.PrevCameraPos, f.PrevCameraPosition );
    EXPECT_FLOAT_EQ( ub.Time, 10.5f );
    EXPECT_FLOAT_EQ( ub.PrevTime, 10.25f );
    EXPECT_FLOAT_EQ( ub.MaterialMipBias, -0.5f );
}

TEST( CameraUBLayout, AStillViewFrameHasNoJitterAndNoHistory )
{
    const glm::vec3 eye( 100.0f, 200.0f, 300.0f );
    const glm::mat4 view       = glm::lookAt( eye, glm::vec3( 0.0f ), glm::vec3( 0.0f, 1.0f, 0.0f ) );
    const glm::mat4 projection = glm::perspective( glm::radians( 60.0f ), 1.5f, 10.0f, 10000.0f );

    const ViewFrame f = Desert::Graphic::MakeStillViewFrame( view, projection, eye, 2.0 );
    EXPECT_EQ( f.ViewProjection, projection * view );
    EXPECT_EQ( f.JitteredViewProjection, f.ViewProjection );
    EXPECT_EQ( f.PrevViewProjection, f.ViewProjection );
    EXPECT_EQ( f.PrevJitteredViewProjection, f.ViewProjection );
    EXPECT_EQ( f.InvJitteredViewProjection, f.InvViewProjection );
    EXPECT_EQ( f.CameraPosition, eye );
    EXPECT_EQ( f.PrevCameraPosition, eye );
    EXPECT_EQ( f.JitterNdc, glm::vec2( 0.0f ) );
    EXPECT_EQ( f.PrevJitterNdc, glm::vec2( 0.0f ) );
    EXPECT_EQ( f.TimeSeconds, 2.0 );
    EXPECT_EQ( f.PrevTimeSeconds, 2.0 );
    EXPECT_FALSE( f.HistoryValid() );
}

// THE ONE WRITER (TAA1 step 3): nothing under the engine or editor sources builds a camera block by hand. The
// struct is constructed only inside MakeCameraUB (ShaderProtocols/Camera.hpp), and MakeCameraUB is called only by
// SceneCameraBind (Materials/SceneLightingBinding.hpp), the single write of CameraUB into a material. Mutation: a
// writer filling `ShaderProtocols::Camera cam; cam.View = ...` again, or calling MakeCameraUB itself, goes red
// here.
TEST( CameraUBLayout, NothingButMakeCameraUBFillsTheCameraBlock )
{
    namespace fs        = std::filesystem;
    const fs::path root = Desert::TestSupport::RepositoryRoot();
    const fs::path owner =
         fs::path( "Desert" ) / "Desert" / "Source" / "Engine" / "Graphic" / "ShaderProtocols" / "Camera.hpp";
    const fs::path binder = fs::path( "Desert" ) / "Desert" / "Source" / "Engine" / "Graphic" / "Materials" /
                            "SceneLightingBinding.hpp";
    const std::regex declared( R"((ShaderProtocols\s*::\s*)Camera\s+[A-Za-z_]\w*\s*(;|\{|=|\())" );
    const std::regex called( R"(\bMakeCameraUB\s*\()" );

    size_t scanned = 0;
    for ( const fs::path& tree : { fs::path( "Desert" ) / "Desert" / "Source", fs::path( "Editor" ) / "Source" } )
    {
        ASSERT_TRUE( fs::is_directory( root / tree ) ) << ( root / tree ).string();
        for ( const auto& entry : fs::recursive_directory_iterator( root / tree ) )
        {
            const std::string ext = entry.path().extension().string();
            if ( !entry.is_regular_file() || ( ext != ".cpp" && ext != ".hpp" && ext != ".h" ) )
                continue;
            ++scanned;
            const fs::path    rel = fs::relative( entry.path(), root );
            std::ifstream     in( entry.path(), std::ios::binary );
            std::stringstream text;
            text << in.rdbuf();
            const std::string source = text.str();
            if ( rel != owner )
                EXPECT_FALSE( std::regex_search( source, declared ) )
                     << rel.generic_string() << " builds a ShaderProtocols::Camera by hand; fill CameraUB through "
                     << "SceneCameraBind( material, ViewFrame ) (MakeStillViewFrame for a camera that is not a "
                        "view)";
            if ( rel != owner && rel != binder )
                EXPECT_FALSE( std::regex_search( source, called ) )
                     << rel.generic_string() << " calls MakeCameraUB itself; SceneCameraBind is the one writer";
        }
    }
    EXPECT_GT( scanned, 100u ) << "the scan found too few sources to mean anything";
}
