#pragma once

#include <Engine/Graphic/RDG/RDGAccess.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Desert::Graphic
{
    class Image;

    // WHAT ONE FRAME-GRAPH NODE TOUCHES, named as engine resources. The system that records the node fills it
    // where it registers the node (RenderGraphBuilder::PassConfig::Declare for a phase pass, the node list of an
    // atmosphere system for a compute dispatch, ExternalPassSpecification::Declare for an editor pass), and
    // SceneRenderer resolves every image to the frame graph's import of it (one registration per engine image, so
    // two nodes naming one image name one graph texture) and declares each entry on the node. The graph then
    // places every barrier and layout change between the node and its neighbours; the node's body records no
    // barrier.
    //
    // An image is named with the access its shaders make (SampledGraphics for a fragment-shader sample,
    // SampledCompute / StorageWrite for a compute dispatch). A buffer is named by the graph handle its owner
    // registered this frame (a buffer is imported by the system that owns it, e.g. ParticleRenderer's state).
    // The attachments a raster node renders into are NOT listed here: they are its target framebuffer, whole.
    class RenderPassDeclaration
    {
    public:
        struct ImageUse
        {
            std::shared_ptr<Image> Image;
            RDG::Access            Access = RDG::Access::None;
            bool                   Writes = false;
            std::string            Name; // the graph name of the import, when this entry registers it first
        };

        struct BufferUse
        {
            RDG::BufferRef Buffer;
            RDG::Access    Access = RDG::Access::None;
            bool           Writes = false;
        };

        void Read( std::shared_ptr<Image> image, RDG::Access access, std::string name )
        {
            if ( image )
                m_Images.push_back( { std::move( image ), access, false, std::move( name ) } );
        }
        void Write( std::shared_ptr<Image> image, RDG::Access access, std::string name )
        {
            if ( image )
                m_Images.push_back( { std::move( image ), access, true, std::move( name ) } );
        }
        void Read( RDG::BufferRef buffer, RDG::Access access )
        {
            m_Buffers.push_back( { buffer, access, false } );
        }
        void Write( RDG::BufferRef buffer, RDG::Access access )
        {
            m_Buffers.push_back( { buffer, access, true } );
        }

        const std::vector<ImageUse>& Images() const
        {
            return m_Images;
        }
        const std::vector<BufferUse>& Buffers() const
        {
            return m_Buffers;
        }

    private:
        std::vector<ImageUse>  m_Images;
        std::vector<BufferUse> m_Buffers;
    };

    // One compute node of a system's frame work: its name in the graph, what it declares, and the body that
    // records its dispatch. A system that dispatches several times in a frame hands one node per dispatch, in
    // order, so the graph orders and synchronises each against the next.
    struct ComputeNodeDeclaration
    {
        std::string           Name;
        RenderPassDeclaration Access;
        std::function<void()> Record;
    };
} // namespace Desert::Graphic
