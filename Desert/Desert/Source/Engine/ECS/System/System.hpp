#pragma once

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <Common/Core/Timestep.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Graphic/Render/RenderCommandBuffer.hpp>

namespace Desert::ECS
{
    class System
    {
    public:
        explicit System() = default;

        System( const System& )            = delete;
        System& operator=( const System& ) = delete;
        System( System&& )                 = delete;
        System& operator=( System&& )      = delete;

        virtual ~System() = default;

        virtual void Update( entt::registry& registry, Graphic::Render::RenderCommandBuffer& renderCommandBuffer,
                             const Common::Timestep& ts ) = 0;

        // Systems that only READ the registry (no structural changes, no writes another system reads
        // within the frame) and emit render commands may return true: the scene runs consecutive
        // parallel-capable systems concurrently on the JobSystem, each with its OWN command buffer
        // (buffers execute in registration order afterwards, so draw order stays deterministic).
        // Anything touching Lua, input, physics, or components other systems consume must stay false.
        virtual bool CanRunParallel() const
        {
            return false;
        }

        // Per-frame active-camera snapshot pushed by the Scene on the main thread BEFORE the (possibly
        // parallel) system group runs, so camera-relative systems read it race-free. Default no-op —
        // only systems that lay out geometry relative to the viewer (e.g. billboarded text) override it.
        virtual void SetCameraSnapshot( const glm::mat4& /*view*/, const glm::vec3& /*position*/ )
        {
        }

        // The EDITOR'S frame time, pushed by the Scene on the main thread before the systems run: the real
        // frame delta while the world is in Edit, zero in Play and Paused (whose time is the gameplay
        // timestep Update receives). Default no-op — only a system that previews in the editor world on the
        // author's request reads it (AnimationECSSystem: AnimationComponent::UpdateAnimationInEditor).
        virtual void SetEditorTick( const Common::Timestep& /*editorTs*/ )
        {
        }
    };

} // namespace Desert::ECS