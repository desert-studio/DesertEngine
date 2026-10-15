#include <Engine/Graphic/GlobalIllumination/GITracing.hpp>

#include <Engine/Graphic/RDG/RDGPassBindings.hpp>

#include <format>

namespace Desert::Graphic::GlobalIllumination
{
    std::string_view TracerFrameMissing( const TracerFrame& frame )
    {
        switch ( frame.Pipeline.Tracer )
        {
            case Tracer::SoftwareDistanceField:
                for ( const RDG::TextureRef& level : frame.GlobalDistanceField )
                    if ( !level.IsValid() )
                        return "the global distance field was not brought up";
                if ( !frame.GlobalDistanceFieldLevels.IsValid() )
                    return "the global distance field's clipmap placement was not uploaded";
                return {};
            case Tracer::HardwareRayQuery:
                return "the ray-query tracer is not in this build (GI-HWRT)";
        }
        return "an unknown tracer";
    }

    void DeclareTracerReads( RDG::BindingBlockBuilder& block, const TracerFrame& frame )
    {
        switch ( frame.Pipeline.Tracer )
        {
            case Tracer::SoftwareDistanceField:
                for ( uint32_t level = 0; level < frame.GlobalDistanceField.size(); ++level )
                    block.Sampled( std::format( "u_GlobalDistanceField{}", level ),
                                   frame.GlobalDistanceField[level], RDG::Access::SampledCompute,
                                   RDG::SubresourceRange::All(), RDG::SamplerDesc::LinearRepeat() );
                block.Storage( "GITracerSoftwareUB", frame.GlobalDistanceFieldLevels, RDG::Access::StorageRead );
                return;
            case Tracer::HardwareRayQuery:
                return; // GI-HWRT declares the TLAS here; TracerFrameMissing refuses the frame until then
        }
    }
} // namespace Desert::Graphic::GlobalIllumination
