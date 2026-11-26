#pragma once

#include <Engine/Assets/CloudModellingVolume.hpp>
#include <Engine/ECS/HeroCloudComponent.hpp>
#include <Engine/Graphic/Clouds/CloudPayload.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Desert::Graphic
{
    /**
     * @file
     * @brief The GPU side of the seam's AUTHORED producer: the hero clouds of a frame, and the ONE place
     *        an entity's transform becomes an instance.
     *
     * The layout is declared once, in CloudLayout.hpp (CloudAuthoredInstanceGpu, CloudAuthoredPayload), and
     * the GLSL half of Common/CloudAuthored.glslh is generated from it; this file is the packer.
     *
     * Units: KILOMETRES, and the frame is the FIELD's — x and z are world kilometres, y is the altitude
     * above the layer's base. That is the vector CloudRaymarch.shader hands SampleCloudField, and the
     * conversion from world centimetres happens here, exactly once, for the same reason PackCloudParams
     * converts the layer's distances exactly once.
     */

    /// How many DIFFERENT sculpted bodies the atlas holds. The same number, and deliberately so: a body
    /// with no instance is not uploaded and an instance needs a body, so a second constant could only ever
    /// be the smaller of the two doing the work of both.
    inline constexpr uint32_t kCloudModellingAtlasMaxSlabs = kCloudAuthoredSlots;

    /// The march's and the shadow map's binding for the instance buffer. Must equal
    /// CLOUD_AUTHORED_BUFFER_BINDING in the two shaders: SetStorageBuffer takes the number as an explicit
    /// argument and never consults the shader's reflection, so a mismatch lands the buffer on a different
    /// descriptor rather than on an error.
    inline constexpr uint32_t kCloudAuthoredBinding = 8u;

    /// The shadow map reads the SAME instance buffer through the SAME slot — an alias rather than a copy, so
    /// the two passes cannot drift apart. The sculpted bodies' atlas is a binding-block entry
    /// (CloudAuthoredAtlasTexture), named rather than numbered.
    inline constexpr uint32_t kCloudShadowAuthoredBinding = kCloudAuthoredBinding;

    // One hero cloud as the ECS collected it: the component, where the entity puts it, and what to call
    // it in a log line.
    //
    // NOT AN INSTANCE YET. Turning this into the bytes the march reads needs the volume's own authored
    // size and the layer's base altitude, and neither is knowable in the ECS — the first lives in a GPU
    // service and the second is computed by the packer from the layer's cloud types. That is the layering
    // rule rather than a preference (Runtime::ResourceRegistry is renderer-only), and it is why the
    // conversion lives in Graphic::PackCloudAuthoredInstance and this struct carries a matrix.
    //
    // THE NAME IS FOR THE WARNING AND NOTHING ELSE. A hero cloud whose body does not fit inside the
    // layer's shell is never sampled where it sticks out, and the symptom is a cumulus with its crown
    // sliced flat by an altitude nobody set; the renderer says which entity, with both altitudes. A
    // string copy for at most eight entities a frame is the price of a diagnosable message.
    struct HeroCloudInstance
    {
        ECS::HeroCloudData Data;
        glm::mat4          WorldTransform{ 1.0f };
        std::string        Name;
    inline constexpr uint32_t kCloudAuthoredPayloadBytes = sizeof( CloudAuthoredPayload );

    /**
     * @brief Where slab @p slot begins, as a depth coordinate of an atlas of @p slabCount bodies — the
     *        C++ side of CloudAuthoredAtlasSlabBaseW in Common/CloudAuthored.glslh.
     *
     * Two statements of one line, which is why Desert/Tests/Engine/CloudAuthored drives both over every
     * slot of every slab count rather than trusting the pair.
     */
    inline float CloudAuthoredAtlasSlabBaseW( uint32_t slot, uint32_t slabCount )
    {
        return static_cast<float>( slot ) / static_cast<float>( slabCount );
    }

    /**
     * @brief Turns one hero cloud into one instance. Pure: no GPU, no globals, no clock.
     *
     * @param worldTransform   the entity's world matrix, world units (centimetres).
     * @param volumeSizeKm     the body's own authored size, from the `.dcmv`. The transform's scale
     *                         multiplies it, exactly as a mesh's own size and its transform do.
     * @param layerBottomKm    the altitude of the cloud shell's base, kilometres — the same number
     *                         CloudGpuPayload::Layer.y carries, taken from the packed block rather than
     *                         recomputed, because an envelope computed twice is an envelope that can
     *                         disagree with itself.
     * @param data             the component.
     * @param slabBaseW      where this body sits in the atlas, from CloudAuthoredAtlasSlabBaseW. It is
     *                         passed in rather than derived here because WHICH slab a body got is the
     *                         atlas's decision and not this function's, and this function stays pure.
     *
     * @return the instance, or nothing when the transform cannot be inverted. A degenerate scale — a zero
     *         on any axis — has no map from the world back into the volume, and the honest answer is that
     *         there is no instance rather than a matrix full of infinities. The caller logs it.
     *
     * THE MAP IT BUILDS. A point of the body has a local coordinate in [-0.5, 0.5]^3; multiplied by the
     * volume's size in world units and then by the entity's transform it is a point in the world. What
     * the march needs is the other direction, from a field-space position in kilometres back to that
     * local cube, so this inverts the composition once, on the CPU, where it costs nothing — the march
     * would otherwise invert a matrix per sample.
     *
     * THE BOUNDS ARE THE AXIS-ALIGNED BOX AROUND THE ROTATED ONE, and they are centred on the instance's
     * own centre, which is what lets the shader recover the translation from them instead of carrying it
     * a second time. The half-extent along an axis is the sum of the absolute values of that ROW of the
     * forward map — the standard OBB-to-AABB bound, exact rather than a padded sphere.
     */
    struct CloudAuthoredPackResult
    {
        bool                     Valid = false;
        CloudAuthoredInstanceGpu Instance{};
    };

    inline CloudAuthoredPackResult PackCloudAuthoredInstance( const glm::mat4& worldTransform,
                                                              const glm::vec3& volumeSizeKm, float layerBottomKm,
                                                              const ECS::HeroCloudData& data, float slabBaseW )
    {
        CloudAuthoredPackResult result;

        if ( !( volumeSizeKm.x > 0.0f ) || !( volumeSizeKm.y > 0.0f ) || !( volumeSizeKm.z > 0.0f ) )
            return result;

        // The forward linear map, local cube -> world displacement in centimetres.
        const glm::vec3 sizeWorld = volumeSizeKm * kCloudWorldUnitsPerKm;

        glm::mat3 forwardWorld( worldTransform );
        forwardWorld[0] *= sizeWorld.x;
        forwardWorld[1] *= sizeWorld.y;
        forwardWorld[2] *= sizeWorld.z;

        // A determinant of zero is a scale of zero on some axis: the body has been flattened out of
        // existence and there is no way back from the world into it. Compared against a threshold in the
        // cube of a centimetre because that is the unit the matrix is in.
        const float determinant = glm::determinant( forwardWorld );
        if ( !std::isfinite( determinant ) || std::abs( determinant ) < 1e-6f )
            return result;

        // ... and in FIELD space, kilometres, which is the frame the march samples in.
        const glm::mat3 forwardField = forwardWorld * ( 1.0f / kCloudWorldUnitsPerKm );
        const glm::mat3 inverseField = glm::inverse( forwardField );

        const glm::vec3 translation = glm::vec3( worldTransform[3] );
        const glm::vec3 centreField{ translation.x / kCloudWorldUnitsPerKm,
                                     translation.y / kCloudWorldUnitsPerKm - layerBottomKm,
                                     translation.z / kCloudWorldUnitsPerKm };

        // The AABB of the rotated box. glm is column-major, so row i of the forward map is the i-th
        // component of each of the three columns.
        glm::vec3 halfExtent{ 0.0f };
        for ( int axis = 0; axis < 3; ++axis )
        {
            halfExtent[axis] = 0.5f * ( std::abs( forwardField[0][axis] ) + std::abs( forwardField[1][axis] ) +
                                        std::abs( forwardField[2][axis] ) );
        }

        const float strength = std::clamp( data.Strength, 0.0f, 1.0f );

        // THE CUTOUT IS SCALED BY THE STRENGTH, and that is what makes Strength a fade rather than a
        // hole-punch: a body wound down to nothing must give the procedural sky back, not leave the gap
        // it was standing in. So the cutout is `suppress * strength` and never anything else, which is
        // why the two travel as ONE SIGNED NUMBER — see the note on BoundsMin.w in
        // Common/CloudAuthored.glslh. The float that frees carries the atlas slab.
        const float signedStrength = data.SuppressProceduralField ? strength : -strength;

        CloudAuthoredInstanceGpu& instance = result.Instance;

        instance.Row0 = glm::vec4( inverseField[0][0], inverseField[1][0], inverseField[2][0],
                                   std::max( data.DetailFactor, 0.0f ) );
        instance.Row1 = glm::vec4( inverseField[0][1], inverseField[1][1], inverseField[2][1],
                                   std::max( data.DensityFactor, 0.0f ) );
        instance.Row2 = glm::vec4( inverseField[0][2], inverseField[1][2], inverseField[2][2],
                                   std::max( data.ExtinctionFactor, 0.0f ) );

        instance.BoundsMin = glm::vec4( centreField - halfExtent, signedStrength );
        instance.BoundsMax = glm::vec4( centreField + halfExtent, slabBaseW );

        result.Valid = true;
        return result;
    }

    /**
     * @brief The relation a hero cloud has to satisfy against the layer it stands in, as a predicate the
     *        renderer can warn about.
     *
     * THE MARCH ONLY SAMPLES BETWEEN THE TWO SHELLS. A body whose top is above the layer's top, or whose
     * base is below its base, is not clipped by anything the artist can see — it is simply never sampled
     * there, and the symptom is a cumulus with its crown sliced flat by an altitude nobody set. That is
     * this programme's own defect taxonomy exactly: two individually-correct numbers that have to agree
     * and nothing checks (`desert-engine-verify` §4), and the answer the house uses is to state the
     * relation and let the engine complain with both numbers in the message.
     *
     * @param instance      as packed above; its bounds are already in field space.
     * @param thicknessKm   the layer's thickness, CloudGpuPayload::Layer.z.
     * @return true when the body lies entirely inside the shell.
     */
    inline bool CloudAuthoredInstanceFitsLayer( const CloudAuthoredInstanceGpu& instance, float thicknessKm )
    {
        return instance.BoundsMin.y >= 0.0f && instance.BoundsMax.y <= thicknessKm;
    }

    /**
     * @brief The relation between the payload and the IMAGE it will be dispatched with: every live
     *        instance must address a slab the bound atlas actually has.
     *
     * WHY THIS IS A PREDICATE AND NOT A COMMENT. The march's fetch takes the slab origin on trust — it is
     * a coordinate, and a coordinate cannot be out of range, only wrong. An instance packed for a
     * four-slab atlas and dispatched against a one-slab one reads the body at a quarter of the depth it
     * meant to and draws a cloud nobody sculpted, with no message anywhere. That includes the case this
     * subsystem has already been burnt by: the FALLBACK image, one texel across, bound because no atlas
     * was built. Against it the only bindable payload is an empty one, which is exactly what this says.
     *
     * @param payload          as the renderer built it.
     * @param atlasSlabCount   how many bodies the image about to be bound holds; 0 for the fallback.
     */
    inline bool CloudAuthoredPayloadIsBindable( const CloudAuthoredPayload& payload, uint32_t atlasSlabCount )
    {
        if ( payload.Count < 0 || static_cast<uint32_t>( payload.Count ) > kCloudAuthoredSlots )
            return false;
        if ( payload.SlabCount < 0 || static_cast<uint32_t>( payload.SlabCount ) > kCloudModellingAtlasMaxSlabs )
            return false;
        if ( static_cast<uint32_t>( payload.SlabCount ) != atlasSlabCount )
            return false;
        if ( payload.SlabCount == 0 )
            return payload.Count == 0;

        for ( int32_t i = 0; i < payload.Count; ++i )
        {
            const float origin = payload.Instances[i].BoundsMax.w;

            bool found = false;
            for ( int32_t slot = 0; slot < payload.SlabCount && !found; ++slot )
            {
                found = origin == CloudAuthoredAtlasSlabBaseW( static_cast<uint32_t>( slot ),
                                                               static_cast<uint32_t>( payload.SlabCount ) );
            }

            if ( !found )
                return false;
        }

        return true;
    }
} // namespace Desert::Graphic
