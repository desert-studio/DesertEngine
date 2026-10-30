#include "CloudModellingCatalogue.hpp"

#include <array>
#include <cstdint>

namespace Desert::Assets
{
    namespace
    {
        using Blob = CloudModellingBlob;
        using glm::vec3;

        // The ellipsoid is the DEFAULT primitive and is therefore never named below — a `kEllipsoid`
        // alias would be a constant nothing reads, which is the dead data this contract forbids.
        constexpr auto kSphere  = CloudModellingPrimitive::Sphere;
        constexpr auto kCapsule = CloudModellingPrimitive::Capsule;

        /// A capsule's axis is its local Y, so this is how one is laid along world X — the rotation that
        /// makes the primitive worth having at all (A1's own note on CloudModellingBlob::RotationDeg).
        constexpr vec3 kAlongX{ 0.0f, 0.0f, 90.0f };

        // ------------------------------------------------------------------------------------------
        // 0. CUMULUS HUMILIS — fair weather, and the genus is defined by what it does NOT do
        // ------------------------------------------------------------------------------------------
        //
        // Wider than it is tall by four to one, a flat base at the condensation level, a top of low
        // rounded puffs and NO vertical growth. Everything about it is the absence of a tower, so the
        // recipe is a pad and four flattened puffs and nothing above them.
        CloudModellingVolumeRecipe MakeHumilis()
        {
            CloudModellingVolumeRecipe r;
            r.SizeKm           = vec3( 1.2f, 0.44f, 1.2f );
            r.BlendRadiusKm    = 0.03f;
            r.ProfileDepthKm   = 0.07f;
            r.EnvelopeMarginKm = 0.05f;
            r.Blobs            = {
                 Blob{ .CentreKm     = vec3( 0.00f, -0.060f, 0.00f ),
                                  .RadiiKm      = vec3( 0.40f, 0.045f, 0.36f ),
                                  .DetailType   = 0.85f,
                                  .DensityScale = 0.85f },
                 Blob{ .CentreKm     = vec3( -0.16f, -0.010f, 0.05f ),
                                  .RadiiKm      = vec3( 0.20f, 0.070f, 0.18f ),
                                  .DetailType   = 0.95f,
                                  .DensityScale = 1.00f },
                 Blob{ .CentreKm     = vec3( 0.18f, -0.020f, -0.06f ),
                                  .RadiiKm      = vec3( 0.18f, 0.065f, 0.16f ),
                                  .DetailType   = 0.95f,
                                  .DensityScale = 1.00f },
                 Blob{ .CentreKm     = vec3( 0.02f, 0.000f, 0.16f ),
                                  .RadiiKm      = vec3( 0.14f, 0.055f, 0.13f ),
                                  .DetailType   = 0.90f,
                                  .DensityScale = 0.90f },
                 Blob{ .CentreKm     = vec3( 0.00f, 0.035f, 0.00f ),
                                  .RadiiKm      = vec3( 0.10f, 0.045f, 0.09f ),
                                  .DetailType   = 1.00f,
                                  .DensityScale = 0.85f },
            };
            return r;
        }

        // ------------------------------------------------------------------------------------------
        // 1. CUMULUS MEDIOCRIS — as tall as it is wide, and that ratio IS the genus
        // ------------------------------------------------------------------------------------------
        CloudModellingVolumeRecipe MakeMediocris()
        {
            CloudModellingVolumeRecipe r;
            r.SizeKm           = vec3( 1.6f, 1.12f, 1.6f );
            r.BlendRadiusKm    = 0.04f;
            r.ProfileDepthKm   = 0.14f;
            r.EnvelopeMarginKm = 0.07f;
            r.Blobs            = {
                 Blob{ .CentreKm     = vec3( 0.00f, -0.30f, 0.00f ),
                                  .RadiiKm      = vec3( 0.46f, 0.09f, 0.42f ),
                                  .DetailType   = 0.85f,
                                  .DensityScale = 0.85f },
                 Blob{ .CentreKm     = vec3( 0.00f, -0.13f, 0.00f ),
                                  .RadiiKm      = vec3( 0.40f, 0.17f, 0.37f ),
                                  .DetailType   = 1.00f,
                                  .DensityScale = 1.00f },
                 Blob{ .CentreKm     = vec3( -0.24f, -0.06f, 0.06f ),
                                  .RadiiKm      = vec3( 0.24f, 0.14f, 0.22f ),
                                  .DetailType   = 0.95f,
                                  .DensityScale = 0.95f },
                 Blob{ .CentreKm     = vec3( 0.25f, -0.08f, -0.07f ),
                                  .RadiiKm      = vec3( 0.23f, 0.13f, 0.21f ),
                                  .DetailType   = 0.95f,
                                  .DensityScale = 0.95f },
                 Blob{ .CentreKm     = vec3( 0.02f, 0.10f, 0.01f ),
                                  .RadiiKm      = vec3( 0.24f, 0.18f, 0.22f ),
                                  .DetailType   = 1.00f,
                                  .DensityScale = 1.00f },
                 Blob{ .CentreKm     = vec3( 0.00f, 0.26f, 0.00f ),
                                  .RadiiKm      = vec3( 0.13f, 0.13f, 0.13f ),
                                  .Primitive    = kSphere,
                                  .DetailType   = 1.00f,
                                  .DensityScale = 0.85f },
            };
            return r;
        }

        // ------------------------------------------------------------------------------------------
        // 2. CUMULUS CONGESTUS — twice as tall as it is wide, a stack of turrets, a cauliflower crown
        // ------------------------------------------------------------------------------------------
        //
        // THE ONE ENTRY THE ABSENT SOLVER COSTS MOST. A real congestus's crown is recursively lumpy at
        // every scale down to metres; eight lumps and an up-rez erosion give a tower with a bumpy outline
        // and a smooth interior. The turrets below are the closest a sum of spheres gets, and the report
        // says so rather than the frame implying otherwise.
        CloudModellingVolumeRecipe MakeCongestus()
        {
            CloudModellingVolumeRecipe r;
            r.SizeKm           = vec3( 2.0f, 3.4f, 2.0f );
            r.BlendRadiusKm    = 0.12f;
            r.ProfileDepthKm   = 0.22f;
            r.EnvelopeMarginKm = 0.09f;

            // THE SAME OVERLAP RULE THE CUMULONIMBUS NEEDED, arrived at the same way: the first version of
            // this recipe was photographed and came back a string of beads. Consecutive lumps have to
            // overlap by about half their own height before the join reads as one tower — a tangency is
            // not an overlap, and the up-rez eats a thin waist before an audience sees the join.
            r.Blobs = {
                 Blob{ .CentreKm     = vec3( 0.00f, -1.12f, 0.00f ),
                       .RadiiKm      = vec3( 0.50f, 0.16f, 0.46f ),
                       .DetailType   = 0.80f,
                       .DensityScale = 0.85f },
                 Blob{ .CentreKm     = vec3( 0.00f, -0.82f, 0.00f ),
                       .RadiiKm      = vec3( 0.46f, 0.30f, 0.43f ),
                       .DetailType   = 1.00f,
                       .DensityScale = 1.00f },
                 Blob{ .CentreKm     = vec3( 0.04f, -0.40f, -0.03f ),
                       .RadiiKm      = vec3( 0.40f, 0.32f, 0.37f ),
                       .DetailType   = 1.00f,
                       .DensityScale = 1.00f },
                 Blob{ .CentreKm     = vec3( -0.03f, 0.02f, 0.02f ),
                       .RadiiKm      = vec3( 0.36f, 0.32f, 0.34f ),
                       .DetailType   = 1.00f,
                       .DensityScale = 1.00f },
                 Blob{ .CentreKm     = vec3( -0.18f, 0.34f, 0.04f ),
                       .RadiiKm      = vec3( 0.24f, 0.24f, 0.24f ),
                       .Primitive    = kSphere,
                       .DetailType   = 1.00f,
                       .DensityScale = 0.95f },
                 Blob{ .CentreKm     = vec3( 0.18f, 0.40f, -0.05f ),
                       .RadiiKm      = vec3( 0.22f, 0.22f, 0.22f ),
                       .Primitive    = kSphere,
                       .DetailType   = 1.00f,
                       .DensityScale = 0.95f },
                 Blob{ .CentreKm     = vec3( 0.00f, 0.66f, 0.00f ),
                       .RadiiKm      = vec3( 0.26f, 0.24f, 0.24f ),
                       .DetailType   = 1.00f,
                       .DensityScale = 0.90f },
                 Blob{ .CentreKm     = vec3( 0.02f, 0.88f, 0.01f ),
                       .RadiiKm      = vec3( 0.16f, 0.16f, 0.16f ),
                       .Primitive    = kSphere,
                       .DetailType   = 1.00f,
                       .DensityScale = 0.80f },
            };
            return r;
        }

        // ------------------------------------------------------------------------------------------
        // 3. CUMULONIMBUS WITH AN ANVIL — the entry the procedural producer cannot reach at all
        // ------------------------------------------------------------------------------------------
        //
        // THE ANVIL IS THE TEST AND IT IS A NUMBER: the body is WIDER AT THE TOP THAN AT THE BOTTOM. The
        // procedural producer's vertical profile is a curve of altitude, which can taper or flare a whole
        // layer but cannot put a 3.9 km ice canopy on top of a 1.2 km tower and leave the air beside the
        // tower empty. `Desert/Tests/Engine/CloudCatalogue` measures the ratio.
        //
        // The canopy's lumps carry Detail Type near zero — ice, which the up-rez erodes into wisps — where
        // the tower's carry 1. That is the second of the volume's four channels doing exactly what the
        // softmax weights of the join were chosen to give for free.
        CloudModellingVolumeRecipe MakeCumulonimbus()
        {
            CloudModellingVolumeRecipe r;
            r.SizeKm           = vec3( 6.6f, 5.6f, 5.6f );
            r.BlendRadiusKm    = 0.20f;
            r.ProfileDepthKm   = 0.30f;
            r.EnvelopeMarginKm = 0.15f;

            // CONSECUTIVE LUMPS OVERLAP BY ABOUT HALF THEIR OWN HEIGHT, and the first version of this
            // recipe did not — it left necks of 40 to 100 m between lumps half a kilometre across, and
            // the frame showed the result plainly: a totem pole of separate beads with the anvil floating
            // above it. A waist that thin is eaten by the up-rez erosion before an audience ever sees the
            // join. It is the same lesson A1 measured on the blend radius, arriving from the other side:
            // the join fuses what OVERLAPS, and a tangency is not an overlap.
            r.Blobs = {
                 Blob{ .CentreKm     = vec3( 0.00f, -1.85f, 0.00f ),
                       .RadiiKm      = vec3( 0.72f, 0.20f, 0.66f ),
                       .DetailType   = 0.80f,
                       .DensityScale = 0.90f },
                 Blob{ .CentreKm     = vec3( 0.00f, -1.40f, 0.00f ),
                       .RadiiKm      = vec3( 0.64f, 0.42f, 0.60f ),
                       .DetailType   = 1.00f,
                       .DensityScale = 1.00f },
                 Blob{ .CentreKm     = vec3( 0.06f, -0.75f, -0.05f ),
                       .RadiiKm      = vec3( 0.58f, 0.46f, 0.54f ),
                       .DetailType   = 1.00f,
                       .DensityScale = 1.00f },
                 Blob{ .CentreKm     = vec3( -0.05f, 0.00f, 0.04f ),
                       .RadiiKm      = vec3( 0.52f, 0.48f, 0.49f ),
                       .DetailType   = 1.00f,
                       .DensityScale = 1.00f },
                 // the canopy: four discs, flatter and wider and wispier as they spread downwind
                 Blob{ .CentreKm     = vec3( 0.00f, 0.62f, 0.00f ),
                       .RadiiKm      = vec3( 0.95f, 0.30f, 0.90f ),
                       .DetailType   = 0.50f,
                       .DensityScale = 0.80f },
                 Blob{ .CentreKm     = vec3( 0.10f, 0.95f, -0.05f ),
                       .RadiiKm      = vec3( 1.60f, 0.20f, 1.45f ),
                       .DetailType   = 0.25f,
                       .DensityScale = 0.60f },
                 Blob{ .CentreKm     = vec3( 0.75f, 1.05f, 0.30f ),
                       .RadiiKm      = vec3( 1.20f, 0.13f, 0.95f ),
                       .DetailType   = 0.15f,
                       .DensityScale = 0.45f },
                 Blob{ .CentreKm     = vec3( 1.45f, 1.00f, 0.10f ),
                       .RadiiKm      = vec3( 0.75f, 0.10f, 0.60f ),
                       .DetailType   = 0.10f,
                       .DensityScale = 0.35f },
            };
            return r;
        }

        // ------------------------------------------------------------------------------------------
        // 4. STRATOCUMULUS — one sheet with a corrugated surface, and BOTH halves matter
        // ------------------------------------------------------------------------------------------
        //
        // Nine horizontal capsules half a kilometre apart, each overlapping its neighbours, so the result
        // is ONE connected body whose surface rolls. That is the pair of properties the procedural
        // producer cannot hold at once: its Alligator lobes make the rolls and can never fuse them, so it
        // renders a sheet of separate cushions where a real stratocumulus is a continuous deck with a
        // pattern IN it.
        CloudModellingVolumeRecipe MakeStratocumulus()
        {
            CloudModellingVolumeRecipe r;
            r.SizeKm           = vec3( 8.0f, 1.8f, 8.0f );
            r.BlendRadiusKm    = 0.12f;
            r.ProfileDepthKm   = 0.16f;
            r.EnvelopeMarginKm = 0.20f;

            // The rolls. A capsule laid along x, which is the shape an ellipsoid cannot stand in for — an
            // ellipsoid of the same length tapers to a point at both ends and reads as a lens (A1).
            const float rollZ[9]   = { -2.2f, -1.65f, -1.1f, -0.55f, 0.0f, 0.55f, 1.1f, 1.65f, 2.2f };
            const float rollY[9]   = { -0.05f, 0.02f, -0.04f, 0.03f, -0.02f, 0.04f, -0.03f, 0.01f, -0.05f };
            const float rollLen[9] = { 2.0f, 2.3f, 2.5f, 2.6f, 2.6f, 2.5f, 2.4f, 2.2f, 1.9f };

            for ( int i = 0; i < 9; ++i )
            {
                r.Blobs.push_back( Blob{ .CentreKm     = vec3( 0.0f, rollY[i], rollZ[i] ),
                                         .RadiiKm      = vec3( 0.30f, rollLen[i], 0.30f ),
                                         .RotationDeg  = kAlongX,
                                         .Primitive    = kCapsule,
                                         .DetailType   = 0.75f,
                                         .DensityScale = 0.80f } );
            }

            // Four puffs riding on top, so the deck is lumpy rather than merely wavy.
            const float puffX[4] = { -1.5f, 1.5f, -1.5f, 1.5f };
            const float puffZ[4] = { -1.5f, -1.5f, 1.5f, 1.5f };
            for ( int i = 0; i < 4; ++i )
            {
                r.Blobs.push_back( Blob{ .CentreKm     = vec3( puffX[i], 0.12f, puffZ[i] ),
                                         .RadiiKm      = vec3( 0.55f, 0.16f, 0.50f ),
                                         .DetailType   = 0.85f,
                                         .DensityScale = 0.90f } );
            }

            return r;
        }

        // ------------------------------------------------------------------------------------------
        // 5. STRATUS — the genus whose defining property is having no features
        // ------------------------------------------------------------------------------------------
        //
        // A featureless slab fifteen times wider than it is thick, thin and wispy at the edges. There is
        // nothing here a sculpting tool is needed for, and that is worth saying plainly: this is the one
        // entry of the ten the procedural producer already does BETTER, because a formless overcast is
        // exactly what a coverage field with the profile flattened produces, at no memory cost and over
        // the whole sky rather than over one 8 km box.
        CloudModellingVolumeRecipe MakeStratus()
        {
            CloudModellingVolumeRecipe r;
            r.SizeKm           = vec3( 10.0f, 1.4f, 10.0f );
            r.BlendRadiusKm    = 0.10f;
            r.ProfileDepthKm   = 0.10f;
            r.EnvelopeMarginKm = 0.25f;
            r.Blobs            = {
                 Blob{ .CentreKm     = vec3( 0.0f, 0.00f, 0.0f ),
                                  .RadiiKm      = vec3( 2.2f, 0.22f, 2.2f ),
                                  .DetailType   = 0.20f,
                                  .DensityScale = 0.55f },
                 Blob{ .CentreKm     = vec3( -1.3f, 0.02f, 1.1f ),
                                  .RadiiKm      = vec3( 1.9f, 0.19f, 1.9f ),
                                  .DetailType   = 0.15f,
                                  .DensityScale = 0.50f },
                 Blob{ .CentreKm     = vec3( 1.4f, -0.02f, -1.0f ),
                                  .RadiiKm      = vec3( 1.8f, 0.18f, 1.8f ),
                                  .DetailType   = 0.15f,
                                  .DensityScale = 0.50f },
                 Blob{ .CentreKm     = vec3( 0.2f, 0.00f, -1.9f ),
                                  .RadiiKm      = vec3( 1.7f, 0.17f, 1.5f ),
                                  .DetailType   = 0.10f,
                                  .DensityScale = 0.45f },
            };
            return r;
        }

        // ------------------------------------------------------------------------------------------
        // 6. ALTOCUMULUS — a regular field of small separate lumps, and it is the OTHER honest refusal
        // ------------------------------------------------------------------------------------------
        //
        // A mackerel sky is many small elements that do NOT touch, laid out on a regular lattice. That is
        // a description of the Alligator's output — separate lobes with a zero on every bisector — so
        // altocumulus is the genus phase Э4 was least needed for. It is in the catalogue because §6 lists
        // it and because a sculpted one can be placed exactly where a shot wants it; it is not in the
        // catalogue because the procedural producer could not do it.
        CloudModellingVolumeRecipe MakeAltocumulus()
        {
            CloudModellingVolumeRecipe r;
            r.SizeKm           = vec3( 6.0f, 0.8f, 6.0f );
            r.BlendRadiusKm    = 0.05f;
            r.ProfileDepthKm   = 0.08f;
            r.EnvelopeMarginKm = 0.10f;

            for ( int ix = -2; ix <= 2; ++ix )
            {
                for ( int iz = -2; iz <= 2; ++iz )
                {
                    const float jitter = 0.02f * static_cast<float>( ( ix * 5 + iz ) % 3 );
                    r.Blobs.push_back( Blob{ .CentreKm     = vec3( static_cast<float>( ix ) * 1.0f, jitter,
                                                                   static_cast<float>( iz ) * 1.0f ),
                                             .RadiiKm      = vec3( 0.28f, 0.07f, 0.26f ),
                                             .DetailType   = 0.70f,
                                             .DensityScale = 0.70f } );
                }
            }

            return r;
        }

        // ------------------------------------------------------------------------------------------
        // 7. CIRRUS — fibrous streaks, and the genus lives in the SECOND channel rather than the first
        // ------------------------------------------------------------------------------------------
        //
        // Six long thin capsules tilted a couple of degrees off horizontal, with fallstreak hooks under
        // them. What makes it read as cirrus is not the silhouette — that is a set of thin rods — but
        // Detail Type near zero, which hands the up-rez the WISPY erosion and turns each rod into fibres.
        // The volume carries the silhouette and the noise carries the cloud, which is the arrangement
        // PLAN_AUTHORED_CLOUDS.md §2 chose 15.6 m voxels on the strength of.
        CloudModellingVolumeRecipe MakeCirrus()
        {
            CloudModellingVolumeRecipe r;
            r.SizeKm           = vec3( 9.0f, 1.8f, 6.0f );
            r.BlendRadiusKm    = 0.10f;
            r.ProfileDepthKm   = 0.09f;
            r.EnvelopeMarginKm = 0.15f;

            const float streakZ[6]    = { -2.0f, -1.2f, -0.4f, 0.4f, 1.2f, 2.0f };
            const float streakX[6]    = { -0.4f, 0.3f, -0.2f, 0.5f, -0.5f, 0.1f };
            const float streakY[6]    = { 0.10f, -0.06f, 0.04f, -0.10f, 0.06f, -0.02f };
            const float streakTilt[6] = { 86.0f, 88.0f, 90.0f, 92.0f, 87.0f, 91.0f };

            for ( int i = 0; i < 6; ++i )
            {
                r.Blobs.push_back( Blob{ .CentreKm     = vec3( streakX[i], streakY[i], streakZ[i] ),
                                         .RadiiKm      = vec3( 0.16f, 2.0f, 0.16f ),
                                         .RotationDeg  = vec3( 0.0f, 0.0f, streakTilt[i] ),
                                         .Primitive    = kCapsule,
                                         .DetailType   = 0.05f,
                                         .DensityScale = 0.30f } );
            }

            // The fallstreaks — small tails hanging below the streaks, which is what makes cirrus uncinus
            // read as hooks rather than as lines.
            const float hookX[5] = { -1.2f, -0.2f, 0.6f, 1.4f, 0.0f };
            const float hookZ[5] = { -1.9f, -1.1f, 0.5f, 1.3f, 2.0f };
            for ( int i = 0; i < 5; ++i )
            {
                r.Blobs.push_back( Blob{ .CentreKm     = vec3( hookX[i], -0.14f, hookZ[i] ),
                                         .RadiiKm      = vec3( 0.34f, 0.14f, 0.16f ),
                                         .DetailType   = 0.00f,
                                         .DensityScale = 0.22f } );
            }

            return r;
        }

        // ------------------------------------------------------------------------------------------
        // 8. LENTICULAR — three smooth lenses, deliberately NOT joined
        // ------------------------------------------------------------------------------------------
        //
        // The "pile of plates" a mountain wave stacks. Its genus marks are that each plate is a smooth
        // convex lens, that it is far longer across the wind than along it, and that the plates are
        // SEPARATE — which is the one place in this catalogue where three components is the right answer
        // and not a failure to fuse. The smoothness is finished on the component side: a lenticular wants
        // Detail Factor at or near 0, which is exactly the knob ECS::HeroCloudData carries for it.
        CloudModellingVolumeRecipe MakeLenticular()
        {
            CloudModellingVolumeRecipe r;
            r.SizeKm           = vec3( 4.0f, 1.5f, 2.4f );
            r.BlendRadiusKm    = 0.06f;
            r.ProfileDepthKm   = 0.20f;
            r.EnvelopeMarginKm = 0.10f;
            r.Blobs            = {
                 Blob{ .CentreKm     = vec3( 0.0f, -0.40f, 0.0f ),
                                  .RadiiKm      = vec3( 1.30f, 0.13f, 0.70f ),
                                  .DetailType   = 1.00f,
                                  .DensityScale = 0.85f },
                 Blob{ .CentreKm     = vec3( 0.0f, 0.00f, 0.0f ),
                                  .RadiiKm      = vec3( 1.55f, 0.16f, 0.85f ),
                                  .DetailType   = 1.00f,
                                  .DensityScale = 1.00f },
                 Blob{ .CentreKm     = vec3( 0.0f, 0.40f, 0.0f ),
                                  .RadiiKm      = vec3( 1.10f, 0.12f, 0.60f ),
                                  .DetailType   = 1.00f,
                                  .DensityScale = 0.80f },
            };
            return r;
        }

        // ------------------------------------------------------------------------------------------
        // 9. FREEFORM — an arch, and it is the proof of the whole phase
        // ------------------------------------------------------------------------------------------
        //
        // Two legs and a span. ONE connected body with a hole straight through it, which is precisely the
        // pair of properties the procedural producer cannot hold at the same time: its coverage field is
        // `best - second`, exactly zero on the bisector between every pair of feature points, so two lobes
        // with air between them are always two COMPONENTS. A shape that is connected AND holed is
        // unreachable by construction, not by tuning, and three tasks measured that before Э4 was
        // approved.
        //
        // It is also what the cutout's envelope was written for: the hole is wider than the bake's margin,
        // so the procedural field is allowed back INSIDE the arch and the audience sees sky through it.
        //
        // THE BASE BAR IS NOT DECORATION AND THE FIRST VERSION OF THIS RECIPE DID NOT HAVE IT. Two legs
        // and a span is a "П": the space under it is open downward, which makes it a BAY and not a hole,
        // and a bay is something the procedural producer manages between any two lobes. Closing the loop
        // at the bottom is what turns it into a tunnel — a slice through the body has air surrounded by
        // cloud on all four sides — and that is the property `CloudCatalogue` measures topologically
        // rather than by a threshold.
        CloudModellingVolumeRecipe MakeFreeform()
        {
            CloudModellingVolumeRecipe r;
            r.SizeKm           = vec3( 3.2f, 2.8f, 1.6f );
            r.BlendRadiusKm    = 0.09f;
            r.ProfileDepthKm   = 0.30f;
            r.EnvelopeMarginKm = 0.10f;
            r.Blobs            = {
                 // the two legs, vertical capsules
                 Blob{ .CentreKm     = vec3( -0.85f, -0.35f, 0.0f ),
                                  .RadiiKm      = vec3( 0.26f, 0.60f, 0.26f ),
                                  .Primitive    = kCapsule,
                                  .DetailType   = 1.00f,
                                  .DensityScale = 1.00f },
                 Blob{ .CentreKm     = vec3( 0.85f, -0.35f, 0.0f ),
                                  .RadiiKm      = vec3( 0.26f, 0.60f, 0.26f ),
                                  .Primitive    = kCapsule,
                                  .DetailType   = 1.00f,
                                  .DensityScale = 1.00f },
                 // the span, the same capsule laid along x — the lump that makes the two legs ONE body
                 Blob{ .CentreKm     = vec3( 0.0f, 0.42f, 0.0f ),
                                  .RadiiKm      = vec3( 0.24f, 1.05f, 0.24f ),
                                  .RotationDeg  = kAlongX,
                                  .Primitive    = kCapsule,
                                  .DetailType   = 1.00f,
                                  .DensityScale = 1.00f },
                 // the base bar, which closes the loop: with it the arch is a ring and the air inside it is
                 // surrounded by cloud, which is the whole claim
                 Blob{ .CentreKm     = vec3( 0.0f, -0.86f, 0.0f ),
                                  .RadiiKm      = vec3( 0.22f, 1.05f, 0.22f ),
                                  .RotationDeg  = kAlongX,
                                  .Primitive    = kCapsule,
                                  .DetailType   = 0.85f,
                                  .DensityScale = 0.90f },
                 // and two feet where it spreads at the condensation level
                 Blob{ .CentreKm     = vec3( -0.88f, -0.92f, 0.0f ),
                                  .RadiiKm      = vec3( 0.34f, 0.13f, 0.30f ),
                                  .DetailType   = 0.85f,
                                  .DensityScale = 0.85f },
                 Blob{ .CentreKm     = vec3( 0.88f, -0.92f, 0.0f ),
                                  .RadiiKm      = vec3( 0.34f, 0.13f, 0.30f ),
                                  .DetailType   = 0.85f,
                                  .DensityScale = 0.85f },
            };
            return r;
        }

        const std::array<CloudModellingVolumeRecipe, kCloudModellingSpeciesCount>& Catalogue()
        {
            static const std::array<CloudModellingVolumeRecipe, kCloudModellingSpeciesCount> catalogue = {
                 MakeHumilis(), MakeMediocris(),   MakeCongestus(), MakeCumulonimbus(), MakeStratocumulus(),
                 MakeStratus(), MakeAltocumulus(), MakeCirrus(),    MakeLenticular(),   MakeFreeform(),
            };
            return catalogue;
        }

        uint32_t Index( CloudModellingSpecies species )
        {
            const uint32_t index = static_cast<uint32_t>( species );
            return index < kCloudModellingSpeciesCount ? index : 0u;
        }
    } // namespace

    const char* CloudModellingSpeciesName( CloudModellingSpecies species )
    {
        static const char* const names[kCloudModellingSpeciesCount] = {
             "Cumulus humilis", "Cumulus mediocris", "Cumulus congestus", "Cumulonimbus",
             "Stratocumulus",   "Stratus",           "Altocumulus",       "Cirrus",
             "Lenticular",      "Freeform (arch)",
        };
        return names[Index( species )];
    }

    const char* CloudModellingSpeciesKey( CloudModellingSpecies species )
    {
        static const char* const keys[kCloudModellingSpeciesCount] = {
             "humilis", "mediocris",   "congestus", "cumulonimbus", "stratocumulus",
             "stratus", "altocumulus", "cirrus",    "lenticular",   "freeform",
        };
        return keys[Index( species )];
    }

    const CloudModellingVolumeRecipe& CloudModellingCatalogueRecipe( CloudModellingSpecies species )
    {
        return Catalogue()[Index( species )];
    }
} // namespace Desert::Assets
