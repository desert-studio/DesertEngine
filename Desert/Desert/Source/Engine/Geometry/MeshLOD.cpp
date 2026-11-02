#include "MeshLOD.hpp"

#include "LODSelection.hpp"
#include "MeshSimplifier.hpp"

#include <meshoptimizer.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace Desert::Geometry
{
    namespace
    {
        // One connected part of a submesh over POSITION-welded topology (a normal/UV seam does not split it):
        // its triangles (indices into the base list), box and surface area.
        struct LODComponent
        {
            std::vector<uint32_t> Triangles;
            glm::vec3             Min{ std::numeric_limits<float>::max() };
            glm::vec3             Max{ -std::numeric_limits<float>::max() };
            float                 Area = 0.0f;
        };

        uint32_t LODFindRoot( std::vector<uint32_t>& parent, uint32_t v )
        {
            while ( parent[v] != v )
            {
                parent[v] = parent[parent[v]];
                v         = parent[v];
            }
            return v;
        }

        // A stable scramble of an element's ordinal: the order elements are kept in, so a coarser level keeps a
        // subset of the finer level's elements spread over the whole mesh, not one end of the vertex order.
        uint32_t LODElementOrderKey( uint32_t ordinal )
        {
            uint32_t h = ordinal * 0x9E3779B1u;
            h ^= h >> 16;
            h *= 0x85EBCA6Bu;
            h ^= h >> 13;
            h *= 0xC2B2AE35u;
            h ^= h >> 16;
            return h;
        }

        glm::vec3 LODPosition( const float* positions, const uint32_t v )
        {
            return { positions[v * 3 + 0], positions[v * 3 + 1], positions[v * 3 + 2] };
        }

        float LODHalfDiagonal( const LODComponent& part )
        {
            return 0.5f * glm::length( part.Max - part.Min );
        }
    } // namespace

    std::vector<std::vector<Index>> SimplifyLODLevels( const float* positions, uint32_t vertexCount,
                                                       const std::vector<Index>& localTris,
                                                       const LODChainSettings& chain, const float* attributes,
                                                       std::vector<LODAddedVertex>* addedVertices )
    {
        const std::vector<uint32_t> targets = LODChainTargets( static_cast<uint32_t>( localTris.size() ), chain );
        std::vector<std::vector<Index>> levels;
        levels.reserve( targets.size() );
        if ( targets.empty() || !positions || vertexCount == 0 )
            return levels;

        std::vector<uint32_t> flat;
        flat.reserve( localTris.size() * 3 );
        for ( const auto& t : localTris )
        {
            flat.push_back( t.V1 );
            flat.push_back( t.V2 );
            flat.push_back( t.V3 );
        }

        // THE SUBMESH'S PARTS. Union over position-welded vertices: a leaf card, a nail, a rail, a trunk.
        std::vector<uint32_t> welded( flat.size() );
        meshopt_generateShadowIndexBuffer( welded.data(), flat.data(), flat.size(), positions, vertexCount,
                                           sizeof( float ) * 3, sizeof( float ) * 3 );
        std::vector<uint32_t> parent( vertexCount );
        for ( uint32_t v = 0; v < vertexCount; ++v )
            parent[v] = v;
        for ( std::size_t i = 0; i + 2 < welded.size(); i += 3 )
        {
            const uint32_t a                          = LODFindRoot( parent, welded[i] );
            parent[LODFindRoot( parent, welded[i + 1] )] = a;
            parent[LODFindRoot( parent, welded[i + 2] )] = a;
        }
        constexpr uint32_t        kNone = std::numeric_limits<uint32_t>::max();
        std::vector<uint32_t>     componentOfRoot( vertexCount, kNone );
        std::vector<LODComponent> components;
        for ( uint32_t t = 0; t < localTris.size(); ++t )
        {
            const uint32_t root = LODFindRoot( parent, welded[t * 3] );
            if ( componentOfRoot[root] == kNone )
            {
                componentOfRoot[root] = static_cast<uint32_t>( components.size() );
                components.emplace_back();
            }
            LODComponent&   part = components[componentOfRoot[root]];
            const glm::vec3 a    = LODPosition( positions, flat[t * 3] );
            const glm::vec3 b    = LODPosition( positions, flat[t * 3 + 1] );
            const glm::vec3 c    = LODPosition( positions, flat[t * 3 + 2] );
            part.Triangles.push_back( t );
            part.Min = glm::min( part.Min, glm::min( a, glm::min( b, c ) ) );
            part.Max = glm::max( part.Max, glm::max( a, glm::max( b, c ) ) );
            part.Area += 0.5f * glm::length( glm::cross( b - a, c - a ) );
        }

        // THE ELEMENTS: parts too small for a reduction of their own to keep them (a leaf card is a quad or two;
        // any collapse folds it to nothing). They are reduced by count with their area preserved; the rest -
        // trunk, branches, rails, posts - by error. Without vertex storage (the load-time chain) or with only a
        // few small parts (a fence's bolts) none is an element: they are solid parts like the rest.
        const float           extent = meshopt_simplifyScale( positions, vertexCount, sizeof( float ) * 3 );
        std::vector<uint32_t> elements;
        for ( uint32_t p = 0; p < components.size(); ++p )
            if ( extent > 0.0f && LODHalfDiagonal( components[p] ) <= kLODElementExtentShare * extent )
                elements.push_back( p );
        if ( elements.size() < kLODMinElements || addedVertices == nullptr )
            elements.clear();
        std::vector<bool> isElement( components.size(), false );
        double            elementArea = 0.0, elementRadiusArea = 0.0;
        std::size_t       elementTris = 0;
        for ( const uint32_t p : elements )
        {
            isElement[p] = true;
            elementTris += components[p].Triangles.size();
            elementArea += components[p].Area;
            elementRadiusArea += static_cast<double>( components[p].Area ) * LODHalfDiagonal( components[p] );
        }
        std::vector<uint32_t> solidFlat;
        solidFlat.reserve( ( localTris.size() - elementTris ) * 3 );
        for ( uint32_t p = 0; p < components.size(); ++p )
            if ( !isElement[p] )
                for ( const uint32_t t : components[p].Triangles )
                    solidFlat.insert( solidFlat.end(), { flat[t * 3], flat[t * 3 + 1], flat[t * 3 + 2] } );
        // The elements' mean radius against the extent (area-weighted): how far inflating one moves its edge.
        const double elementRadiusShare =
             elementArea > 0.0 && extent > 0.0f ? elementRadiusArea / elementArea / extent : 0.0;
        std::sort( elements.begin(), elements.end(),
                   []( uint32_t x, uint32_t y ) { return LODElementOrderKey( x ) < LODElementOrderKey( y ); } );

        const std::size_t     totalTris  = localTris.size();
        const std::size_t     solidTris  = solidFlat.size() / 3;
        std::size_t           prevCount  = flat.size(); // each level must strictly decrease
        float                 screenSize = kLOD0ScreenSize;
        std::vector<uint32_t> remap( vertexCount, kNone );
        for ( std::size_t level = 0; level < targets.size(); ++level )
        {
            // THE LEVEL'S ERROR BUDGET: kLODMaxPixelError reference pixels at the screen size the automatic
            // progression starts this level at (there the bounding sphere's diameter is screenSize x 1080 rows,
            // and the extent the relative error is measured against is no longer than that diameter). A part
            // thinner than the budget may go - it is under a pixel there; a rail wider than it stays.
            screenSize = ComputeLODScreenSize( static_cast<uint32_t>( level + 1 ), screenSize, 0, 1.0f );
            const float errorBudget =
                 kLODMaxPixelError / ( std::max( screenSize, 1e-6f ) * kAutoLODReferenceScreenRows );
            const double share = static_cast<double>( targets[level] ) / static_cast<double>( totalTris );

            const std::size_t     addedBefore = addedVertices ? addedVertices->size() : 0;
            std::vector<uint32_t> out;
            float                 error = 0.0f;
            if ( solidTris > 0 )
            {
                const auto  solidTarget = static_cast<std::size_t>( static_cast<double>( solidTris ) * share );
                const float ratio = ( static_cast<float>( solidTarget ) + 0.5f ) / static_cast<float>( solidTris );
                auto lod = SimplifyMesh( positions, vertexCount, solidFlat, ratio, errorBudget, attributes );
                if ( !lod.StopReason.empty() )
                    LOG_INFO( "[MeshLOD] LOD {}: solid parts stopped at {} of target {} tris: {}", level + 1,
                              lod.Indices.size() / 3, solidTarget, lod.StopReason );
                out   = std::move( lod.Indices );
                error = lod.Error;
            }

            // ELEMENTS (UE Nanite Preserve Area): keep a scrambled subset reaching the level's share of their
            // triangles, but never less area than the budget lets the kept ones grow back (one inflated by s
            // moves its edge by (s - 1) x its radius), and inflate each kept one about its centre by
            // sqrt(all area / kept area), so a crown covers what it covered with fewer, larger cards.
            std::size_t keptElements = 0;
            float       inflation    = 1.0f;
            if ( !elements.empty() )
            {
                const double maxInflation =
                     elementRadiusShare > 0.0 ? 1.0 + static_cast<double>( errorBudget ) / elementRadiusShare : 1.0;
                const double minArea   = elementArea / ( maxInflation * maxInflation );
                const auto   triTarget = static_cast<std::size_t>( static_cast<double>( elementTris ) * share );
                std::size_t  keptTris  = 0;
                double       keptArea  = 0.0;
                while ( keptElements < elements.size() && ( keptTris < triTarget || keptArea < minArea ) )
                {
                    keptTris += components[elements[keptElements]].Triangles.size();
                    keptArea += components[elements[keptElements]].Area;
                    ++keptElements;
                }
                inflation          = keptArea > 0.0 ? static_cast<float>( std::sqrt( elementArea / keptArea ) ) : 1.0f;
                const bool inflate = inflation > 1.0f + 1e-4f;
                std::vector<uint32_t> touched;
                for ( std::size_t e = 0; e < keptElements; ++e )
                {
                    const LODComponent& part   = components[elements[e]];
                    const glm::vec3     centre = 0.5f * ( part.Min + part.Max );
                    for ( const uint32_t t : part.Triangles )
                        for ( uint32_t k = 0; k < 3; ++k )
                        {
                            const uint32_t v = flat[t * 3 + k];
                            if ( !inflate )
                            {
                                out.push_back( v );
                                continue;
                            }
                            if ( remap[v] == kNone )
                            {
                                remap[v] = vertexCount + static_cast<uint32_t>( addedVertices->size() );
                                addedVertices->push_back(
                                     { v, centre + inflation * ( LODPosition( positions, v ) - centre ) } );
                                touched.push_back( v );
                            }
                            out.push_back( remap[v] );
                        }
                }
                for ( const uint32_t v : touched )
                    remap[v] = kNone;
            }

            LOG_INFO( "[MeshLOD] LOD {}: target {} tris, result {} of {} tris, solid deviation {:.4f} of the extent "
                      "(budget {:.4f} at screen size {:.4f}); {} of {} elements kept, inflated x{:.3f}{}",
                      level + 1, targets[level], out.size() / 3, totalTris, error, errorBudget, screenSize,
                      keptElements, elements.size(), inflation, attributes ? "" : " (positions only)" );
            std::vector<Index> tris;
            if ( out.size() >= 3 && out.size() < prevCount )
            {
                tris.reserve( out.size() / 3 );
                for ( std::size_t i = 0; i + 2 < out.size(); i += 3 )
                    tris.push_back( { out[i], out[i + 1], out[i + 2] } );
                prevCount = out.size();
            }
            else if ( addedVertices )
                addedVertices->resize( addedBefore ); // the level is not kept: neither are its inflated elements
            levels.push_back( std::move( tris ) ); // empty => reuse previous at assembly
        }
        return levels;
    }

    void AppendSimplifyAttributes( const glm::vec3& normal, const glm::vec2& uv, std::vector<float>& out )
    {
        out.push_back( normal.x );
        out.push_back( normal.y );
        out.push_back( normal.z );
        out.push_back( uv.x );
        out.push_back( uv.y );
    }

    std::vector<Index> BuildLODIndexBuffer( const std::vector<Vertex>& vertices,
                                            const std::vector<Index>&  baseIndices,
                                            std::vector<Submesh>&      submeshes,
                                            const LODChainSettings&    chain )
    {
        std::vector<Index> gpuIndices = baseIndices;

        for ( auto& sm : submeshes )
        {
            sm.LODs.clear();
            sm.LODs.push_back( { sm.IndexOffset, sm.IndexCount } ); // LOD0 = original

            const uint32_t triCount = sm.IndexCount / 3;
            if ( triCount < 8 || sm.VertexCount == 0 )
                continue; // too small to bother

            // A cooked chain is the submesh's whole chain, possibly empty (UE: a cooked mesh's LODs are its
            // LODResources); only a mesh no cook made generates its chain now. Both are submesh-local triangles
            // (indices relative to VertexOffset -> same baseVertex).
            std::vector<std::vector<Index>> levels;
            if ( sm.LodChainCooked )
            {
                levels = sm.BakedLODs;
            }
            else
            {
                std::vector<Index> localTris;
                localTris.reserve( triCount );
                const uint32_t triStart = sm.IndexOffset / 3;
                for ( uint32_t t = 0; t < triCount; ++t )
                    localTris.push_back( baseIndices[triStart + t] );

                std::vector<float> pos;
                std::vector<float> attrs;
                pos.reserve( static_cast<size_t>( sm.VertexCount ) * 3 );
                attrs.reserve( static_cast<size_t>( sm.VertexCount ) * kSimplifyAttributeCount );
                for ( uint32_t v = 0; v < sm.VertexCount; ++v )
                {
                    const auto& vertex = vertices[sm.VertexOffset + v];
                    pos.push_back( vertex.Position.x );
                    pos.push_back( vertex.Position.y );
                    pos.push_back( vertex.Position.z );
                    AppendSimplifyAttributes( vertex.Normal, vertex.TexCoord, attrs );
                }
                levels = SimplifyLODLevels( pos.data(), sm.VertexCount, localTris, chain, attrs.data() );
            }

            for ( const auto& lvl : levels )
            {
                const uint32_t lvlIndexCount = static_cast<uint32_t>( lvl.size() * 3 );
                if ( lvl.empty() || lvlIndexCount >= sm.LODs.back().IndexCount )
                {
                    sm.LODs.push_back( sm.LODs.back() ); // no improvement -> reuse previous (monotonic)
                    continue;
                }
                const uint32_t offset = static_cast<uint32_t>( gpuIndices.size() * 3 );
                for ( const auto& tri : lvl )
                    gpuIndices.push_back( tri );
                sm.LODs.push_back( { offset, lvlIndexCount } );
            }
        }

        return gpuIndices;
    }
} // namespace Desert::Geometry
