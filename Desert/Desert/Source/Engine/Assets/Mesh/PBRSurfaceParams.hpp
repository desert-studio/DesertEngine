#pragma once

#include <glm/glm.hpp>
#include <optional>

#include <Engine/Assets/Common.hpp> // Desert::Assets::AssetHandle
#include <Engine/Assets/MaterialData.hpp>

namespace Desert::Assets
{
    // Typed VIEW of a standard-surface (PBR) material's parameters.
    //
    // NOT a protocol and NOT serialized: the .demat stores only the generic MaterialData canon.
    // This struct exists for the optimized PBR backend (StaticMaterialPBR / SkinnedMaterialPBR /
    // BuildPBRGpuMaterial — direct field access on the render hot path beats name lookups) and
    // for the mesh importer (typed extraction from FBX/glTF). Field names match the StaticMeshPBR
    // shader schema 1:1 — FromMaterialData/ToMaterialData convert by those names.
    struct PBRSurfaceParams
    {
        glm::vec4 AlbedoColor       = glm::vec4( 1.0f );
        float     MetallicFactor    = 0.0f;
        float     RoughnessFactor   = 0.5f;
        float     AOStrength        = 1.0f;
        glm::vec4 EmissiveColor     = glm::vec4( 0.0f, 0.0f, 0.0f, 1.0f );
        float     EmissiveIntensity = 1.0f;
        float     AlphaCutoff       = 0.0f;
        float     Transmission      = 0.0f;
        float     IOR               = 1.5f;
        glm::vec4 GlassTint         = glm::vec4( 1.0f );

        AssetHandle AlbedoTexture{ 0ULL };
        AssetHandle NormalTexture{ 0ULL };
        AssetHandle MetallicTexture{ 0ULL };
        AssetHandle RoughnessTexture{ 0ULL };
        AssetHandle AOTexture{ 0ULL };
        AssetHandle EmissiveTexture{ 0ULL };
        AssetHandle OpacityTexture{ 0ULL };

        std::optional<glm::vec2> UVTiling;

        static PBRSurfaceParams FromMaterialData( const MaterialData& m )
        {
            PBRSurfaceParams p;
            p.AlbedoColor       = m.GetParam( "AlbedoColor", p.AlbedoColor );
            p.MetallicFactor    = m.GetFloat( "MetallicFactor", p.MetallicFactor );
            p.RoughnessFactor   = m.GetFloat( "RoughnessFactor", p.RoughnessFactor );
            p.AOStrength        = m.GetFloat( "AOStrength", p.AOStrength );
            p.EmissiveColor     = m.GetParam( "EmissiveColor", p.EmissiveColor );
            p.EmissiveIntensity = m.GetFloat( "EmissiveIntensity", p.EmissiveIntensity );
            p.AlphaCutoff       = m.GetFloat( "AlphaCutoff", p.AlphaCutoff );
            p.Transmission      = m.GetFloat( "Transmission", p.Transmission );
            p.IOR               = m.GetFloat( "IOR", p.IOR );
            p.GlassTint         = m.GetParam( "GlassTint", p.GlassTint );
            if ( const auto* v = m.FindParam( "UVTiling" ) )
                p.UVTiling = glm::vec2( *v );

            p.AlbedoTexture    = AssetHandle( m.GetTexture( "u_AlbedoTexture" ) );
            p.NormalTexture    = AssetHandle( m.GetTexture( "u_NormalTexture" ) );
            p.MetallicTexture  = AssetHandle( m.GetTexture( "u_MetallicTexture" ) );
            p.RoughnessTexture = AssetHandle( m.GetTexture( "u_RoughnessTexture" ) );
            p.AOTexture        = AssetHandle( m.GetTexture( "u_AOTexture" ) );
            p.EmissiveTexture  = AssetHandle( m.GetTexture( "u_EmissiveTexture" ) );
            p.OpacityTexture   = AssetHandle( m.GetTexture( "u_OpacityTexture" ) );

            return p;
        }

        MaterialData ToMaterialData() const
        {
            MaterialData m;
            m.SetParam( "AlbedoColor", AlbedoColor );
            m.SetParam( "MetallicFactor", glm::vec4( MetallicFactor, 0, 0, 0 ) );
            m.SetParam( "RoughnessFactor", glm::vec4( RoughnessFactor, 0, 0, 0 ) );
            m.SetParam( "AOStrength", glm::vec4( AOStrength, 0, 0, 0 ) );
            m.SetParam( "EmissiveColor", EmissiveColor );
            m.SetParam( "EmissiveIntensity", glm::vec4( EmissiveIntensity, 0, 0, 0 ) );
            m.SetParam( "AlphaCutoff", glm::vec4( AlphaCutoff, 0, 0, 0 ) );
            m.SetParam( "Transmission", glm::vec4( Transmission, 0, 0, 0 ) );
            m.SetParam( "IOR", glm::vec4( IOR, 0, 0, 0 ) );
            m.SetParam( "GlassTint", GlassTint );
            m.SetParam( "UVTiling", glm::vec4( UVTiling.value_or( glm::vec2( 1.0f ) ), 0, 0 ) );

            m.SetTexture( "u_AlbedoTexture", static_cast<uint64_t>( AlbedoTexture ) );
            m.SetTexture( "u_NormalTexture", static_cast<uint64_t>( NormalTexture ) );
            m.SetTexture( "u_OpacityTexture", static_cast<uint64_t>( OpacityTexture ) );
            m.SetTexture( "u_MetallicTexture", static_cast<uint64_t>( MetallicTexture ) );
            m.SetTexture( "u_RoughnessTexture", static_cast<uint64_t>( RoughnessTexture ) );
            m.SetTexture( "u_AOTexture", static_cast<uint64_t>( AOTexture ) );
            m.SetTexture( "u_EmissiveTexture", static_cast<uint64_t>( EmissiveTexture ) );

            return m;
        }
    };
} // namespace Desert::Assets
