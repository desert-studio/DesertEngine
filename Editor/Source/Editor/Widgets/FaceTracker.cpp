#include "FaceTracker.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#if defined( DESERT_WITH_DLIB )
#include <dlib/array2d.h>
#include <dlib/image_processing/frontal_face_detector.h>
#include <dlib/image_processing/shape_predictor.h>
#include <dlib/serialize.h>
#endif

namespace Desert::Editor
{
#if defined( DESERT_WITH_DLIB )

    struct FaceTracker::Impl
    {
        dlib::frontal_face_detector detector = dlib::get_frontal_face_detector();
        dlib::shape_predictor       predictor;
        bool                        loaded = false;
    };

    FaceTracker::FaceTracker() : m_Impl( std::make_unique<Impl>() )
    {
    }
    FaceTracker::~FaceTracker() = default;

    bool FaceTracker::Compiled()
    {
        return true;
    }

    bool FaceTracker::LoadModel( const std::string& datPath )
    {
        m_Impl->loaded = false;
        std::error_code ec;
        if ( datPath.empty() || !std::filesystem::exists( datPath, ec ) )
            return false;
        try
        {
            dlib::deserialize( datPath ) >> m_Impl->predictor;
            m_Impl->loaded = true;
        }
        catch ( ... )
        {
            m_Impl->loaded = false;
        }
        return m_Impl->loaded;
    }

    bool FaceTracker::Ready() const
    {
        return m_Impl && m_Impl->loaded;
    }

    std::vector<glm::vec2> FaceTracker::Detect( const uint8_t* rgba, int width, int height )
    {
        std::vector<glm::vec2> out;
        if ( !Ready() || rgba == nullptr || width <= 0 || height <= 0 )
            return out;

        // Downscale for the HOG detector (points are scaled back to full-res coordinates afterwards).
        constexpr int kMaxDim = 480;
        int           scale   = 1;
        while ( ( width / scale ) > kMaxDim || ( height / scale ) > kMaxDim )
            scale *= 2;
        const int dw = width / scale;
        const int dh = height / scale;
        if ( dw <= 0 || dh <= 0 )
            return out;

        dlib::array2d<unsigned char> img;
        img.set_size( dh, dw );
        for ( int y = 0; y < dh; ++y )
        {
            const std::size_t srcY = static_cast<std::size_t>( y * scale ) * static_cast<std::size_t>( width );
            for ( int x = 0; x < dw; ++x )
            {
                const uint8_t* p = rgba + ( srcY + static_cast<std::size_t>( x * scale ) ) * 4;
                img[y][x]        = static_cast<unsigned char>( ( 30 * p[0] + 59 * p[1] + 11 * p[2] ) / 100 );
            }
        }

        std::vector<dlib::rectangle> faces = m_Impl->detector( img );
        if ( faces.empty() )
            return out;

        std::size_t best = 0;
        long        area = 0;
        for ( std::size_t i = 0; i < faces.size(); ++i )
        {
            const long a = faces[i].area();
            if ( a > area )
            {
                area = a;
                best = i;
            }
        }

        const dlib::full_object_detection shape = m_Impl->predictor( img, faces[best] );
        out.reserve( shape.num_parts() );
        for ( unsigned long i = 0; i < shape.num_parts(); ++i )
        {
            const auto& pt = shape.part( i );
            out.emplace_back( static_cast<float>( pt.x() * scale ), static_cast<float>( pt.y() * scale ) );
        }
        return out;
    }

#else // no dlib — no-op stub so the Editor builds without the optional dependency

    struct FaceTracker::Impl
    {
    };

    FaceTracker::FaceTracker()  = default;
    FaceTracker::~FaceTracker() = default;

    bool FaceTracker::Compiled()
    {
        return false;
    }
    bool FaceTracker::LoadModel( const std::string& )
    {
        return false;
    }
    bool FaceTracker::Ready() const
    {
        return false;
    }
    std::vector<glm::vec2> FaceTracker::Detect( const uint8_t*, int, int )
    {
        return {};
    }

#endif
} // namespace Desert::Editor
