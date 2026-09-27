#pragma once

namespace Desert::Graphic
{
    typedef unsigned int RenderingID;
    typedef unsigned int BindingPoint;
    typedef unsigned int SetPoint;

    enum class BufferUsage
    {
        None    = 0,
        Static  = 1,
        Dynamic = 2
    };

    // `enum class BindUsage { Bind, Unbind }` STOOD HERE AND EXISTED ONLY TO BE THE PARAMETER OF A
    // FUNCTION NOBODY CALLED. It went with `Use`/`RT_Use`, the OpenGL "bind this object, now unbind it"
    // idiom: eight pure virtuals across five bases (Image, Shader, VertexBuffer, IndexBuffer,
    // Framebuffer), seventeen implementations under them, and EVERY ONE OF THE SEVENTEEN HAD AN EMPTY
    // BODY. There was nothing to call, so nothing called it, so nothing broke — for as long as it took
    // the pure-virtual census (Desert/Tests/Engine/PureVirtualCensus) to count them.
    //
    // WHY DELETED RATHER THAN IMPLEMENTED, stated so it is not re-derived: a Vulkan backend does not
    // bind objects one at a time. It binds a descriptor SET, built once per material and bound once per
    // draw, and vertex/index buffers are named in the draw command itself. `RendererAPIType` has exactly
    // two values — `None` and `Vulkan` — and the tree carries no other rendering backend or RHI
    // library. There is no second backend for this seam to serve, and if one is ever written it will
    // not want `Unbind` either.
    //
    // What this cost to keep: five base interfaces each carried a method every implementer had to write
    // and could not omit, so the abstraction taxed every new backend object with a body that does
    // nothing. That is the argument against leaving a dead pure virtual in place — it is not inert,
    // it is a standing instruction to write empty code.

} // namespace Desert::Graphic