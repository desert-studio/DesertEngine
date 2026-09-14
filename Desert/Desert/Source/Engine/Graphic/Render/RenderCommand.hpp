#pragma once

// PRE-EXISTING CYCLE, NAMED RATHER THAN HIDDEN. SceneRenderer.hpp includes the command buffer, which
// includes this. clang-tidy reports it against whichever unit in the cycle a change touches, so it
// surfaced when Г26 edited SceneRenderer.hpp; breaking the cycle means forward-declaring SceneRenderer
// here and moving Execute's body out of every command header, which is a task of its own.
// NOLINTNEXTLINE(misc-header-include-cycle)
#include <Engine/Graphic/SceneRenderer.hpp>

namespace Desert::Graphic::Render
{
    struct RenderCommand
    {
        virtual ~RenderCommand()                                 = default;
        virtual void Execute( Graphic::SceneRenderer& renderer ) = 0;
    };

} // namespace Desert::Graphic::Render