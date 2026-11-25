#pragma once

// THE FILE FORM OF A LANDSCAPE SPLINE SET (UE ULandscapeSplinesComponent's ControlPoints + Segments): the
// `Splines` object of a LandscapeSpline block, {ControlPoints: [{Location, Width, SideFalloff, EndFalloff,
// LayerWidthRatio}], Segments: [{Start, End, RaiseTerrain, LowerTerrain, LayerName, SplineMeshes: [{Mesh,
// Primitive, Scale, ForwardAxis, CenterH, CenterAdjust}]}]}, world cm (absent SplineMeshes = none).
//
// A light header on purpose (Common::Json and the splines only): AuthoredComponentIO.hpp writes and reads the
// component with it, and WorldPartitionRules.hpp - which must not include Components.hpp - reads the same block
// to place the spline entity by its band, so the two can never read a different spline.

#include <Common/Json/Document.hpp>
#include <Engine/Geometry/PrimitiveType.hpp>
#include <LandscapeCore/Public/LandscapeSplines.hpp>

#include <cstddef>
#include <string>
#include <utility>

namespace Desert::World::Landscape
{
    // The `Splines` object of @p splines.
    [[nodiscard]] inline Common::Json::Object WriteLandscapeSplinesJson( const LandscapeSplines& splines )
    {
        Common::Json::Value::Array points;
        for ( const World::Landscape::LandscapeSplineControlPoint& p : splines.ControlPoints )
            points.emplace_back( Common::Json::ObjectBuilder()
                                      .Set( "Location", p.Location )
                                      .Set( "Width", p.Width )
                                      .Set( "SideFalloff", p.SideFalloff )
                                      .Set( "EndFalloff", p.EndFalloff )
                                      .Set( "LayerWidthRatio", p.LayerWidthRatio )
                                      .Build() );
        Common::Json::Value::Array segments;
        for ( const World::Landscape::LandscapeSplineSegment& s : splines.Segments )
        {
            Common::Json::Value::Array meshes;
            for ( const World::Landscape::LandscapeSplineMeshEntry& m : s.SplineMeshes )
                meshes.emplace_back(
                     Common::Json::ObjectBuilder()
                          .Set( "Mesh", m.Mesh )
                          .Set( "Primitive",
                                std::string( m.Primitive ? Geometry::PrimitiveTypeName( *m.Primitive ) : "" ) )
                          .Set( "Scale", m.Scale )
                          .Set( "ForwardAxis",
                                std::string( World::Landscape::LandscapeSplineMeshAxisName( m.ForwardAxis ) ) )
                          .Set( "CenterH", m.CenterH )
                          .Set( "CenterAdjust", m.CenterAdjust )
                          .Build() );
            segments.emplace_back( Common::Json::ObjectBuilder()
                                        .Set( "Start", s.Start )
                                        .Set( "End", s.End )
                                        .Set( "RaiseTerrain", s.RaiseTerrain )
                                        .Set( "LowerTerrain", s.LowerTerrain )
                                        .Set( "LayerName", s.LayerName )
                                        .Set( "SplineMeshes", Common::Json::Value( std::move( meshes ) ) )
                                        .Build() );
        }
        Common::Json::Object object;
        object["ControlPoints"] = Common::Json::Value( std::move( points ) );
        object["Segments"]      = Common::Json::Value( std::move( segments ) );
        return object;
    }

    // The splines are read whole or not at all, like the stack: an unreadable field, or a set
    // ValidateLandscapeSplines refuses, is an issue and the current splines stay. Absent keeps them (rule 1).
    inline void ReadLandscapeSplinesJson( const Common::Json::Node& from, LandscapeSplines& out,
                                          Common::Json::Issues& issues )
    {
        const auto value = from.Find( "Splines" );
        if ( !value || !value->ExpectKind( Common::Json::Kind::Object, issues ) )
            return;
        const size_t     before = issues.size();
        LandscapeSplines read;
        if ( const auto points = value->Find( "ControlPoints" );
             points && points->ExpectKind( Common::Json::Kind::Array, issues ) )
            points->ForEachElement(
                 [&]( std::size_t, const Common::Json::Node& element )
                 {
                     if ( !element.ExpectKind( Common::Json::Kind::Object, issues ) )
                         return;
                     LandscapeSplineControlPoint p;
                     element.ReadInto( "Location", p.Location, issues );
                     element.ReadInto( "Width", p.Width, issues );
                     element.ReadInto( "SideFalloff", p.SideFalloff, issues );
                     element.ReadInto( "EndFalloff", p.EndFalloff, issues );
                     element.ReadInto( "LayerWidthRatio", p.LayerWidthRatio, issues );
                     read.ControlPoints.push_back( p );
                 } );
        if ( const auto segments = value->Find( "Segments" );
             segments && segments->ExpectKind( Common::Json::Kind::Array, issues ) )
            segments->ForEachElement(
                 [&]( std::size_t, const Common::Json::Node& element )
                 {
                     if ( !element.ExpectKind( Common::Json::Kind::Object, issues ) )
                         return;
                     LandscapeSplineSegment s;
                     element.ReadInto( "Start", s.Start, issues );
                     element.ReadInto( "End", s.End, issues );
                     element.ReadInto( "RaiseTerrain", s.RaiseTerrain, issues );
                     element.ReadInto( "LowerTerrain", s.LowerTerrain, issues );
                     element.ReadInto( "LayerName", s.LayerName, issues );
                     if ( const auto meshes = element.Find( "SplineMeshes" );
                          meshes && meshes->ExpectKind( Common::Json::Kind::Array, issues ) )
                         meshes->ForEachElement(
                              [&]( std::size_t, const Common::Json::Node& mesh )
                              {
                                  if ( !mesh.ExpectKind( Common::Json::Kind::Object, issues ) )
                                      return;
                                  LandscapeSplineMeshEntry m;
                                  std::string              primitive, axis = "X";
                                  mesh.ReadInto( "Mesh", m.Mesh, issues );
                                  mesh.ReadInto( "Primitive", primitive, issues );
                                  mesh.ReadInto( "Scale", m.Scale, issues );
                                  mesh.ReadInto( "ForwardAxis", axis, issues );
                                  mesh.ReadInto( "CenterH", m.CenterH, issues );
                                  mesh.ReadInto( "CenterAdjust", m.CenterAdjust, issues );
                                  for ( int t = 0; t < static_cast<int>( Geometry::PrimitiveType::Count ); ++t )
                                      if ( primitive == Geometry::PrimitiveTypeName(
                                                             static_cast<Geometry::PrimitiveType>( t ) ) )
                                          m.Primitive = static_cast<Geometry::PrimitiveType>( t );
                                  if ( !primitive.empty() && !m.Primitive )
                                      issues.push_back( { mesh.Where().Key( "Primitive" ).ToString(),
                                                          "a built-in shape name", primitive } );
                                  if ( axis == "X" || axis == "Y" || axis == "Z" )
                                      m.ForwardAxis = static_cast<LandscapeSplineMeshAxis>( axis[0] - 'X' );
                                  else
                                      issues.push_back( { mesh.Where().Key( "ForwardAxis" ).ToString(),
                                                          "\"X\", \"Y\" or \"Z\"", axis } );
                                  s.SplineMeshes.push_back( std::move( m ) );
                              } );
                     read.Segments.push_back( s );
                 } );
        if ( issues.size() != before )
            return;
        if ( auto valid = ValidateLandscapeSplines( read ); !valid )
        {
            issues.push_back( { value->Where().ToString(), "valid landscape splines", valid.GetError() } );
            return;
        }
        out = std::move( read );
    }

} // namespace Desert::World::Landscape
