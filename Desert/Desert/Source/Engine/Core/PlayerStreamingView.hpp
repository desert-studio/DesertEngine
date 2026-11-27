#pragma once

// THE PLAYER'S STREAMING VIEW (WP-STREAM-SOURCE-VIEW).
//
// A player streams the world from WHERE IT LOOKS, not from where its body stands: UE's APlayerController is an
// IWorldPartitionStreamingSourceProvider whose source is GetPlayerViewPoint() (the camera manager's view), and a
// character that must stand on loaded ground away from that view carries its own UWorldPartitionStreamingSourceComponent.
// Here the same two: the view source below, made from Scene::PlayerViewPoint() (the view target camera's world
// transform, or the camera pinned from outside - a scenario's `--camera`), and ECS::StreamingSourceComponent on the
// entities the content authors it on (Pawn_Player.deprefab carries one; Core::SpawnDefaultPawn adds none).
//
// One owner per value: the scene owns the view point (Scene::PlayerViewPoint), the streamer owns the sources it
// streams from (WorldStreamer::GatherSources), the client reports the view to the server through Net::ViewReport
// (Relevancy.hpp). This header is the contract between WorldStreamer.cpp (makes), ClientNetDriver.cpp (reports)
// and the scenarios (read the log lines named below). Implemented in WorldStreamer.cpp.
//
// Code reference: C++ Core Guidelines F.8 (pure functions over plain values); one definition of each constant.

#include <Engine/Core/Serialize/WorldPartitionStreamingRules.hpp>

#include <glm/vec3.hpp>

#include <cstdint>
#include <optional>

namespace Desert::Core
{
    class Scene;

    // Where the player looks from and along: the eye (translation of the view matrix) and the forward (-Z of it).
    struct PlayerStreamingView
    {
        glm::vec3 Position{ 0.0f };
        glm::vec3 Facing{ 0.0f, 0.0f, -1.0f };
    };

    // A streaming source that moves farther than this between two Ticks has TELEPORTED (UE FStreamingSourceVelocity:
    // a jump beyond its teleport distance clears the velocity history). A sprinting avatar covers ~20 cm per 60 Hz
    // tick, ~600 cm per 1 s hitch; 10 m in one tick is a relocation, never travel. The streamer then stamps velocity
    // 0 (no look-ahead sweep along the jump, no velocity in the order), and - while BlockOnSlowStreaming holds play
    // for the cell under the jumped source - releases none of the cells the source left (ResidencyGate::MayRelease),
    // so the destination is made before the origin drops.
    inline constexpr float kStreamingTeleportDistanceCm = 1000.0f;

    // The key the view source has in the streamer's per-source velocity history (WorldStreamer::StampVelocities keys
    // entity sources by their entt id; the view is no entity). entt::null's integer is 0xFFFFFFFF on a 32-bit entity.
    inline constexpr std::uint32_t kPlayerViewSourceKey = 0xFFFFFFFEu;

    // The player's view, when Play (or Paused) has a camera to look through: Scene::PlayerViewPoint() read as eye
    // and forward. nullopt outside Play or with no camera - then the player is no source (the level's authored
    // components and the instruments remain; UseSources reports a world with none).
    [[nodiscard]] std::optional<PlayerStreamingView> PlayerStreamingViewOf( Scene& scene );

    // The view as a streaming source: Position, Facing, the grid's loading range (no override), priority 0, no
    // shapes (one circle of the grid's range; a sector is content's, authored on a StreamingSourceComponent). The
    // same function on the client and in the streamer, so what the client reports is what it streams with.
    [[nodiscard]] Rules::StreamingSource PlayerViewStreamingSource( const PlayerStreamingView& view );

    // The view's yaw in degrees about +Y, 0 facing -Z, counter-clockwise seen from above (Rules::Detail::TurnAboutUp;
    // Net::ViewReport::FacingYawDeg) - the axis (-sin, -cos) on the ground plane. A view looking straight up or
    // down keeps its last meaningful yaw: Facing's X/Z under 1e-4 yields 0.
    [[nodiscard]] float PlayerViewYawDeg( const PlayerStreamingView& view );

    // LOG LINES THE SCENARIOS READ (WorldStreamer prints them; the text is the contract, not the format call):
    //   "[WorldStreamer] '{scene}': the player's view is a streaming source at ({x:.0f}, {y:.0f}, {z:.0f}) cm,
    //    yaw {yaw:.0f}" - once, the first Tick the view source exists, and again whenever it reappears after none.
    //   "[WorldStreamer] '{scene}': streaming source {name} jumped {d:.0f} cm to ({x:.0f}, {y:.0f}, {z:.0f}) cm:
    //    velocity reset; {n} cell(s) it left are held until the cell under it is resident" - on every teleport
    //    (name = "the player's view" or "entity {id}"); n = 0 when BlockOnSlowStreaming is off or nothing waits.
    //   "[WorldStreamer] '{scene}': teleport hold released after {frames} frame(s): {n} cell(s) leave" - when the
    //    held cells are let go (the cell under the source became resident, or play stopped waiting).
} // namespace Desert::Core
