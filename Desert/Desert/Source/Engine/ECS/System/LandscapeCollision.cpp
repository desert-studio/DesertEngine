#include <Engine/ECS/System/LandscapeCollision.hpp>

#include <Engine/ECS/Components.hpp>
#include <Common/Core/Logger.hpp>

#include <unordered_set>

namespace Desert::ECS
{
    namespace
    {
        namespace Landscape = World::Landscape;

        bool SameFrame( const Landscape::LandscapeFrame& a, const Landscape::LandscapeFrame& b )
        {
            return a.OriginX == b.OriginX && a.OriginZ == b.OriginZ && a.BaseY == b.BaseY &&
                   a.SpacingCm == b.SpacingCm && a.ZScale == b.ZScale;
        }

        // The heights Jolt is given: the same decode the renderer's shader and SampleLandscapeHeight use,
        // relative to the frame's base (the body stands at BaseY).
        void DecodeHeights( const Landscape::LandscapeTileData& tile, float zScale, std::vector<float>& out )
        {
            const auto& samples = tile.Samples();
            out.resize( samples.size() );
            for ( size_t i = 0; i < samples.size(); ++i )
                out[i] = Landscape::LandscapeHeightCm( samples[i], zScale );
        }

        Physics::HeightFieldDesc DescFor( const Landscape::LandscapeTileData& tile,
                                          const Landscape::LandscapeFrame&    frame,
                                          const std::vector<float>&           heights )
        {
            Physics::HeightFieldDesc desc;
            desc.Position    = glm::vec3( frame.OriginX, frame.BaseY, frame.OriginZ );
            desc.SampleCount = tile.SamplesX();
            desc.SpacingCm   = frame.SpacingCm;
            desc.HeightsCm   = heights;
            return desc;
        }
    } // namespace

    LandscapeCollision::~LandscapeCollision()
    {
        Detach();
        for ( const auto& [entity, body] : m_Bodies )
            m_World->RemoveBody( body.Body );
    }

    void LandscapeCollision::Attach( entt::registry& registry )
    {
        if ( m_Registry == &registry )
            return;
        Detach();
        registry.on_destroy<LandscapeTileComponent>().connect<&LandscapeCollision::OnTileDestroyed>( this );
        m_Registry = &registry;
    }

    void LandscapeCollision::Detach()
    {
        if ( !m_Registry )
            return;
        m_Registry->on_destroy<LandscapeTileComponent>().disconnect( this );
        m_Registry = nullptr;
    }

    void LandscapeCollision::OnTileDestroyed( entt::registry&, entt::entity entity )
    {
        Release( entity );
    }

    void LandscapeCollision::Release( entt::entity entity )
    {
        const auto found = m_Bodies.find( entity );
        if ( found == m_Bodies.end() )
            return;
        m_World->RemoveBody( found->second.Body );
        m_Bodies.erase( found );
    }

    Physics::BodyHandle LandscapeCollision::BodyOf( entt::entity entity ) const
    {
        const auto found = m_Bodies.find( entity );
        return found == m_Bodies.end() ? Physics::kInvalidBody : found->second.Body;
    }

    void LandscapeCollision::Sync( std::span<const LandscapeTileRef> tiles )
    {
        std::unordered_set<entt::entity> live;
        for ( const LandscapeTileRef& tile : tiles )
        {
            const entt::entity              entity    = tile.Entity;
            const LandscapeTileComponent&   component = *tile.Component;
            Landscape::LandscapeTileData&   heights   = *tile.Component->Heights;
            const Landscape::LandscapeFrame frame     = tile.Frame;
            live.insert( entity );

            const auto existing = m_Bodies.find( entity );
            if ( existing != m_Bodies.end() && SameFrame( existing->second.Frame, frame ) )
            {
                const auto rects = heights.TakeDirtyRects( Landscape::LandscapeDirtyConsumer::Physics );
                if ( rects.empty() || existing->second.Body == Physics::kInvalidBody )
                    continue;
                DecodeHeights( heights, frame.ZScale, m_HeightsCm );
                const auto desc = DescFor( heights, frame, m_HeightsCm );
                for ( const auto& rect : rects )
                {
                    const auto updated = m_World->UpdateHeightField( existing->second.Body, desc, rect.X0, rect.Z0,
                                                                     rect.X1, rect.Z1 );
                    if ( !updated.IsSuccess() )
                        LOG_WARN( "[Landscape] tile ({}, {}): collision not updated: {}", component.TileX,
                                  component.TileZ, updated.GetError() );
                }
                continue;
            }

            if ( existing != m_Bodies.end() )
                Release( entity );
            // The body about to be built carries every height there is, so nothing pending is owed to it.
            heights.TakeDirtyRects( Landscape::LandscapeDirtyConsumer::Physics );
            DecodeHeights( heights, frame.ZScale, m_HeightsCm );
            const auto body = m_World->CreateHeightField( DescFor( heights, frame, m_HeightsCm ) );
            if ( !body.IsSuccess() )
                LOG_WARN( "[Landscape] tile ({}, {}) has no collision: {}", component.TileX, component.TileZ,
                          body.GetError() );
            // A refusal is remembered under this frame so it is said once, not every frame; a new frame
            // (the root moved or was re-tiled) tries again.
            m_Bodies[entity] = TileBody{ body.IsSuccess() ? body.GetValue() : Physics::kInvalidBody, frame };
        }

        for ( auto it = m_Bodies.begin(); it != m_Bodies.end(); )
        {
            if ( live.contains( it->first ) )
            {
                ++it;
                continue;
            }
            m_World->RemoveBody( it->second.Body );
            it = m_Bodies.erase( it );
        }
    }
} // namespace Desert::ECS
