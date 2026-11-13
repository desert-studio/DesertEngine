#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>

#include <cstdint>
#include <memory>
#include <span>

namespace Desert::ShaderResources
{
    class StorageBuffer;
}

namespace Desert::Graphic
{
    class ComputePipeline;
    class GraphicsPipeline;
}

namespace Desert::Graphic::RDG
{
    class Builder;
    class PassBuilder;
    class PassContext;
}

// GRASS-VIS: THE OWNER OF A VIEW'S VISIBILITY BUFFER (UE 5 Nanite's VisBuffer64 and its InitVisBuffer clear). One
// per SceneRenderer (a view): the storage buffer Common/VisibilityBuffer.glslh reads as `VisibilityBuffer` (a
// 16-byte header with the extent, then one uint64 word per pixel), sized to the view's render extent and recreated
// when it changes, emptied every frame by "VirtualGeometry: Visibility Clear" before the rasters that atomic-max
// into it; after the pixels' words it holds one record per 16 x 16 tile (Common/VisibilityTiles.glslh), written by
// "Grass: Visibility Classify" (FPS-GRASS-RESOLVE; UE 5 Nanite material tile culling) after the last raster. It
// also owns the resolve's argument table: the grass resolve is two triangles per tile per blade variety drawn
// through the mesh renderer's indirect path, so its record ({3, 2 x tiles, 0, 0, 0}: the blade strip's first three
// indices, which Vertex_GrassBlade.glslh's resolve branch places over a tile - or collapses, when the tile holds
// none of the variety's rows - the instance count written by the classify pass) lives in a GpuInstanceArgs-shaped
// buffer (header + one record per submesh; records past the first are zero and hidden by the draw's submesh mask).
// And it owns "Grass: Visibility Depth" (UE Nanite's EmitDepthTargets): the words' depth
// written into the G-buffer depth without the material, between phase 1's raster and the HZB phase 2 tests.
namespace Desert::Graphic::VirtualGeometry
{
    // This frame's graph handles of the buffer and the resolve's arguments.
    struct VisibilityBufferFrame
    {
        RDG::BufferRef Words;       // Common/VisibilityBuffer.glslh `VisibilityBuffer` (binding 62)
        RDG::BufferRef ResolveArgs; // GpuInstanceArgs header + record 0 = {3, 2 x tiles, 0, 0, 0}
        RDG::BufferRef Sections;    // count + the sections' first rows (VisibilityMaterialDepth.shader)
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
            return m_Clear != nullptr && m_Classify != nullptr && m_Depth != nullptr &&
                   m_MaterialDepth != nullptr && m_ResolveArgs != nullptr;
        }

        // Registers the buffer (resized to @p width x @p height pixels), the resolve's arguments and this frame's
        // sections (@p sectionFirstRows: the first raster-stream row of every section GrassRenderer handed rows
        // to, ascending - GrassRenderer::VisibilitySectionFirstRows) in @p graph and adds "VirtualGeometry:
        // Visibility Clear". An invalid frame with the error when it could not.
        [[nodiscard]] Common::ResultStr<VisibilityBufferFrame>
        AddFrame( RDG::Builder& graph, uint32_t width, uint32_t height,
                  std::span<const uint32_t> sectionFirstRows );

        // Adds "Grass: Visibility Material Depth" (VisibilityMaterialDepth.shader; UE 5 Nanite's
        // EmitMaterialDepth) and returns its target: a DEPTH32F of the frame's extent, cleared to 0, holding at
        // every pixel a drawn word not behind @p sceneDepth (the G-buffer depth, sampled) the material depth of
        // the section owning its row (Common/VisibilityTiles.glslh DesertVisibilityMaterialDepth). The resolves
        // draw on it as their depth target and test EQUAL. After the last raster into @p frame's words; invalid
        // when nothing was added.
        [[nodiscard]] RDG::TextureRef AddMaterialDepth( RDG::Builder& graph, const VisibilityBufferFrame& frame,
                                                        RDG::TextureRef sceneDepth ) const;

        // Adds "Grass: Visibility Classify" (VisibilityClassify.shader): every tile's record of the rows its
        // pixels hold, and record 0's instance count (two triangles per tile). After the LAST raster into
        // @p frame's words
        // (phase 2's) and before the resolves, whose draws read both.
        void AddClassify( RDG::Builder& graph, const VisibilityBufferFrame& frame ) const;

        // SETUP of "Grass: Visibility Depth" (a raster node on the G-buffer depth alone, LOADed): its one block,
        // @p words read by the fragment stage (Programs/VirtualGeometry/VisibilityDepth.shader).
        void DeclareDepthBindings( RDG::PassBuilder& pass, RDG::BufferRef words ) const;
        // Inside that node's render pass: the full-screen triangle that writes each drawn word's depth.
        [[nodiscard]] Common::BoolResultStr RecordDepth( const RDG::PassContext& context ) const;

    private:
        std::shared_ptr<ComputePipeline>                m_Clear;
        mutable ShaderBindingLayoutCache                m_ClearLayout;
        std::shared_ptr<ComputePipeline>                m_Classify;
        mutable ShaderBindingLayoutCache                m_ClassifyLayout;
        std::shared_ptr<GraphicsPipeline>               m_Depth;
        mutable ShaderBindingLayoutCache                m_DepthLayout;
        std::shared_ptr<GraphicsPipeline>               m_MaterialDepth;
        mutable ShaderBindingLayoutCache                m_MaterialDepthLayout;
        // The sections' first rows (count, then the rows; per frame in flight), grown to the largest count seen.
        std::shared_ptr<ShaderResources::StorageBuffer> m_Sections;
        RDG::ExternalBuffer                             m_SectionsImport;
        uint32_t                                        m_SectionCapacity = 0;
        std::shared_ptr<ShaderResources::StorageBuffer> m_Words;
        std::shared_ptr<ShaderResources::StorageBuffer> m_ResolveArgs;
        RDG::ExternalBuffer                             m_WordsImport;
        RDG::ExternalBuffer                             m_ResolveArgsImport;
        uint32_t                                        m_Width  = 0;
        uint32_t                                        m_Height = 0;
    };
} // namespace Desert::Graphic::VirtualGeometry
