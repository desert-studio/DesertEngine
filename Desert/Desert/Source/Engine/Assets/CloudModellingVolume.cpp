#include "CloudModellingVolume.hpp"

#include <Engine/Assets/ContainerBytes.hpp>

#include <Common/Core/Math/Rounding.hpp>
#include <Common/Core/JobSystem.hpp>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <span>
#include <mutex>
#include <tuple>

namespace Desert::Assets
{
    namespace
    {
        // The one pixel format a volume may be in. Written into the header and CHECKED on read, so a file
        // from a future build that stored half-floats is refused by name instead of being read as bytes.
        constexpr uint32_t kFormatRgba8 = 0u;

        // How many lumps one body may be made of. The bake is `voxels x blobs` ellipsoid evaluations —
        // 1 048 576 x 64 is 67 million — so the ceiling is a bake time an artist will wait through rather
        // than an expressive limit, and Г10 bought the ceiling a lot of room by handing the z-slabs to the
        // pool: the shipped eight-lump body went from 1 383 ms to 206 ms, Debug, best of three. The shipped
        // example uses eight.
        constexpr uint32_t kMaxBlobs = 64u;

        // The weight's range, and it is a statement about DISTANCE rather than a taste. A weight dilates
        // its lump by `BlendRadiusKm * ln(Weight)` (see CloudModellingBlob::Weight), so these two bounds
        // are +/- ln(8) = 2.08 blend radii. Past that the lump has stopped being where its centre says it
        // is, and the honest edit is to move it.
        constexpr float kMinWeight = 0.125f;
        constexpr float kMaxWeight = 8.0f;

        // How closely two of a lump's radii must agree before a sphere or a capsule's cross-section counts
        // as round. One millimetre against bodies measured in hundreds of metres: loose enough that a
        // float round-trip through the container can never trip it, tight enough that a typo cannot pass.
        constexpr float kRadiusAgreementKm = 1e-6f;

        bool IsFinite( float value )
        {
            return std::isfinite( value );
        }

        bool IsFinite( const glm::vec3& value )
        {
            return IsFinite( value.x ) && IsFinite( value.y ) && IsFinite( value.z );
        }

        /// The lump's own frame. The engine's euler convention exactly — the construction
        /// TransformComponent::GetTransform uses — so that a lump's rotation and an entity's rotation are
        /// the same three numbers meaning the same three things.
        glm::mat3 BlobRotation( const glm::vec3& rotationDeg )
        {
            return glm::mat3( glm::quat( glm::radians( rotationDeg ) ) );
        }

        /**
         * The signed distance to an ellipsoid, in kilometres, negative inside.
         *
         * Inigo Quilez's bounded form. It is EXACT for a sphere — substitute r = R and it reduces to
         * `|p| - R` — and a tight underestimate otherwise, which is the safe direction here: an
         * underestimate makes the body slightly smaller than the bound Validate checked, never larger.
         *
         * The guard at the centre is not defensive padding: `k1` is zero exactly at p = 0 and the
         * expression is 0/0 there. The answer at the centre is the shortest semi-axis, which is what a
         * sphere's own formula gives and what the limit approaches along the short axis.
         */
        float EllipsoidDistanceKm( const glm::vec3& p, const glm::vec3& radii )
        {
            const glm::vec3 scaled  = p / radii;
            const glm::vec3 scaled2 = scaled / radii;

            const float k1 = glm::length( scaled2 );
            if ( k1 < 1e-9f )
                return -std::min( radii.x, std::min( radii.y, radii.z ) );

            const float k0 = glm::length( scaled );
            return k0 * ( k0 - 1.0f ) / k1;
        }

        /**
         * The signed distance to a sphere, in kilometres, negative inside.
         *
         * This is what the ellipsoid form above REDUCES TO when the three radii are equal — the header of
         * that function says so — and it is written out separately because the compiler cannot perform
         * that reduction: it cannot know at the call site that the radii agree. Evaluating it directly
         * saves two vector divides, a `length` and a division per lump per voxel, and it is EXACT where
         * the general form is a tight underestimate.
         */
        float SphereDistanceKm( const glm::vec3& p, float radiusKm )
        {
            return glm::length( p ) - radiusKm;
        }

        /**
         * The signed distance to a capsule lying along the lump's local Y, in kilometres, negative inside.
         *
         * Inigo Quilez's swept-sphere form, and it is EXACT rather than a bound: clamping the point's
         * height onto the segment gives the nearest point ON THE SEGMENT, and the distance to a
         * sphere-swept segment is the distance to that point less the radius. Exactness matters here for
         * the same reason it matters everywhere in this file — the joined field IS the Dimensional
         * Profile, so an inexact distance is a mis-shaped profile and not merely a mis-shaped bound.
         *
         * @param halfHeightKm the TOTAL half-height, caps included, so that `RadiiKm` is the lump's
         *        bounding half-extent for a capsule exactly as it is for the other two primitives. The
         *        segment is therefore shorter than the lump by one radius at each end.
         */
        float CapsuleDistanceKm( const glm::vec3& p, float radiusKm, float halfHeightKm )
        {
            // Non-negative because Validate refuses a capsule shorter than its own radius; at exactly zero
            // this is a sphere, which is the correct limit rather than a special case.
            const float segmentHalfKm = halfHeightKm - radiusKm;

            glm::vec3 q = p;
            q.y -= std::clamp( q.y, -segmentHalfKm, segmentHalfKm );
            return glm::length( q ) - radiusKm;
        }

        /// The signed distance to whichever solid the lump is, measured in the lump's OWN frame.
        float PrimitiveDistanceKm( CloudModellingPrimitive primitive, const glm::vec3& local,
                                   const glm::vec3& radiiKm )
        {
            switch ( primitive )
            {
                case CloudModellingPrimitive::Sphere:
                    return SphereDistanceKm( local, radiiKm.x );
                case CloudModellingPrimitive::Capsule:
                    return CapsuleDistanceKm( local, radiiKm.x, radiiKm.y );
                case CloudModellingPrimitive::Ellipsoid:
                    break;
            }
            return EllipsoidDistanceKm( local, radiiKm );
        }

        unsigned char Quantize( float unit )
        {
            return Common::Math::QuantiseUnitToByte( unit );
        }

        /**
         * The distance by which the exponential smooth minimum pushes the zero set outward when every
         * lump is equidistant. Exact rather than a safety factor: `-r*ln(SUM w_k * exp(-d/r))` is
         * `d - r*ln(SUM w_k)`.
         *
         * @param weightSum the sum of the lumps' weights, which is the lump COUNT when every weight is 1 —
         *        so this is A0's `r*ln(N)` generalised rather than replaced.
         *
         * Floored at zero. A weight sum below one shrinks the surface inward, and the true bound would
         * then be negative; demanding no less room than the lumps themselves occupy costs nothing and
         * keeps the box test monotone in a quantity an artist is allowed to lower.
         */
        float JoinInflationKm( float blendRadiusKm, float weightSum )
        {
            if ( weightSum <= 1.0f )
                return 0.0f;
            return blendRadiusKm * std::log( weightSum );
        }

        /**
         * @brief The recipe, put into the form the per-voxel loop wants, ONCE.
         *
         * WHY THIS TYPE EXISTS AT ALL: the full bake and the panel's slice preview must agree exactly, and
         * the reliable way to make two loops agree is to give them one body rather than two copies of a
         * formula. Everything that does not depend on the voxel — the canonical sort, the rotations
         * inverted, the reciprocals — is done here, and `EvaluateVoxel` is the only place a voxel's four
         * bytes are ever decided.
         *
         * Not thread-safe and not meant to be: it is const after construction and its only scratch is on
         * the stack, so it is safe to SHARE across threads and unsafe to mutate — which is the right way
         * round for a pure bake.
         */
        class BakedField
        {
        public:
            explicit BakedField( const CloudModellingVolumeRecipe& recipe )
                 : m_BlendRadiusKm( recipe.BlendRadiusKm ), m_EnvelopeMarginKm( recipe.EnvelopeMarginKm ),
                   m_InvBlend( 1.0f / recipe.BlendRadiusKm ), m_InvProfile( 1.0f / recipe.ProfileDepthKm ),
                   m_InvEnvelope( 1.0f / recipe.EnvelopeMarginKm )
            {
                // THE LUMPS ARE SORTED INTO A CANONICAL ORDER BEFORE ANYTHING IS SUMMED, and this is a
                // MEASURED correction rather than tidiness.
                //
                // The exponential smooth minimum is commutative and associative in REAL arithmetic, which
                // is the first of the three properties it was chosen for (PLAN_AUTHORED_CLOUDS.md section
                // 3) — but floating-point addition is neither, so `sum += exp(...)` over a shuffled list
                // produces a slightly different total. Measured on the shipped recipe by phase A0:
                // shuffling the eight lumps moved 6 bytes of 4 194 304, each by one 255th. Tiny, and still
                // the wrong shape of answer — a bake is a build artefact, and a build artefact whose bytes
                // depend on the order its inputs happened to be listed in cannot be compared, cached by
                // hash, or asserted equal.
                //
                // Sorting once, here, makes the summation order a function of the lumps themselves and not
                // of the sequence they arrived in. It costs one sort of at most 64 elements against 8.4
                // million exponentials.
                //
                // THE KEY COVERS EVERY AUTHORED NUMBER, including the three this phase added. That is not
                // optional bookkeeping: two lumps identical in A0's eight fields but differing in rotation
                // are DIFFERENT lumps, and a key that could not tell them apart would leave their relative
                // order decided by `std::sort`'s internals — which is the very non-determinism the sort is
                // here to remove.
                std::vector<CloudModellingBlob> sorted = recipe.Blobs;
                SortCloudModellingBlobs( sorted );

                m_Blobs.reserve( sorted.size() );
                for ( const CloudModellingBlob& blob : sorted )
                    m_Blobs.push_back( PrepareCloudModellingBlob( blob ) );
            }

            size_t BlobCount() const
            {
                return m_Blobs.size();
            }

            /**
             * @brief The four bytes at one point of the body. THE ONLY PLACE A VOXEL IS DECIDED.
             *
             * @param out four bytes, always written — including the all-zero case, so that the caller
             *        never has to remember to clear.
             */
            void EvaluateVoxel( const glm::vec3& point, unsigned char* out ) const
            {
                // On the stack and bounded by kMaxBlobs, which Validate enforces. A heap buffer here would
                // be one allocation per voxel or one piece of shared mutable state; a fixed 64 floats is
                // neither.
                float distances[kMaxBlobs];
                float weights[kMaxBlobs];

                const size_t count = m_Blobs.size();

                // Each lump's distance is computed ONCE and kept. The nearest of them is then the shift
                // below, and the same numbers feed the sum — where A0 evaluated every ellipsoid twice, for
                // the same answer at twice the cost.
                float nearest = 0.0f;
                for ( size_t k = 0; k < count; ++k )
                {
                    distances[k] = CloudModellingBlobDistanceKm( m_Blobs[k], point );
                    nearest      = ( k == 0 ) ? distances[0] : std::min( nearest, distances[k] );
                }

                // THE SHIFT IS WHAT KEEPS THIS FINITE, and it lives in CloudModellingJoinTerm now because
                // the procedural producer's bake performs the same arithmetic in a different loop order.
                float sum = 0.0f;
                for ( size_t k = 0; k < count; ++k )
                {
                    const float weight =
                         CloudModellingJoinTerm( m_Blobs[k].Weight, distances[k], nearest, m_InvBlend );
                    weights[k] = weight;
                    sum += weight;
                }

                // The exponential smooth minimum, shifted back. Commutative and associative in the
                // distances, so the order of the lumps cannot reach the answer.
                const float joined = CloudModellingJoinKm( nearest, sum, m_BlendRadiusKm );

                // Everything outside the body is four zeroes — including the envelope, past its own margin
                // — so the early-out in the march is a single comparison and the empty parts of a hero
                // cloud's box cost exactly what empty sky costs.
                if ( joined >= m_EnvelopeMarginKm )
                {
                    out[0] = 0u;
                    out[1] = 0u;
                    out[2] = 0u;
                    out[3] = 0u;
                    return;
                }

                // THE TWO MATERIAL CHANNELS ARE THE JOIN'S OWN WEIGHTS, not a second construction.
                // `weights` is already the softmax of the negated distances, so dividing by its sum gives a
                // partition of unity over the lumps: a point deep inside one lump takes that lump's
                // numbers, and a point in the crease between two takes their blend, with the blend's width
                // being the same blend radius that fused the shapes. That is the second of the three
                // properties the exponential smooth-min was chosen for.
                const float invSum       = 1.0f / sum;
                float       detailType   = 0.0f;
                float       densityScale = 0.0f;
                for ( size_t k = 0; k < count; ++k )
                {
                    const float share = weights[k] * invSum;
                    detailType += share * m_Blobs[k].DetailType;
                    densityScale += share * m_Blobs[k].DensityScale;
                }

                // 0 at the surface and 1 at ProfileDepth inside, which is Guerrilla's Dimensional Profile
                // (deck p.85) obtained analytically instead of by a distance transform.
                const float profile = std::clamp( -joined * m_InvProfile, 0.0f, 1.0f );

                // 1 throughout the body and falling to 0 at the margin outside it. Above zero wherever the
                // profile is, and past it, which is what makes it conservative.
                const float envelope = std::clamp( ( m_EnvelopeMarginKm - joined ) * m_InvEnvelope, 0.0f, 1.0f );

                out[0] = Quantize( profile );
                out[1] = Quantize( detailType );
                out[2] = Quantize( densityScale );
                out[3] = Quantize( envelope );
            }

        private:
            std::vector<CloudModellingPreparedBlob> m_Blobs;

            float m_BlendRadiusKm    = 0.0f;
            float m_EnvelopeMarginKm = 0.0f;
            float m_InvBlend         = 0.0f;
            float m_InvProfile       = 0.0f;
            float m_InvEnvelope      = 0.0f;
        };

        /// Where the centre of voxel (@p x, @p y, @p z) sits, kilometres, relative to the box's centre.
        /// One function so the full bake and a single-plane preview cannot disagree about WHERE a voxel is
        /// — the "two statements of one fact" defect class of contract §2.3.1, closed by construction.
        glm::vec3 VoxelCentreKm( const CloudModellingVolumeRecipe& recipe, uint32_t x, uint32_t y, uint32_t z )
        {
            const glm::vec3 half = recipe.SizeKm * 0.5f;
            return glm::vec3(
                 ( ( static_cast<float>( x ) + 0.5f ) / static_cast<float>( kCloudModellingVolumeWidth ) ) *
                           recipe.SizeKm.x -
                      half.x,
                 ( ( static_cast<float>( y ) + 0.5f ) / static_cast<float>( kCloudModellingVolumeHeight ) ) *
                           recipe.SizeKm.y -
                      half.y,
                 ( ( static_cast<float>( z ) + 0.5f ) / static_cast<float>( kCloudModellingVolumeDepth ) ) *
                           recipe.SizeKm.z -
                      half.z );
        }

    } // namespace

    CloudModellingPreparedBlob PrepareCloudModellingBlob( const CloudModellingBlob& blob )
    {
        CloudModellingPreparedBlob prepared;
        prepared.CentreKm     = blob.CentreKm;
        prepared.RadiiKm      = blob.RadiiKm;
        prepared.Primitive    = blob.Primitive;
        prepared.Weight       = blob.Weight;
        prepared.DetailType   = blob.DetailType;
        prepared.DensityScale = blob.DensityScale;

        // The INVERSE rotation, because the point travels into the lump's frame rather than the lump into
        // the world's. A rotation matrix is orthonormal, so its inverse is its transpose — exact, and free
        // of the numerical drift a general inverse would add.
        prepared.IntoLocal = glm::transpose( BlobRotation( blob.RotationDeg ) );
        return prepared;
    }

    float CloudModellingBlobDistanceKm( const CloudModellingPreparedBlob& blob, const glm::vec3& pointKm )
    {
        return PrimitiveDistanceKm( blob.Primitive, blob.IntoLocal * ( pointKm - blob.CentreKm ), blob.RadiiKm );
    }

    glm::vec3 CloudModellingBlobHalfExtentKm( const CloudModellingBlob& blob )
    {
        const glm::mat3 rotation = BlobRotation( blob.RotationDeg );

        glm::mat3 magnitude( 0.0f );
        for ( int column = 0; column < 3; ++column )
        {
            for ( int row = 0; row < 3; ++row )
                magnitude[column][row] = std::abs( rotation[column][row] );
        }

        return magnitude * blob.RadiiKm;
    }

    void SortCloudModellingBlobs( std::vector<CloudModellingBlob>& blobs )
    {
        std::sort( blobs.begin(), blobs.end(),
                   []( const CloudModellingBlob& a, const CloudModellingBlob& b )
                   {
                       const auto key = []( const CloudModellingBlob& blob )
                       {
                           return std::tie( blob.CentreKm.x, blob.CentreKm.y, blob.CentreKm.z, blob.RadiiKm.x,
                                            blob.RadiiKm.y, blob.RadiiKm.z, blob.DetailType, blob.DensityScale,
                                            blob.RotationDeg.x, blob.RotationDeg.y, blob.RotationDeg.z,
                                            blob.Primitive, blob.Weight );
                       };
                       return key( a ) < key( b );
                   } );
    }

    const char* CloudModellingChannelName( CloudModellingChannel channel )
    {
        switch ( channel )
        {
            case CloudModellingChannel::DimensionalProfile:
                return "Dimensional Profile (depth inside the body)";
            case CloudModellingChannel::DetailType:
                return "Detail Type (0 wispy, 1 billowy)";
            case CloudModellingChannel::DensityScale:
                return "Density Scale (per-voxel multiplier)";
            case CloudModellingChannel::CutoutEnvelope:
                return "Cutout Envelope (the body, dilated)";
        }
        return "unknown";
    }

    const char* CloudModellingPrimitiveName( CloudModellingPrimitive primitive )
    {
        switch ( primitive )
        {
            case CloudModellingPrimitive::Ellipsoid:
                return "Ellipsoid";
            case CloudModellingPrimitive::Sphere:
                return "Sphere";
            case CloudModellingPrimitive::Capsule:
                return "Capsule";
        }
        return "unknown";
    }

    const char* CloudModellingAxisName( CloudModellingAxis axis )
    {
        switch ( axis )
        {
            case CloudModellingAxis::X:
                return "X (looking east along the body)";
            case CloudModellingAxis::Y:
                return "Y (the horizontal cut, looking down)";
            case CloudModellingAxis::Z:
                return "Z (looking north along the body)";
        }
        return "unknown";
    }

    uint32_t CloudModellingAxisExtent( CloudModellingAxis axis )
    {
        switch ( axis )
        {
            case CloudModellingAxis::X:
                return kCloudModellingVolumeWidth;
            case CloudModellingAxis::Y:
                return kCloudModellingVolumeHeight;
            case CloudModellingAxis::Z:
                return kCloudModellingVolumeDepth;
        }
        return 0u;
    }

    Common::BoolResultStr ValidateCloudModellingRecipe( const CloudModellingVolumeRecipe& recipe )
    {
        if ( !IsFinite( recipe.SizeKm ) || recipe.SizeKm.x <= 0.0f || recipe.SizeKm.y <= 0.0f ||
             recipe.SizeKm.z <= 0.0f )
            return Common::MakeFormattedError<bool>( "Size must be positive on every axis, got {} x {} x {} km",
                                                     recipe.SizeKm.x, recipe.SizeKm.y, recipe.SizeKm.z );

        if ( !IsFinite( recipe.BlendRadiusKm ) || recipe.BlendRadiusKm <= 0.0f )
            return Common::MakeFormattedError<bool>(
                 "Blend Radius must be strictly positive — it is the reciprocal of the join's sharpness and "
                 "zero is a division — got {} km",
                 recipe.BlendRadiusKm );

        if ( !IsFinite( recipe.ProfileDepthKm ) || recipe.ProfileDepthKm <= 0.0f )
            return Common::MakeFormattedError<bool>( "Profile Depth must be strictly positive, got {} km",
                                                     recipe.ProfileDepthKm );

        if ( !IsFinite( recipe.EnvelopeMarginKm ) || recipe.EnvelopeMarginKm <= 0.0f )
            return Common::MakeFormattedError<bool>( "Envelope Margin must be strictly positive, got {} km",
                                                     recipe.EnvelopeMarginKm );

        if ( recipe.Blobs.empty() )
            return Common::MakeError<bool>( "a modelling volume with no lumps in it is an empty box, not a "
                                            "cloud; add at least one" );

        if ( recipe.Blobs.size() > kMaxBlobs )
            return Common::MakeFormattedError<bool>( "{} lumps is past the ceiling of {}", recipe.Blobs.size(),
                                                     kMaxBlobs );

        for ( size_t i = 0; i < recipe.Blobs.size(); ++i )
        {
            const CloudModellingBlob& blob = recipe.Blobs[i];

            if ( !IsFinite( blob.CentreKm ) )
                return Common::MakeFormattedError<bool>( "lump {} has a centre that is not a finite number", i );

            if ( !IsFinite( blob.RadiiKm ) || blob.RadiiKm.x <= 0.0f || blob.RadiiKm.y <= 0.0f ||
                 blob.RadiiKm.z <= 0.0f )
                return Common::MakeFormattedError<bool>(
                     "lump {} has radii {} x {} x {} km; every semi-axis must be strictly positive, because a "
                     "zero axis divides by zero in the distance function",
                     i, blob.RadiiKm.x, blob.RadiiKm.y, blob.RadiiKm.z );

            if ( !IsFinite( blob.DetailType ) || blob.DetailType < 0.0f || blob.DetailType > 1.0f )
                return Common::MakeFormattedError<bool>( "lump {} has Detail Type {}, outside [0, 1]", i,
                                                         blob.DetailType );

            if ( !IsFinite( blob.DensityScale ) || blob.DensityScale < 0.0f || blob.DensityScale > 1.0f )
                return Common::MakeFormattedError<bool>( "lump {} has Density Scale {}, outside [0, 1]", i,
                                                         blob.DensityScale );

            if ( !IsFinite( blob.RotationDeg ) )
                return Common::MakeFormattedError<bool>( "lump {} has a rotation that is not a finite number", i );

            if ( !IsFinite( blob.Weight ) || blob.Weight < kMinWeight || blob.Weight > kMaxWeight )
                return Common::MakeFormattedError<bool>(
                     "lump {} has weight {}, outside [{}, {}]. The weight dilates the lump by "
                     "BlendRadius * ln(weight); past these bounds that is more than two blend radii and "
                     "moving the lump is the honest edit",
                     i, blob.Weight, kMinWeight, kMaxWeight );

            // THE PRIMITIVE'S CONSTRAINT ON ITS OWN RADII. `RadiiKm` is the lump's bounding half-extent for
            // every primitive (CloudModellingBlob), which is what keeps the box arithmetic below one
            // formula instead of three — but a sphere and a capsule do not USE all three numbers freely,
            // and a stored number that the maths ignores is exactly the dead data the contract forbids.
            // Refusing here makes every component of every lump's radii load-bearing.
            switch ( blob.Primitive )
            {
                case CloudModellingPrimitive::Ellipsoid:
                    break;

                case CloudModellingPrimitive::Sphere:
                    if ( std::abs( blob.RadiiKm.x - blob.RadiiKm.y ) > kRadiusAgreementKm ||
                         std::abs( blob.RadiiKm.x - blob.RadiiKm.z ) > kRadiusAgreementKm )
                        return Common::MakeFormattedError<bool>(
                             "lump {} is a Sphere but its radii are {} x {} x {} km; a sphere has one "
                             "radius. Make them equal, or make the lump an Ellipsoid",
                             i, blob.RadiiKm.x, blob.RadiiKm.y, blob.RadiiKm.z );
                    break;

                case CloudModellingPrimitive::Capsule:
                    if ( std::abs( blob.RadiiKm.x - blob.RadiiKm.z ) > kRadiusAgreementKm )
                        return Common::MakeFormattedError<bool>(
                             "lump {} is a Capsule but its cross-section is {} x {} km; a capsule is a "
                             "swept SPHERE, so the two axes across its length must agree",
                             i, blob.RadiiKm.x, blob.RadiiKm.z );

                    if ( blob.RadiiKm.y < blob.RadiiKm.x )
                        return Common::MakeFormattedError<bool>(
                             "lump {} is a Capsule of half-height {} km and radius {} km. The half-height "
                             "is the TOTAL one, caps included, so it cannot be less than the radius — a "
                             "capsule that short is a sphere, and saying so is the honest edit",
                             i, blob.RadiiKm.y, blob.RadiiKm.x );
                    break;

                default:
                    return Common::MakeFormattedError<bool>(
                         "lump {} declares primitive {}, which this build does not know", i,
                         static_cast<uint32_t>( blob.Primitive ) );
            }
        }

        // THE BODY MUST NOT REACH ITS OWN BOX, and the slack is the join's inflation plus the envelope's
        // dilation. Both are real distances the bake adds to the lumps' own extents, so checking the
        // lumps alone would pass a recipe whose cloud is cut flat by the volume's face — and, every
        // sampler in this engine being REPEAT, whose top would then blend with its own bottom.
        //
        // THE INFLATION IS DRIVEN BY THE SUM OF THE WEIGHTS AND NOT BY THE COUNT. They are the same number
        // whenever every weight is 1, which is A0's case; they part company the moment an artist turns a
        // weight up, and it is exactly then that the surface moves outward. Counting lumps instead would
        // under-reserve by `BlendRadius * ln(weightSum / N)` — and the failure that hides behind an
        // under-reserved box is a cloud with a flat face, which reads as a modelling mistake rather than
        // as a bound that was computed from the wrong quantity.
        float weightSum = 0.0f;
        for ( const CloudModellingBlob& blob : recipe.Blobs )
            weightSum += blob.Weight;

        const float inflationKm = JoinInflationKm( recipe.BlendRadiusKm, weightSum );
        const float slackKm     = inflationKm + recipe.EnvelopeMarginKm;

        glm::vec3 reach{ 0.0f };
        for ( const CloudModellingBlob& blob : recipe.Blobs )
        {
            // The ROTATED half-extent, so a capsule laid on its side is measured along the axis it
            // actually occupies. For an unrotated lump this is its radii unchanged.
            const glm::vec3 extent = CloudModellingBlobHalfExtentKm( blob );

            reach.x = std::max( reach.x, std::abs( blob.CentreKm.x ) + extent.x );
            reach.y = std::max( reach.y, std::abs( blob.CentreKm.y ) + extent.y );
            reach.z = std::max( reach.z, std::abs( blob.CentreKm.z ) + extent.z );
        }

        const glm::vec3 needed = reach + glm::vec3( slackKm );
        const glm::vec3 half   = recipe.SizeKm * 0.5f;

        if ( needed.x > half.x || needed.y > half.y || needed.z > half.z )
            return Common::MakeFormattedError<bool>(
                 "the body reaches {:.3f} x {:.3f} x {:.3f} km from the centre once the join's inflation "
                 "({:.3f} km over {} lumps of total weight {:.2f}) and the envelope's margin ({:.3f} km) are "
                 "added, which does not fit inside the half-size {:.3f} x {:.3f} x {:.3f} km. Either grow Size "
                 "or move the lumps in.",
                 needed.x, needed.y, needed.z, inflationKm, recipe.Blobs.size(), weightSum,
                 recipe.EnvelopeMarginKm, half.x, half.y, half.z );

        return Common::MakeSuccess( true );
    }

    Common::ResultStr<std::vector<unsigned char>>
    AssembleCloudModellingAtlas( const std::vector<const std::vector<unsigned char>*>& bodies )
    {
        if ( bodies.empty() )
            return Common::MakeError<std::vector<unsigned char>>(
                 "an atlas of no bodies is a volume of zero depth, which no device will create — bind the "
                 "fallback image instead" );

        std::vector<unsigned char> atlas;
        atlas.reserve( bodies.size() * static_cast<size_t>( kCloudModellingVoxelBytes ) );

        for ( size_t slot = 0; slot < bodies.size(); ++slot )
        {
            const std::vector<unsigned char>* body = bodies[slot];

            if ( body == nullptr )
                return Common::MakeFormattedError<std::vector<unsigned char>>( "slab {} has no body", slot );

            if ( body->size() != static_cast<size_t>( kCloudModellingVoxelBytes ) )
                return Common::MakeFormattedError<std::vector<unsigned char>>(
                     "slab {} is {} bytes where a modelling volume is {}", slot, body->size(),
                     kCloudModellingVoxelBytes );

            // The concatenation IS the stacking, because z varies slowest in the layout above.
            atlas.insert( atlas.end(), body->begin(), body->end() );
        }

        return Common::MakeSuccess( std::move( atlas ) );
    }

    Common::ResultStr<std::vector<unsigned char>>
    GenerateCloudModellingVolume( const CloudModellingVolumeRecipe& recipe )
    {
        return GenerateCloudModellingVolume( recipe, {} );
    }

    Common::ResultStr<std::vector<unsigned char>>
    GenerateCloudModellingVolume( const CloudModellingVolumeRecipe&   recipe,
                                  const CloudModellingBakeProgressFn& onProgress )
    {
        if ( auto valid = ValidateCloudModellingRecipe( recipe ); !valid )
            return Common::MakeFormattedError<std::vector<unsigned char>>( "recipe is not usable: {}",
                                                                           valid.GetError() );

        const uint32_t width  = kCloudModellingVolumeWidth;
        const uint32_t height = kCloudModellingVolumeHeight;
        const uint32_t depth  = kCloudModellingVolumeDepth;

        const glm::vec3 voxelSizeKm =
             recipe.SizeKm /
             glm::vec3( static_cast<float>( width ), static_cast<float>( height ), static_cast<float>( depth ) );

        // The canonical sort, the inverted rotations and the reciprocals all happen here, once.
        const BakedField field( recipe );

        std::vector<unsigned char> voxels( static_cast<size_t>( kCloudModellingVoxelBytes ), 0u );

        // ONE SLAB PER CLAIM, ON THE ENGINE'S POOL. Slab z owns the bytes
        // [z*height*width*4, (z+1)*height*width*4) and no other slab touches them, so the only thing the
        // participants share is the read-only BakedField — which the note on that class already promised
        // was safe to share and until now nothing did. `field` is the reason this is a small change rather
        // than a rewrite: the sort, the inverted rotations and the reciprocals happened once, above, and
        // EvaluateVoxel's only scratch is its own stack.
        //
        // THE PROGRESS COUNTER IS NOT `z` ANY MORE, and it cannot be. Slabs finish out of order, so
        // reporting z/depth would hand the artist's bar a sequence that goes backwards — which
        // CloudModellingRecipe asserts against by name. A counter advanced under the same lock that guards
        // the callback keeps the reported sequence exactly what it was: 0, 1/depth, 2/depth, ... The lock
        // also makes the STOP single: the first callback to answer no sets the flag, and every later slab
        // tests it under the same lock and never reaches the callback, so "cancelled after exactly three
        // calls" stays a true sentence about a bake running on ten threads.
        std::mutex progressMutex;
        uint32_t   slabsStarted = 0u;
        bool       cancelled    = false;

        Common::JobSystem::Get().ParallelRanges(
             depth, 1u,
             [&]( size_t zBegin, size_t zEnd )
             {
                 for ( uint32_t z = static_cast<uint32_t>( zBegin ); z < static_cast<uint32_t>( zEnd ); ++z )
                 {
                     // BETWEEN SLABS AND NOT INSIDE THEM. 128 calls over a bake of hundreds of milliseconds
                     // is a progress bar that moves smoothly and costs nothing measurable; per voxel it
                     // would be a million indirect calls through a std::function and would dominate the
                     // arithmetic it is reporting on.
                     if ( onProgress )
                     {
                         std::lock_guard<std::mutex> lk( progressMutex );
                         if ( cancelled )
                             return;
                         if ( !onProgress( static_cast<float>( slabsStarted ) / static_cast<float>( depth ) ) )
                         {
                             cancelled = true;
                             return;
                         }
                         ++slabsStarted;
                     }

                     for ( uint32_t y = 0; y < height; ++y )
                     {
                         for ( uint32_t x = 0; x < width; ++x )
                         {
                             const size_t index = ( ( static_cast<size_t>( z ) * height + y ) * width + x ) *
                                                  kCloudModellingBytesPerVoxel;

                             field.EvaluateVoxel( VoxelCentreKm( recipe, x, y, z ), voxels.data() + index );
                         }
                     }
                 }
             } );

        // After the loop, because ParallelRanges returns only once every claimed slab has finished — which
        // is the first moment "somebody said stop" is settled rather than still being decided.
        if ( cancelled )
            return Common::MakeError<std::vector<unsigned char>>( "the bake was cancelled before it finished" );

        // THE BOUND WAS ARITHMETIC; THIS IS THE MEASUREMENT. Validate's slack is a conservative estimate
        // of the join's inflation, and a conservative estimate is exactly the kind of thing that is right
        // until somebody changes the distance function. Walking the six faces costs 0.6 % of the bake and
        // turns "the body should fit" into "the body does fit".
        const auto faceIsEmpty = [&]( uint32_t x, uint32_t y, uint32_t z )
        {
            const size_t index =
                 ( ( static_cast<size_t>( z ) * height + y ) * width + x ) * kCloudModellingBytesPerVoxel;
            return voxels[index + 0] == 0u && voxels[index + 3] == 0u;
        };

        for ( uint32_t z = 0; z < depth; ++z )
        {
            for ( uint32_t y = 0; y < height; ++y )
            {
                for ( uint32_t x = 0; x < width; ++x )
                {
                    const bool onFace =
                         ( x == 0 || x == width - 1 || y == 0 || y == height - 1 || z == 0 || z == depth - 1 );
                    if ( !onFace || faceIsEmpty( x, y, z ) )
                        continue;

                    return Common::MakeFormattedError<std::vector<unsigned char>>(
                         "the baked body touches the volume's boundary at voxel ({}, {}, {}) — the cloud would "
                         "be cut flat there, and a REPEAT sampler would blend that face with the opposite one. "
                         "Grow Size or move the lumps in; the voxel is {:.1f} x {:.1f} x {:.1f} m",
                         x, y, z, voxelSizeKm.x * 1000.0f, voxelSizeKm.y * 1000.0f, voxelSizeKm.z * 1000.0f );
                }
            }
        }

        // Reported only once the boundary check has passed, so "100 %" and "there is a volume" are the same
        // event rather than two the caller has to reconcile.
        if ( onProgress )
            onProgress( 1.0f );

        return Common::MakeSuccess( std::move( voxels ) );
    }

    Common::ResultStr<CloudModellingSlice> GenerateCloudModellingSlice( const CloudModellingVolumeRecipe& recipe,
                                                                        CloudModellingAxis axis, uint32_t index )
    {
        if ( auto valid = ValidateCloudModellingRecipe( recipe ); !valid )
            return Common::MakeFormattedError<CloudModellingSlice>( "recipe is not usable: {}", valid.GetError() );

        const uint32_t extent = CloudModellingAxisExtent( axis );
        if ( extent == 0u )
            return Common::MakeFormattedError<CloudModellingSlice>( "axis {} is not one this build knows",
                                                                    static_cast<uint32_t>( axis ) );

        if ( index >= extent )
            return Common::MakeFormattedError<CloudModellingSlice>(
                 "slice {} is outside the volume: axis {} is {} voxels deep", index,
                 CloudModellingAxisName( axis ), extent );

        CloudModellingSlice slice;
        switch ( axis )
        {
            case CloudModellingAxis::X:
                slice.Width  = kCloudModellingVolumeDepth;
                slice.Height = kCloudModellingVolumeHeight;
                break;
            case CloudModellingAxis::Y:
                slice.Width  = kCloudModellingVolumeWidth;
                slice.Height = kCloudModellingVolumeDepth;
                break;
            case CloudModellingAxis::Z:
                slice.Width  = kCloudModellingVolumeWidth;
                slice.Height = kCloudModellingVolumeHeight;
                break;
        }

        slice.Pixels.assign( static_cast<size_t>( slice.Width ) * slice.Height * kCloudModellingBytesPerVoxel,
                             0u );

        const BakedField field( recipe );

        for ( uint32_t v = 0; v < slice.Height; ++v )
        {
            for ( uint32_t u = 0; u < slice.Width; ++u )
            {
                // The one place the plane's two axes are named. Deliberately a mapping into (x, y, z)
                // rather than a second addressing scheme, so the voxel a pixel shows is found by the same
                // VoxelCentreKm the full bake uses.
                uint32_t x = 0;
                uint32_t y = 0;
                uint32_t z = 0;
                switch ( axis )
                {
                    case CloudModellingAxis::X:
                        x = index;
                        z = u;
                        y = v;
                        break;
                    case CloudModellingAxis::Y:
                        y = index;
                        x = u;
                        z = v;
                        break;
                    case CloudModellingAxis::Z:
                        z = index;
                        x = u;
                        y = v;
                        break;
                }

                const size_t at = ( static_cast<size_t>( v ) * slice.Width + u ) * kCloudModellingBytesPerVoxel;

                field.EvaluateVoxel( VoxelCentreKm( recipe, x, y, z ), slice.Pixels.data() + at );
            }
        }

        return Common::MakeSuccess( std::move( slice ) );
    }

    std::vector<unsigned char> EncodeCloudModellingPayload( const CloudModellingVolumeData& data )
    {
        std::vector<unsigned char> blobBytes;
        blobBytes.reserve( data.Recipe.Blobs.size() * kCloudModellingBlobBytes );
        for ( const CloudModellingBlob& blob : data.Recipe.Blobs )
        {
            WriteF32( blobBytes, blob.CentreKm.x );
            WriteF32( blobBytes, blob.CentreKm.y );
            WriteF32( blobBytes, blob.CentreKm.z );
            WriteF32( blobBytes, blob.RadiiKm.x );
            WriteF32( blobBytes, blob.RadiiKm.y );
            WriteF32( blobBytes, blob.RadiiKm.z );
            WriteF32( blobBytes, blob.DetailType );
            WriteF32( blobBytes, blob.DensityScale );
            WriteF32( blobBytes, blob.RotationDeg.x );
            WriteF32( blobBytes, blob.RotationDeg.y );
            WriteF32( blobBytes, blob.RotationDeg.z );
            WriteU32( blobBytes, static_cast<uint32_t>( blob.Primitive ) );
            WriteF32( blobBytes, blob.Weight );
        }

        std::vector<unsigned char> out;
        out.reserve( kCloudModellingHeaderSize + blobBytes.size() + data.Voxels.size() );

        WriteU32( out, data.GeneratorVersion );
        WriteU32( out, kCloudModellingVolumeWidth );
        WriteU32( out, kCloudModellingVolumeHeight );
        WriteU32( out, kCloudModellingVolumeDepth );
        WriteU32( out, kFormatRgba8 );

        // The channel meanings are STORED, not implied. A reader that finds an order it does not know can
        // say so; a reader that assumed the order would render the density scale as a silhouette and look
        // merely wrong.
        WriteU32( out, static_cast<uint32_t>( CloudModellingChannel::DimensionalProfile ) );
        WriteU32( out, static_cast<uint32_t>( CloudModellingChannel::DetailType ) );
        WriteU32( out, static_cast<uint32_t>( CloudModellingChannel::DensityScale ) );
        WriteU32( out, static_cast<uint32_t>( CloudModellingChannel::CutoutEnvelope ) );

        WriteF32( out, data.Recipe.SizeKm.x );
        WriteF32( out, data.Recipe.SizeKm.y );
        WriteF32( out, data.Recipe.SizeKm.z );
        WriteF32( out, data.Recipe.BlendRadiusKm );
        WriteF32( out, data.Recipe.ProfileDepthKm );
        WriteF32( out, data.Recipe.EnvelopeMarginKm );
        WriteU32( out, static_cast<uint32_t>( data.Recipe.Blobs.size() ) );

        WriteU64( out, static_cast<uint64_t>( data.Voxels.size() ) );
        WriteU32( out, Crc32( data.Voxels.data(), data.Voxels.size() ) );
        WriteU32( out, Crc32( blobBytes.data(), blobBytes.size() ) );

        out.insert( out.end(), blobBytes.begin(), blobBytes.end() );
        out.insert( out.end(), data.Voxels.begin(), data.Voxels.end() );
        return out;
    }

    Common::ResultStr<std::vector<unsigned char>>
    EncodeCloudModellingVolume( const CloudModellingVolumeData& data )
    {
        namespace CC                             = Common::Content;
        const std::vector<unsigned char> payload = EncodeCloudModellingPayload( data );

        CC::AssetEnvelope envelope;
        envelope.Asset.Kind       = CC::ContentKind::CloudModellingVolume;
        envelope.Asset.Guid       = data.Guid.IsNull() ? CC::AssetGuid::Generate() : data.Guid;
        envelope.Asset.Subsystems = { { kCloudModellingSubsystemTag, kCloudModellingContainerVersion } };
        const std::span<const std::byte> payloadBytes = std::as_bytes( std::span( payload ) );
        envelope.Sections.push_back( { CC::EnvelopeSection::Payload, CC::EnvelopeCodec::Stored,
                                       std::vector<std::byte>( payloadBytes.begin(), payloadBytes.end() ) } );

        auto file = CC::WriteAssetEnvelope( envelope );
        if ( !file )
            return Common::MakeFormattedError<std::vector<unsigned char>>( "cannot wrap modelling volume: {}",
                                                                           file.GetError() );
        const std::vector<std::byte>& wrapped = file.GetValue();
        std::vector<unsigned char>    out( wrapped.size() );
        std::memcpy( out.data(), wrapped.data(), wrapped.size() );
        return Common::MakeSuccess( std::move( out ) );
    }

    Common::ResultStr<CloudModellingVolumeData>
    DecodeCloudModellingVolume( const std::vector<unsigned char>& file )
    {
        namespace CC = Common::Content;

        // A BARE CONTAINER IS REFUSED BY NAME, never read: it has no GUID, so reading it would hand the
        // volume a path-derived handle nothing else agrees with. The migrator wraps it once, in the file.
        if ( file.size() >= 8u &&
             std::memcmp( file.data(), kCloudModellingMagic, sizeof( kCloudModellingMagic ) ) == 0 )
            return Common::MakeFormattedError<CloudModellingVolumeData>(
                 "a bare 'DCMV' container version {} (no asset envelope, no GUID); this build reads version {} "
                 "inside the envelope - run Tools/SceneMigrator --write over the file",
                 ReadU32( file.data() + 4 ), kCloudModellingContainerVersion );

        const CC::SubsystemVersion kKnown[] = { { kCloudModellingSubsystemTag, kCloudModellingContainerVersion } };
        auto                       envelope =
             CC::ReadAssetEnvelope( std::as_bytes( std::span( file ) ), CC::AssetHeaderReadContext{ kKnown } );
        if ( !envelope )
            return Common::MakeFormattedError<CloudModellingVolumeData>(
                 "not a cloud modelling volume envelope: {}", envelope.GetError() );
        const CC::AssetEnvelope& e = envelope.GetValue();
        if ( e.Asset.Kind != CC::ContentKind::CloudModellingVolume )
            return Common::MakeFormattedError<CloudModellingVolumeData>(
                 "the envelope's kind is {}, not CloudModellingVolume", CC::KindName( e.Asset.Kind ) );
        if ( e.Asset.Subsystems.size() != 1u || e.Asset.Subsystems[0].Tag != kCloudModellingSubsystemTag )
            return Common::MakeFormattedError<CloudModellingVolumeData>(
                 "the envelope states {} subsystem versions; a modelling volume states exactly one, under 'DCMV'",
                 e.Asset.Subsystems.size() );
        const auto section = std::find_if( e.Sections.begin(), e.Sections.end(),
                                           []( const auto& s ) { return s.Tag == CC::EnvelopeSection::Payload; } );
        if ( section == e.Sections.end() )
            return Common::MakeFormattedError<CloudModellingVolumeData>(
                 "the envelope has no {} section",
                 CC::FourCCToString( static_cast<uint32_t>( CC::EnvelopeSection::Payload ) ) );

        std::vector<unsigned char> payload( section->Bytes.size() );
        std::memcpy( payload.data(), section->Bytes.data(), section->Bytes.size() );
        auto decoded = DecodeCloudModellingPayload( payload );
        if ( !decoded )
            return decoded;
        CloudModellingVolumeData data = decoded.ExtractValue();
        data.Guid                     = e.Asset.Guid;
        return Common::MakeSuccess( std::move( data ) );
    }

    Common::ResultStr<Common::Content::AssetGuid> ReadCloudModellingVolumeGuid( std::istream& in )
    {
        namespace CC                        = Common::Content;
        const CC::SubsystemVersion kKnown[] = { { kCloudModellingSubsystemTag, kCloudModellingContainerVersion } };
        const CC::AssetHeaderReadContext context{ kKnown };

        auto header = CC::ReadEnvelopeHeader( in, context );
        if ( !header )
            return Common::MakeFormattedError<CC::AssetGuid>( "not a cloud modelling volume envelope: {}",
                                                              header.GetError() );
        if ( header.GetValue().Asset.Kind != CC::ContentKind::CloudModellingVolume )
            return Common::MakeFormattedError<CC::AssetGuid>(
                 "the envelope's kind is {}, not CloudModellingVolume",
                 CC::KindName( header.GetValue().Asset.Kind ) );
        return Common::MakeSuccess( header.GetValue().Asset.Guid );
    }

    Common::ResultStr<CloudModellingVolumeData>
    DecodeCloudModellingPayload( const std::vector<unsigned char>& bytes )
    {
        if ( bytes.size() < kCloudModellingHeaderSize )
            return Common::MakeFormattedError<CloudModellingVolumeData>(
                 "payload is {} bytes, shorter than the {}-byte header", bytes.size(), kCloudModellingHeaderSize );

        const unsigned char* at = bytes.data();

        CloudModellingVolumeData data;
        data.GeneratorVersion = ReadU32( at + 0 );

        const uint32_t width  = ReadU32( at + 4 );
        const uint32_t height = ReadU32( at + 8 );
        const uint32_t depth  = ReadU32( at + 12 );
        const uint32_t format = ReadU32( at + 16 );

        if ( width != kCloudModellingVolumeWidth || height != kCloudModellingVolumeHeight ||
             depth != kCloudModellingVolumeDepth )
            return Common::MakeFormattedError<CloudModellingVolumeData>(
                 "volume is {}x{}x{} voxels; this build reads {}x{}x{}, which the format fixes so the shader "
                 "can clamp a fetch by half a texel at compile time",
                 width, height, depth, kCloudModellingVolumeWidth, kCloudModellingVolumeHeight,
                 kCloudModellingVolumeDepth );

        if ( format != kFormatRgba8 )
            return Common::MakeFormattedError<CloudModellingVolumeData>(
                 "pixel format {} is not RGBA8 ({}), which is the only format a volume may be in", format,
                 kFormatRgba8 );

        for ( uint32_t channel = 0; channel < 4u; ++channel )
        {
            const uint32_t stored = ReadU32( at + 20u + static_cast<size_t>( channel ) * 4u );
            if ( stored != channel )
                return Common::MakeFormattedError<CloudModellingVolumeData>(
                     "channel {} declares meaning {}, but this build reads volumes whose channels are "
                     "Dimensional Profile / Detail Type / Density Scale / Cutout Envelope in that order",
                     channel, stored );
        }

        data.Recipe.SizeKm           = glm::vec3( ReadF32( at + 36 ), ReadF32( at + 40 ), ReadF32( at + 44 ) );
        data.Recipe.BlendRadiusKm    = ReadF32( at + 48 );
        data.Recipe.ProfileDepthKm   = ReadF32( at + 52 );
        data.Recipe.EnvelopeMarginKm = ReadF32( at + 56 );

        const uint32_t blobCount   = ReadU32( at + 60 );
        const uint64_t voxelBytes  = ReadU64( at + 64 );
        const uint32_t storedVoxel = ReadU32( at + 72 );
        const uint32_t storedBlob  = ReadU32( at + 76 );

        if ( blobCount == 0u || blobCount > kMaxBlobs )
            return Common::MakeFormattedError<CloudModellingVolumeData>( "header declares {} lumps, outside 1..{}",
                                                                         blobCount, kMaxBlobs );

        // The extents and the payload length are two statements of one fact, and the whole class of
        // defects this programme keeps meeting is two statements of one fact that disagree. Checked here,
        // once, rather than trusted into an out-of-bounds upload later.
        if ( voxelBytes != kCloudModellingVoxelBytes )
            return Common::MakeFormattedError<CloudModellingVolumeData>(
                 "header says {} payload bytes but a {}x{}x{} RGBA8 volume is {} bytes", voxelBytes,
                 kCloudModellingVolumeWidth, kCloudModellingVolumeHeight, kCloudModellingVolumeDepth,
                 kCloudModellingVoxelBytes );

        const uint64_t expected = static_cast<uint64_t>( kCloudModellingHeaderSize ) +
                                  blobCount * kCloudModellingBlobBytes + voxelBytes;

        if ( bytes.size() != expected )
            return Common::MakeFormattedError<CloudModellingVolumeData>(
                 "payload is {} bytes; a header, {} lumps and the voxels are {}", bytes.size(), blobCount,
                 expected );

        const unsigned char* blobAt = at + kCloudModellingHeaderSize;

        if ( Crc32( blobAt, blobCount * kCloudModellingBlobBytes ) != storedBlob )
            return Common::MakeFormattedError<CloudModellingVolumeData>(
                 "the recipe's checksum does not match the {:08X} in the header; the file is corrupt",
                 storedBlob );

        data.Recipe.Blobs.resize( blobCount );
        for ( uint32_t i = 0; i < blobCount; ++i )
        {
            const unsigned char* b = blobAt + static_cast<size_t>( i ) * kCloudModellingBlobBytes;

            data.Recipe.Blobs[i].CentreKm   = glm::vec3( ReadF32( b + 0 ), ReadF32( b + 4 ), ReadF32( b + 8 ) );
            data.Recipe.Blobs[i].RadiiKm    = glm::vec3( ReadF32( b + 12 ), ReadF32( b + 16 ), ReadF32( b + 20 ) );
            data.Recipe.Blobs[i].DetailType = ReadF32( b + 24 );
            data.Recipe.Blobs[i].DensityScale = ReadF32( b + 28 );
            data.Recipe.Blobs[i].RotationDeg =
                 glm::vec3( ReadF32( b + 32 ), ReadF32( b + 36 ), ReadF32( b + 40 ) );

            // The primitive is the one field in a lump that is an ENUM and not a magnitude, so an unknown
            // value cannot be clamped into sense the way a radius could — it is refused by number here
            // rather than falling through to whatever the switch's default happens to do.
            const uint32_t primitive = ReadU32( b + 44 );
            if ( primitive > static_cast<uint32_t>( CloudModellingPrimitive::Capsule ) )
                return Common::MakeFormattedError<CloudModellingVolumeData>(
                     "lump {} declares primitive {}; this build knows Ellipsoid ({}), Sphere ({}) and Capsule "
                     "({})",
                     i, primitive, static_cast<uint32_t>( CloudModellingPrimitive::Ellipsoid ),
                     static_cast<uint32_t>( CloudModellingPrimitive::Sphere ),
                     static_cast<uint32_t>( CloudModellingPrimitive::Capsule ) );

            data.Recipe.Blobs[i].Primitive = static_cast<CloudModellingPrimitive>( primitive );
            data.Recipe.Blobs[i].Weight    = ReadF32( b + 48 );
        }

        const unsigned char* voxelAt = blobAt + static_cast<size_t>( blobCount ) * kCloudModellingBlobBytes;
        data.Voxels.assign( voxelAt, voxelAt + voxelBytes );

        const uint32_t actualVoxel = Crc32( data.Voxels.data(), data.Voxels.size() );
        if ( actualVoxel != storedVoxel )
            return Common::MakeFormattedError<CloudModellingVolumeData>(
                 "payload checksum {:08X} does not match the {:08X} in the header; the file is corrupt",
                 actualVoxel, storedVoxel );

        // Validated LAST, so a corrupt file is reported as corrupt rather than as an illegal recipe read
        // out of its wreckage.
        if ( auto valid = ValidateCloudModellingRecipe( data.Recipe ); !valid )
            return Common::MakeFormattedError<CloudModellingVolumeData>( "header recipe is not usable: {}",
                                                                         valid.GetError() );

        return Common::MakeSuccess( std::move( data ) );
    }

    const CloudModellingVolumeRecipe& CloudModellingDefaultRecipe()
    {
        // A CUMULUS, SCULPTED AS ONE CONNECTED BODY. Every lump below overlaps the one it grew from by
        // more than the blend radius, so the join fuses them into a single surface rather than a row of
        // beads — which is the whole point of the phase, because the procedural producer's Alligator lobes
        // cannot merge by construction.
        //
        // The proportions are a cumulus mediocris rising into a congestus: a flattened base pad 1.2 km
        // across, two shoulders, a body, a tower and a crown, plus one wispy tail shorn off downwind. The
        // box is 2 x 1 x 2 km at 128 x 64 x 128 voxels, so a voxel is 15.6 m on the horizontal axes and
        // 15.6 m on the vertical one — the arithmetic of PLAN_AUTHORED_CLOUDS.md §2, and the 8 m of the
        // deck is not the target: 8 m of DATA is 0.5 m of VISIBLE detail there because the up-rez carries
        // it, and the up-rez is Common/CloudField.glslh's erosion here.
        static const CloudModellingVolumeRecipe recipe = []
        {
            CloudModellingVolumeRecipe r;
            r.SizeKm           = glm::vec3( 2.0f, 1.0f, 2.0f );
            r.BlendRadiusKm    = 0.05f;
            r.ProfileDepthKm   = 0.18f;
            r.EnvelopeMarginKm = 0.09f;
            // WRITTEN FIELD BY FIELD AND NOT POSITIONALLY. A lump grew three members in this phase, and
            // the positional form this recipe used at A0 did not fail to compile at every site — it failed
            // at some and silently re-pointed at others, which is precisely how a "content" edit becomes a
            // shape change nobody ordered. Naming the fields makes the record's order a private matter of
            // the format again.
            //
            // THE NUMBERS ARE A0'S, UNCHANGED, AND DELIBERATELY SO. Every lump here is an unrotated
            // unit-weight ellipsoid, which is the identity case of everything this phase added, so the
            // shipped cloud is the same body it was — the re-bake this format bump forces moves the
            // container version and not one voxel. Desert/Tests/Engine/CloudModellingRecipe pins that.
            r.Blobs = {
                 // base pad — flat and wide, the underside a cumulus has because it sits on the
                 // condensation level
                 CloudModellingBlob{ .CentreKm     = glm::vec3( 0.00f, -0.17f, 0.00f ),
                                     .RadiiKm      = glm::vec3( 0.60f, 0.11f, 0.54f ),
                                     .DetailType   = 0.90f,
                                     .DensityScale = 0.85f },
                 // west lobe
                 CloudModellingBlob{ .CentreKm     = glm::vec3( -0.30f, -0.06f, 0.08f ),
                                     .RadiiKm      = glm::vec3( 0.36f, 0.17f, 0.32f ),
                                     .DetailType   = 0.95f,
                                     .DensityScale = 1.00f },
                 // east lobe
                 CloudModellingBlob{ .CentreKm     = glm::vec3( 0.32f, -0.08f, -0.12f ),
                                     .RadiiKm      = glm::vec3( 0.34f, 0.16f, 0.30f ),
                                     .DetailType   = 0.95f,
                                     .DensityScale = 1.00f },
                 // the body the tower rises out of
                 CloudModellingBlob{ .CentreKm     = glm::vec3( 0.02f, 0.02f, -0.02f ),
                                     .RadiiKm      = glm::vec3( 0.40f, 0.20f, 0.36f ),
                                     .DetailType   = 1.00f,
                                     .DensityScale = 1.00f },
                 // tower
                 CloudModellingBlob{ .CentreKm     = glm::vec3( -0.06f, 0.11f, 0.04f ),
                                     .RadiiKm      = glm::vec3( 0.25f, 0.17f, 0.23f ),
                                     .DetailType   = 1.00f,
                                     .DensityScale = 0.95f },
                 // crown
                 CloudModellingBlob{ .CentreKm     = glm::vec3( -0.03f, 0.20f, 0.02f ),
                                     .RadiiKm      = glm::vec3( 0.15f, 0.085f, 0.14f ),
                                     .DetailType   = 1.00f,
                                     .DensityScale = 0.80f },
                 // shoulder puff
                 CloudModellingBlob{ .CentreKm     = glm::vec3( 0.30f, 0.08f, 0.22f ),
                                     .RadiiKm      = glm::vec3( 0.19f, 0.12f, 0.17f ),
                                     .DetailType   = 0.85f,
                                     .DensityScale = 0.75f },
                 // the wispy tail: the one lump whose Detail Type is near zero, so the join hands the
                 // up-rez a WISPY erosion there and a billowy one everywhere else — two channels of the
                 // volume written by the same weights that fused the shapes
                 CloudModellingBlob{ .CentreKm     = glm::vec3( -0.52f, -0.02f, -0.30f ),
                                     .RadiiKm      = glm::vec3( 0.24f, 0.09f, 0.19f ),
                                     .DetailType   = 0.30f,
                                     .DensityScale = 0.50f },
            };
            return r;
        }();

        return recipe;
    }
} // namespace Desert::Assets
