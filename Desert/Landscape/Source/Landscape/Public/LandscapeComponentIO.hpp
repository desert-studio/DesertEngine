#pragma once

// THE FILE FORM OF THE LANDSCAPE'S HAND-MAPPED COMPONENTS (root, tile, spline): the Landscape module's half of
// Engine/Core/Serialize/AuthoredComponentIO.hpp, whose header gives the rules (a UUID as its decimal string, a
// wrong-typed value never silent). The module's serializers (LandscapeComponentSerializers.cpp) read and write
// through these; Engine names none of them (UE: the Landscape module serializes its own actors' components).

#include <Engine/Core/Serialize/AuthoredComponentIO.hpp>
#include <LandscapeCore/Public/LandscapeSplinesJson.hpp>
#include <Landscape/Public/LandscapeComponent.hpp>
#include <Landscape/Public/LandscapeSplineComponent.hpp>
#include <Landscape/Public/LandscapeTileComponent.hpp>

#include <Common/Json/Document.hpp>

#include <string>

namespace Desert::Core::Serialize
{
    // ── LANDSCAPE ──────────────────────────────────────────────────────────────────────────────────
    // Hand-mapped for the reason the file header gives: the tile's `Landscape` is a Common::UUID, which is
    // not a reflectable field type, and the tile carries loaded heights that must NOT be written. The root
    // goes with it so the two halves of one feature are read in one place.
    // `Layers` is not here: it names `.delayerinfo` assets by {Guid, Path}, and resolving a GUID needs the
    // asset manager — the module's Landscape serializer (LandscapeComponentSerializers.cpp) writes and reads it.
    // `EditLayers` is the stack as [{Guid, Name, Visible, Locked, HeightAlpha, WeightAlpha, Kind}], bottom first,
    // written even when empty (an undo restores the whole block, and an absent key keeps the current value).
    // `Kind` is "Manual" or "Splines". The root carries no splines (scene v76): each is a LandscapeSpline entity.
    inline Common::Json::Object WriteComponent( const ECS::LandscapeComponent& c )
    {
        Common::Json::Object block = Common::Json::ObjectBuilder()
                                          .Set( "QuadsPerTile", c.QuadsPerTile )
                                          .Set( "SpacingCm", c.SpacingCm )
                                          .Set( "ZScale", c.ZScale )
                                          .Build();
        Common::Json::Value::Array layers;
        layers.reserve( c.EditLayers.Layers.size() );
        for ( const World::Landscape::LandscapeEditLayer& layer : c.EditLayers.Layers )
            layers.emplace_back(
                 Common::Json::ObjectBuilder()
                      .Set( "Guid", layer.Guid )
                      .Set( "Name", layer.Name )
                      .Set( "Visible", layer.Visible )
                      .Set( "Locked", layer.Locked )
                      .Set( "HeightAlpha", layer.HeightAlpha )
                      .Set( "WeightAlpha", layer.WeightAlpha )
                      .Set( "Kind", std::string( layer.Kind == World::Landscape::LandscapeEditLayerKind::Splines
                                                      ? "Splines"
                                                      : "Manual" ) )
                      .Build() );
        block["EditLayers"] = Common::Json::Value( std::move( layers ) );
        return block;
    }

    inline void ReadComponent( const Common::Json::Node& from, ECS::LandscapeComponent& c,
                               Common::Json::Issues& issues )
    {
        from.ReadInto( "QuadsPerTile", c.QuadsPerTile, issues );
        from.ReadInto( "SpacingCm", c.SpacingCm, issues );
        from.ReadInto( "ZScale", c.ZScale, issues );

        // The stack is read whole or not at all: a layer with an unreadable field, or a stack that
        // ValidateLandscapeEditLayerStack refuses (a null or repeated Guid, an alpha out of range), is an issue
        // at the list's path and the current stack stays — never a stack with the bad layer dropped or clamped.
        // A landscape has at least its Base layer (UE: every landscape has one edit layer), so a read that
        // leaves the stack empty - the key absent over an empty stack, or an empty list - is an issue at
        // `EditLayers`, never a silently made Base.
        const auto value = from.Find( "EditLayers" );
        if ( !value )
        {
            if ( c.EditLayers.Layers.empty() )
                issues.push_back( { from.Where().Key( "EditLayers" ).ToString(),
                                    "the edit layer stack (at least the Base layer)", "absent" } );
            return; // absent: the stack stays as it is (rule 1)
        }
        if ( value->GetKind() != Common::Json::Kind::Array )
        {
            issues.push_back( { value->Where().ToString(), "an array of edit layers", "not an array" } );
            return;
        }
        const size_t                              before = issues.size();
        World::Landscape::LandscapeEditLayerStack read;
        value->ForEachElement(
             [&]( std::size_t, const Common::Json::Node& element )
             {
                 if ( !element.ExpectKind( Common::Json::Kind::Object, issues ) )
                     return;
                 World::Landscape::LandscapeEditLayer layer;
                 element.ReadInto( "Guid", layer.Guid, issues );
                 element.ReadInto( "Name", layer.Name, issues );
                 element.ReadInto( "Visible", layer.Visible, issues );
                 element.ReadInto( "Locked", layer.Locked, issues );
                 element.ReadInto( "HeightAlpha", layer.HeightAlpha, issues );
                 element.ReadInto( "WeightAlpha", layer.WeightAlpha, issues );
                 std::string kind = "Manual";
                 element.ReadInto( "Kind", kind, issues );
                 if ( kind == "Splines" )
                     layer.Kind = World::Landscape::LandscapeEditLayerKind::Splines;
                 else if ( kind != "Manual" )
                     issues.push_back(
                          { element.Where().Key( "Kind" ).ToString(), "\"Manual\" or \"Splines\"", kind } );
                 read.Layers.push_back( std::move( layer ) );
             } );
        if ( issues.size() != before )
            return;
        if ( read.Layers.empty() )
        {
            issues.push_back(
                 { value->Where().ToString(), "at least one edit layer (the Base layer)", "an empty list" } );
            return;
        }
        if ( auto valid = World::Landscape::ValidateLandscapeEditLayerStack( read ); !valid )
        {
            issues.push_back( { value->Where().ToString(), "a valid edit layer stack", valid.GetError() } );
            return;
        }
        c.EditLayers = std::move( read );
    }

    // `Heights` is not written: it is what `HeightFile` decodes to, and the registry's LandscapeTile
    // serializer is what loads it (LandscapeComponentSerializers.cpp). Writing both would be two copies of one
    // terrain.
    inline Common::Json::Object WriteComponent( const ECS::LandscapeTileComponent& c )
    {
        return Common::Json::ObjectBuilder()
             .Set( "Landscape", c.Landscape )
             .Set( "TileX", c.TileX )
             .Set( "TileZ", c.TileZ )
             .Set( "HeightFile", c.HeightFile )
             .Build();
    }

    inline void ReadComponent( const Common::Json::Node& from, ECS::LandscapeTileComponent& c,
                               Common::Json::Issues& issues )
    {
        from.ReadInto( "Landscape", c.Landscape, issues );
        from.ReadInto( "TileX", c.TileX, issues );
        from.ReadInto( "TileZ", c.TileZ, issues );
        from.ReadInto( "HeightFile", c.HeightFile, issues );
    }

    // A LANDSCAPE SPLINE ENTITY (UE ALandscapeSplineActor): `Landscape` is the root's id (a decimal string, rule
    // 2), `Splines` the block LandscapeSplinesJson.hpp spells, written always (an undo restores the whole block).
    inline Common::Json::Object WriteComponent( const ECS::LandscapeSplineComponent& c )
    {
        Common::Json::Object block = Common::Json::ObjectBuilder().Set( "Landscape", c.Landscape ).Build();
        block["Splines"] = Common::Json::Value( World::Landscape::WriteLandscapeSplinesJson( c.Splines ) );
        return block;
    }

    inline void ReadComponent( const Common::Json::Node& from, ECS::LandscapeSplineComponent& c,
                               Common::Json::Issues& issues )
    {
        from.ReadInto( "Landscape", c.Landscape, issues );
        World::Landscape::ReadLandscapeSplinesJson( from, c.Splines, issues );
    }
} // namespace Desert::Core::Serialize
