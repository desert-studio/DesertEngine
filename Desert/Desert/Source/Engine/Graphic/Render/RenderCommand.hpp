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
// FORWARD-DECLARED, NOT INCLUDED, AND THE CYCLE IS THE REASON.
//
// This header's only use of SceneRenderer is a reference parameter, and including it closed a loop:
// RenderCommand.hpp -> SceneRenderer.hpp -> Core/Scene.hpp -> ECS/System/System.hpp ->
// Render/RenderCommandBuffer.hpp -> RenderCommand.hpp. The build survived it on `#pragma once` plus the
// order a .cpp happens to include things in, which is not a property anyone can rely on — and it is
// observable: with RenderCommandBuffer.hpp as the main file, Scene.hpp does not parse at all
// ("no member named 'System' in namespace 'Desert::ECS'"), because System.hpp reaches it half-defined.
// clang-tidy analyses a changed header exactly that way, so the loop also made every edit to that header
// report six errors about somebody else's file.
namespace Desert::Graphic
{
    class SceneRenderer;
}

namespace Desert::Graphic::Render
{
    // A RECORDED COMMAND HAS TWO HALVES, AND ONLY THE FIRST MAY WRITE WHAT THE COMMAND DOES NOT OWN (CPU-ONE-SYNC
    // step 4; UE: a proxy's render resources are initialised before the rendering thread draws it, and the mesh
    // draw command passes only read).
    struct RenderCommand
    {
        virtual ~RenderCommand() = default;
        // THE INTAKE. GAME THREAD, ONCE PER RECORDING, at the update's fence (Scene::OnUpdate: no job in flight,
        // before the first replay): what the replays need from an object the command only points at is made
        // here - a mesh's device buffers from its CPU geometry (SceneRenderer::PrepareMesh). Default: nothing.
        virtual void Prepare()
        {
        }
        // THE REPLAY. Any number of times per recording - the capture faces at the fence, then every view on the
        // render task - so it READS the command and what it points at and writes only @p renderer's per-frame
        // lists. It never moves a member out (the next replay would get a moved-from value) and never writes a
        // mesh, a material or the world.
        virtual void Execute( Graphic::SceneRenderer& renderer ) = 0;
    };

} // namespace Desert::Graphic::Render
