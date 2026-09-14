#pragma once

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>

#include <Engine/Assets/Common.hpp>
#include <Engine/Reflection/ReflectionMacros.hpp>

#include <cstdint>

namespace Desert::ECS
{
    // The volumetric cloud layer: a spherical shell around the planet, marched per pixel by
    // Graphic::System::VolumetricCloudRenderer.
    //
    // THE LOOK LIVES IN THE MATERIAL, NOT HERE (O1, decision D-35). This component carries exactly what
    // UVolumetricCloudComponent carries — tracing budgets, pass routing and world integration — and one
    // `Material` handle, which is the same split UE ships: not one property below describes the cloud's
    // shape, colour, density, phase or weather. All of that is the schema of the Volume-domain shader
    // (Editor/Resources/Shaders/Programs/Clouds/CloudRaymarch.shader, Properties block), authored in a
    // `.demat` and resolved by the renderer through Runtime::MaterialService. An EMPTY material slot is
    // the schema's own defaults — a scene must not depend on a file being present.
    //
    // The v11 -> v12 scene migration moved thirty-three fields from this struct into that schema, names
    // unchanged, values verbatim (Tools/SceneMigrator). Their calibration histories are in this file's own
    // git history before O1-A and in Docs/Clouds/CALIBRATION.md.
    //
    // WIND STAYS HERE DELIBERATELY, and it is the one named divergence from the UE split (UE scrolls its
    // material by Time in the graph). Our wind offset is scene state ACCUMULATED by the ECS collector
    // (VolumetricCloudECSSystem::AdvanceWind), and an ECS system must not touch Runtime::ResourceRegistry
    // — that rule is stated at the collector's own declaration. Accumulating in the renderer instead
    // would let two viewports of one scene drift apart.
    //
    // EVERY FIELD BELOW IS READ, and Desert/Tests/Engine/SettingConsumers names the consumer; the schema
    // params have their own census (Desert/Tests/Engine/CloudMaterialSchema) on the same terms.
    //
    // UNITS. Distances are world units — centimetres (Length) — and are converted to kilometres exactly
    // once, in Graphic::PackCloudParams. The planet radius is the one exception and is authored in
    // kilometres, because 6360 km is 636 000 000 cm and no slider is useful at that scale; UE authors it
    // in kilometres for the same reason.
    //
    // ALTITUDES ARE ANCHORED TO METEOROLOGY, not to a relation, and they are NOT AUTHORED HERE. A cloud
    // TYPE carries its own base and top in kilometres, and the shell the march intersects is COMPUTED from
    // them by Graphic::PackCloudParams — the types are named by the material's CloudType1..4 slots now,
    // and nothing that has to agree with the type is left for a hand to move (§2.3.1).

    // The top of the shadow ray's sample count, in ONE place because THREE of them disagreed. The slider's
    // Range, the clamp in Graphic::PackCloudParams and the clamp in Programs/Clouds/CloudRaymarch.shader
    // are three copies of one number, and while they were three literals an artist could drag the slider
    // to a value the shader silently threw away. The first two now read this constant; the shader cannot,
    // so Desert/Tests/Engine/SettingConsumers reads the shader's text and asserts the literal matches —
    // §2.3.1's "two values obliged to agree", asserted rather than remembered.
    //
    // SIXTY-FOUR because that is where the measurement stops moving: 48 and 64 render the sunward zenith
    // p95 identically to three decimals (0.798), so nothing above it is buying anything. Unreal's own
    // ceiling is 80 (r.VolumetricCloud.Shadow.ViewRaySampleMaxCount); the difference is not a shortfall,
    // it is the plateau measured on OUR quadrature instead of copied from theirs.
    inline constexpr int32_t kCloudLightMarchMaxSamples = 64;

    // The top of the multiple-scattering series, on exactly the terms above and for the same reason: the
    // slider's Range, the clamp in Graphic::PackCloudParams and the clamp inside
    // Common/CloudLighting.glslh's CloudMultiScatterStep are three copies of one number. They were three
    // literal 3s and agreed only by inspection; Desert/Tests/Engine/SettingConsumers now reads the
    // header's text and fails if they part company.
    //
    // THREE, AND Р18 MEASURED WHY IT CANNOT SIMPLY BE RAISED. Against a converged Monte Carlo of a
    // homogeneous lobe, running the SHIPPED decay rule out to six octaves makes the picture WORSE, not
    // better: each further octave's extinction factor is the previous one squared times the last step, so
    // by the fourth it is 6.1e-5 and the term is a perfectly uniform glow with no direction and no
    // gradient in it. Measured on the lobe at 45 /km, the tonal contrast error against the reference goes
    // -0.228 at three octaves to -0.290 at six. More terms of this particular series buy flatness.
    inline constexpr int32_t kCloudMultiScatterMaxOctaves = 3;

    // How far a scene may lift its deck above the altitudes its cloud types were authored at, in ONE place
    // for the reason the two constants above give: the slider's Range and the clamp in
    // Graphic::CloudLiftSpeciesSet are two copies of one number, and while they were two literals an
    // artist could type a value into the `.desce` that the packer silently threw away.
    //
    // TWELVE KILOMETRES IS THE SHIPPED LIBRARY'S OWN VERTICAL SPAN, rounded up, and not a taste. The nine
    // `.decloudtype` files run from the stratus deck's base at 0.15 km to the cumulonimbus canopy's
    // ceiling at 11.30 km (9.50 + 1.80) — 11.15 km end to end. A ceiling of twelve therefore lets a scene
    // put ANY of them anywhere the library itself reaches, and buys nothing beyond that: a deck lifted
    // further would sit above every altitude this project has a cloud for.
    inline constexpr float kCloudLayerAltitudeOffsetMaxKm = 12.0f;

    struct VolumetricCloudData
    {
        REFLECT()

        // ---- Cloud Layer ----------------------------------------------------------------------------

        PROPERTY( DisplayName( "Enabled" ), Category( "Cloud Layer" ), Summary,
                  Tooltip( "Master switch. Off dispatches nothing: a scene with the clouds disabled pays "
                           "zero GPU cost, exactly like a scene without the component." ) )
        bool Enabled = true;

        // ---- Materials ------------------------------------------------------------------------------

        // THE SEAM ITSELF — the one field O1 added while thirty-three left. Hidden from the reflected
        // Details pass on the same terms as TerrainData::Material: the builder's asset slot is
        // texture-oriented, and the cloud entry draws a material row with New/Edit/Clear that opens the
        // Material Editor window (ComponentEditorRegistrations.cpp), which after Stage 3 is the only
        // place a material is authored. Still serialized; Hidden is editor-only.
        //
        // Read by Graphic::System::VolumetricCloudRenderer::SetCloudSettings, which resolves it through
        // Runtime::MaterialService::ResolveOverrides into Graphic::CloudMaterialValues — schema defaults
        // first, the `.demat`'s values over them. Null means the defaults, which are byte-for-byte the
        // component defaults the moved fields used to carry.
        PROPERTY( DisplayName( "Material" ), Category( "Materials" ), Asset<MaterialAsset>, Hidden )
        Assets::AssetHandle Material;

        PROPERTY( DisplayName( "Planet Radius" ), Category( "Cloud Layer" ), Units( "km" ),
                  Range( 100.0f, 7000.0f ), Advanced,
                  Tooltip( "Radius of the planet the layer curves around. It is what puts the horizon "
                           "where it belongs: a flat layer has no horizon at all and either fills the "
                           "whole lower sky or ends at an invisible edge." ) )
        float PlanetRadius = 6360.0f;

        PROPERTY( DisplayName( "Layer Altitude Offset" ), Category( "Cloud Layer" ), Units( "km" ),
                  Range( 0.0f, ::Desert::ECS::kCloudLayerAltitudeOffsetMaxKm ),
                  Tooltip( "Raises this SCENE's deck above the altitudes its cloud types were authored "
                           "at. Zero is exactly where the types say. It is an offset and not an "
                           "altitude, so swapping a type still gets that type's own band — lifted by "
                           "this much. Raising the base makes every cloud SMALLER on screen, because "
                           "the cloud itself does not grow: to keep the apparent size, multiply the "
                           "material's Weather Tile Size by the same ratio the base grew by." ) )
        // WHY AN OFFSET AND NOT A LAYER BOTTOM. Unreal authors the shell directly (Layer Bottom + Layer
        // Height on the component) because its types do not carry altitudes; ours do, and the shell is
        // DERIVED from them in one place — Graphic::CloudTypeSetEnvelopeKm, the union of the bands of the
        // types the layer actually carries. An absolute Layer Bottom here would be a SECOND author for
        // that same shell, and the symptom of the two disagreeing is not an error: it is a cumulonimbus
        // with its anvil sliced off by a ceiling nobody remembers setting (the defect commit 54330ab9
        // fixed, and §4.2's double write one component further out).
        //
        // A DISPLACEMENT IS A DIFFERENT QUANTITY FROM AN ALTITUDE, so it cannot disagree with one. The
        // types keep saying how tall and how thick; the scene says how high the whole thing sits. Change
        // the type set and the new bands are lifted by the same amount — the authored value stays
        // meaningful instead of becoming a stale absolute, and nothing silently wins.
        //
        // ZERO TO TWELVE KILOMETRES, one-directional on purpose. Lowering would have to stop at sea
        // level — the shell is floored at zero in both Graphic::PackCloudParams and
        // Graphic::ApplyCloudMaterialToBakeParams while the bodies are not — so the usable travel of a
        // negative side would depend on which types were in the slots, which is a slider that lies about
        // its own range. A deck lower than its types is asking for a different kind of cloud, and the
        // `.decloudtype` is where that is said.
        float LayerAltitudeOffset = 0.0f;

        PROPERTY( DisplayName( "Max View Distance" ), Category( "Cloud Layer" ), Length,
                  Range( 100000.0f, 40000000.0f ),
                  Tooltip( "How far along the ray the march may run, measured FROM THE POINT THE RAY "
                           "ENTERS THE LAYER. Measured from the camera instead it would cut the layer at "
                           "a fixed radius and draw a circular edge across the sky." ) )
        // SIXTY KILOMETRES, and it is half of a PAIR rather than a number of its own. Divided by
        // WeatherTileSize it gives the number of times the coverage field REPEATS between the camera and
        // the vanishing point, and a repeating field seen end-on reads as streaks radiating from that
        // point. Docs/Clouds/CALIBRATION.md section 4 records the failure and its cure: at 150 km against
        // an 8 km tile the field repeated about twenty times and the moire was unmissable, and the pair
        // that fixed it was 60 km against 12 km — five repeats. These defaults ARE that pair, and
        // ComponentReflection asserts the ratio rather than the two numbers, because it is the ratio that
        // was measured.
        float MaxViewDistance = 6000000.0f; // 60 km

        PROPERTY( DisplayName( "Tracing Start Max Distance" ), Category( "Cloud Layer" ), Length,
                  Range( 1000000.0f, 100000000.0f ), Advanced,
                  Tooltip( "If a ray only enters the layer beyond this distance, it is not traced at all. "
                           "It bounds the cost of grazing rays, and it is the guard that keeps a ray whose "
                           "entry the geometry reports thousands of kilometres away from ever being "
                           "marched." ) )
        float TracingStartMaxDistance = 35000000.0f; // 350 km — UE's shipped default

        PROPERTY( DisplayName( "Tracing Start Distance" ), Category( "Cloud Layer" ), Length,
                  Range( 0.0f, 5000000.0f ), Advanced,
                  Tooltip( "Pushes the start of the march away from the camera. Useful when the camera "
                           "is inside the layer and the nearest metres are both the most expensive and "
                           "the least interesting." ) )
        float TracingStartDistance = 0.0f;

        // ---- Weather --------------------------------------------------------------------------------

        // THE TWO FIELDS THAT USED TO STAND HERE ARE GONE, and neither has moved anywhere. `Cloud Type`
        // was a scalar between "flat sheet" and "heaped cloud" that fed one analytic curve, and
        // `Cloud Type Variance` mixed noise into it so that neighbouring clouds would not all reach the
        // same ceiling. Both are answered by the profile TABLE and answered better: the table's second
        // axis is the placement pattern's own value, so a cloud is low and flat at the rim of a patch and
        // a tower in its middle — height that is CORRELATED with how much cloud is there, instead of
        // height sprinkled by a noise that knew nothing about the patch it was decorating. A tower on a
        // thin edge was the visible cost of the old arrangement.
        //
        // A THIRD FIELD WAS BUILT HERE AND MEASURED AWAY. `Shape Distortion` drove Unreal's
        // height-dependent domain warp — the second noise that displaces the coverage field's coordinates
        // so that one flat pattern cannot be extruded upward into a column. Over the WHOLE travel of the
        // control it moved ImageStat contrast by at most 0.018 and with no consistent sign, against the
        // 0.08 the profile table itself moved and the 0.2 that separates two species; the six rows of
        // numbers are in Common/CloudField.glslh. It buys nothing HERE because our coverage field is
        // already sampled in full three dimensions, which is a different solution to the same problem —
        // and it cost one fetch of the noise volume per sample, in the march and in every light-march
        // sample beneath it. A control that moves nothing is what §1.3 of the contract calls a TODO
        // wearing a feature's clothes.

        PROPERTY( DisplayName( "Region Size" ), Category( "Weather" ), Length, Range( 1600000.0f, 12000000.0f ),
                  Advanced,
                  Tooltip( "How much world the camera-centric modelling volume covers, and the distance "
                           "over which the sky repeats beyond it. Larger is more sky before the "
                           "repetition shows and a coarser voxel; the volume's 256 voxels are spread over "
                           "exactly this." ) )
        // FORTY-EIGHT KILOMETRES, and it is half of a relation rather than a taste.
        //
        // BELOW, by what the march can find: the volume is 256 voxels across, trilinear filtering cannot
        // express a feature narrower than two of them, and the march SEARCHES at
        // CloudFinestResolvableChordKm — 125 m at the default Max Steps. A region under 16 km would put
        // structure in the volume that no ray can be relied on to sample, and
        // Assets::ValidateCloudProceduralParams refuses it by name. The slider's floor is that bound.
        //
        // ABOVE, by the repetition: the volume is periodic, so past the region the sky is the region
        // again. Max View Distance over this number is how many times it repeats between the camera and
        // the vanishing point — 60 over 48 is 1.25, against the five repeats CALIBRATION.md §4 measured
        // as the cure for the moire at twenty. At the shipped pair the voxel is 187.5 m.
        float RegionSize = 4800000.0f; // 48 km -> 187.5 m per voxel, 1.25 repeats to the vanishing point

        // ---- Detail ---------------------------------------------------------------------------------
        //
        // The Placement, Layout and per-sample Detail knobs that stood between Weather and here are
        // MATERIAL schema now (CloudRaymarch.shader's Properties, O1) — where a cloud is and what its
        // edge is are the look, and the look is authored in the `.demat`. What remains in this category
        // is march control: the near-camera fade pair, which bounds what the camera meets, not what the
        // cloud is.

        PROPERTY( DisplayName( "Near Fade Start Distance" ), Category( "Detail" ), Length,
                  Range( 0.0f, 2000000.0f ), Advanced,
                  Tooltip( "Where the near-camera fade begins. A camera that enters the layer otherwise "
                           "meets a wall of density at arm's length; UE fades the nearest metres out for "
                           "the same reason. The fade is OFF unless End is strictly past Start." ) )
        float NearFadeStartDistance = 0.0f;

        PROPERTY( DisplayName( "Near Fade End Distance" ), Category( "Detail" ), Length, Range( 0.0f, 2000000.0f ),
                  Advanced,
                  Tooltip( "Where the near-camera fade is complete and the cloud is at full density. It "
                           "must lie strictly past Start; at or below it the pair describes no interval "
                           "and the fade is switched OFF rather than guessed at." ) )
        // THE TWO ARE ONE SETTING. Graphic::CloudResolveNearFade repairs them as a pair, because the march
        // evaluates smoothstep(Start, End, t) and GLSL leaves that undefined unless End is strictly past
        // Start — and each of the two is individually legal at any value its own slider allows.
        float NearFadeEndDistance = 0.0f;

        // ---- Lighting -------------------------------------------------------------------------------

        PROPERTY( DisplayName( "Sky Occlusion Volume" ), Category( "Lighting" ),
                  Tooltip( "Occlude the sky light by the cloud STANDING OVER a sample instead of by the "
                           "sample's own depth inside its body. Off, a sample under three kilometres of "
                           "congestus receives exactly what a sample under clear sky receives, so a deck "
                           "has no dark side and the clouds read as flat white lobes. On, a second compute "
                           "pass builds a 128x16x128 volume of how much cloud stands over every column and "
                           "the march reads it — which costs one dispatch and two megabytes per view.\n\n"
                           "On by default. Turn it off for a layer whose cloud never stands over anything "
                           "— a thin cirrus veil — and the dispatch and the two megabytes are not paid at "
                           "all." ) )
        // DEFAULT ON SINCE Р12, and the frame with it OFF is still exactly the frame without this feature
        // — the march's gate is a push-constant flag and the pass is not dispatched, so a layer that turns
        // it off allocates nothing and reads nothing. That is what keeps the A/B in any report a property
        // of one binary rather than of two, and it is the same arrangement Unreal ships its own second
        // volume under.
        //
        // WHY IT IS ON. Decision D-26 held it off pending Р9 and Р9 closed as a refusal, so the hold had
        // nothing left to hold. The argument that decided it is not about one sky: every measurement this
        // programme takes is taken against the SHIPPED one, and with this off that sky's largest single
        // discrepancy — Docs/Clouds/DIAGNOSIS_CARTOON.md's ranked #1, an ambient term with no geometric
        // occluder — was in every frame anybody measured. Р11 spent about nine hundred captures against
        // it. The price is one dispatch of 0.410 ms and a FIXED 2.00 MiB per view (128x16x128, not
        // resolution-scaled): 20.5 % of decision D-9's 2 ms budget and 3.1 % of its 64 MB.
        //
        // WHAT IT IS STILL A FLAG FOR. The local term is not wrong — Р0 measured it recovering 34 % of the
        // gap at the sunward zenith, which is exactly where the occluder IS the sample's own body — and
        // the volume costs a dispatch a thin veil has no use for. Turning it off is an authoring choice
        // with a cost attached, which is what a flag is; it is no longer the default because the default
        // is what every unauthored scene and every future measurement inherits.
        //
        // AND IT IS NOT A CLEAN WIN ON EVERY STATISTIC, which is recorded here rather than left to be
        // rediscovered. Measured over the whole dome on Clouds_Protocol (Docs/Clouds/CALIBRATION.md §Р12):
        // all forty tiles darken, and p95-p05 contrast FALLS at thirty-four of them. That reading is an
        // artefact of the statistic and the mechanism is exact — where a frame contains clear sky, p05 IS
        // the sky, this term touches only cloud, so p05 cannot move while the sunlit top falls a little.
        // The six tiles that gain contrast are exactly the six whose p05 is deck rather than sky. Of the
        // protocol's own six points, four close 34/29/24/9 % of the gap to the UE reference and two lose
        // 0.022 and 0.012. A scene authored against the flat frame may want its exposure looked at once.
        //
        // Read by Engine/Graphic/Systems/Scene/Clouds/VolumetricCloudRenderer.cpp, which decides whether
        // to dispatch Editor/Resources/Shaders/Programs/Clouds/CloudSkyOcclusionVolume.shader, and by
        // Editor/Resources/Shaders/Programs/Clouds/CloudRaymarch.shader through CloudPush::Frame.
        bool SkyOcclusionVolume = true;

        PROPERTY( DisplayName( "Per Sample Atmosphere Transmittance" ), Category( "Lighting" ),
                  Tooltip( "Re-evaluate how much of the sun the ATMOSPHERE has already absorbed at every "
                           "sample's own altitude, instead of using the single value computed on the "
                           "ground.\n\nOff, the whole shell — kilometres of it — is lit by the colour the "
                           "sun has at sea level, so a deck at four kilometres is reddened by air it is "
                           "standing above. On, each sample reads the atmosphere's transmittance LUT at "
                           "its own height and zenith angle, which brightens and de-reddens cloud with "
                           "altitude; the difference is largest at a low sun and vanishes at noon.\n\nOff "
                           "by default, as Unreal ships it: it adds one texture fetch to every sample of "
                           "the view march. Needs the Physical Atmosphere sky model — with the artistic "
                           "gradient there is no transmittance to read and the flag does nothing." ) )
        // DEFAULT OFF, WHICH IS UNREAL'S DEFAULT for the same field
        // (UVolumetricCloudComponent::bUsePerSampleAtmosphericLightTransmittance), and the frame with it
        // off is byte-for-byte the frame without this feature: the packer sends the same ground-level sun
        // colour it always sent, and every march's gate is a flag that skips the fetch entirely. That is
        // what makes an A/B a property of one binary rather than of two.
        //
        // WHAT IT ACTUALLY CHANGES. UE's default sun colour for clouds is
        // `OuterSpaceIlluminance * T(groundLevel)` — one colour for the whole shell, which this engine
        // packs at Graphic::PackCloudParams from AtmosphereEnv::SunIlluminanceOnGround. Turning this on
        // swaps the base for AtmosphereEnv::SunOuterSpaceIlluminance and moves the transmittance into the
        // march, where it becomes `T(sample)` — the ABSOLUTE form, with no division anywhere. The ratio
        // form `T(sample)/T(ground)` is what an engine whose base was NOT already multiplied by T(ground)
        // would need, and it is an infinity at a low sun in whichever channel's transmittance reaches zero
        // first.
        //
        // Read by Engine/Graphic/Clouds/CloudPayload.hpp, which chooses which of the two illuminances the
        // parameter block carries, and applied by BOTH marches of the field: the screen march
        // (Editor/Resources/Shaders/Programs/Clouds/CloudRaymarch.shader, through CloudPush::Frame) and
        // the environment bake (Editor/Resources/Shaders/Programs/Compute/BakeProceduralSky.shader,
        // through CloudBakeBinding::PerSampleSunTransmittance). Both, because they share the ONE packed
        // block: a bake that did not apply the transmittance would light the IBL panorama with the sun's
        // outer-space illuminance and nothing else.
        bool PerSampleAtmosphereTransmittance = false;

        PROPERTY( DisplayName( "Light March Distance" ), Category( "Lighting" ), Length,
                  Range( 10000.0f, 2000000.0f ),
                  Tooltip( "How far toward the sun the shadow ray marches. Short rays light the cloud "
                           "flatly because they never leave its own body; long ones cost proportionally "
                           "more per sample, and past the thickness of the layer they buy nothing at all "
                           "because the ray has already left it." ) )
        // FIFTEEN KILOMETRES, which is UE's ShadowTracingDistance, and the previous 500 m was the reason
        // the clouds were flat. A shadow ray started inside a two-kilometre cloud and only 500 m long
        // never leaves it: every sample inside the body sees roughly the same optical depth, so the body
        // is shaded uniformly and reads as a cut-out. At fifteen kilometres a sample near the top exits
        // into clear air almost at once and is bright, while one near the base has the whole cloud above
        // it and is dark — and that difference IS the shape.
        //
        // LENGTHENING THIS RAY COARSENS ITS NEAR FIELD BY THE SAME FACTOR, and the sample count has to
        // move with it. The samples are on a SQUARED distribution, so over a march of length M with N
        // samples the FIRST segment is M / N^2 — the resolution nearest the shaded point, which is where
        // almost all of the material is. At the old 500 m and six samples that was 13.9 m; at fifteen
        // kilometres and the same six it is 417 m, thirty times coarser, and a shadow ray whose first
        // step steps straight over the cloud it starts inside reports the cloud transparent to the sun.
        // See LightMarchSamples below and Docs/Clouds/CALIBRATION.md section OE-FIX for the frames.
        float LightMarchDistance = 1500000.0f; // 15 km

        PROPERTY( DisplayName( "Light March Samples" ), Category( "Lighting" ),
                  Range( 1, ::Desert::ECS::kCloudLightMarchMaxSamples ),
                  Tooltip( "Samples along the shadow ray toward the sun. They are placed on a squared "
                           "distribution, so the first segment is the march length over the SQUARE of "
                           "this number — which is why a long ray needs many more of them than a short "
                           "one. Below about 24 the sunward highlights blow out; above 48 nothing "
                           "measurable changes. This is the hottest loop in the subsystem: the ray is "
                           "traced at every sample inside every cloud, and the cost is linear in this "
                           "number." ) )
        // THIRTY-TWO, and the number is a measurement rather than a preference. Rendered p95 of the
        // zenith frame looking INTO the sun, Clouds_Demo, in linear scene radiance after exposure (the
        // 8-bit scale is nearly flat above 0.95 and hides the size of this entirely):
        //
        //     N          6      10      16      24      32      48      64
        //     linear   4.29    2.27    1.29    1.05    0.99    0.97    0.97
        //     x conv   4.42    2.35    1.34    1.08    1.02    1.00    1.00
        //
        // Thirty-two is the first value on the plateau — 2% from converged, and 1% from the 0.979 the UE
        // reference frame's own p95 implies. Sixteen, the ceiling this component used to carry, is still
        // 34% too bright; ten, which is Unreal's BaseShadowRaySampleCount, is still 135% too bright on
        // OUR quadrature. The away-from-sun azimuth converges by ten and hides the whole defect, which is
        // why it went unseen: the error is a multiplier on sun visibility and the sunward phase function
        // is ~16x the away one.
        //
        // THE COST IS REAL AND IS THE ENTRY TO A QUALITY TIER. Measured by the frame-count slope on a
        // debug build at 1280x766 (machine shared): 0.23 ms of frame time per shadow sample, so 6 -> 32
        // is 7.1 -> 12.9 ms/frame, +81%. A tier that wants the speed back should lower THIS number first
        // — 24 costs 4.0 ms less than 32 and is 7% from converged — and it is the reason the range now
        // reaches 64 rather than stopping where the defect lived.
        int32_t LightMarchSamples = 32;

        PROPERTY( DisplayName( "Aerial Perspective Start Distance" ), Category( "Lighting" ), Length,
                  Range( 0.0f, 20000000.0f ), Advanced,
                  Tooltip( "Distance inside which the clouds are shown through NO atmosphere at all. Zero "
                           "— the default here and in UE — is the honest answer: the cloud is seen through "
                           "the same air as the sky beside it. Raise it only to keep a distant band louder "
                           "than the air allows, and know that it removes the haze from everything nearer "
                           "as well." ) )
        // ZERO, AND THE JUSTIFICATION FOR THE OLD DEFAULT WAS MEASURED AND FOUND FALSE (D21). What stood
        // here claimed that at 0 "ninety kilometres of air erases a cloud on the horizon completely". Our
        // own medium says otherwise: walking the aerial-perspective volume with the shipped Earth
        // parameters, the mean transmittance along a horizon ray is 0.81 at 10 km, 0.56 at 30 km, 0.36 at
        // 60 km and 0.24 at 90 km — and 60 km is where the clouds STOP, because that is what
        // MaxViewDistance holds in every scene in the repository. A third of the contrast surviving is a
        // hazed band, not an erased one, and no cloud in this engine is ever at 90 km to begin with.
        //
        // The knob is real and stays: it is UE's (AerialPespectiveRayleighScatteringStartDistance, also 0
        // by default), and a sky that wants its far band louder than the air allows is a legitimate
        // request. What it is NOT is a fix for the atmosphere being too strong — the term is exactly the
        // sky's own (Common/CloudAerial.glslh, and the relations pinned in Tests/Engine/SkyScattering),
        // so a cloud it has taken away has become the sky rather than gone dark.
        //
        // The shape of the dial is worth reading before reaching for it: BELOW the start distance the
        // atmosphere is not attenuated, it is absent. 43 scenes here carried 30 km / 90 km, which deleted
        // the aerial perspective outright inside 30 km — where most of a layer lives — and admitted a
        // third of it at the farthest cloud the layer can hold. It was in three scenes whose cloud layer
        // is switched OFF, which is what settled that it was a copy and not a decision.
        //
        // WHAT REMOVING IT COST AND BOUGHT, on Clouds_Protocol at the six dome points (noise floor 0
        // bytes, measured by repeat): the frame's own contrast did not move at all — 0.545 -> 0.541 at the
        // horizon, 0.399 -> 0.395 at 45 degrees, 0.456 -> 0.455 at the zenith — so the atmosphere is not
        // flattening anything. What moved is COLOUR: mean saturation 0.137 -> 0.207 at the horizon,
        // 0.132 -> 0.180 at mid, 0.227 -> 0.248 at the zenith, the gain scaling with how much air is in
        // front, which is the signature it should have. Over the far band alone (30-60 km) the internal
        // contrast falls to 0.49 of what it was and the saturation rises by half — and 0.49 is the air's
        // own transmittance over that band, which is the whole argument: the term is exactly as strong as
        // the medium says and not a step stronger.
        float AerialPerspectiveStartDistance = 0.0f;

        PROPERTY( DisplayName( "Aerial Perspective Fade Distance" ), Category( "Lighting" ), Length,
                  Range( 0.0f, 20000000.0f ), Advanced,
                  Tooltip( "Distance over which the haze ramps from nothing to full once it has started. "
                           "Zero applies it in full immediately, which is what makes the start distance "
                           "above inert on its own." ) )
        float AerialPerspectiveFadeDistance = 0.0f;

        // ---- Shadows --------------------------------------------------------------------------------
        //
        // THE LAYER SHADING THE WORLD UNDER IT, through the map described in
        // Editor/Resources/Shaders/Common/CloudShadowMap.glslh. Two fields and no more, and the two that
        // are absent are absent for a stated reason:
        //
        //   * THE MAP'S EXTENT AND RESOLUTION are engine constants, like the step schedule and for the
        //     same reason (Common/CloudGeometry.glslh): they trade cost against quality identically in
        //     every scene, which is why Unreal carries them as cvars. They are also not free to choose —
        //     the texel is derived from the finest cloud chord the march can resolve, so an artist moving
        //     one of them would be moving a number the producer's own step schedule fixes.
        //   * THE SKY-LIGHT OCCLUSION under a deck is a DIFFERENT quantity with a different geometry (a
        //     hemisphere rather than a direction) and it is still not approximated with this map, because
        //     a directional occlusion applied to an omnidirectional term is wrong in a way that looks
        //     tuned rather than broken. It has its OWN volume now — Sky Occlusion Volume in the Lighting
        //     group above, built by Programs/Clouds/CloudSkyOcclusionVolume.shader — which is where Р4
        //     took the "named as out of scope rather than half-done" note this paragraph used to end on.

        PROPERTY( DisplayName( "Cast Shadows" ), Category( "Shadows" ), Summary,
                  Tooltip( "Whether the layer shades the world beneath it. Off dispatches nothing and "
                           "allocates nothing: a scene with this off pays exactly what a scene with no "
                           "clouds pays for the shadow map, which is zero." ) )
        bool CastShadows = true;

        PROPERTY( DisplayName( "Shadow Strength" ), Category( "Shadows" ), Range( 0.0f, 1.0f ),
                  Tooltip( "How strongly the cloud shadow darkens the sun on the world beneath it. It "
                           "scales the OPTICAL DEPTH, not the resulting light, so 0.5 is half as much "
                           "cloud in the way rather than half the darkness — which is what keeps a thin "
                           "cloud thin and a thick one thick as the dial moves. At 0 the pass is skipped "
                           "entirely, exactly as if Cast Shadows were off." ) )
        // ONE, WHICH IS UNREAL'S DEFAULT (CloudShadowStrength on the directional light) and is also the
        // only value that is physics rather than art: the map holds the medium's own extinction, so 1 is
        // the shadow the cloud that is drawn in the sky actually casts. It is a dial at all because the
        // approximation the layer is lit BY is not energy-exact either, and a sky that wants its ground
        // back should be able to say so with a number rather than by turning the feature off.
        float ShadowStrength = 1.0f;

        // ---- Quality --------------------------------------------------------------------------------

        PROPERTY( DisplayName( "Max Steps" ), Category( "Quality" ), Range( 8, 512 ),
                  Tooltip( "Ceiling on the number of samples along a view ray. The count itself rises "
                           "with the length of the segment inside the layer and saturates at this "
                           "value, so it is a cost ceiling rather than a fixed cost." ) )
        // 256, WHICH IS AFFORDABLE ONLY BECAUSE OF THE SKIP. The march has two step sizes and spends the
        // coarse one — four times longer — on empty sky, so a ray that crosses mostly clear air finishes
        // in a quarter of these iterations. Raising the ceiling therefore buys resolution INSIDE cloud
        // without charging for it outside, which is the opposite of what raising it did before the skip
        // existed.
        int32_t MaxSteps = 256;

        PROPERTY( DisplayName( "Stop Transmittance" ), Category( "Quality" ), Range( 0.0f, 0.2f ), Advanced,
                  Tooltip( "The march stops once this little light is still getting through. Raising it "
                           "is usually the cheapest saving available, because the samples it skips are "
                           "behind material that has already hidden them." ) )
        float StopTransmittance = 0.005f;

        PROPERTY( DisplayName( "Volume Resolution" ), Category( "Quality" ), Range( 128, 256 ), Advanced,
                  Tooltip( "Voxels per horizontal side of the camera-centric modelling volume the layer is "
                           "baked into. It is the cost of the BAKE, not of the frame: the bake is a loop "
                           "over side x side columns, so 128 costs a QUARTER of 256 — measured at 229 ms "
                           "against 961 ms. Lower it when a view has to follow an edit quickly (an asset "
                           "preview does) and leave it at 256 for a level, where the sky is baked once and "
                           "looked at for hours. It stops at 128 because 64 was measured to invent four "
                           "points of sky rather than draw the same sky more coarsely. It is a FLOOR and "
                           "not a command: a grid too coarse to express a cloud type's placement cell "
                           "would move the clouds rather than blur them, so the bake raises it back to 256 "
                           "for such a type and says so in the log." ) )
        // A FLOOR SINCE O13. Graphic::CloudBakeSideForSpecies raises the grid when the asked-for side could
        // not carry a type's authored placement cell — Assets::CloudProceduralCellExtentKm floors that cell
        // at four voxels, so at 128 over the shipped 48 km region the floor is 1.50 km and two of the nine
        // shipped types author finer (Altocumulus 0.90 km, Stratocumulus 1.05 km). The clamp was silent and
        // it did not soften the sky, it relaid it: 99.77 % of the frame, mean 35.1 of 255 on
        // SIL_Altocumulus against a repeat floor of 0.
        //
        // 256, WHICH IS THE ONLY VALUE A LEVEL SHOULD USE, and the field exists for the other end. The
        // resolution used to be a constant, and the consequence was that a 512-pixel material-preview pane
        // baked exactly what a whole level bakes: measured on this machine in Debug, an artist dragging
        // Coverage waited 15.42 s from their last edit to a sky that showed it. It is here rather than in
        // Assets:: because it is a property of the VIEW — see Docs/RENDERER_FRAME_STATE.md — in precisely
        // the way MaxSteps above it already is, and PreviewViewport::SceneSetup sets both from one place.
        //
        // RAISING IT ABOVE 256 IS NOT AN OPTION AND THAT IS A MEASUREMENT, not caution: 512 was built,
        // measured at +1.7 m of silhouette on 94.3 for four times the memory and +14.3 % of march time, and
        // refused (Assets/CloudProceduralVolume.hpp carries the table).
        //
        // THE THREE NUMBERS ARE WRITTEN OUT rather than taken from Assets::kCloudProceduralVolumeSide and
        // Assets::kCloudProceduralVolumeSideMin, for the same reason CloudLayerLatticeKm below spells out
        // 100 000: including Assets/CloudProceduralVolume.hpp here would drag Engine/Graphic into Engine/ECS
        // through CloudTypeShape.hpp, and that layering rule is not negotiable. So this is a MIRROR, and it
        // is guarded rather than trusted — Desert/Tests/Engine/ComponentReflection
        // (`DefaultsAreTheOnesTheComponentArguesFor`) asserts that the default and BOTH ends of the Range
        // above equal the Assets constants, so the day one of them moves the other is a red test naming it
        // rather than a slider whose top half bakes nothing and whose bottom half changes the sky's scale.
        int32_t VolumeResolution = 256;

        // ---- Animation ------------------------------------------------------------------------------

        PROPERTY( DisplayName( "Wind Direction" ), Category( "Animation" ),
                  Tooltip( "Direction the layer drifts. Normalized by the renderer; a zero vector simply "
                           "leaves the sky still." ) )
        glm::vec3 WindDirection = { 1.0f, 0.0f, 0.0f };

        PROPERTY( DisplayName( "Wind Speed" ), Category( "Animation" ), Length, Range( 0.0f, 50000.0f ),
                  Tooltip( "How fast the layer drifts, in world units per second. The wind moves the "
                           "SAMPLE POSITION rather than the data, which is what makes the motion free and "
                           "seamless." ) )
        float WindSpeed = 3000.0f; // 30 m/s
    };

    /**
     * @brief The layer's PLACEMENT LATTICE in kilometres — Weather Tile Size divided by four.
     *
     * FOUR CELLS TO A TILE is a ratio the component's own tooltip has stated since phase T1 ("12 km ->
     * 3 km cells, a cumulus field") and that Graphic::System::VolumetricCloudRenderer turns into
     * `CloudProceduralSpecies::CellKm`. It is stated HERE, once, because a second reader appeared: the
     * Cloud Layout panel measures a painting's strokes against the cell, and a panel that computed the
     * ratio for itself would quote a resolution the sky does not have the day somebody changes it.
     *
     * A TYPE'S Placement Scale STILL MULTIPLIES IT — this is the layer's own lattice, before the kind of
     * cloud in a slot says how much coarser or finer than the layer it is.
     */
    // Takes the tile size in world units (centimetres) rather than the component, because the value is a
    // MATERIAL parameter since O1 — the component no longer carries it, and this helper's two readers
    // (the renderer and the Cloud Layout panel) both hold the resolved material values.
    inline float CloudLayerLatticeKm( float weatherTileSize )
    {
        // 100 000 world units to the kilometre, the project-wide centimetre convention. Written out
        // rather than taken from Graphic::kCloudWorldUnitsPerKm because Engine/ECS must not depend on
        // Engine/Graphic, and the number is the unit of the whole project rather than the renderer's.
        constexpr float unitsPerKm = 100000.0f;
        return ( weatherTileSize > 1.0f ? weatherTileSize : 1.0f ) / unitsPerKm / 4.0f;
    }

    /// The four cloud type slots of a layer's MATERIAL (CloudType1..4 of the CloudRaymarch schema), in
    /// the order the Material Editor draws them. One statement of that order, so nobody spells
    /// `{ CloudType1, CloudType2, CloudType3, CloudType4 }` a second time.
    constexpr uint32_t kCloudTypeSlots = 4u;

    /**
     * @brief Which of a layer's four cloud type slots become SPECIES, and in what order.
     *
     * WHY THIS IS NOT THE IDENTITY, AND WHY THAT MATTERS OUTSIDE THE RENDERER. The four slots an artist
     * fills in Details are COMPACTED before they reach the field: an empty slot is skipped, and the same
     * type twice is dropped, because two identical placement fields are two skies of one kind of cloud at
     * twice the cost. So a layer whose only authored type sits in `Cloud Type 3` has exactly ONE species,
     * and that species is number ZERO.
     *
     * THAT RENUMBERING IS A CONVENTION THE ARTIST CAN SEE THE CONSEQUENCES OF. A painted layout's channels
     * are indexed by SPECIES, so in the layer above it is the painting's RED channel that decides where
     * `Cloud Type 3` goes — not its blue one. The Cloud Layout panel names the type behind each channel
     * rather than leaving the artist to discover it from a sky, and it can only do that because the rule
     * lives here instead of inside the renderer that used to own it.
     */
    struct CloudSpeciesResolution
    {
        /// How many species the layer has, 1..kCloudTypeSlots. Never zero: see BuiltInDefault.
        uint32_t Count = 0u;

        /// For each species, which of the four Details slots it came from. Meaningless when
        /// BuiltInDefault, which is why that flag exists rather than a sentinel slot index.
        uint32_t AuthoredSlot[kCloudTypeSlots] = { 0u, 0u, 0u, 0u };

        /// True when NO slot is authored. The layer then has one species — the engine's built-in cumulus
        /// congestus — because a scene nobody has chosen a type for still has to have a sky.
        bool BuiltInDefault = false;
    };

    // Takes the four slot handles rather than the component: the slots are MATERIAL parameters since O1
    // (CloudType1..4 of the CloudRaymarch schema), and this rule's readers hold the resolved values.
    inline CloudSpeciesResolution ResolveCloudSpecies( const Assets::AssetHandle ( &authored )[kCloudTypeSlots] )
    {
        CloudSpeciesResolution resolved;

        for ( uint32_t slot = 0; slot < kCloudTypeSlots; ++slot )
        {
            if ( authored[slot] == Assets::AssetHandle::Null() )
                continue;

            bool seen = false;
            for ( uint32_t taken = 0; taken < resolved.Count; ++taken )
                seen = seen || authored[resolved.AuthoredSlot[taken]] == authored[slot];
            if ( seen )
                continue;

            resolved.AuthoredSlot[resolved.Count] = slot;
            ++resolved.Count;
        }

        if ( resolved.Count == 0u )
        {
            resolved.Count          = 1u;
            resolved.BuiltInDefault = true;
        }

        return resolved;
    }

    struct VolumetricCloudComponent
    {
        VolumetricCloudData Data;
    };
} // namespace Desert::ECS
