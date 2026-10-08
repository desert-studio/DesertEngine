#pragma once

#include <Engine/Graphic/Materials/Material.hpp>
#include <Engine/Graphic/Materials/Properties/UniformBufferProperty.hpp>
#include <Engine/Graphic/Materials/SceneLightingBinding.hpp>
#include <Engine/Core/Camera.hpp>

#include <memory>

namespace Desert::Graphic
{
    // Billboard material for the GPU particle system. Feeds only the CameraUB (the "ParticleBillboard" shader
    // reads size/colour that the compute pass baked into each particle, so there are no per-emitter uniforms).
    // The particle storage buffer is a pass parameter: ParticlePass binds its graph handle as "Particles"
    // through RDG::PassBindings, from the StorageRead it declares.
    //
    // ONE INSTANCE PER EMITTER (ParticleRenderer::EmitterGpu), never one shared across emitters. The
    // buffer is a descriptor, a descriptor set belongs to the material, and the set is written at most
    // once per frame BEFORE its first bind (VulkanMaterialBackend's per-frame stamp; rewriting a set
    // bound in a recording command buffer is illegal without update-after-bind). The previous contract
    // here — "rebound each draw" on a shared material — was unimplementable: the second and later
    // emitters' rebinds were silently swallowed and every emitter drew the first one's buffer.
    class MaterialParticleBillboard final : public Material
    {
    public:
        MaterialParticleBillboard() : Material( "MaterialParticleBillboard", "ParticleBillboard" )
        {
        }

        // The camera block from the view the particles are drawn for (MakeCameraUB).
        void Update( const ViewFrame& view )
        {
            SceneCameraBind( this, view );
        }
    };
} // namespace Desert::Graphic
