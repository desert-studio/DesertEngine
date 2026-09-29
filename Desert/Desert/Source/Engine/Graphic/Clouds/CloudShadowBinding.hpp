#pragma once

#include <Engine/Graphic/Clouds/CloudShadowPayload.hpp>

#include <Engine/Graphic/Materials/Material.hpp>
#include <Engine/Graphic/Materials/Properties/Texture2DProperty.hpp>
#include <Engine/Graphic/Materials/Properties/UniformBufferProperty.hpp>

#include <cstddef>

namespace Desert::Graphic
{
    /**
     * THE ONE WRITER of a shader's cloud-shadow bindings.
     *
     * Every shader that receives the cloud layer's shadow compiles the same receiver text
     * (Engine/Content/Shaders/Common/CloudShadowReceiver.glslh), which names exactly two resources —
     * `u_CloudShadowMap` and the `CloudShadowUB` block. The shaders declare them at different SLOT
     * NUMBERS (the deferred composite is a fullscreen material with a layout of its own, the four mesh
     * shaders share one with the G-buffer pass, the terrain has a small one), but every material in this
     * engine binds by NAME, so one function can serve all of them.
     *
     * It is a function and not four call sites for the reason Р16 made the ambient one header and Р20
     * made the direct-light BRDF one header: the two render paths shade the same materials in the same
     * scene, and every quantity this project has ever had two implementations of eventually disagreed.
     * The measured disagreement here was the whole remaining gap between the paths — 64.93/255 of ground
     * through the deferred path against 109.11/255 through the forward one on Clouds_Showcase with clouds
     * on, a factor of 1.68 — because one shader in the tree read the map and none of the others did.
     *
     * When the layer is not casting (no cloud component, clouds off, casting off, strength zero, or a
     * renderer whose scene has no sky) nothing is bound: the sampler keeps its dummy image and `Params.y`
     * is 0, which is the one number the shader tests before it fetches. A scene with no clouds therefore
     * costs one uniform upload of eighty bytes and not a texture read per pixel.
     *
     * @param material  the material whose descriptor set carries the pair. Silently does nothing for a
     *                  material whose shader declares neither — that is not a fallback, it is how a
     *                  shared frame-state applier reaches materials of several shaders (the shadow
     *                  cascades and the environment cubes are bound the same way, in
     *                  Graphic::SceneShadowBind / Graphic::SceneEnvironmentBind).
     */
    inline void CloudShadowBind( Material* material, const CloudShadowInput& cloudShadow )
    {
        if ( !material )
            return;

        const CloudShadowUniforms data = CloudShadowPackUniforms( cloudShadow );

        if ( auto* ub = material->Get<UniformBufferProperty>( "CloudShadowUB" ) )
            ub->SetRawData( reinterpret_cast<const std::byte*>( &data ), sizeof( data ) );

        // The image only when there is one. Binding a null would drop the descriptor's dummy and leave
        // the slot undefined for a shader that is about to be told, by Params.y, not to read it.
        if ( cloudShadow.IsLive() )
            if ( auto* tex = material->Get<Texture2DProperty>( "u_CloudShadowMap" ) )
                tex->SetImage( cloudShadow.Map );
    }
} // namespace Desert::Graphic
