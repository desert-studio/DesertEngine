#pragma once

#include <Editor/Core/Selection/ModelingState.hpp>

#include <Common/Core/ResultStr.hpp>
#include <Common/Core/UUID.hpp>

#include <Engine/Geometry/EditMeshBridge.hpp>
#include <Engine/Geometry/VoxelBlockout.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <format>
#include <functional>
#include <string_view>
#include <vector>

namespace Desert::Editor::Tools
{
    // What Accept and Cancel do to the Cube Grid's in-progress piece and to the tool, apart from ImGui and
    // the scene so a suite can drive them. CG1: one refused Accept had already ended the tool (the tool bar
    // ended it before the tool resolved the request), so the piece stayed the tool's - never selected or
    // recorded, its overlay kept drawing, and the next grid's Cancel destroyed it with the new cells. So:
    //  - Accept commits and forgets the piece only when Output's write succeeded; a refusal keeps the piece
    //    AND the tool and is returned for the tool to show.
    //  - Only a successful Accept that asked to end the tool (the tool bar's Accept, not "Accept and Start
    //    New") ends it.
    //  - Cancel destroys only the piece the session still holds - never an accepted one.
    using BlockoutWrite  = std::function<Common::BoolResultStr( const Common::UUID& piece )>;
    using BlockoutCommit = std::function<void( const Common::UUID& piece )>;

    [[nodiscard]] inline Common::BoolResultStr AcceptBlockout( Common::UUID& piece, Core::ModelingState& ms,
                                                               bool endTool, const BlockoutWrite& write,
                                                               const BlockoutCommit& commit )
    {
        if ( piece != Common::UUID::Null() )
        {
            if ( auto written = write( piece ); !written.IsSuccess() )
                return written;
            commit( piece );
            piece = Common::UUID::Null();
        }
        if ( endTool )
            ms.ActiveTool = Core::ModelingState::Tool::None;
        return Common::MakeSuccess( true );
    }

    inline void CancelBlockout( Common::UUID& piece, const BlockoutCommit& destroy )
    {
        if ( piece != Common::UUID::Null() )
            destroy( piece );
        piece = Common::UUID::Null();
    }

    // The key the staleness check compares (VoxelBlockout::MeshKey) of the mesh a tool reads as its target.
    [[nodiscard]] inline uint64_t MeshKeyOf( const Geometry::DynamicMesh3& mesh )
    {
        std::vector<glm::vec3> positions;
        positions.reserve( static_cast<size_t>( mesh.VertexCount() ) );
        for ( const int v : mesh.VertexIndicesItr() )
            positions.emplace_back( mesh.GetVertex( v ) );
        return Geometry::VoxelBlockout::MeshKey( std::move( positions ), mesh.TriangleCount() );
    }

    // REOPEN (UE: the Cube Grid tool takes the selected mesh as its target, and Push / Pull goes on over its
    // geometry). What the tool edits when it is opened on entity `name`: the voxels the entity carries, carried
    // into the world by the entity's transform, and that transform as a grid frame (the bake goes back through
    // its inverse). Refused, by name and reason, when the entity carries no voxels, when its mesh no longer is
    // what they baked to (another tool edited it: re-baking would silently overwrite that edit), when it is
    // scaled (a grid frame has no scale), or when the voxels do not load.
    struct ReopenedBlockout
    {
        Geometry::VoxelBlockout::Volume    Volume;
        Geometry::VoxelBlockout::GridFrame EntityFrame;
    };
    [[nodiscard]] inline Common::ResultStr<ReopenedBlockout>
    ReopenBlockout( std::string_view name, const Geometry::VoxelBlockout::SavedBlockout* saved,
                    const Common::ResultStr<uint64_t>& meshKey, const glm::mat4& world )
    {
        namespace VB = Geometry::VoxelBlockout;
        if ( !saved )
            return Common::MakeError<ReopenedBlockout>(
                 std::format( "'{}' was not built by CubeGrid: it carries no voxels to edit", name ) );
        if ( !meshKey.IsSuccess() )
            return Common::MakeError<ReopenedBlockout>(
                 std::format( "the mesh of '{}' cannot be read: {}", name, meshKey.GetError() ) );
        if ( VB::ParseMeshKey( saved->MeshKey ) != meshKey.GetValue() )
            return Common::MakeError<ReopenedBlockout>(
                 std::format( "the mesh of '{}' was edited after CubeGrid built it (key {} is now {:016x}); "
                              "reopening would overwrite that edit",
                              name, saved->MeshKey, meshKey.GetValue() ) );
        for ( int axis = 0; axis < 3; ++axis )
            if ( const float len = glm::length( glm::vec3( world[axis] ) ); std::abs( len - 1.0f ) > 1e-3f )
                return Common::MakeError<ReopenedBlockout>(
                     std::format( "'{}' is scaled ({:.3f} along axis {}), and a CubeGrid grid has no scale", name,
                                  len, axis ) );
        auto loaded = VB::Load( *saved );
        if ( !loaded.IsSuccess() )
            return Common::MakeError<ReopenedBlockout>( std::format( "'{}': {}", name, loaded.GetError() ) );
        if ( loaded.GetValue().m_Frozen.empty() )
            return Common::MakeError<ReopenedBlockout>( std::format( "'{}' carries an empty blockout", name ) );
        ReopenedBlockout out;
        out.EntityFrame.Origin   = glm::vec3( world[3] );
        out.EntityFrame.Rotation = glm::normalize( glm::quat_cast( glm::mat3( world ) ) );
        out.Volume               = VB::Reframed( loaded.GetValue(), out.EntityFrame );
        return Common::MakeSuccess( std::move( out ) );
    }
} // namespace Desert::Editor::Tools
