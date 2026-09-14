#pragma once

// A FORWARD DECLARATION AND NOT AN INCLUDE, BECAUSE THE INCLUDE WAS A CYCLE.
//
// This header used to `#include <Engine/Graphic/SceneRenderer.hpp>`, and SceneRenderer.hpp reaches
// Scene.hpp, which reaches ECS/System/System.hpp, which includes RenderCommandBuffer.hpp, which includes
// THIS file. Compiling a translation unit that reached RenderCommand.hpp second was fine — the guard had
// already fired and the type was complete by then — so the cycle never broke a build and nobody saw it.
// It shows the moment this header is the ROOT of a translation unit, which is exactly what clang-tidy
// does when it analyses a changed header: `RenderCommandBuffer.hpp:33: use of undeclared identifier
// 'RenderCommand'`. Measured 2026-09-14 (Г26), while editing SceneRenderer.hpp for an unrelated reason.
//
// `Execute` takes SceneRenderer by REFERENCE, so this file never needed the definition. The command
// headers that CALL something on the renderer include SceneRenderer.hpp themselves — which is where the
// dependency actually is, and which is what makes the graph acyclic rather than merely ordered.
namespace Desert::Graphic
{
    class SceneRenderer;
}

namespace Desert::Graphic::Render
{
    struct RenderCommand
    {
        virtual ~RenderCommand()                                 = default;
        virtual void Execute( Graphic::SceneRenderer& renderer ) = 0;
    };

} // namespace Desert::Graphic::Render
