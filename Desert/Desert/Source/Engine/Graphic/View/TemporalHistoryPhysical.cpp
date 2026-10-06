// TemporalHistory's device step (SceneViewState.hpp: AllocatePhysical). Kept out of SceneViewState.cpp so that
// Prepare / Register / Swap stay device-free; this file reaches the device only through the two seams
// (IImageFactory, IGraphImageImporter), so the TemporalViewContract suite runs it against mocks.
#include <Engine/Graphic/View/SceneViewState.hpp>

#include <Engine/Graphic/GraphImageImporter.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/ImageFactory.hpp>

#include <string>
#include <utility>

namespace Desert::Graphic
{
    bool TemporalHistory::HasPhysical() const
    {
        return m_Images.size() == m_Pairs.size();
    }

    Common::BoolResultStr TemporalHistory::AllocatePhysical( const IImageFactory&       images,
                                                             const IGraphImageImporter& importer )
    {
        if ( HasPhysical() )
            return Common::MakeSuccess( true );

        // Built aside and committed only when every side succeeded: a failure leaves the pairs image-less, and
        // the next call retries them all.
        std::vector<std::array<RDG::ExternalTexture, 2>>     pairs;
        std::vector<std::array<std::shared_ptr<Image2D>, 2>> made;
        pairs.reserve( m_Descs.size() );
        made.reserve( m_Descs.size() );
        for ( const HistoryTextureDesc& history : m_Descs )
        {
            const RDG::TextureDesc& desc = history.Desc;
            if ( desc.Dim != RDG::TextureDim::Tex2D || desc.Layers != 1 || desc.Samples != 1 )
                return Common::MakeFormattedError<bool>(
                     "TemporalHistory::AllocatePhysical: history '{}' is not a single-sample 2D texture with one "
                     "layer ({} layers, {} samples)",
                     history.Name, desc.Layers, desc.Samples );

            std::array<RDG::ExternalTexture, 2>     pair;
            std::array<std::shared_ptr<Image2D>, 2> sides;
            for ( uint32_t side = 0; side < 2; ++side )
            {
                const Core::Formats::Image2DSpecification spec = {
                     .Tag        = std::string( history.Name ) + ( side == 0 ? "[0]" : "[1]" ),
                     .Width      = desc.Size.Width,
                     .Height     = desc.Size.Height,
                     .Format     = desc.Format,
                     .Mips       = desc.Mips,
                     .Usage      = Core::Formats::Image2DUsage::Image2D,
                     .Properties = Core::Formats::Storage | Core::Formats::Sample,
                };
                sides[side] = images.CreateImage2D( spec );
                if ( !sides[side] )
                    return Common::MakeFormattedError<bool>(
                         "TemporalHistory::AllocatePhysical: the image factory made no image for history '{}' "
                         "side {} ({}x{})",
                         history.Name, side, desc.Size.Width, desc.Size.Height );

                const Common::BoolResultStr imported = importer.ImportImage( sides[side], pair[side] );
                if ( !imported.IsSuccess() )
                    return Common::MakeFormattedError<bool>(
                         "TemporalHistory::AllocatePhysical: importing history '{}' side {} failed: {}",
                         history.Name, side, imported.GetError() );
                if ( !pair[side].Physical )
                    return Common::MakeFormattedError<bool>( "TemporalHistory::AllocatePhysical: history '{}' "
                                                             "side {} was imported without a physical image",
                                                             history.Name, side );
                const RDG::TextureDesc& got = pair[side].Desc;
                if ( got.Size.Width != desc.Size.Width || got.Size.Height != desc.Size.Height ||
                     got.Format != desc.Format || got.Mips != desc.Mips )
                    return Common::MakeFormattedError<bool>(
                         "TemporalHistory::AllocatePhysical: history '{}' side {} was made {}x{} with {} mips, "
                         "declared {}x{} with {} mips (or another format)",
                         history.Name, side, got.Size.Width, got.Size.Height, got.Mips, desc.Size.Width,
                         desc.Size.Height, desc.Mips );
            }
            pairs.push_back( std::move( pair ) );
            made.push_back( std::move( sides ) );
        }

        // The external keeps its own state from here on (the graph writes the final state back each Execute), so
        // the import happens once per image, not once per frame.
        m_Pairs  = std::move( pairs );
        m_Images = std::move( made );
        return Common::MakeSuccess( true );
    }
} // namespace Desert::Graphic
