#pragma once

// REPLICATION BY DISTANCE (WP-FAR-15; NET-ARCH §3.5 relevancy, §3.6 priority and bandwidth; folds NET-RELEVANCY).
// THE VIEW (WP-STREAM-SOURCE-VIEW): the connection's view is the CLIENT'S VIEW POINT, reported, not its pawn.
//
// Per connection the gateway keeps ONE VIEW: the position the client's player looks from and the SHAPE it looks
// through (ViewReport: where the eye is, where it faces, how wide and how far - the same geometry its World
// Partition streaming source loads with, so relevancy, the server's per-player residency and the client's residency
// stay one geometry). The eye is the client's word, bounded by the server: it may stand at most MaxViewRangeCm from
// the connection's own pawn (the authoritative position, from the sim); a report farther than that is pulled back
// along its direction to that distance (ResolveViewPosition). A connection that never reported looks from its pawn.
// A dynamic object (a pawn) is RELEVANT while it stands inside that shape: a circle of the connection's cull range,
// plus the reported sector (a scope looking 1 km down a street) when the client states one. It LEAVES only past the
// shape widened by the exit margin (the World Partition UnloadMargin, passed in as a value): no Spawn/Destroy
// flicker on the edge. The connection's OWN pawn is always relevant, wherever the view is.
//
// Every relevant pawn has a SEND PERIOD, in server steps, from its distance to the view (the bands) - near pawns go
// in every snapshot, far ones in every N-th - and a pawn whose state has not changed since the connection last got
// it is DORMANT and goes at the dormant period (a keep-alive; UE NetDormancy / adaptive net update frequency). Each
// snapshot entry carries the period it was sent at (SnapshotEntry::PeriodSteps, Replication.hpp): the client sets
// that pawn's interpolation delay from it, so a 6 Hz pawn is drawn two of ITS intervals behind, not two of the
// 30 Hz ones. Due pawns are written by priority (how overdue they are, nearest first) until the connection's byte
// budget for the snapshot is spent; the rest wait and grow more overdue (Iris: priority accumulates until sent).
//
// Technique reference: UE Replication Graph (ReplicationGraph.h: FClassReplicationInfo::ReplicationPeriodFrame,
// CullDistanceSquared, DistancePriorityScale, StarvationPriorityScale; FConnectionReplicationActorInfo::
// NextReplicationFrameNum), UE NetworkDriver.cpp ServerReplicateActors_PrioritizeActors (the viewer is
// FNetViewer: the controller's GetPlayerViewPoint, not the pawn), UE5 Iris NetObjectPrioritizer /
// SphereWithOwnerBoostNetObjectPrioritizer, APlayerController::ServerUpdateCamera (the view the server prioritises
// by comes from the client, as a position and rotation). Code: plain value types + pure functions (C++ Core
// Guidelines F.8), one owner per value (the project file owns the settings, the sim owns the pawn position, the
// client owns its view). Engine-free: centimetres as plain floats, degrees for angles.

#include <Net/NetHandshake.hpp>
#include <Net/Replication.hpp> // NetVec3

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Desert::Net
{
    // ---- SETTINGS (the server's; Project::NetSettings is their authored form, Server/main.cpp converts) -----------

    // One distance band: pawns closer than ToCm (from the view position, ground distance X/Z) are sent every
    // EverySteps server steps. Bands are ascending by ToCm; the last one is open (ToCm ignored).
    struct RateBand
    {
        float         ToCm       = 0.f;
        std::uint32_t EverySteps = 2;
    };

    struct RelevancySettings
    {
        // Ascending; Bands.front().EverySteps is also the snapshot cadence (the own pawn's ack period).
        std::vector<RateBand> Bands{ { 5000.f, 2 }, { 15000.f, 4 }, { 40000.f, 10 }, { 0.f, 20 } };
        // A pawn whose state equals what the connection last received goes every this many steps (UE dormancy
        // keep-alive; MinNetUpdateFrequency 2 Hz -> here 1 Hz).
        std::uint32_t DormantEverySteps = 60;
        // The circle every connection sees pawns within, whatever its streaming shape (UE NetCullDistance of the
        // pawn class; UE default 150 m - a sniper's target is farther). Never below the main grid's loading range
        // for what the client has streamed anyway.
        float PawnCullRangeCm = 50000.f;
        // The farthest a client's reported shape may reach, AND the farthest its reported eye may stand from its
        // own pawn (a cheat stating a view 100 km away still streams and sees 1.5 km around its body).
        float MaxViewRangeCm = 150000.f;
        // A pawn leaves relevancy only past the shape widened by this fraction (the WP UnloadMargin, 0.25: enter
        // at R, leave at 1.25 R).
        float ExitMargin = 0.25f;
        // Bytes of snapshot entries per second per connection (NET-ARCH §3.6 default 64 KB/s).
        std::uint32_t BudgetBytesPerSecond = 65536;
    };

    // The settings are validated ONCE, by Project::ValidateNetSettings (Projects/NetSettings.hpp), before the server
    // derives them; Net states no second copy of that rule.

    // ---- THE CLIENT'S VIEW (ViewReport, client -> server, State lane, message EReplicationMessage::ViewReport) ----

    // Where the client looks from and what it looks through: its player's view point (Core::PlayerStreamingView,
    // engine side) and its view streaming source's shape. A circle (SectorAngleDeg = 360) of RangeCm around the
    // eye, or a sector that wide about the yaw. RangeCm 0 = no range of its own (the connection's PawnCullRangeCm
    // alone). Wire: 6 x F32 (kProtocolVersion 10: a report without the eye is a version mismatch, refused at Hello,
    // never a decode fallback). Sent when the eye moved by kViewPositionStepCm, the yaw/angle by kViewAngleStepDeg,
    // the range by kViewRangeStepCm, or kViewResendSeconds passed (ReplicationClient::ReportView).
    struct ViewReport
    {
        NetVec3 Position;             // the eye, cm, Y-up (Core::PlayerStreamingView::Position)
        float   FacingYawDeg   = 0.f;   // world yaw of the view axis, degrees about +Y (0 faces -Z)
        float   SectorAngleDeg = 360.f; // (0, 360]; 360 is the circle
        float   RangeCm        = 0.f;   // how far the shape reaches; 0 = none of its own
    };
    std::vector<std::byte>    EncodeViewReport( const ViewReport& report );
    std::optional<ViewReport> DecodeViewReport( std::span<const std::byte> data );

    // The eye the server streams and prioritises for one connection: the report's position when it stands within
    // settings.MaxViewRangeCm of @p ownPawn (ground distance X/Z), else that position pulled back toward the pawn
    // onto the circle of MaxViewRangeCm (height kept); @p ownPawn itself without a report. Pure.
    [[nodiscard]] NetVec3 ResolveViewPosition( const RelevancySettings& settings, const NetVec3& ownPawn,
                                               const std::optional<ViewReport>& report );

    // The resolved shape of one connection for one send: the circle every connection has, plus the report's
    // sector when it reaches farther. Built by the gateway from the settings and the newest report.
    struct ViewShape
    {
        float CircleRangeCm  = 0.f;   // max(settings.PawnCullRangeCm, main grid loading range)
        float SectorRangeCm  = 0.f;   // min(report.RangeCm, settings.MaxViewRangeCm); 0 = no sector
        float SectorAngleDeg = 360.f; // the report's
        float FacingYawDeg   = 0.f;   // the report's
    };
    [[nodiscard]] ViewShape ResolveViewShape( const RelevancySettings& settings, float mainGridLoadingRangeCm,
                                              const std::optional<ViewReport>& report );

    // How far into the shape a point stands: the point's distance over the reach of the shape at its bearing
    // (ground plane X/Z): < 1 inside, 1 on the edge, > 1 outside. Inside the sector the reach is the sector's
    // range, elsewhere the circle's. The gateway enters a pawn at <= 1 and leaves it at > 1 + ExitMargin.
    // @p viewPosition is ResolveViewPosition's.
    [[nodiscard]] float RangeRatio( const ViewShape& shape, const NetVec3& viewPosition, const NetVec3& at );

    // Ground distance (X/Z) in cm between two points.
    [[nodiscard]] float GroundDistanceCm( const NetVec3& a, const NetVec3& b );

    // ---- PERIODS AND PRIORITY -------------------------------------------------------------------------------------

    // The band a pawn at @p distanceCm falls in and its period; the last band for anything past the others.
    [[nodiscard]] std::uint32_t SendPeriodSteps( const RelevancySettings& settings, float distanceCm );

    // The entry's priority for the budget cut: overdue steps over its period (>= 1 when due), nearer first among
    // equals (distance in km as the tie-break fraction). Higher goes first. @p sinceSentSteps: steps since the
    // connection last received this pawn (its period for a pawn never sent).
    [[nodiscard]] float SendPriority( std::uint32_t sinceSentSteps, std::uint32_t periodSteps, float distanceCm );

    // Bytes one connection may spend on snapshot entries in one snapshot: the budget per second over the snapshot
    // cadence (Bands.front().EverySteps at kNetStepHz). Never below one entry.
    [[nodiscard]] std::size_t SnapshotEntryBudgetBytes( const RelevancySettings& settings, std::size_t entryBytes );

    // The client's interpolation delay for a pawn received every @p periodSteps: two of its intervals plus the
    // measured jitter, never below @p baseDelaySeconds (the connection's own adaptive delay, NET-5). And how far
    // past its newest sample such a pawn may be extrapolated before it holds: one and a half of its intervals,
    // never below kMaxExtrapolationSeconds (Replication.hpp).
    [[nodiscard]] double PawnInterpolationDelay( std::uint32_t periodSteps, double jitterSeconds,
                                                 double baseDelaySeconds );
    [[nodiscard]] double PawnExtrapolationCap( std::uint32_t periodSteps );

    // LOG LINES THE SCENARIOS READ (the gateway prints them through its m_Log):
    //   "player {p}: view at ({x:.0f}, {y:.0f}, {z:.0f}) cm{, pulled back to {d:.0f} m from the pawn}" - whenever
    //    the resolved eye moves by more than kViewLogStepCm (below) from the last logged one, or the clamp
    //    engages/releases.
    //   "player {p}: view shape - circle {c:.0f} m, sector {a:.0f} deg facing {y:.0f} deg to {r:.0f} m (reported
    //    {rr:.0f} m)" - unchanged from WP-FAR-15, whenever the shape changes.
    inline constexpr float kViewLogStepCm = 500.f;
} // namespace Desert::Net
