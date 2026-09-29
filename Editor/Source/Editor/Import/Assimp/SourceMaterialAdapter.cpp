#include <Editor/Import/Assimp/SourceMaterialAdapter.hpp>

#include <Common/Core/Logger.hpp>

#include <assimp/GltfMaterial.h>
#include <assimp/material.h>

#include <format>
#include <optional>

namespace Desert::Editor
{
    std::string_view SourceFormatOf( const std::filesystem::path& sourceFile )
    {
        std::string ext = sourceFile.extension().string();
        for ( char& c : ext )
            c = static_cast<char>( std::tolower( static_cast<unsigned char>( c ) ) );
        return ext == ".gltf" || ext == ".glb" ? "gltf" : "fbx";
    }

    SourceMaterialRead
    ReadSourceMaterial( const aiMaterial& mat, std::string_view format, std::string name,
                        const std::function<std::filesystem::path( const std::string& )>& findTexture )
    {
        SourceMaterialRead read;
        read.Material.Name = std::move( name );
        auto& entries      = read.Material.Entries;

        struct SourceTexture
        {
            std::filesystem::path                      File;
            std::optional<::Desert::Core::Formats::SamplerState> Sampler;
        };
        // The source's sampler for the texture of @p type: assimp folds glTF wrapS/wrapT (and FBX's wrap
        // mode) into MAPPINGMODE_U/V and keeps glTF magFilter as GLTF_MAPPINGFILTER_MAG. The default state
        // is reported as absent so an ordinary import writes nothing new into the .demat.
        const auto samplerOf = [&]( aiTextureType type ) -> std::optional<::Desert::Core::Formats::SamplerState>
        {
            using ::Desert::Core::Formats::SamplerWrap;
            const auto wrapOf = [&]( const char* key, unsigned t, unsigned index ) -> SamplerWrap
            {
                int mode = aiTextureMapMode_Wrap;
                if ( mat.Get( key, t, index, mode ) != AI_SUCCESS )
                    return SamplerWrap::Repeat;
                switch ( mode )
                {
                    case aiTextureMapMode_Clamp:
                    case aiTextureMapMode_Decal:
                        return SamplerWrap::Clamp;
                    case aiTextureMapMode_Mirror:
                        return SamplerWrap::Mirror;
                    default:
                        return SamplerWrap::Repeat;
                }
            };
            ::Desert::Core::Formats::SamplerState state;
            state.WrapU = wrapOf( AI_MATKEY_MAPPINGMODE_U( type, 0 ) );
            state.WrapV = wrapOf( AI_MATKEY_MAPPINGMODE_V( type, 0 ) );
            unsigned magFilter = 0;
            if ( mat.Get( AI_MATKEY_GLTF_MAPPINGFILTER_MAG( type, 0 ), magFilter ) == AI_SUCCESS )
                state.Filter = ::Desert::Core::Formats::SamplerFilterFromGltf( static_cast<int>( magFilter ) );
            if ( state == ::Desert::Core::Formats::SamplerState{} )
                return std::nullopt;
            return state;
        };
        const auto texture = [&]( aiTextureType type ) -> std::optional<SourceTexture>
        {
            aiString path;
            if ( mat.GetTextureCount( type ) == 0 || mat.GetTexture( type, 0, &path ) != AI_SUCCESS )
                return std::nullopt;
            const std::filesystem::path found = findTexture( path.C_Str() );
            if ( found.empty() )
            {
                LOG_WARN( "[Import][Tex] material '{}': texture '{}' (type {}) NOT FOUND", read.Material.Name,
                          path.C_Str(), static_cast<int>( type ) );
                return std::nullopt;
            }
            return SourceTexture{ found, samplerOf( type ) };
        };
        const auto colour = [&]( const char* key, unsigned type, unsigned index ) -> std::optional<glm::vec4>
        {
            aiColor4D c;
            if ( mat.Get( key, type, index, c ) != AI_SUCCESS )
                return std::nullopt;
            return glm::vec4( c.r, c.g, c.b, c.a );
        };
        const auto scalar = [&]( const char* key, unsigned type, unsigned index ) -> std::optional<glm::vec4>
        {
            ai_real f = 0;
            if ( mat.Get( key, type, index, f ) != AI_SUCCESS )
                return std::nullopt;
            return glm::vec4( static_cast<float>( f ), 0.0f, 0.0f, 0.0f );
        };
        const auto put = [&]( std::string_view key, std::optional<glm::vec4> value,
                              const std::optional<SourceTexture>& file = std::nullopt )
        {
            if ( value || file )
                entries[std::format( "{}.{}", format, key )] = {
                     value, file ? std::optional( file->File ) : std::nullopt,
                     file ? file->Sampler : std::nullopt };
        };

        const std::optional<SourceTexture> baseColour = texture( aiTextureType_DIFFUSE );
        read.Alpha = ResolveSourceAlpha( mat, baseColour ? baseColour->File : std::filesystem::path{} );

        if ( format == "fbx" )
        {
            put( "DiffuseColor", colour( AI_MATKEY_COLOR_DIFFUSE ), baseColour );
            put( "NormalMap", std::nullopt, texture( aiTextureType_NORMALS ) );
            std::optional<glm::vec4> emissive = colour( AI_MATKEY_COLOR_EMISSIVE );
            if ( emissive )
                emissive->a = 1.0f;
            put( "EmissiveColor", emissive, texture( aiTextureType_EMISSIVE ) );
            put( "TransparentColor", std::nullopt, texture( aiTextureType_OPACITY ) );
            // PBR keys as assimp's FBX converter hands them over (FBXConverter.cpp SetTextureProperties /
            // SetShadingPropertiesCommon): Maya Stingray PBS and 3ds Max Physical maps land in METALNESS,
            // DIFFUSE_ROUGHNESS and AMBIENT_OCCLUSION; their factors in METALLIC_FACTOR and ROUGHNESS_FACTOR (the
            // latter also derived from a Phong ShininessExponent, Blender's rule). A map in SHININESS (a Phong
            // exponent map, or 3ds Max's inverted roughness/glossiness) is carried as GlossinessMap so the
            // unread-key warning names it: no template reads a glossiness image today.
            put( "Metalness", scalar( AI_MATKEY_METALLIC_FACTOR ), texture( aiTextureType_METALNESS ) );
            put( "Roughness", scalar( AI_MATKEY_ROUGHNESS_FACTOR ), texture( aiTextureType_DIFFUSE_ROUGHNESS ) );
            put( "AmbientOcclusion", std::nullopt, texture( aiTextureType_AMBIENT_OCCLUSION ) );
            put( "GlossinessMap", std::nullopt, texture( aiTextureType_SHININESS ) );
            if ( read.Alpha.AlphaCutoff > 0.0f )
                put( "alphaCutoff", glm::vec4( read.Alpha.AlphaCutoff, 0, 0, 0 ) );
            return read;
        }

        put( "baseColorFactor", colour( AI_MATKEY_BASE_COLOR ) );
        put( "baseColorTexture", std::nullopt, baseColour );
        put( "metallicFactor", scalar( AI_MATKEY_METALLIC_FACTOR ) );
        put( "roughnessFactor", scalar( AI_MATKEY_ROUGHNESS_FACTOR ) );
        put( "metallicRoughnessTexture", std::nullopt, texture( aiTextureType_METALNESS ) );
        if ( const auto occlusion = texture( aiTextureType_LIGHTMAP ) )
        {
            put( "occlusionTexture", std::nullopt, occlusion );
            put( "occlusionStrength", scalar( AI_MATKEY_GLTF_TEXTURE_STRENGTH( aiTextureType_LIGHTMAP, 0 ) ) );
        }
        if ( const auto normal = texture( aiTextureType_NORMALS ) )
        {
            put( "normalTexture", std::nullopt, normal );
            put( "normalScale", scalar( AI_MATKEY_GLTF_TEXTURE_SCALE( aiTextureType_NORMALS, 0 ) ) );
        }
        std::optional<glm::vec4> emissive = colour( AI_MATKEY_COLOR_EMISSIVE );
        if ( emissive )
            emissive->a = 1.0f;
        put( "emissiveFactor", emissive );
        put( "emissiveTexture", std::nullopt, texture( aiTextureType_EMISSIVE ) );
        put( "emissiveStrength", scalar( AI_MATKEY_EMISSIVE_INTENSITY ) );

        // The mask is the base colour's ALPHA (glTF 2.0 §3.9.4): the albedo image itself is the opacity slot's
        // source, and the value half names its channel (3 = A) for the template's OpacityChannel.
        if ( read.Alpha.AlphaCutoff > 0.0f && baseColour &&
             ( read.Alpha.Kind == SourceAlphaKind::Mask || read.Alpha.Kind == SourceAlphaKind::BlendAsMask ) )
        {
            put( "alphaCutoff", glm::vec4( read.Alpha.AlphaCutoff, 0, 0, 0 ) );
            put( "alphaMask", glm::vec4( 3.0f, 0, 0, 0 ), baseColour );
        }

        aiUVTransform transform;
        if ( baseColour && mat.Get( AI_MATKEY_UVTRANSFORM( aiTextureType_DIFFUSE, 0 ), transform ) == AI_SUCCESS )
        {
            put( "uvOffset", glm::vec4( transform.mTranslation.x, transform.mTranslation.y, 0, 0 ) );
            put( "uvScale", glm::vec4( transform.mScaling.x, transform.mScaling.y, 0, 0 ) );
            put( "uvRotation", glm::vec4( transform.mRotation, 0, 0, 0 ) );
        }
        for ( const aiTextureType type : { aiTextureType_DIFFUSE, aiTextureType_METALNESS, aiTextureType_LIGHTMAP,
                                           aiTextureType_NORMALS, aiTextureType_EMISSIVE } )
        {
            int set = 0;
            if ( mat.GetTextureCount( type ) > 0 && mat.Get( AI_MATKEY_UVWSRC( type, 0 ), set ) == AI_SUCCESS &&
                 set != 0 )
                put( "texCoord", glm::vec4( static_cast<float>( set ), 0, 0, 0 ) );
        }
        int twoSided = 0;
        if ( mat.Get( AI_MATKEY_TWOSIDED, twoSided ) == AI_SUCCESS && twoSided != 0 )
            put( "doubleSided", glm::vec4( 1.0f, 0, 0, 0 ) );
        int shading = 0;
        if ( mat.Get( AI_MATKEY_SHADING_MODEL, shading ) == AI_SUCCESS && shading == aiShadingMode_Unlit )
            entries[std::format( "{}.KHR_materials_unlit", format )] = {};

        put( "KHR_materials_clearcoat", scalar( AI_MATKEY_CLEARCOAT_FACTOR ) );
        put( "KHR_materials_transmission", scalar( AI_MATKEY_TRANSMISSION_FACTOR ) );
        put( "KHR_materials_sheen", colour( AI_MATKEY_SHEEN_COLOR_FACTOR ) );
        put( "KHR_materials_specular", scalar( AI_MATKEY_SPECULAR_FACTOR ) );
        put( "KHR_materials_ior", scalar( AI_MATKEY_REFRACTI ) );
        put( "KHR_materials_volume", scalar( AI_MATKEY_VOLUME_THICKNESS_FACTOR ) );
        return read;
    }
} // namespace Desert::Editor
