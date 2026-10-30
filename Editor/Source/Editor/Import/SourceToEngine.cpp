#include "SourceToEngine.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <variant>
#include <vector>

namespace Desert::Editor
{
    namespace Ser = Assets::Serialization;

    glm::mat4 SourceToEngine( const Assets::MeshImportSettings& settings )
    {
        glm::mat4 m = glm::scale( glm::mat4( 1.0f ), glm::vec3( settings.UniformScale ) );
        if ( settings.UpAxis == Assets::MeshSourceUpAxis::Z )
            m = glm::rotate( glm::mat4( 1.0f ), glm::radians( -90.0f ), glm::vec3( 1.0f, 0.0f, 0.0f ) ) * m;
        return m;
    }

    std::vector<Assets::Serialization::ImportRecordLight>
    SourceLightsToEngine( std::span<const ImportedLight> lights, const Assets::MeshImportSettings& settings )
    {
        const glm::mat4                                       c = SourceToEngine( settings );
        std::vector<Assets::Serialization::ImportRecordLight> out;
        out.reserve( lights.size() );
        for ( const ImportedLight& light : lights )
        {
            Assets::Serialization::ImportRecordLight stated;
            stated.Name = light.Name;
            switch ( light.Type )
            {
                case ImportedLightType::Point:
                    stated.Type = "Point";
                    break;
                case ImportedLightType::Spot:
                    stated.Type           = "Spot";
                    stated.InnerConeAngle = std::min( light.InnerConeDegrees, light.OuterConeDegrees );
                    stated.OuterConeAngle = light.OuterConeDegrees;
                    break;
                case ImportedLightType::Directional:
                    stated.Type = "Directional";
                    break;
            }
            const glm::vec3 position  = glm::vec3( c * glm::vec4( light.Position, 1.0f ) );
            const glm::vec3 direction = glm::normalize( glm::vec3( c * glm::vec4( light.Direction, 0.0f ) ) );
            stated.Position           = { position.x, position.y, position.z };
            stated.Direction          = { direction.x, direction.y, direction.z };
            const glm::vec3 radiance  = glm::max( light.Radiance, glm::vec3( 0.0f ) );
            const float     peak      = std::max( { radiance.r, radiance.g, radiance.b } );
            const glm::vec3 color     = peak > 0.0f ? radiance / peak : glm::vec3( 1.0f );
            stated.Color              = { color.r, color.g, color.b };
            stated.Intensity          = peak;
            if ( light.Radius && light.Type != ImportedLightType::Directional )
                stated.Radius = *light.Radius * settings.UniformScale;
            out.push_back( std::move( stated ) );
        }
        return out;
    }

    Common::Math::AABB SourceToEngineBounds( const Common::Math::AABB&         box,
                                             const Assets::MeshImportSettings& settings )
    {
        const glm::mat4 c = SourceToEngine( settings );
        glm::vec3       mn( std::numeric_limits<float>::max() );
        glm::vec3       mx( std::numeric_limits<float>::lowest() );
        for ( int k = 0; k < 8; ++k )
        {
            const glm::vec3 corner = glm::vec3( c * glm::vec4( ( k & 1 ) != 0 ? box.Max.x : box.Min.x,
                                                               ( k & 2 ) != 0 ? box.Max.y : box.Min.y,
                                                               ( k & 4 ) != 0 ? box.Max.z : box.Min.z, 1.0f ) );
            mn                     = glm::min( mn, corner );
            mx                     = glm::max( mx, corner );
        }
        return Common::Math::AABB{ mn, mx };
    }

    void ApplySourceToEngine( const Assets::MeshImportSettings& settings, Ser::MeshAssetData* mesh,
                              Ser::SkeletonAssetData* skeleton, std::vector<Animation::AnimationClip>& animations )
    {
        if ( settings.UniformScale == 1.0f && settings.UpAxis != Assets::MeshSourceUpAxis::Z )
            return;

        const glm::mat4 c         = SourceToEngine( settings );
        const glm::mat4 cInverse  = glm::inverse( c );
        const glm::mat3 rotation  = glm::mat3( c ) / settings.UniformScale;
        const glm::quat qc        = glm::quat_cast( rotation );
        const auto      point     = [&c]( const glm::vec3& p ) { return glm::vec3( c * glm::vec4( p, 1.0f ) ); };
        const auto      vector    = [&c]( const glm::vec3& v ) { return glm::vec3( c * glm::vec4( v, 0.0f ) ); };
        const auto      conjugate = [&c, &cInverse]( const glm::mat4& m ) { return c * m * cInverse; };

        if ( mesh != nullptr )
        {
            const auto direction = [&rotation]( glm::vec3& v ) { v = rotation * v; };
            for ( auto& v : mesh->SkinnedVertices )
            {
                v.Position = point( v.Position );
                direction( v.Normal );
                direction( v.Tangent );
                direction( v.Bitangent );
            }
            for ( auto& v : mesh->StaticVertices )
            {
                v.Position = point( v.Position );
                direction( v.Normal );
                direction( v.Tangent );
                direction( v.Bitangent );
            }
            for ( auto& morph : mesh->MorphTargets )
            {
                for ( auto& d : morph.DeltaPositions )
                    d = vector( d );
                for ( auto& d : morph.DeltaNormals )
                    direction( d );
            }
            for ( auto& sub : mesh->Submeshes )
            {
                sub.Transform = conjugate( sub.Transform );
                const Common::Math::AABB moved =
                     SourceToEngineBounds( { sub.BoundingBox.Min, sub.BoundingBox.Max }, settings );
                sub.BoundingBox.Min = moved.Min;
                sub.BoundingBox.Max = moved.Max;
            }
        }

        if ( skeleton != nullptr )
            for ( auto& bone : skeleton->Bones )
            {
                bone.LocalBindTransform = conjugate( bone.LocalBindTransform );
                bone.OffsetMatrix       = conjugate( bone.OffsetMatrix );
            }

        // A local scale's axes move with C's rotation; C turns only by quarter turns, so the rotated vector's
        // magnitudes are the scale re-ordered.
        const auto reorder = [&rotation]( const glm::vec3& s ) { return glm::abs( rotation * s ); };
        // A clip's keys are its Transform sections' channels (ANIM 6); the components of one key share an index
        // across X/Y/Z(/W), which is how every producer keys a transform.
        const auto vec3Keys = []( Animation::Timeline::VectorChannel& c, auto&& map )
        {
            for ( std::size_t k = 0; k < c.X.Keys.size() && k < c.Y.Keys.size() && k < c.Z.Keys.size(); ++k )
            {
                const auto part = [&]( auto member )
                { return glm::vec3( c.X.Keys[k].*member, c.Y.Keys[k].*member, c.Z.Keys[k].*member ); };
                const glm::vec3  v = map( part( &Animation::ScalarKey::Value ), true );
                const glm::vec3  a = map( part( &Animation::ScalarKey::ArriveTangent ), false );
                const glm::vec3  l = map( part( &Animation::ScalarKey::LeaveTangent ), false );
                const std::array axes{ &c.X, &c.Y, &c.Z };
                for ( int i = 0; i < 3; ++i )
                {
                    auto& key         = axes[static_cast<std::size_t>( i )]->Keys[k];
                    key.Value         = v[i];
                    key.ArriveTangent = a[i];
                    key.LeaveTangent  = l[i];
                }
            }
        };
        for ( auto& clip : animations )
            for ( auto& track : clip.Sequence.Tracks )
                for ( auto& section : track.Sections )
                {
                    auto* content = std::get_if<Animation::Timeline::Channel>( &section.Content );
                    auto* channel = content != nullptr
                                         ? std::get_if<Animation::Timeline::TransformChannel>( content )
                                         : nullptr;
                    if ( channel == nullptr )
                        continue;
                    vec3Keys( channel->Translation, [&]( const glm::vec3& v, bool ) { return vector( v ); } );
                    auto& r = channel->Rotation;
                    for ( std::size_t k = 0; k < r.X.Keys.size(); ++k )
                    {
                        const glm::quat q = qc *
                                            glm::quat( r.W.Keys[k].Value, r.X.Keys[k].Value, r.Y.Keys[k].Value,
                                                       r.Z.Keys[k].Value ) *
                                            glm::conjugate( qc );
                        r.X.Keys[k].Value = q.x;
                        r.Y.Keys[k].Value = q.y;
                        r.Z.Keys[k].Value = q.z;
                        r.W.Keys[k].Value = q.w;
                    }
                    vec3Keys( channel->Scale, [&]( const glm::vec3& v, bool value )
                              { return value ? reorder( v ) : rotation * v; } );
                }
    }
} // namespace Desert::Editor
