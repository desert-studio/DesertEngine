#pragma once

#include <Engine/Assets/CloudProceduralVolume.hpp>
#include <Engine/ECS/VolumetricCloudComponent.hpp>
#include <Engine/Graphic/Clouds/CloudMaterialValues.hpp>
#include <Engine/Graphic/Clouds/CloudPayload.hpp>
#include <Engine/Graphic/Clouds/CloudTypeShape.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace Desert::Graphic
{
    /**
     * @brief What the BAKE takes from the layer's component rather than from its material.
     *
     * THREE OF THESE ARE THE VIEW'S OWN BUDGET AND ONE IS THE WORLD'S. A preview pane and a viewport of the
     * same scene legitimately bake at different resolutions, so the budget travels through the component
     * beside Max Steps and never through the material — the look must be the same in both windows and the
     * cost must not be. They are gathered into a struct rather than passed as four loose floats so that
     * adding one is a compile error at the single call site instead of a silently reordered argument list.
     */
    struct CloudBakeLayerInputs
    {
        float     RegionSize       = 4800000.0f; ///< world units (cm) across the baked region
        int32_t   VolumeResolution = static_cast<int32_t>( Assets::kCloudProceduralVolumeSide );
        int32_t   MaxSteps         = 128; ///< the march's budget; decides the finest resolvable chord
        glm::vec3 WindDirection    = { 1.0f, 0.0f, 0.0f };
    };

    /**
     * @brief The bake grid a set of species can actually be expressed on, voxels per horizontal side.
     *
     * WHAT THIS CLOSES, and it was a silent one. Assets::CloudProceduralCellExtentKm floors a species'
     * placement cell at four voxels — a cluster narrower than the trilinear filter's own support cannot be
     * carried by the volume at all — and that floor is `std::max( cell, 4 * regionSize / side )`. It is a
     * MIDDLE LINK THAT DROPS A PROPERTY: the type authors a cell, the grid quietly enlarges it, and both
     * ends look right. The symptom is not a blurrier sky, it is a DIFFERENT one — the lattice the clusters
     * are placed on has a different pitch, so the clouds are in different places.
     *
     * Measured on the shipped library at the 48 km region: the floor is 0.75 km at 256 voxels and 1.50 km
     * at 128, and two of the nine shipped types sit under the second — Altocumulus authors 0.90 km (x1.67)
     * and Stratocumulus 1.05 km (x1.43). Rendering SIL_Altocumulus at 128 against 256 from the same camera
     * moves 99.77 % of the frame, mean 35.1 of 255 against a repeat-shot floor of exactly 0, and the two
     * frames are not the same sky with softer edges: the holes are elsewhere. The asset preview bakes at
     * 128 (Editor PreviewViewport), so an artist tuning either of those two types was authoring against
     * clouds the level would never draw.
     *
     * THE ANSWER IS TO RAISE THE GRID, NOT TO ACCEPT THE CLAMP, and it only ever raises: a budget is a
     * promise about cost, and a cost is the one thing a preview may legitimately differ on. What it may not
     * do is show a different field. The other seven types are unaffected and keep the cheap bake, which is
     * the measured saving O8 shipped (229 ms against 961 ms).
     *
     * THE LADDER IS THE TWO RUNGS THIS SUBSYSTEM HAS MEASURED — the component's own Range, 128 and 256.
     * An intermediate side would be a number nobody has priced or shot.
     *
     * @param regionSizeKm  the baked region across, kilometres
     * @param species       the resolved species, whose CellKm is the authored placement pitch
     * @param authoredSide  the side the view asked for, already clamped to the component's Range
     */
    inline uint32_t CloudBakeSideForSpecies( float                                              regionSizeKm,
                                             const std::vector<Assets::CloudProceduralSpecies>& species,
                                             uint32_t                                           authoredSide )
    {
        for ( const Assets::CloudProceduralSpecies& one : species )
        {
            // Solve the voxel half of CloudProceduralCellExtentKm's floor for the side: the cell survives
            // while `4 * regionSizeKm / side <= cell`. The CHORD half of that floor is a different relation
            // — what the march can find — and no grid can buy it off, so it is deliberately not read here.
            const float neededSide = 4.0f * std::max( regionSizeKm, 0.0f ) / std::max( one.CellKm, 1e-3f );
            if ( neededSide > static_cast<float>( authoredSide ) )
                return static_cast<uint32_t>( Assets::kCloudProceduralVolumeSide );
        }
        return authoredSide;
    }

    /**
     * @brief Everything the procedural bake takes FROM THE MATERIAL, and nothing it takes from a service.
     *
     * WHY THIS IS A FREE FUNCTION AND NOT A RENDERER METHOD. It is the operative half of the answer to the
     * one question a cloud parameter cannot answer for itself: *does moving this knob re-run the bake?* The
     * schema states that answer as ShaderParamTiming, and a stated answer with no way to check it is the
     * exact shape of the nine comments in this tree that promised a guarantee the code did not give. With
     * the material half of the bake isolated here, Desert/Tests/Engine/CloudMaterialTiming can perturb one
     * field of CloudMaterialValues at a time, run this, and ask Assets::CloudProceduralParamsEqual — which
     * is the renderer's OWN rebake decision — whether the volume has to be built again. The declared
     * timing and the measured one are then asserted equal, so a parameter cannot be labelled "Immediate"
     * while costing seconds, in either direction.
     *
     * WHAT IS DELIBERATELY NOT HERE: the two `.dclayout` slots and the four cloud type slots. Those are
     * ASSET HANDLES that only become values by way of Runtime::ResourceRegistry, and a header that resolved
     * them would drag a service into a function whose whole purpose is to be callable without one. They
     * arrive already resolved — the paintings as @p params.PatternSource / MaskSource which the caller
     * fills, the types as @p shapes — and the test drives them through those same two inputs.
     *
     * @param look          the layer's resolved material values
     * @param layer         the component-side budget and world facts (see CloudBakeLayerInputs)
     * @param shapes        the resolved species shapes, @p speciesCount of them
     * @param speciesCount  0..kCloudSpeciesSlots
     * @param params        filled in place; the caller has already set PatternSource / MaskSource
     */
    inline void ApplyCloudMaterialToBakeParams( const CloudMaterialValues& look, const CloudBakeLayerInputs& layer,
                                                const CloudTypeShape* shapes, uint32_t speciesCount,
                                                Assets::CloudProceduralFieldParams& params )
    {
        params.RegionSizeKm = std::max( layer.RegionSize, 1.0f ) / kCloudWorldUnitsPerKm;

        // THIS VIEW'S BAKE BUDGET, clamped to the component's own Range for the reason the four placement
        // numbers below state: a scene file is a text file and an out-of-range number in one must produce a
        // sky rather than a refusal. It is RAISED again below, once the species are known, if this grid
        // could not express one of their cells — see CloudBakeSideForSpecies.
        const uint32_t authoredSide = static_cast<uint32_t>(
             std::clamp( layer.VolumeResolution, static_cast<int32_t>( Assets::kCloudProceduralVolumeSideMin ),
                         static_cast<int32_t>( Assets::kCloudProceduralVolumeSide ) ) );
        params.VolumeSideVoxels = authoredSide;

        // THE SHELL, TAKEN FROM THE SPECIES AND NOT FROM THE COMPONENT, because that is where the packer
        // takes it from too: the layer's geometry is the UNION of its types' altitude ranges (decision
        // D-13's envelope), and a volume spread over a different shell than the one the march intersects
        // would put every cloud at the wrong altitude — the "sky was a ceiling" defect in a new costume.
        const CloudEnvelopeKm envelope = CloudTypeSetEnvelopeKm( shapes, speciesCount );

        params.LayerBottomKm    = std::max( envelope.BottomKm, 0.0f );
        params.LayerThicknessKm = std::max( envelope.TopKm - params.LayerBottomKm, 0.001f );

        params.Coverage         = std::clamp( look.Coverage, 0.0f, 1.0f );
        params.CoverageContrast = std::max( look.CoverageContrast, 0.01f );
        params.Seed             = static_cast<uint32_t>( look.Seed );

        // THE FOUR PLACEMENT NUMBERS PASS THROUGH UNCHANGED, and the clamps here are the component's own
        // ranges rather than second opinions: a scene file is a text file and an out-of-range number in
        // one must produce a sky rather than a refusal. Assets::ValidateCloudProceduralParams refuses
        // anything outside them by name, so a clamp that disagreed with a range would turn an artist's
        // typo into a layer that never bakes.
        params.PlacementDensity     = std::clamp( look.PlacementDensity, 0.25f, 8.0f );
        params.PlacementScatter     = std::clamp( look.PlacementScatter, 0.0f, 4.0f );
        params.PlacementSizeVariety = std::clamp( look.PlacementSizeVariety, 0.0f, 1.0f );
        params.PatchStrength        = std::clamp( look.PatchStrength, 0.0f, 1.0f );

        // THE PATCH IS THE ONE THAT CAN REFUSE, because it is half of a RELATION — a modulation finer than
        // three cells decides cells one at a time and reads as a checkerboard. Floored against the
        // lattice HERE rather than left to fail validation, for the same reason: the layer has to draw a
        // sky for whatever the file says. An artist who wants finer patches gets them by shrinking the
        // weather tile, which is what the tooltip names.
        params.PatchTileKm = std::max( look.PatchTileSize, 1.0f ) / kCloudWorldUnitsPerKm;

        // THE BLEND RADIUS AND THE PROFILE DEPTH ARE DERIVED FROM THE LATTICE rather than exposed, and
        // that is a decision with a number behind it. The join inflates its own surface by
        // `BlendRadius * ln(sum of weights in range)`, so with hundreds of overlapping lumps a generous
        // radius does not soften a crease, it floods the sky — at a 3 km cell and 24 lumps in range, a
        // radius of a fifth of the cell would dilate every body by 1.9 km. Two per cent of the cell keeps
        // that dilation under 200 m while still fusing lobes that already overlap, which is where the
        // fusion comes from. An artist who wants softer clouds has Detail Strength, which is the knob that
        // means it.
        // ONE STATEMENT OF "four cells to a tile", shared with the Cloud Layout panel, which measures a
        // painting's strokes against the cell and must not compute the ratio a second time.
        const float latticeKm = ECS::CloudLayerLatticeKm( look.WeatherTileSize );

        params.BlendRadiusKm  = std::max( 0.02f * latticeKm, 1e-3f );
        params.ProfileDepthKm = std::max( 0.12f * latticeKm, 1e-3f );

        // THE MARCH'S OWN SEARCH STEP, handed in rather than assumed by the generator. It is one half of
        // the relation this programme has been bitten by twice — what the field places against what the
        // ray can find — and taking it from the component's Max Steps is what makes an artist who lowers
        // that number get coarser lumps rather than speckle.
        params.ResolvableChordKm =
             CloudFinestResolvableChordKm( static_cast<float>( std::clamp( layer.MaxSteps, 8, 512 ) ) );

        params.WindAxis = glm::vec2( layer.WindDirection.x, layer.WindDirection.z );

        params.Species.clear();
        params.Species.reserve( speciesCount );
        for ( uint32_t slot = 0; slot < speciesCount; ++slot )
        {
            Assets::CloudProceduralSpecies species;
            species.Shape = shapes[slot];
            // A TYPE STATES HOW MUCH COARSER OR FINER THAN THE LAYER IT IS, which is what Placement Scale
            // has always meant, and the layer's own tile is the pair Max View Distance is calibrated
            // against (CALIBRATION.md §4). Four cells to a tile, which is the ratio the component's own
            // tooltip has stated since T1: "12 km -> 3 km cells, a cumulus field".
            species.CellKm     = latticeKm * std::max( shapes[slot].PlacementScale, 1e-3f );
            species.Anisotropy = std::max( shapes[slot].PlacementAnisotropy, 1e-3f );
            params.Species.push_back( species );
        }

        // THE GRID AGAINST THE CELLS, and it has to be here: the cell is what the relation is against, so
        // the side cannot be settled before the species are known, and CloudProceduralCellExtentKm — which
        // the patch floor below calls — reads the side it settles on.
        params.VolumeSideVoxels = CloudBakeSideForSpecies( params.RegionSizeKm, params.Species, authoredSide );

        // THE PATCH AGAINST THE LATTICE, floored after the species are known because the CELL is what the
        // relation is against and a type's Placement Scale and Anisotropy both move it. Three cells is the
        // bound Assets::ValidateCloudProceduralParams refuses below: a modulation whose period is near a
        // cell's decides cells one at a time, which is a checkerboard and not a weather system.
        for ( const Assets::CloudProceduralSpecies& species : params.Species )
        {
            const glm::vec2 extent = Assets::CloudProceduralCellExtentKm( params, species );
            params.PatchTileKm     = std::max( params.PatchTileKm, 3.0f * std::max( extent.x, extent.y ) );
        }

        // THE PAINTED LAYOUT'S PLACEMENT. The two SOURCES are the caller's — resolving a handle needs a
        // service — but every number that positions a painting is the material's and is applied here so
        // that the timing census can reach it.
        //
        // THE CLAMPS ARE THE COMPONENT'S OWN RANGES rather than second opinions, for the reason the four
        // placement numbers state above them: a scene file is a text file, an out-of-range number in one
        // must produce a sky rather than a refusal, and Assets::ValidateCloudLayoutPlacement refuses
        // anything outside them by name — so a clamp that disagreed with a range would turn a typo into a
        // layer that never bakes.
        params.LayoutPlacement.RepeatsPerRegion = static_cast<uint32_t>( std::clamp( look.LayoutRepeats, 1, 16 ) );
        params.LayoutPlacement.QuarterTurns     = static_cast<uint32_t>( std::clamp( look.LayoutRotation, 0, 3 ) );
        params.LayoutPlacement.OffsetKm =
             glm::vec2( look.LayoutOffset.x, look.LayoutOffset.y ) / kCloudWorldUnitsPerKm;
        params.LayoutPlacement.PatternStrength = std::clamp( look.LayoutPatternStrength, 0.0f, 1.0f );
        params.LayoutPlacement.MaskStrength    = std::clamp( look.LayoutMaskStrength, 0.0f, 1.0f );
    }
} // namespace Desert::Graphic
