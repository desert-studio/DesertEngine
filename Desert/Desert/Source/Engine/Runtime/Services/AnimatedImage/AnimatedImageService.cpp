#include "AnimatedImageService.hpp"

#include <Engine/Core/IO/ImageReader.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>

namespace Desert::Runtime
{
    namespace
    {
        bool HasGifExtension( const std::string& path )
        {
            if ( path.size() < 4 )
                return false;
            std::string ext = path.substr( path.size() - 4 );
            std::transform( ext.begin(), ext.end(), ext.begin(),
                            []( unsigned char c ) { return static_cast<char>( std::tolower( c ) ); } );
            return ext == ".gif";
        }
    } // namespace

    const AnimatedImageService::Anim& AnimatedImageService::GetOrDecode( const Assets::AssetHandle& handle )
    {
        if ( auto it = m_Anims.find( handle ); it != m_Anims.end() )
            return it->second;

        Anim       anim; // stays Animated=false unless we successfully decode a multi-frame GIF
        auto*      texService = ResourceRegistry::GetTextureService();
        const auto source     = texService ? texService->GetSourcePath( handle ) : std::string();
        if ( HasGifExtension( source ) )
        {
            const auto gif = Core::IO::ImageReader::ReadGif( source );
            if ( gif.FrameCount > 1 && gif.Width > 0 && gif.Height > 0 )
            {
                const size_t frameBytes = static_cast<size_t>( gif.Width ) * gif.Height * 4;
                float        cum        = 0.0f;
                anim.Frames.reserve( gif.FrameCount );
                anim.CumEndMs.reserve( gif.FrameCount );

                for ( uint32_t i = 0; i < gif.FrameCount; ++i )
                {
                    std::vector<unsigned char> pixels( gif.Data.begin() + i * frameBytes,
                                                       gif.Data.begin() + ( i + 1 ) * frameBytes );
                    auto                       texResult =
                         Graphic::Texture2D::Create( "GIF frame", gif.Width, gif.Height,
                                                     Core::Formats::ImageFormat::RGBA8F, std::move( pixels ) );
                    // A BARE `continue` STOOD HERE, AND IT WAS NOT A SKIP — IT WAS A DIFFERENT
                    // ANIMATION. The two arrays below are the animation's timeline: dropping a frame
                    // without its delay shortens the loop and shifts every later frame earlier, so the
                    // GIF plays at the wrong speed and out of step with itself, silently. Recording the
                    // refusal does not repair the timeline; it is what makes a wrong one attributable
                    // instead of being read as a badly authored file.
                    if ( !texResult.IsSuccess() )
                    {
                        LOG_ERROR( "[AnimatedImage] frame {} of {} was not uploaded and is missing from "
                                   "the animation: {}",
                                   i, gif.FrameCount, texResult.GetError() );
                        continue;
                    }
                    anim.Frames.push_back( texResult.ExtractValue() );
                    cum += static_cast<float>( gif.DelaysMs[i] );
                    anim.CumEndMs.push_back( cum );
                }
                anim.Animated = anim.Frames.size() > 1 && anim.CumEndMs.back() > 0.0f;
            }
        }

        return m_Anims.emplace( handle, std::move( anim ) ).first->second;
    }

    Graphic::Image2D* AnimatedImageService::Resolve( const Assets::AssetHandle& handle )
    {
        if ( static_cast<uint64_t>( handle ) == 0 )
            return nullptr;

        const Anim& anim = GetOrDecode( handle );
        if ( !anim.Animated )
            return nullptr;

        const float total = anim.CumEndMs.back();
        const float elapsed =
             std::chrono::duration<float, std::milli>( std::chrono::steady_clock::now() - m_Epoch ).count();
        const float t = std::fmod( elapsed, total );

        // First frame whose cumulative end time passes the wrapped playhead.
        size_t frame = 0;
        while ( frame + 1 < anim.CumEndMs.size() && t >= anim.CumEndMs[frame] )
            ++frame;

        auto* imgService = ResourceRegistry::GetImageService();
        if ( !imgService )
            return nullptr;
        return static_cast<Graphic::Image2D*>( imgService->Resolve( anim.Frames[frame]->GetImageHandle() ) );
    }

    void AnimatedImageService::Clear()
    {
        m_Anims.clear();
    }
} // namespace Desert::Runtime
