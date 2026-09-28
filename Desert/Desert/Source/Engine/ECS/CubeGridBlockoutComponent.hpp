#pragma once

#include <Common/Json/Document.hpp>

#include <Engine/Geometry/VoxelBlockout.hpp>

#include <string>
#include <utility>

namespace Desert::ECS
{
    // THE VOXELS A CUBE GRID BLOCKOUT WAS BAKED FROM, kept beside its mesh (Modeling spec A3) so the tool can be
    // opened on the entity again and Push / Pull carries on over the same pieces (UE: UCubeGridTool takes the
    // selected mesh as its target). Frames are in the ENTITY's space, so moving or turning the entity carries
    // the grid with it. Written by the tool's Accept, read by its reopen; the mesh stays the entity's source of
    // truth for drawing - this is the tool's input, not a second copy of the geometry.
    //
    // Its own header, not Components.hpp: only the tool, the registry and its tests need it.
    struct CubeGridBlockoutComponent
    {
        Geometry::VoxelBlockout::SavedBlockout Saved;
    };

    // The scene block is the saved form as it is (reflect-cpp writes the struct).
    inline Common::Json::Value WriteCubeGridBlockout( const CubeGridBlockoutComponent& c )
    {
        return Common::Json::Value( Common::Json::ObjectBuilder()
                                         .Set( "Layers", c.Saved.Layers )
                                         .Set( "MeshKey", c.Saved.MeshKey )
                                         .Build() );
    }

    // A block that does not read, or reads to voxels Geometry::VoxelBlockout::Load refuses, is an Issue naming
    // the block and the reason, and gives no component: a blockout that cannot be reopened must not look as if
    // it could.
    inline std::optional<CubeGridBlockoutComponent> ReadCubeGridBlockout( const Common::Json::Node& block,
                                                                          Common::Json::Issues&     issues )
    {
        auto saved = block.As<Geometry::VoxelBlockout::SavedBlockout>();
        if ( !saved.IsSuccess() )
        {
            block.Report( issues, "a CubeGrid blockout (" + saved.GetError() + ")" );
            return std::nullopt;
        }
        if ( auto loaded = Geometry::VoxelBlockout::Load( saved.GetValue() ); !loaded.IsSuccess() )
        {
            block.Report( issues, "a CubeGrid blockout (" + loaded.GetError() + ")" );
            return std::nullopt;
        }
        if ( !Geometry::VoxelBlockout::ParseMeshKey( saved.GetValue().MeshKey ) )
        {
            block.Report( issues, "a CubeGrid blockout (MeshKey '" + saved.GetValue().MeshKey +
                                       "' is not 16 hex digits)" );
            return std::nullopt;
        }
        return CubeGridBlockoutComponent{ saved.ExtractValue() };
    }
} // namespace Desert::ECS
