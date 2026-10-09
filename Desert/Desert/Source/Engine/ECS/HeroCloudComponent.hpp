#pragma once

#include <Engine/Assets/Common.hpp>
#include <Engine/Reflection/ReflectionMacros.hpp>

namespace Desert::ECS
{
    // A HERO CLOUD: one sculpted body, placed in the sky by this entity's own transform.
    //
    // WHAT IT IS FOR, and it is the whole reason phase Э4 exists. The cloud field has two producers behind
    // one seam (Docs/Clouds/ANALYSIS_APPROACH.md §4). The procedural one fills the sky from parameters and
    // has a MEASURED limit three separate tasks found independently: the Alligator's lobes cannot merge,
    // because the field is `best - second` and that lays a zero between every pair of cells, so a
    // procedural sky is a deck of separate cushions and never one fused convective mass. This component is
    // the other producer, and a fused mass is exactly what it puts in the sky.
    //
    // IT IS NOT A SECOND CLOUD SYSTEM. The body joins the procedural field at
    // Common/CloudField.glslh's `SampleCloudField` as a plain `max` of the two profiles, and everything
    // below that line — the march, the erosion, the lighting, the shadow map, the temporal resolve, the
    // composite — is the same code marching the same shell. A hero cloud is lit like the sky around it and
    // casts the same shadow on the world, because it is the same cloud field.
    //
    // WHERE IT LIVES IN SPACE. The entity's TransformComponent, and nothing here duplicates it. The
    // volume carries its own authored size in kilometres and the transform's scale multiplies it, exactly
    // as a mesh asset and a transform divide the same work. The one relation that matters — the body has
    // to be inside the LAYER, because the march only samples between the two shells — is checked by
    // Graphic::System::VolumetricCloudRenderer, which logs the two altitudes when they disagree rather
    // than leaving the artist with a cloud whose top has been sliced off.
    //
    // ZERO COST WHEN ABSENT. A scene with no hero cloud, or with this one disabled, sends the march an
    // instance count of zero and the loop that reads them does not execute. The measurement is in
    // Docs/Clouds/CALIBRATION.md §A0 and the frames it is asserted against are byte for byte the frames
    // the procedural sky rendered before this component existed.
    struct HeroCloudData
    {
        REFLECT()

        // ---- Hero Cloud -----------------------------------------------------------------------------

        PROPERTY( DisplayName( "Enabled" ), Category( "Hero Cloud" ), Summary,
                  Tooltip( "Master switch. Off is not a strength of zero: the instance is not collected at "
                           "all, so the march's authored loop does not run for it and the frame is the one "
                           "the procedural sky renders on its own." ) )
        bool Enabled = true;

        PROPERTY( DisplayName( "Volume" ), Category( "Hero Cloud" ), Summary, Asset<CloudModellingVolumeAsset>,
                  Tooltip( "The sculpted body this cloud is — drag a .dcmv from the Content Browser. The "
                           "file carries the lumps it was baked from, its own size in kilometres and the "
                           "four channels the march reads: how deep inside the body a point is, whether "
                           "the edge there erodes into wisps or billows, how much matter is in it, and "
                           "the envelope the cutout uses. An empty slot means no cloud, not a default "
                           "one: a body nobody sculpted appearing in a scene is a cloud nobody can "
                           "explain." ) )
        Assets::AssetHandle Volume;

        PROPERTY( DisplayName( "Strength" ), Category( "Hero Cloud" ), Range( 0.0f, 1.0f ),
                  Tooltip( "How strongly this cloud asserts itself, 0 to 1. It scales the body's depth "
                           "profile AND its cutout together, so winding it down does not leave a hole "
                           "where the cloud was — the procedural field comes back as the body fades. That "
                           "is what makes it the dial to animate a hero cloud in or out of a shot." ) )
        float Strength = 1.0f;

        PROPERTY( DisplayName( "Suppress Procedural Field" ), Category( "Hero Cloud" ),
                  Tooltip( "Stop the procedural clouds growing through this body. Without it a procedural "
                           "blob sprouts through a sculpted cloud and the composition turns to soup "
                           "(ANALYSIS_APPROACH.md §4.3); with it, the volume's own dilated envelope is "
                           "what pushes them away, so a tunnel through the cloud shows sky and there is "
                           "no rectangular hole around the box." ) )
        bool SuppressProceduralField = true;

        // ---- Material -------------------------------------------------------------------------------
        //
        // THE SAME THREE NUMBERS A CLOUD TYPE CARRIES (Graphic::CloudTypeShape), and deliberately so: what
        // a cloud is MADE OF is a property of the cloud and not of which producer drew it, so a hero cloud
        // and a procedural species describe themselves in the same vocabulary and 1 means the same thing
        // in both. The fourth of that set, Detail Character, is not here because the volume carries it per
        // voxel — the wispy tail of a body erodes differently from its core, which is exactly what a
        // single number per cloud cannot say.

        PROPERTY( DisplayName( "Detail Factor" ), Category( "Material" ), Range( 0.0f, 4.0f ),
                  Tooltip( "Multiplier on the layer's Detail Strength for this body — how deeply the "
                           "up-rez noise erodes its edge. 1 is the layer as authored; 0 leaves the "
                           "sculpted silhouette untouched, which is what a smooth lenticular wants." ) )
        float DetailFactor = 1.0f;

        PROPERTY( DisplayName( "Density Factor" ), Category( "Material" ), Range( 0.0f, 4.0f ),
                  Tooltip( "Multiplier on the layer's Density Scale for this body, on top of the "
                           "per-voxel density the volume carries. 1 is the layer as authored." ) )
        float DensityFactor = 1.0f;

        PROPERTY( DisplayName( "Extinction Factor" ), Category( "Material" ), Range( 0.0f, 4.0f ),
                  Tooltip( "Multiplier on the layer's Extinction Scale for this body — how opaque its "
                           "matter is to the sun as well as to the eye. A quarter is ice; 1 is the "
                           "layer as authored." ) )
        float ExtinctionFactor = 1.0f;
    };

    struct HeroCloudComponent
    {
        COMPONENT( Key( "HeroCloud" ), Block( Data ), Run( SkyAndAtmosphere ) )
        HeroCloudData Data;
    };
} // namespace Desert::ECS
