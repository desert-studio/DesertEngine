#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>

#include <cstdint>
#include <memory>

namespace Desert::ShaderResources
{
    class StorageBuffer;
}

namespace Desert::Graphic
{
    class ComputePipeline;
}

namespace Desert::Graphic::RDG
{
    class Builder;
}

// GRASS-VIS: THE OWNER OF A VIEW'S VISIBILITY BUFFER (UE 5 Nanite's VisBuffer64 and its InitVisBuffer clear). One
// per SceneRenderer (a view): the storage buffer Common/VisibilityBuffer.glslh reads as `VisibilityBuffer` (a
// 16-byte header with the extent, then one uint64 word per pixel), sized to the view's render extent and recreated
// when it changes, emptied every frame by "VirtualGeometry: Visibility Clear" before the rasters that atomic-max
// into it. It also owns the resolve's argument table: the grass resolve is one full-screen triangle per blade
// variety drawn through the mesh renderer's indirect path, so its record ({3, 1, 0, 0, 0}: the blade strip's first
// three indices, which Vertex_GrassBlade.glslh's resolve branch turns into a full-screen triangle) lives in a
// GpuInstanceArgs-shaped buffer (header + one record per submesh; records past the first are zero and hidden by
// the draw's submesh mask).
namespace Desert::Graphic::VirtualGeometry
{
    // This frame's graph handles of the buffer and the resolve's arguments.
    struct VisibilityBufferFrame
    {
        RDG::BufferRef Words;       // Common/VisibilityBuffer.glslh `VisibilityBuffer` (binding 62)
        RDG::BufferRef ResolveArgs; // GpuInstanceArgs header + record 0 = {3, 1, 0, 0, 0}
        uint32_t       Width  = 0;
        uint32_t       Height = 0;

        [[nodiscard]] bool IsValid() const
        {
            return Words.IsValid() && ResolveArgs.IsValid();
        }
    };

    class VisibilityBuffer
    {
    public:
        // The clear program (Programs/VirtualGeometry/VisibilityClear.shader) and the resolve's argument table;
        // an error names what is missing (the view then refuses the visibility path by that error).
        [[nodiscard]] Common::BoolResultStr Initialize();
        [[nodiscard]] bool                  IsInitialized() const
        {
            return m_Clear != nullptr && m_ResolveArgs != nullptr;
        }

        // Registers the buffer (resized to @p width x @p height pixels) and the resolve's arguments in @p graph
        // and adds "VirtualGeometry: Visibility Clear". An invalid frame with the error when it could not.
        [[nodiscard]] Common::ResultStr<VisibilityBufferFrame> AddFrame( RDG::Builder& graph, uint32_t width,
                                                                         uint32_t height );

    private:
        std::shared_ptr<ComputePipeline>                m_Clear;
        mutable ShaderBindingLayoutCache                m_ClearLayout;
        std::shared_ptr<ShaderResources::StorageBuffer> m_Words;
        std::shared_ptr<ShaderResources::StorageBuffer> m_ResolveArgs;
        RDG::ExternalBuffer                             m_WordsImport;
        RDG::ExternalBuffer                             m_ResolveArgsImport;
        uint32_t                                        m_Width  = 0;
        uint32_t                                        m_Height = 0;
    };
} // namespace Desert::Graphic::VirtualGeometry
