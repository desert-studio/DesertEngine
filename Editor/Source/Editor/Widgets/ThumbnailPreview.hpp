#pragma once

#include <Engine/Assets/ThumbnailInfo.hpp>

#include <optional>
#include <string>
#include <utility>

// THE LIVE PREVIEW'S QUEUE (UE: in Edit Thumbnail mode the tile is rendered in real time with the orbit being
// dragged - a temporary ThumbnailInfo, written into the asset once, on release). It is a SLOT, not a queue: a
// drag produces a new orbit every frame and only the newest one is worth a capture, so a request replaces the
// one still waiting ("the last wins"), and an orbit already captured or in flight is not asked for again.
// Pure and header-only so the ThumbnailEdit suite asserts the dedup; ThumbnailService owns one and carries the
// renderer request in @p Payload.
namespace Desert::Editor::ThumbnailPreview
{
    template <class Payload>
    class Slot
    {
    public:
        struct Entry
        {
            std::string            Identity; // ThumbnailKey::Identity of the asset
            Assets::ThumbnailOrbit Orbit;
            Payload                What;
        };

        /// Ask for a picture of @p identity seen from @p orbit. Replaces a waiting request (of any asset); nothing
        /// is kept when that picture is the one in flight, or the one landed last and nothing is in flight.
        /// Answers whether the request was kept.
        bool Put( std::string identity, const Assets::ThumbnailOrbit& orbit, Payload what )
        {
            const auto same = [&]( const std::optional<Entry>& e )
            { return e && e->Identity == identity && e->Orbit == orbit; };
            if ( same( m_InFlight ) || ( !m_InFlight && same( m_Landed ) ) )
            {
                m_Waiting.reset(); // the newest wish is the picture already on its way or on disk
                return false;
            }
            m_Waiting = Entry{ std::move( identity ), orbit, std::move( what ) };
            return true;
        }

        /// The waiting request, now in flight (nothing while one is in flight already: one capture at a time).
        std::optional<Entry> Take()
        {
            if ( m_InFlight || !m_Waiting )
                return std::nullopt;
            m_InFlight = std::move( m_Waiting );
            m_Waiting.reset();
            return m_InFlight;
        }

        /// The capture in flight finished (written or refused): it is the landed one now.
        void Land()
        {
            if ( m_InFlight )
                m_Landed = std::move( m_InFlight );
            m_InFlight.reset();
        }

        /// The gesture on @p identity ended: its waiting request is dropped and its landed picture forgotten
        /// (the next gesture starts from the stated orbit). A capture in flight still lands.
        void End( const std::string& identity )
        {
            if ( m_Waiting && m_Waiting->Identity == identity )
                m_Waiting.reset();
            if ( m_Landed && m_Landed->Identity == identity )
                m_Landed.reset();
        }

        [[nodiscard]] bool Waiting() const
        {
            return m_Waiting.has_value();
        }
        [[nodiscard]] bool InFlight() const
        {
            return m_InFlight.has_value();
        }
        /// The orbit of @p identity's last landed picture, if it has one.
        [[nodiscard]] std::optional<Assets::ThumbnailOrbit> LandedOrbit( const std::string& identity ) const
        {
            if ( m_Landed && m_Landed->Identity == identity )
                return m_Landed->Orbit;
            return std::nullopt;
        }

    private:
        std::optional<Entry> m_Waiting;
        std::optional<Entry> m_InFlight;
        std::optional<Entry> m_Landed;
    };
} // namespace Desert::Editor::ThumbnailPreview
