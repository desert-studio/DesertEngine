#include "RectLightLtcService.hpp"

#include <Engine/Core/Formats/ImageFormat.hpp>
#include <Engine/Graphic/Image.hpp>

#include <Common/Core/Logger.hpp>

#include <algorithm>
#include <cstddef>
#include <vector>

namespace Desert::Runtime
{
    namespace
    {
#include "RectLightLtcTable.inl"

        // The fit's resolution (RectLightLtcTable.inl) and Mesh/RectLight.glslh RECT_LIGHT_LTC_SIZE.
        constexpr uint32_t kLtcSize        = 64;
        constexpr size_t   kLtcTableTexels = size_t( kLtcSize ) * kLtcSize;
        constexpr size_t   kLtcTableFloats = kLtcTableTexels * 4;
        static_assert( sizeof( kRectLightLtcMatrix ) / sizeof( float ) == kLtcTableFloats );
        static_assert( sizeof( kRectLightLtcAmplitude ) / sizeof( float ) == kLtcTableFloats );
    } // namespace

    int32_t RectLightLtcService::LtcTexture() noexcept
    {
        const std::lock_guard<std::mutex> lock( m_Mutex );
        if ( !m_Table && !m_Refused )
        {
            // The matrix block above the amplitude block, one image (RectLight.glslh samples each half).
            std::vector<float> texels( kLtcTableFloats * 2 );
            std::copy_n( kRectLightLtcMatrix, kLtcTableFloats, texels.begin() );
            std::copy_n( kRectLightLtcAmplitude, kLtcTableFloats,
                         texels.begin() + static_cast<std::ptrdiff_t>( kLtcTableFloats ) );
            const Core::Formats::Image2DSpecification spec  = { .Tag        = "RectLightLtc",
                                                                .Width      = kLtcSize,
                                                                .Height     = kLtcSize * 2,
                                                                .Format     = Core::Formats::ImageFormat::RGBA32F,
                                                                .Data       = texels,
                                                                .Usage      = Core::Formats::Image2DUsage::Image2D,
                                                                .Properties = Core::Formats::Sample };
            std::shared_ptr<Graphic::Image2D>         table = Graphic::Image2D::Create( spec );
            if ( !table || !table->GetBindlessSlot().IsValid() )
            {
                m_Refused = true;
                LOG_ERROR( "[RectLight] The GGX LTC table ({}x{} RGBA32F) was not created; rect lights reflect "
                           "no specular.",
                           kLtcSize, kLtcSize * 2 );
            }
            else
            {
                m_Table = std::move( table );
            }
        }
        return m_Table ? static_cast<int32_t>( m_Table->GetBindlessSlot().Index ) : 0;
    }

    void RectLightLtcService::Clear() noexcept
    {
        const std::lock_guard<std::mutex> lock( m_Mutex );
        m_Table.reset();
        m_Refused = false;
    }
} // namespace Desert::Runtime
