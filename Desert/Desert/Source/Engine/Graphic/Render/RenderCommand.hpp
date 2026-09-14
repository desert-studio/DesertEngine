#pragma once

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
    struct RenderCommand
    {
        virtual ~RenderCommand()                                 = default;
        virtual void Execute( Graphic::SceneRenderer& renderer ) = 0;
    };

} // namespace Desert::Graphic::Render