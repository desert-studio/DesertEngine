#pragma once

#include "../RenderCommand.hpp"
// The renderer itself: every Execute below calls a method on it, and RenderCommand.hpp only
// forward-declares the type (see the note there on the include cycle that cost).
#include <Engine/Graphic/SceneRenderer.hpp>

#include <Engine/ECS/VolumetricCloudComponent.hpp>
#include <Engine/Graphic/Clouds/CloudAuthoredPayload.hpp>

#include <glm/glm.hpp>

#include <utility>
#include <vector>

namespace Desert::Graphic::Render
{
    // Carries this frame's cloud layer from the ECS to the cloud renderer.
    //
    // `Present` is said explicitly instead of being implied by the command's absence: the renderer keeps
    // its settings across frames, so a scene whose cloud component was deleted would otherwise go on
    // marching the last layer it was told about.
    //
    // `WindOffset` rides here rather than being recomputed by the renderer because the accumulator needs
    // the frame's timestep, which only the ECS tick has. Two accumulators — one per side — would drift
    // apart the first time a frame was skipped on one of them, and the symptom would be the sky sliding
    // relative to its own shadows.
    struct VolumetricCloudCommand : RenderCommand
    {
        bool                     Present;
        ECS::VolumetricCloudData Data;
        glm::vec3                WindOffset; // world units, accumulated

        // THE HERO CLOUDS RIDE IN THE LAYER'S COMMAND rather than in one of their own, and the reason is
        // that they are not a second subsystem: they are slot A of the same field, joined by a `max`
        // inside the same shader, marched by the same loop. A separate command would have created an
        // order between two halves of one frame's cloud state, and the first frame they arrived out of
        // step would have marched last frame's bodies against this frame's layer.
        //
        // Empty is the ordinary case and costs nothing to carry: a scene with no hero cloud sends an
        // empty vector, the renderer packs a count of zero, and the march's authored loop does not run.
        std::vector<HeroCloudInstance> HeroClouds;

        VolumetricCloudCommand( bool present, const ECS::VolumetricCloudData& data, const glm::vec3& windOffset,
                                std::vector<HeroCloudInstance> heroClouds = {} )
             : Present( present ), Data( data ), WindOffset( windOffset ), HeroClouds( std::move( heroClouds ) )
        {
        }

        void Execute( SceneRenderer& renderer ) override
        {
            renderer.SetVolumetricClouds( Present, Data, WindOffset, HeroClouds );
        }
    };
} // namespace Desert::Graphic::Render
