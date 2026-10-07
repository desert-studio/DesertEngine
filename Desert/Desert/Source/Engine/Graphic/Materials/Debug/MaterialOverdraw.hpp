#pragma once

#include <Engine/Graphic/Materials/Material.hpp>
#include <Engine/Graphic/Materials/Properties/UniformBufferProperty.hpp>
#include <Engine/Graphic/Materials/SceneLightingBinding.hpp>
#include <Engine/Core/Camera.hpp>

namespace Desert::Graphic
{
    // Overdraw accumulation material: only feeds the shared camera UB (the per-mesh transform is pushed
    // automatically by Renderer::RenderMesh). Drives Overdraw.shader. Header-only (no new .cpp -> no premake
    // regen).
    class MaterialOverdraw final : public Material
    {
    public:
        MaterialOverdraw() : Material( "MaterialOverdraw", "Overdraw" )
        {
        }

        // The camera block from the view the heat map is drawn for (MakeCameraUB).
        void UpdateCamera( const ViewFrame& view )
        {
            SceneCameraBind( this, view );
        }
    };
} // namespace Desert::Graphic
