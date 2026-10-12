#pragma once

#include <Engine/Graphic/Materials/Material.hpp>
#include <Engine/Graphic/Materials/Properties/StorageBufferProperty.hpp>
#include <Engine/Core/Camera.hpp>
#include <Engine/Graphic/Materials/SceneLightingBinding.hpp>

#include <glm/glm.hpp>

#include <vector>

namespace Desert::Graphic
{
    // World-space debug line list (AABB wireframes, gizmos, ...). Endpoints live in a storage buffer the
    // "DebugLine" shader pulls by gl_VertexIndex; drawn via Renderer::SubmitPulled on a Lines-topology
    // pipeline - or a Triangles one, where every three vertices are a flat-coloured triangle (the Create Shape
    // tool's translucent preview mesh). Feeds only the shared CameraUB + the Lines storage buffer.
    class MaterialDebugLine final : public Material
    {
    public:
        struct LineVertex
        {
            glm::vec4 PositionWS; // xyz world position
            glm::vec4 Color;      // rgba
        };

        MaterialDebugLine() : Material( "MaterialDebugLine", "DebugLine" )
        {
        }

        // The camera block from the view the lines are drawn over (MakeCameraUB; the shader reads the unjittered
        // ViewProjection), then the lines themselves.
        void Update( const ViewFrame& view, const std::vector<LineVertex>& lines )
        {
            SceneCameraBind( this, view );

            if ( !lines.empty() )
                if ( auto* sb = Get<StorageBufferProperty>( "Lines" ) )
                    sb->SetRawData( const_cast<LineVertex*>( lines.data() ),
                                    static_cast<uint32_t>( lines.size() * sizeof( LineVertex ) ) );
        }
    };
} // namespace Desert::Graphic
