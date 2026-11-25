#include <Landscape/Public/LandscapeTileComponent.hpp>

#include <LandscapeCore/Public/LandscapeData.hpp>

#include <memory>
#include <utility>

namespace Desert::ECS
{
    LandscapeTileHeights::LandscapeTileHeights( const LandscapeTileHeights& other )
        : m_Data( other.m_Data ? std::make_unique<Data>( *other.m_Data ) : nullptr )
    {
    }

    LandscapeTileHeights::~LandscapeTileHeights() = default;

    LandscapeTileHeights& LandscapeTileHeights::operator=( const LandscapeTileHeights& other )
    {
        if ( this != &other )
        {
            if ( other.m_Data )
                *this = *other.m_Data;
            else
                reset();
        }
        return *this;
    }

    LandscapeTileHeights& LandscapeTileHeights::operator=( LandscapeTileHeights&& other ) noexcept = default;

    LandscapeTileHeights::LandscapeTileHeights( std::nullopt_t ) noexcept {}

    LandscapeTileHeights& LandscapeTileHeights::operator=( std::nullopt_t ) noexcept
    {
        reset();
        return *this;
    }

    LandscapeTileHeights& LandscapeTileHeights::operator=( Data&& data )
    {
        if ( m_Data )
            *m_Data = std::move( data );
        else
            m_Data = std::make_unique<Data>( std::move( data ) );
        return *this;
    }

    LandscapeTileHeights& LandscapeTileHeights::operator=( const Data& data )
    {
        if ( m_Data )
            *m_Data = data;
        else
            m_Data = std::make_unique<Data>( data );
        return *this;
    }

    void LandscapeTileHeights::reset() noexcept
    {
        m_Data.reset();
    }
} // namespace Desert::ECS
