#pragma once

// THE LANDSCAPE'S COLLISION SHAPE (UE ULandscapeHeightfieldCollisionComponent): one tile's heights as the Jolt
// heightfield takes them. One decode for every world that collides with a landscape - the client's scene
// (ECS::LandscapeCollision) and the dedicated server's cells (Server/ServerCellCollision) - so a pawn stands on
// the same ground on both sides of a connection. Render-free (module Core).

#include <LandscapeCore/Public/LandscapeData.hpp>
#include <LandscapeCore/Public/LandscapeFrame.hpp>
#include <PhysicsCore/PhysicsWorld.hpp>

#include <vector>

namespace Desert::World::Landscape
{
    // The heights Jolt is given: the same decode the renderer's shader and SampleLandscapeHeight use, relative to
    // the frame's base (the body stands at BaseY). A hole in the visibility layer is kHeightFieldNoCollision, so
    // every triangle touching it is gone (UE: the collision component's hole material / visibility data).
    void DecodeLandscapeCollisionHeights( const LandscapeTileData& tile, float zScale, std::vector<float>& out );

    // The heightfield of @p tile standing in @p frame over @p heights (DecodeLandscapeCollisionHeights; read by
    // CreateHeightField / UpdateHeightField only). Profile is left for the caller (the world's register).
    [[nodiscard]] Physics::HeightFieldDesc LandscapeHeightFieldDesc( const LandscapeTileData& tile,
                                                                    const LandscapeFrame&    frame,
                                                                    const std::vector<float>& heights );
} // namespace Desert::World::Landscape
