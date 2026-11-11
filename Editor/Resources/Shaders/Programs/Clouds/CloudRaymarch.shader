// DesertAsset {"Kind":"Shader","Guid":"e5f5868d2f307814ec90df16057f3698","Versions":{"SHDR":1},"Dependencies":[]}
Shader "CloudRaymarch"
{
    // THE CLOUD LOOK IS A MATERIAL (O1, D-35): everything below describes the MEDIUM and the FIELD —
    // what the clouds are, where they sit, how they scatter — and is authored in a `.demat` whose slot is
    // VolumetricCloudComponent::Material. The component keeps only tracing budgets, pass routing and
    // world integration. An EMPTY material slot means exactly these defaults, which are byte-for-byte the
    // component defaults they replaced (Desert/Tests/Engine/CloudMaterialSchema pins that census).
    //
    // NO Binding() ON PURPOSE. This block is SCHEMA ONLY: the values travel through the existing
    // CloudGpuPayload (Engine/Graphic/Clouds/CloudPayload.hpp), packed on the CPU by the renderer that
    // resolved the material — so the march's own GLSL below reads the same `Cloud*` fields it always
    // read, and no second GPU transport exists to disagree with the first. Every parameter here is
    // consumed by name in Engine/Graphic/Clouds/CloudMaterialValues.hpp; a name the C++ side does not
    // read fails the CloudMaterialSchema census, which is DEV_CONTRACT §1.3 made compile-adjacent.
    //
    // UNITS: 1 world unit = 1 cm project-wide, and every Length-marked value here is in CENTIMETRES
    // exactly as the component fields were — the migration copies scene numbers verbatim. Extinction is
    // per kilometre, as its display name says.
    //
    // EVERY PROPERTY DECLARES ITS `Timing`, AND THAT IS NOT DECORATION. Twenty of the thirty-five below are
    // inputs to a CPU BAKE of a 256x32x256 volume over several thousand cloud bodies, so moving one of them
    // re-runs that bake — measured at 3.3 to 14.1 s on the development machine — and the sky goes on showing
    // the PREVIOUS volume until the new one lands. The other fifteen answer in the frame that is drawn next:
    // fourteen are read per sample by the march below, and the fifteenth is the authored Medium, which is
    // COMPILED INTO it. Until this attribute existed the two were indistinguishable in the Material Editor,
    // and the owner reported the layer as "not updating" twice.
    //
    // THE CLAIM IS CHECKED, not asserted: Desert/Tests/Engine/CloudMaterialTiming perturbs one value at a
    // time through Graphic::ApplyCloudMaterialToBakeParams and asks Graphic's own rebake decision
    // (Assets::CloudProceduralParamsEqual) whether the volume has to be built again — so `Timing(Immediate)`
    // on a value that costs seconds, or `Timing(Rebake)` on one that costs nothing, is a RED SUITE and not a
    // stale comment. A property with no Timing at all is red too: the census refuses an unclassified one.
    //
    // The calibration histories that stood beside these values as component fields (Coverage's measured
    // table, DetailStrength's two superseded defaults, ExtinctionScale's Р18 refusal, AmbientScale's
    // D-25/D-32/D-33 record) live in the git history of ECS/VolumetricCloudComponent.hpp before O1-A and
    // in Docs/Clouds/CALIBRATION.md; the tooltips carry the operative conclusions.

    Domain Volume
    Role CloudMaterial

    Properties
    {
        CloudType CloudType1 ("Cloud Type 1", Category("Cloud Types"), Timing(Rebake), Tooltip("Which kind of cloud this layer is made of - drag a .decloudtype from the Content Browser or double-click one there to edit it. The type carries its own base and top in kilometres, its own family of vertical profiles, its own edge character, its own density and its own placement scale, and the shell the march intersects is COMPUTED from all of them together. A layer may carry up to four; they OVERLAP rather than divide the sky between them, so a low deck and a tall tower can stand in the same place. All four empty uses the engine's built-in cumulus congestus."))
        CloudType CloudType2 ("Cloud Type 2", Category("Cloud Types"), Timing(Rebake), Tooltip("A second kind of cloud in the same sky. Its placement field is its OWN - its own scale, its own stretch along the wind, its own patches - so it is not a share of the first type's sky but an independent one laid over it. Where two types meet, the deeper body wins and keeps its own edge and density."))
        CloudType CloudType3 ("Cloud Type 3", Category("Cloud Types"), Timing(Rebake), Tooltip("A third kind of cloud. Costs one noise fetch per sample only at altitudes its own base and top actually reach - a cirrus at eight kilometres is free everywhere a cumulus lives."))
        CloudType CloudType4 ("Cloud Type 4", Category("Cloud Types"), Timing(Rebake), Tooltip("The fourth and last kind. Four is the ceiling because a type owns one channel of the vertical profile table, which is how Unreal arranges it too."))

        Float Coverage ("Coverage", Range(0.0, 1.0), Category("Weather"), Timing(Immediate), Tooltip("How busy the weather is where there is weather. The march cuts the baked cloud field at 1 - Coverage, so 0 is a clear sky and 1 keeps every cloud, and moving it costs no re-bake (the weather drives it live); the weather map then thins the clouds toward its clear gaps, which stay open even at 1. An overcast sky is its own cloud type (a stratus deck), not Coverage 1.")) = 0.45

        Float CloudTypeWeight1 ("Cloud Type 1 Weight", Range(0.0, 1.0), Category("Weather"), Timing(Immediate), Tooltip("How much of Cloud Type 1 the sky shows, 0..1, as its own Coverage: the march cuts that type's channel of the baked field at 1 - Coverage x this weight. The four weights are independent (UE's Layout_CloudType RGBA), so a weather can fade one type out while another fades in without a re-bake; the world's weather drives them (WTH-OVERCAST).")) = 1.0

        Float CloudTypeWeight2 ("Cloud Type 2 Weight", Range(0.0, 1.0), Category("Weather"), Timing(Immediate), Tooltip("How much of Cloud Type 2 the sky shows, 0..1, as its own Coverage: the march cuts that type's channel of the baked field at 1 - Coverage x this weight. The four weights are independent (UE's Layout_CloudType RGBA), so a weather can fade one type out while another fades in without a re-bake; the world's weather drives them (WTH-OVERCAST).")) = 1.0

        Float CloudTypeWeight3 ("Cloud Type 3 Weight", Range(0.0, 1.0), Category("Weather"), Timing(Immediate), Tooltip("How much of Cloud Type 3 the sky shows, 0..1, as its own Coverage: the march cuts that type's channel of the baked field at 1 - Coverage x this weight. The four weights are independent (UE's Layout_CloudType RGBA), so a weather can fade one type out while another fades in without a re-bake; the world's weather drives them (WTH-OVERCAST).")) = 1.0

        Float CloudTypeWeight4 ("Cloud Type 4 Weight", Range(0.0, 1.0), Category("Weather"), Timing(Immediate), Tooltip("How much of Cloud Type 4 the sky shows, 0..1, as its own Coverage: the march cuts that type's channel of the baked field at 1 - Coverage x this weight. The four weights are independent (UE's Layout_CloudType RGBA), so a weather can fade one type out while another fades in without a re-bake; the world's weather drives them (WTH-OVERCAST).")) = 1.0
        Float CoverageContrast ("Coverage Contrast", Range(0.1, 4.0), Category("Weather"), Timing(Rebake), Tooltip("Sharpness of the transition from clear to cloudy, as the WIDTH of the ramp from an empty cell to a full one. Above 1 the ramp narrows and the sky is decisively cloud or decisively clear; below 1 it widens and the cloud sizes spread out, which is what a broken deck looks like.")) = 1.0
        Float WeatherTileSize ("Weather Tile Size (cm)", Range(200000.0, 8000000.0), Category("Weather"), Timing(Rebake), Tooltip("World size the cloud lattice is measured against, in centimetres (1200000 = 12 km). One cell is a QUARTER of it, and a cell carries Cloud Density clouds on average - which is also what decides whether there is any cloud overhead at all: a cell much larger than the layer altitude cannot fit one above the camera, and the zenith comes out empty however high the coverage is set.")) = 1200000.0
        Int Seed ("Seed", Range(0, 65535), Category("Weather"), Timing(Rebake), Tooltip("Which sky. Two layers with different seeds have unrelated clouds; the same seed always gives the same sky, in the same places, for the same settings.")) = 1

        Float PlacementDensity ("Cloud Density", Range(0.25, 8.0), Category("Placement"), Timing(Rebake), Tooltip("How many clouds a lattice cell carries on average - a whole number is drawn per cell with this mean, so 'exactly one per cell' stops being a property of the sky. It does NOT change how much cloud there is: each cloud is narrowed as this rises, so the matter is redistributed rather than added and Coverage keeps meaning what it says. Raise it for many small clouds at the same cover, lower it for few large ones. On its own it makes the lattice MORE visible, not less - Cloud Scatter is what removes that.")) = 1.75
        Float PlacementScatter ("Cloud Scatter", Range(0.0, 4.0), Category("Placement"), Timing(Rebake), Tooltip("How far a cloud may wander from its lattice site, measured in CELLS - at 1 it may sit anywhere in a cell-wide box around its site and so crosses into its neighbours' ground. THIS IS THE KNOB THAT REMOVES THE GRID: at 0 the sky is a lattice with a wobble, and the measured lattice bump falls by six times between 0 and 1. It is not free - independently placed clouds overlap where a lattice keeps them apart, so the sky covers a few points less than Coverage says at 0 and a few points more when this is returned to zero.")) = 1.0
        Float PlacementSizeVariety ("Cloud Size Variety", Range(0.0, 1.0), Category("Placement"), Timing(Rebake), Tooltip("How much cloud sizes differ from each other. At 0 every cloud in the sky is the size its cell's coverage says, which is the second half of what reads as a pattern; at 1 the widest is about four times the narrowest. The draw is uniform in AREA rather than in width, so the mean cloud covers the same ground at every setting and Coverage does not move with it. A smaller cloud is also a flatter one, which is what a field of cumulus looks like.")) = 0.75
        Float PatchTileSize ("Weather Patch Size (cm)", Range(500000.0, 20000000.0), Category("Placement"), Timing(Rebake), Tooltip("World size over which the sky's BUSY and CLEAR regions alternate, in centimetres - the scale of a weather system rather than of a cloud. Bounded from BELOW at three lattice cells and raised to it silently if set finer, because a modulation whose period is near a cell's decides cells one at a time and reads as a checkerboard rather than as weather. The weather is marched in WORLD space with a 997 km period, so a period longer than Region Size still completes its cycle across the sky.")) = 3000000.0
        Float PatchStrength ("Weather Patch Strength", Range(0.0, 1.0), Category("Placement"), Timing(Rebake), Tooltip("The fraction of the sky the weather leaves clear. The rest is weather at full strength, where Coverage is the cloud cover; the two are joined by a narrow shore. At 0 the weather is everywhere and the sky is uniformly occupied; at 1 the sky is clear. The clear gaps are as wide as a weather system and no Coverage fills them. It only removes cloud, never adds it.")) = 0.29

        CloudLayout LayoutPattern ("Global Pattern", Category("Layout"), Timing(Rebake), Tooltip("WHERE THE CLOUDS ARE - a painted map of the sky, and Unreal's Layout_CloudGlobalPattern by another name. Drag a .dclayout from the Content Browser, or double-click one there to paint it, import a picture into it or export what is in it. Its four channels say where each of this layer's four cloud type slots lives. EMPTY IS THE NORMAL STATE: with no pattern the sky is placed procedurally exactly as before, and Weather Patch Strength decides which parts of it are busy. Only the PATTERN table of whatever is dropped here is read - the mask has its own slot below, so one painting can place the clouds and a different one clear a region. The painting is read when the clouds are placed, not while they are drawn, so it costs the frame nothing."))
        CloudLayout LayoutMask ("Global Cloud Mask", Category("Layout"), Timing(Rebake), Tooltip("HOW MUCH CLOUD TO ADD OR TAKE AWAY, region by region - Unreal's Layout_GlobalCloudMask, and the second of its two layout textures. Mid-grey changes nothing, brighter adds cloud, darker removes it. It is ADDITIVE and applied after everything else, so unlike the pattern it does move the sky's total cover - which is what it is for. Only the MASK table of whatever is dropped here is read, and a .dclayout with no mask in it will say so in the log rather than quietly doing nothing. Leave it empty and the slot costs exactly zero, which is what Unreal's own default mask of (0,0,0,1) also comes to."))
        Float LayoutPatternStrength ("Layout Pattern Strength", Range(0.0, 1.0), Category("Layout"), Timing(Rebake), Tooltip("How strongly the painting decides where clouds are. At 1 the painted pattern rules: bright regions of a channel fill with that slot's kind of cloud and dark ones empty. AT 0 THE PROCEDURAL PATCH FIELD TAKES OVER INSTEAD - the sky goes back to Weather Patch Strength's busy and clear regions, so this end of the slider is a live sky and not an absence of one. The painting is applied about its OWN AVERAGE, so it moves cloud around the sky rather than adding it and Coverage keeps meaning the fraction of sky it delivers.")) = 1.0
        Float LayoutMaskStrength ("Layout Mask Strength", Range(0.0, 1.0), Category("Layout"), Timing(Rebake), Tooltip("How hard the painted mask adds and removes cloud. Mid-grey changes nothing, brighter adds cloud, darker takes it away. Unlike the pattern it is NOT balanced about its own average - adding cloud where you paint is what it is for - so it does move the sky's total cover. With the Global Cloud Mask slot empty it contributes nothing at any setting.")) = 1.0
        Int LayoutRepeats ("Layout Repeats", Range(1, 16), Category("Layout"), Timing(Rebake), Tooltip("How many times the painting repeats across Region Size. At 1 the whole painting covers the region once - 48 km at the default - and at 16 it repeats every 3 km, which is one cloud lattice cell. A WHOLE NUMBER because the sky repeats at Region Size: a painting whose period did not divide it would put a hard seam across every region boundary.")) = 1
        Int LayoutRotation ("Layout Rotation", Range(0, 3), Category("Layout"), Timing(Rebake), Tooltip("Quarter turns of the painting about the vertical axis: 0, 1, 2 or 3 - that is 0, 90, 180 and 270 degrees. Use it to point a painted band north-south instead of east-west without repainting. QUARTER TURNS AND NOT A FREE ANGLE, because only a quarter turn maps the sky's own repeat onto itself; any other angle would put a seam at the distance the sky repeats.")) = 0
        Vec2 LayoutOffset ("Layout Offset (cm)", Category("Layout"), Timing(Rebake), Tooltip("Where the painting's origin sits in the world, east and north, in centimetres. Slide it to put a painted feature over a particular place on the ground. Continuous, unlike the rotation - sliding a repeating pattern along its own axis leaves it repeating, so there is nothing here for the sky's own period to disagree with.")) = (0.0, 0.0)

        Float DetailTileSize ("Detail Tile Size (cm)", Range(20000.0, 3000000.0), Category("Detail"), Timing(Immediate), Tooltip("World size over which the erosion field repeats, in centimetres - the scale of the billows and wisps cut into the cloud's edge. It is bounded from BOTH sides and the bounds are measured: above by the size of a cloud, because an erosion wave longer than a body scales that body instead of texturing it; below by the march's own search step, because structure the march cannot find is dither.")) = 100000.0
        Float DetailStrength ("Detail Strength", Range(0.0, 1.0), Category("Detail"), Timing(Immediate), Tooltip("How deeply the erosion cuts, for the LAYER. At 0 the cloud keeps the smooth silhouette of its coverage field; at 1 the edge is eaten away into wisps. The cloud type multiplies this by its own factor, so a lenticular stays smooth and a cirrus stays wispy at whatever the layer is set to.")) = 0.65
        Float DensityScale ("Density Scale", Range(0.0, 2.0), Category("Detail"), Timing(Immediate), Tooltip("Multiplies the eroded density, for the LAYER. Below 1 the whole layer thins toward haze; above 1 the thin edges fill in. The cloud type multiplies this by how much water that kind of cloud is made of, so 1 keeps meaning 'this type as it is' whichever type is in the slot.")) = 1.0
        Float ExtinctionScale ("Extinction Scale (/km)", Range(0.5, 300.0), Category("Detail"), Timing(Immediate), Tooltip("How strongly the medium absorbs and scatters, per kilometre at full density. This is what makes a cloud opaque rather than merely visible. The cloud type multiplies it: ice at a quarter of a cumulus, a storm at a third above it. The default is PHYSICAL: a cumulus of 0.5 g/m3 liquid water in 10-micron droplets extinguishes at about 75/km (real cumulus 50-150/km). At that number a cloud's edge is a surface - light crosses a few tens of metres of it, not hundreds - which is what separates a cumulus from a cotton ball. Lower it for haze, not for a cumulus.")) = 75.0

        Color3 ScatteringAlbedo ("Scattering Albedo", Category("Lighting"), Timing(Immediate), Tooltip("Fraction of extinguished light that is scattered rather than absorbed, PER COLOUR. Water droplets barely absorb at all and absorb every wavelength alike, which is why a cloud is white and why the default is a neutral 0.98 - keep it grey for water. Tinting it is how the medium itself becomes something other than water: a warm cast for dust or smoke, a cold one for ice haze. It is the MEDIUM's colour and not a filter over the frame, so the tint compounds through multiple scattering exactly as it would in the real thing, and the deep interior of a body ends up further from white than its edge. Values much below 1 in every channel read as smoke.")) = (0.98, 0.98, 0.98)
        Float PhaseG ("Phase G", Range(-0.9, 0.9), Category("Lighting"), Timing(Immediate), Tooltip("Asymmetry of the Henyey-Greenstein phase function. Positive scatters forward, which is what puts the bright rim on a cloud you are looking at through the sun.")) = 0.8
        Float PhaseGBackward ("Phase G Backward", Range(-0.9, 0.9), Category("Lighting"), Timing(Immediate), Tooltip("Asymmetry of the SECOND phase lobe. Near zero it is almost isotropic, which is what carries the body of the cloud while the first lobe carries the bright rim against the sun. One lobe cannot do both: strong enough for the rim leaves the body black, weak enough for the body loses the rim.")) = 0.1667
        Float PhaseBlend ("Phase Blend", Range(0.0, 1.0), Category("Lighting"), Timing(Immediate), Tooltip("How much of the second lobe is mixed in. UE's shipped instance weights it toward the BODY at 0.575, so more than half the answer is the near-isotropic lobe and the sharp one is a highlight on top.")) = 0.575
        Float AmbientOcclusionStrength ("Ambient Occlusion Strength", Range(0.0, 1.0), Category("Lighting"), Timing(Immediate), Tooltip("How strongly the sky light reaching a sample is occluded. Which occluder is measured is Sky Occlusion Volume's choice (on the component); this is how much of it is applied, either way. At 0 the core of a three-kilometre cumulus is lit as brightly as a wisp on its edge, which reads as a flat white cut-out.")) = 1.0
        Float SkyLightCloudBottomOcclusion ("Sky Light Cloud Bottom Occlusion", Range(0.0, 1.0), Category("Lighting"), Timing(Immediate), Tooltip("How much of the sky light is taken away toward the BOTTOM of the layer - Unreal's Sky Light Cloud Bottom Occlusion, and the reason a cumulus base reads grey-blue under a white top. The sky light a sample receives is scaled from 1 - this value at the layer's base up to the full amount at its top, so the base sees the dark ground below it rather than the open sky. At 0 every height is lit by the same sky and a thick body is as bright underneath as on top; at 1 the base gets no sky light at all and goes black on the shaded side.")) = 0.5
        Int MultiScatterOctaves ("Multiple Scattering Octaves", Range(1, 3), Category("Lighting"), Timing(Immediate), Tooltip("How many scattering orders are approximated. ONE IS SINGLE SCATTERING, and a cloud lit by single scattering alone is physically grey: the light that makes a real cloud white has bounced many times inside it. Two or three is where it starts to look like weather. The ceiling of 3 is ECS::kCloudMultiScatterMaxOctaves; the CloudMaterialSchema census pins the two numbers together.")) = 3
        Float MultiScatterContribution ("Multiple Scattering Contribution", Range(0.0, 1.0), Category("Lighting"), Timing(Immediate), Tooltip("How much each successive scattering order contributes. The factor is SQUARED at every octave, so the series falls away quickly and the third order is already a small correction.")) = 0.667
        Float MultiScatterOcclusion ("Multiple Scattering Occlusion", Range(0.0, 1.0), Category("Lighting"), Timing(Immediate), Tooltip("How much less each successive order is absorbed. This is what lets light that has already scattered reach the core of a cloud that the direct beam never gets into - the reason a thick cumulus glows rather than going black. 0.4847 (the cube root of the similarity factor) puts the third octave exactly on the medium's diffusion length; 0.25 is the shipped calibration (D-32); it was set at 8/km and holds at the physical 75/km (CLOUD-SHAPE-e frames).")) = 0.25
        Float MultiScatterEccentricity ("Multiple Scattering Eccentricity", Range(0.0, 1.0), Category("Lighting"), Timing(Immediate), Tooltip("How much directionality each successive order keeps. Light that has bounced many times has forgotten where it came from, so the higher orders blend toward an isotropic phase.")) = 0.18
        ShaderProgram Medium ("Cloud Medium", Category("Medium"), Timing(Immediate), Tooltip("WHAT A CLOUD IS at a point in space - the density, the extinction, the albedo, the emission and the sky occlusion of the medium itself, authored as a node graph. Create one with New Shader Graph > Cloud Medium in the Content Browser, then drop it here. EMPTY IS THE NORMAL STATE and means the engine's own medium, which is what every scene drew before this slot existed and costs exactly the same. A graph with nothing wired into its Volume Output is that same medium again, so authoring starts from the sky you already have rather than from nothing. Note what the graph CANNOT reach: about half of the values in this window are inputs to a CPU bake that has already run by the time the march samples anything, and they are marked as such - a graph reads only the ones that are still in scope, and the palette offers no way to name the others - Cloud Material Param lists exactly those and nothing else."))

        Color3 AmbientScale ("Ambient Scale", Category("Lighting"), Timing(Immediate), Tooltip("Scales the sky's ambient contribution to the clouds. White is the full contribution; black lights them by the sun alone and leaves their shadowed sides black. Lowering it is NOT how the clouds get their form back: deleting it is the largest single knockout in the subsystem (D-25, D-32), and the contrast that appears when it goes is the sun's, uncovered - the frame loses half its light and goes warm.")) = (1.0, 1.0, 1.0)
    }

    Compute
    {
        // GPU-FARFIELD-4: the segment lanes of one ray compose their results through subgroupShuffle, and the
        // march counters are summed per subgroup (subgroupAdd) before one atomic each.
        #extension GL_KHR_shader_subgroup_basic : require
        #extension GL_KHR_shader_subgroup_shuffle : require
        #extension GL_KHR_shader_subgroup_arithmetic : require

        // The volumetric cloud march. Writes RGBA16F: .rgb = in-scattered radiance, PREMULTIPLIED, linear
        // HDR; .a = the transmittance of the cloud layer along that ray. The composite pass then needs no
        // more than `scene = rgb + scene * a`, which is the same premultiplied over-operator the height
        // fog already uses — and the same one UE writes, where the alpha channel is likewise transmittance
        // and not opacity.
        //
        // WHY COMPUTE. The march needs the scene depth to stop at geometry, and the fullscreen composite
        // draws into a framebuffer that has the depth attachment BOUND — sampling a bound attachment is a
        // feedback loop. The depth read therefore happens here, outside any render pass, with
        // ComputeImageBeginRead handling the layout round-trip. One path serves Forward and Deferred,
        // because both write this same depth attachment.
        //
        // QUARTER RESOLUTION, JITTERED. This pass writes a target a QUARTER of the framebuffer's size —
        // Unreal's VolumetricRenderTarget mode 0. Its output is not composited directly:
        // CloudTemporalResolve.shader reconstructs a HALF-resolution image from it and from that image's
        // own history, and the composite upsamples THAT.
        //
        // Each quarter-res texel stands for a 2x2 block of half-resolution pixels, and each frame traces
        // ONE of the four — CloudTraceParams.xy says which. The offset is applied to the RAY, not to the
        // store: the texel is written where it always was, but the ray it holds passes through the
        // half-res pixel the frame owns. That is Unreal's HackAddTemporalAAProjectionJitter, and the half
        // before it, HackRemoveTemporalAAProjectionJitter, is the matrix: InverseViewProjection is the
        // view's UNJITTERED inverse (VolumetricCloudRenderer, ViewFrame::InvViewProjection). This layer walks
        // its own sub-pixels and reprojects its own history through the unjittered previous matrix; a TAA
        // jitter in its rays is an offset its history never undoes (SKY-FLICKER).
        //
        // Four TRACES therefore cover every half-res pixel of a texel's block exactly once. A frame traces one
        // phase of the quarter grid's 2x2 lattice (main, Graphic::CloudTraceGridPhase), so the four traces of a
        // texel span Graphic::kCloudTraceWalkFrames = 16 frames, and the reconstruction is what turns those
        // quarter-res traces into one half-res image instead of one blurred one.
        //
        // LINEAR HDR ONLY — no tonemap, no gamma, no exposure. The engine's tonemap owns the curve and
        // runs later in the frame.
        //
        // WHAT IS WHERE. The geometry is Common/CloudGeometry.glslh, the shape is Common/CloudField.glslh,
        // the scattering is Common/CloudLighting.glslh, and all three are compiled as C++ by their unit
        // tests from this same text. What is left in this file is ray reconstruction, the loop, and a
        // store.

        #include <Common/CloudNoise.glslh>
        #include <Common/CloudGeometry.glslh>
        #include <Common/CloudLighting.glslh>
        #include <Common/CloudAerial.glslh>

        // For SKY_DISTANT_LIGHT_SPHERE_TEXEL only — the index of the full-sphere texel in the distant
        // sky light image. Taken from the sky's own header rather than written as a literal here, so the
        // two cannot disagree about where the value lives. SkyMedium is SkyScattering's prerequisite; the
        // integrator inside it stays inert because this pass defines neither LUT callback macro.
        #include <Common/SkyMedium.glslh>
        #include <Common/SkyScattering.glslh>

        // rgba16f: radiance is pre-tonemap HDR and transmittance is in [0,1]. Half precision carries three
        // decimal digits, an order more than an over-operator needs.
        layout(binding = 0, rgba16f) restrict writeonly uniform image2D CloudScatterTexture;

        // THE DEPTH GUIDE, same size as the scatter target, and the reason the composite can upsample
        // this pass without smearing its silhouettes. Two channels, both read by CloudComposite.shader:
        //
        //   .x  CLOUD FRONT DISTANCE, km — where along this ray the first material was found. NOT the
        //       transmittance-weighted mean the aerial perspective uses below: that mean sits inside the
        //       cloud, and what separates a cloud texel from an empty one is the FIRST hit.
        //   .y  SCENE DISTANCE, km — where the ray was cut, i.e. the geometry distance under a reversed-Z
        //       depth greater than zero and the far-plane distance otherwise.
        //
        // WRITTEN ON EVERY PATH, including rays that never reach the layer. A texel the march skipped is
        // still one of the four the composite fetches, and an unwritten one carries whatever the previous
        // frame left — a stale silhouette that moves with the camera one frame late.
        //
        // .zw ARE NOT USED and are written zero. That is not a reservation: this engine's
        // Core::Formats::ImageFormat has no two-channel float format (RGBA8F / RGBA16F / RGBA32F only),
        // so a two-channel guide has to be allocated four-channel. Nothing reads .zw and nothing should
        // start reading them without a format to match.
        layout(binding = 6, rgba16f) restrict writeonly uniform image2D CloudGuideTexture;

        // The scene depth attachment, presented to this dispatch by ComputeImageBeginRead and handed back
        // afterwards. Point-sampled with texelFetch, never filtered: a filtered depth across a silhouette
        // averages foreground and background into a distance where nothing is.
        // Shared sampler states (UE's shared samplers): every input below is a separate texture read through one
        // of these, bound once by the pass; a same-name macro after each texture keeps the sampled form.
        Uniform sampler GlobalFilterSampler;
        Uniform sampler LinearRepeatSampler;
        Uniform(2) texture2D SceneDepthTexture;
        #define SceneDepthTexture sampler2D(SceneDepthTexture, GlobalFilterSampler)
        #include <Common/CloudSceneDepth.glslh>

        // The noise volume, now an ASSET rather than a bake: four named channels from the Nubis deck
        // (p.96) — R,G Curly-Alligator LF/HF, B,A Alligator LF/HF — generated on the CPU by
        // Engine::Assets::CloudNoiseVolumeGenerator from the same Common/CloudNoise.glslh this shader
        // includes, and shipped as a .dcnv file. REPEAT and LINEAR,
        // which every volume sampler in this engine is; the volume was baked to tile exactly under that
        // assumption.
        Uniform(3) texture3D CloudNoiseTexture;
        #define CloudNoiseTexture sampler3D(CloudNoiseTexture, LinearRepeatSampler)

        // AND THREE MORE OF THEM, because a layer carries four cloud types and a type names its own
        // volume. Separate bindings and not an array: this engine's reflection refuses an array of
        // descriptors in so many words (VulkanShaderReflection.cpp, "arrays of descriptors are not
        // supported — declare separate bindings"). ALWAYS BOUND, all four, on the terms every other
        // sampler here is bound on — Graphic::ResolveCloudNoiseVolumes fills the slots a layer does not
        // need with slot 0's image, so an unused one is a second descriptor onto bytes that are already
        // resident rather than a hole in the set.
        Uniform(10) texture3D CloudNoise1Texture;
        #define CloudNoise1Texture sampler3D(CloudNoise1Texture, LinearRepeatSampler)
        Uniform(11) texture3D CloudNoise2Texture;
        #define CloudNoise2Texture sampler3D(CloudNoise2Texture, LinearRepeatSampler)
        Uniform(12) texture3D CloudNoise3Texture;
        #define CloudNoise3Texture sampler3D(CloudNoise3Texture, LinearRepeatSampler)

        // THE FOUR-WAY SELECT, and it is a chain of compares because a sampler is not indexable in this
        // dialect. It costs the wave every branch the wave takes — which is why the CPU DEDUPLICATES the
        // slots before sending them: eight of the nine shipped types name no volume of their own, so the
        // ordinary sky sends 0 for every species, `slot` is uniform, and this is one fetch exactly as it
        // was before a type could name a volume at all.
        vec4 CloudFetchNoise( int slot, vec3 p )
        {
            if ( slot == 1 )
                return texture( CloudNoise1Texture, p );
            if ( slot == 2 )
                return texture( CloudNoise2Texture, p );
            if ( slot == 3 )
                return texture( CloudNoise3Texture, p );
            return texture( CloudNoiseTexture, p );
        }

        // The sky's DISTANT SKY LIGHT: one texel holding the average radiance of the whole sky, marched
        // this frame by Programs/Sky/SkyDistantLight.shader. It is the physical model's ambient, and a
        // cloud is lit from every direction at once, so the full-sphere mean is the right quantity — the
        // same one the height fog reads. ALWAYS BOUND, even when it will not be read: a declared sampler
        // with no image is an INVALID descriptor set rather than an unused one, and this engine's compute
        // path answers that by silently skipping the whole dispatch — which would lose the clouds with
        // nothing in the log. CloudAmbient.w decides whether it is sampled.
        Uniform(4) texture2D DistantSkyLightTexture;
        #define DistantSkyLightTexture sampler2D(DistantSkyLightTexture, GlobalFilterSampler)

        // The sky's CAMERA AERIAL-PERSPECTIVE volume, 32x32x16 froxels of pre-integrated atmosphere
        // (Programs/Sky/SkyAerialPerspectiveLut.shader). Bound on the same terms as the texel above and
        // read only when CloudAerial.z says the volume exists.
        Uniform(5) texture3D CloudAerialPerspectiveTexture;
        #define CloudAerialPerspectiveTexture sampler3D(CloudAerialPerspectiveTexture, LinearRepeatSampler)

        // THE PROCEDURAL MODELLING VOLUME, 256 x 32 x 256 RGBA8, baked on the CPU by
        // Assets::BakeCloudProceduralVolume and uploaded by Runtime::CloudProceduralVolumeService
        // whenever the layer's settings change or the camera crosses a snap of the lump lattice —
        // NEVER per frame. Channel k is species k's Dimensional Profile: 0 at the surface of the body
        // and 1 in its core, already three-dimensional and already fused, because the bake joined the
        // lumps with an exponential smooth minimum.
        //
        // IT TOOK THE PROFILE TABLE'S BINDING, and that is the whole shape of phase Э5 in one line. The
        // table was `f(height in the envelope, how deep inside the patch)` multiplied by a threshold on
        // the Alligator noise — and the Alligator is `best - second`, which is ZERO wherever two feature
        // points contribute equally, so no setting of any slider could ever fuse two lobes. One 3D fetch
        // now answers for all four species where that arrangement cost a 2D fetch plus a 3D fetch per
        // live species.
        //
        // LINEAR AND REPEAT, like every sampler this engine creates, and here the REPEAT is load-bearing
        // rather than tolerated: the bake splats every lump at its wrapped positions, so the volume is
        // exactly periodic and sampling past the region is the degenerate far path rather than a seam.
        Uniform(7) texture3D CloudModellingTexture;
        #define CloudModellingTexture sampler3D(CloudModellingTexture, LinearRepeatSampler)
        // THE CONSERVATIVE DENSITY of the same three levels (Graphic::CloudProceduralClipmap::GetColumnImage):
        // side x (3 x CLOUD_COLUMN_MAX_BANDS) x side RGBA8, row k * CLOUD_COLUMN_MAX_BANDS + b = height band b of
        // level k, texel = each species' maximum over that band's rows of the column, dilated by the probe radius,
        // toroidal like the volume. texelFetched by the coarse tier's probe
        // (CloudProceduralColumnsEmpty) and nothing else; the march's own binding, not the field's.
        Uniform(20) texture3D CloudColumnMaxTexture;
        #define CloudColumnMaxTexture sampler3D(CloudColumnMaxTexture, LinearRepeatSampler)
        // The WORLD WEATHER map (Graphic::kCloudFarWeatherBinding): the remap Common/CloudField.glslh makes
        // of the baked profile by CloudWeather.
        // Always bound, on the terms every sampler here is.
        Uniform(15) texture2D CloudFarWeatherTexture;
        #define CloudFarWeatherTexture sampler2D(CloudFarWeatherTexture, LinearRepeatSampler)
        Uniform(16) texture2D CloudLayoutPatternTexture;
        #define CloudLayoutPatternTexture sampler2D(CloudLayoutPatternTexture, LinearRepeatSampler)
        Uniform(17) texture2D CloudLayoutMaskTexture;
        #define CloudLayoutMaskTexture sampler2D(CloudLayoutMaskTexture, LinearRepeatSampler)

        // THE SCULPTED HERO-CLOUD BODY — slot A of the seam. 128 x 64 x 128 RGBA8 of dimensional
        // profile, detail type, density scale and cutout envelope, loaded from a `.dcmv` and uploaded by
        // Runtime::CloudModellingService. ALWAYS BOUND, exactly like the two above and for exactly the
        // same reason: a declared sampler with no image is an INVALID descriptor set rather than an
        // unused one, and this backend answers an invalid set by silently skipping the dispatch — the
        // clouds would vanish with nothing in the log. When the scene has no hero cloud the renderer
        // binds the engine's fallback volume and the instance count is zero, so nothing reads it.
        Uniform(9) texture3D CloudAuthoredAtlasTexture;
        #define CloudAuthoredAtlasTexture sampler3D(CloudAuthoredAtlasTexture, LinearRepeatSampler)

        // THE SKY-LIGHT OCCLUSION VOLUME — 128 x 16 x 128 RGBA16F over the procedural modelling volume's
        // own region, .r holding the diffuse transmittance of everything ABOVE that column at that
        // altitude, written this frame by Programs/Clouds/CloudSkyOcclusionVolume.shader.
        //
        // IT IS THE QUANTITY CloudPhase.z's OTHER OCCLUDER CANNOT EXPRESS. CloudAmbientOcclusion is a
        // function of the sample's own Profile — its depth inside its OWN body — so a sample under three
        // kilometres of congestus receives exactly what a sample under clear sky receives. See
        // Common/CloudLighting.glslh, which owns both the maths and the addressing, and
        // Docs/Clouds/DIAGNOSIS_CARTOON.md §1, which is the measurement that asked for this.
        //
        // ALWAYS BOUND, fallback included, on the terms every sampler here is bound on: a declared sampler
        // with no image is an INVALID descriptor set, and this backend answers one by silently skipping
        // the dispatch. CloudFrame.x decides whether it is read.
        Uniform(13) texture3D CloudSkyOcclusionVolumeTexture;
        #define CloudSkyOcclusionVolumeTexture sampler3D(CloudSkyOcclusionVolumeTexture, LinearRepeatSampler)

        // THE ATMOSPHERE'S TRANSMITTANCE LUT — 256x64 RGBA16F of what survives the trip from a point in
        // the atmosphere to space along a given zenith angle, marched by
        // Programs/Sky/SkyTransmittanceLut.shader in Bruneton's (r, mu) mapping and read here through
        // Common/SkyScattering.glslh's SkySunAtAltitude — the exact inverse of the mapping the LUT was
        // written under, clamped to the texel centres it was written at.
        //
        // WHAT IT IS FOR. CloudSunColour is ONE colour for the whole shell. Unreal's default computes it
        // as `SunOuterSpaceIlluminance * T(groundLevel)` and so does this engine, which means a deck four
        // kilometres up is lit through four kilometres of air it is standing above. With CloudFrame.y
        // raised, the packer sends the OUTER-SPACE illuminance instead and this pass applies T at the
        // sample's own altitude and its own local zenith — the absolute form, with no ratio and therefore
        // no division by a transmittance that reaches zero at a low sun.
        //
        // ALWAYS BOUND, fallback included, on the terms every sampler above is bound on.
        Uniform(14) texture2D CloudSunTransmittanceLutTexture;
        #define CloudSunTransmittanceLutTexture sampler2D(CloudSunTransmittanceLutTexture, GlobalFilterSampler)

        // The seam's three callbacks. Declared here, next to the samplers, because Common/CloudField.glslh
        // must stay free of samplers to remain compilable as C++ by its tests.
        #define CLOUD_SAMPLE_NOISE(s, p) CloudFetchNoise((s), (p))
        // textureLod for the reason the authored atlas gives below, and for the same reason.
        #define CLOUD_SAMPLE_MODELLING(p) textureLod(CloudModellingTexture, (p), 0.0f)
        #define CLOUD_SAMPLE_WEATHER(uv) textureLod(CloudFarWeatherTexture, (uv), 0.0f).r
        #define CLOUD_SAMPLE_LAYOUT_PATTERN(uv) textureLod(CloudLayoutPatternTexture, (uv), 0.0f)
        #define CLOUD_SAMPLE_LAYOUT_MASK(uv) textureLod(CloudLayoutMaskTexture, (uv), 0.0f).r
        // textureLod AND NOT texture: a compute shader has no derivatives, so the implicit level of
        // detail is undefined. The volume has one level, so every implementation happens to pick it — but
        // "happens to" is the state three other sites in this engine were found in.
        #define CLOUD_SAMPLE_AUTHORED(p) textureLod(CloudAuthoredAtlasTexture, (p), 0.0f)
        // The probe's texel of image row @p row (level * CLOUD_COLUMN_MAX_BANDS + band) at the toroidal
        // coordinate @p uv in [0, 1): the texel the address falls in, never a filtered read — the dilation was
        // built around whole texels.
        vec4 CloudFetchColumnMax(int row, vec2 uv)
        {
            ivec2 side  = textureSize(CloudColumnMaxTexture, 0).xz;
            ivec2 texel = min(ivec2(uv * vec2(side)), side - ivec2(1));
            return texelFetch(CloudColumnMaxTexture, ivec3(texel.x, row, texel.y), 0);
        }
        #define CLOUD_SAMPLE_COLUMN_MAX(row, uv) CloudFetchColumnMax((row), (uv))

        // Slot A's instance list. Included BEFORE the seam, because the seam's authored producer reads
        // the block this declares and GLSL has no forward declarations.
        #define CLOUD_AUTHORED_BUFFER_BINDING 8
        #include <Common/CloudAuthored.glslh>

        #include <Common/CloudField.glslh>
        #include <Common/CloudParams.glslh>

        // THE CLOUD SHADOW MAP AS THIS MARCH'S FAR SHADOW TERM (GPU-CLOUD-MARCH). The map the ground reads
        // (Programs/Clouds/CloudShadowMap.shader, built earlier in the frame) holds per light-ray texel the
        // triple (front depth, mean extinction, optical depth across the shell); a sample inside the layer
        // marches its first kilometre toward the sun itself and reads the rest of the way from the map in
        // one fetch — CloudSelfShadowOpticalDepth below. One source for the cloud's shadow on the ground and
        // on itself. The C++ side of the block is Graphic::CloudSelfShadowParams (CloudShadowPayload.hpp),
        // whose static_asserts pin every offset.
        //
        // ALWAYS BOUND, fallback included, on the terms every sampler above is bound on; CloudSelfShadowMap.y
        // says whether it holds this frame's map. REPEAT like the ground receiver's fetch: the border fade
        // takes the weight to zero before the wrap could matter.
        #include <Common/CloudShadowMap.glslh>
        Uniform(18) texture2D CloudSelfShadowMapTexture;
        #define CloudSelfShadowMapTexture sampler2D(CloudSelfShadowMapTexture, LinearRepeatSampler)
        layout(std430, binding = 19) readonly buffer CloudSelfShadowBuffer
        {
            mat4  CloudSelfShadowWorldToMap; // world (cm) -> the map's clip space, this frame's projection
            // x = the kilometres the map's clip z spans, y = 1 when the map is this frame's and must be read,
            // z = the UV width of the border fade, w = the short ray's length (km).
            vec4  CloudSelfShadowMap;
            float CloudSelfShadowLocalSamples; // the short ray's sample count
        };

        // GPU-FARFIELD-4: THE MARCH COUNTERS, zeroed per frame and read back into the profile dump
        // (Graphic::System::CloudMarchCounters, "[Profiler] Clouds march"). One word each, in this order.
        Buffer(21) CloudMarchCountersUB
        {
            uint cloudMarchCounters[];
        };
        #define CLOUD_MARCH_COUNT_RAYS            0u // rays that reached the loop
        #define CLOUD_MARCH_COUNT_FIELD_SAMPLES   1u // view-ray field evaluations: the primary steps
        #define CLOUD_MARCH_COUNT_LIT_SAMPLES     2u // samples with density, one shadow ray each
        #define CLOUD_MARCH_COUNT_LIGHT_SAMPLES   3u // the light samples those scheduled
        #define CLOUD_MARCH_COUNT_EARLY_EXIT      4u // rays whose composed transmittance fell under the stop
        #define CLOUD_MARCH_COUNT_CEILING_LANES   5u // segment lanes that ran out of iterations
        #define CLOUD_MARCH_COUNT_COLUMN_JUMPS    6u // column-probe jumps over empty columns
        #define CLOUD_MARCH_COUNT_FULL_LIGHT_RAYS 7u // lit samples outside the shadow map (the full light ray)
        void CloudMarchCount(uint word, uint value)
        {
            uint total = subgroupAdd(value);
            if (subgroupElect() && total > 0u)
                atomicAdd(cloudMarchCounters[word], total);
        }

        // GPU-FARFIELD-4: LANES PER VIEW RAY (VolumetricCloudRenderer.cpp kMarchSegmentLanes, the same number).
        // The march traces 1.57 % of the screen's pixels a frame (one 2x2 phase of a quarter-resolution grid,
        // about 8k rays at 1080p), a quarter of what Unreal's quarter-resolution trace dispatches, so the pass is
        // too narrow to hide a fetch's latency: its time was the serial chain of the longest ray in a wave, not
        // the ray count. Each of these lanes marches one contiguous stretch of the ray's segment with its own
        // local transmittance; the lanes are then composed front to back, which is exact for an
        // emission-absorption integral: L = sum_k P_k L_k, T = prod_k T_k, P_k = prod_{j<k} T_j (and the
        // transmittance-weighted front the same way). The lanes of a ray sit side by side in x, so an 8-wide
        // group row IS one ray and a ray's lanes are eight consecutive subgroup lanes, aligned (every subgroup
        // size Vulkan ships for compute, 8 to 64, is a multiple of the row).
        //
        // EIGHT, NOT FOUR (FPS-CLOUDS-MARCH-2). At four the FIELD_Barn trace was 3259 rays = 13k invocations, about
        // 400 waves on a GPU with room for thousands: the pass still waited on the serial chain of its longest
        // stretch (about 24 primary and 22 light field evaluations a ray). Eight lanes halve that chain and double
        // the waves that hide each other's fetches, with the same samples per ray and the same exact composition,
        // so the picture is High's own; what it costs is one more lane start per ray (below, the step-back keeps
        // that start on the ray's dither) and four more shuffles an iteration.
        #define CLOUD_MARCH_SEGMENT_LANES 8u

        // THE COARSE TIER'S LOOKAHEAD (FPS-CLOUDS-MARCH-3): how many lattice points of the empty-space search one
        // iteration fetches together. The search was one probe and one field evaluation per iteration, each
        // waiting on the last, and on the FIELD_Barn turn that chain was the march (34 field evaluations and 9
        // probe jumps a ray against 0.8 lit samples). The points' fetches do not depend on each other - only the
        // walk that reads them does - so four are issued at once, the way CLOUD_LIGHT_GROUP groups the shadow ray,
        // and the walk then visits exactly the points the one-at-a-time search visited.
        #define CLOUD_COARSE_SEARCH_GROUP 4

        PushConstant CloudPush
        {
            mat4 InverseViewProjection;
            vec4 CameraPosition; // xyz = camera position in world units, w = trace cycle (CloudMarchStartOffset)
            // xy = this frame's sub-pixel inside the 2x2 block of HALF-resolution pixels each quarter-res
            //      texel covers, each 0 or 1. zw = the HALF-resolution grid's size in pixels, which this
            //      pass cannot derive: imageSize() reports the quarter-res target it writes, and the two
            //      round-ups that produced it are not invertible on an odd viewport.
            vec4 CloudTraceParams;
            // WHAT THIS FRAME'S RESOURCES ARE, as opposed to what the artist asked for. Graphic::CloudPush
            // ::Frame, member for member.
            //
            // x = 1 when the sky-light occlusion volume was written for THIS frame and must be read. Its
            //     region and side are NOT here: the volume shares the modelling volume's frame exactly, so
            //     CloudLevel[2] already carries both and a second copy would be one fact on the wire twice.
            // y = 1 when CloudSunTransmittanceLutTexture holds this frame's atmosphere AND the layer asked for
            //     per-sample sun transmittance. It also says WHAT CloudSunColour.rgb IS: the sun's
            //     outer-space illuminance when 1, the ground-level product when 0.
            // z, w = the atmosphere shell's bottom and top radii, kilometres from the planet centre.
            //     Written whatever y says, read only when y is 1.
            vec4 CloudFrame;
            // x = the diagnostic mode, Graphic::RenderConfig::CloudVisualize (0 = the ordinary picture).
            // y = this frame's 2x2 lattice phase of the trace grid, 0..3 (Graphic::CloudTraceGridPhase, encoded as
            //     Graphic::CloudTracePhase::Index): only the texels whose (x & 1) | (y & 1) << 1 equals it are
            //     marched (main).
            // z = 1 when the lit step grows with the pixel footprint (CloudFootprintStepKm), 0 = fixed fine step.
            vec4 CloudVisualize;
        };

        LocalSize(8, 8, 1);

        // THE SUN'S COLOUR AT A SAMPLE, which is one colour for the whole shell unless the layer asked
        // otherwise.
        //
        // THE TWO THINGS THE LUT IS ASKED FOR ARE BOTH THE SAMPLE'S OWN, and that is the entire content of
        // this function:
        //
        //   * its ALTITUDE ABOVE THE GROUND, measured from the planet centre and therefore following the
        //     curvature. The cloud shell's frame and the atmosphere's differ by where their floors are, so
        //     the altitude is taken against the cloud layer's planet radius and then re-based onto the
        //     atmosphere's — two shells, one ground.
        //   * its LOCAL ZENITH, which at the hundred and fifty kilometres a layer can span has tilted by
        //     1.35 degrees from the camera's. Taking the world's +Y here would give the far deck the near
        //     deck's sun angle, which is the same class of mistake CloudHeightFraction's own comment
        //     describes for the height.
        //
        // Everything else — the domain guard on the radius, the planet's own shadow, the texel-centre
        // clamp the engine's REPEAT samplers make mandatory — is SkySunAtAltitude's, shared with the
        // environment bake so the visible deck and the baked one cannot drift apart. The early return
        // below the terminator is the night fast path and not an optimisation of the general case: below
        // the band every lane in the dispatch takes it, so the fetch is skipped rather than diverged.
        //
        // textureLod AND NOT texture, for the reason every other fetch in this file gives: a compute
        // shader has no derivatives, so the implicit level of detail is undefined.
        vec3 CloudSunColourAt(CloudLayer layer, vec3 positionKm, vec3 toSun)
        {
            if (CloudFrame.y < 0.5f)
                return CloudSunColour.rgb;

            float sunZenithCos = clamp(dot(CloudLocalUp(positionKm), toSun), -1.0f, 1.0f);

            // textureSize() and not a literal 256x64, so the clamp cannot drift from the image the
            // renderer actually bound.
            SkySunAtPoint sun = SkySunAtAltitude(CloudFrame.z, CloudFrame.w,
                                                 CloudAltitudeKm(layer, positionKm), sunZenithCos,
                                                 vec2(textureSize(CloudSunTransmittanceLutTexture, 0)));

            if (sun.PlanetShadow <= 0.0f)
                return vec3(0.0f, 0.0f, 0.0f);

            return CloudSunColour.rgb *
                   (sun.PlanetShadow * textureLod(CloudSunTransmittanceLutTexture, sun.Uv, 0.0f).rgb);
        }

        // The sun-transmittance quadrature is Common/CloudField.glslh's — see the note there. It moved out
        // of this file when the sky's environment bake began lighting the same field, because a second copy
        // of it beside this one is the mirror that drifts.

        // THE OPTICAL DEPTH FROM A SAMPLE TO THE SUN, which the whole lighting of the cloud reuses (one shadow
        // ray serves every scattering order).
        //
        // WHERE THE MAP COVERS THE SAMPLE: a short march over CloudSelfShadowMap.w on the same squared
        // quadrature as the full ray — the near field, where the sample's own body is and where the map's
        // one-mean-per-column model is least true — plus the map's reconstruction of everything past it.
        // The price is CloudSelfShadowLocalSamples samples instead of lightSamples, per sample of the view
        // march; that inner product was the subsystem's whole cost (GPU-COST census, candidate 2).
        //
        // WHERE IT DOES NOT — outside the map's square, or a frame with no map at all — the full ray of
        // lightSamples over lightMarchKm, which is the quantity the covered region approximates and the one
        // the environment bake marches. In the border band the two are blended by the same fade the ground
        // receiver uses, so the edge of the map is a gradient and not a ring in the sky; only the band's
        // pixels pay for both.
        //
        // @p lightCost is the march-cost census's (--cloud-visualize 4): x the light samples this call scheduled
        // (the near march's, the full ray's, both in the border band), y 1 where the full ray ran.
        float CloudSelfShadowOpticalDepth(CloudLayer layer, CloudFieldParams params, vec3 samplePos, vec3 toSun,
                                          float lightMarchKm, int lightSamples, float extinction,
                                          out vec2 lightCost)
        {
            lightCost      = vec2(0.0f, 0.0f);
            float coverage = 0.0f;
            float nearFar  = 0.0f;
            if (CloudSelfShadowMap.y > 0.5f)
            {
                // Planet-centre kilometres back to world centimetres: the inverse of the march's own
                // `originKm = camera + planetRadius on y`, which is also the sphere CloudBuildShadowMapView
                // aims the map at.
                vec3 worldPos = vec3(samplePos.x, samplePos.y - layer.PlanetRadiusKm, samplePos.z) *
                                CLOUD_WORLD_UNITS_PER_KM;
                vec4 mapClip  = CloudSelfShadowWorldToMap * vec4(worldPos, 1.0f);
                mapClip      /= max(mapClip.w, 1e-6f);
                vec2 uv       = CloudShadowClipToUv(mapClip.xy);
                coverage      = CloudShadowBorderFade(uv, CloudSelfShadowMap.z);
                if (coverage > 0.0f)
                {
                    // textureLod AND NOT texture: a compute shader has no derivatives.
                    CloudShadowTexel texel   = CloudShadowDecode(textureLod(CloudSelfShadowMapTexture, uv, 0.0f).rgb);
                    float            depthKm = CloudShadowDepthKm(mapClip.z, CloudSelfShadowMap.x);
                    float            localKm = CloudSelfShadowMap.w;
                    // SIXTEEN, and it must equal ECS::kCloudLightMarchLocalMaxSamples — the packer clamps to
                    // the same constant (Graphic::CloudSelfShadowPack); this one only keeps a corrupt block
                    // from running an unbounded loop.
                    int              local   = int(clamp(CloudSelfShadowLocalSamples, 1.0f, 16.0f));
                    nearFar = CloudLightOpticalDepth(layer, params, samplePos, toSun, localKm, local, extinction) +
                              CloudShadowFarOpticalDepth(texel, depthKm, localKm);
                    lightCost.x = float(local);
                }
            }

            if (coverage >= 1.0f)
                return nearFar;

            float full = CloudLightOpticalDepth(layer, params, samplePos, toSun, lightMarchKm, lightSamples, extinction);
            lightCost += vec2(float(lightSamples), 1.0f);
            return mix(full, nearFar, coverage);
        }

        // THE MARCH'S START OFFSET, in [0, 1) of a fine step — Unreal's
        // `t = TMin + BlueNoiseScalar(FullResPixelCoord, View.StateFrameIndexMod8) * StepT`
        // (VolumetricCloud.usf:797-805, whose own note reads "Blue noise works best" over the white Rand3DPCG16
        // it replaced). The offset this replaces was that white hash: every trace of a pixel drew an
        // independent offset, so the reconstruction's history held one realization at a time and each new
        // trace replaced it by an unrelated one — the noise of the cloud itself over the whole sky
        // (SKY-FLICKER-2). It is low-discrepancy on all three axes the reconstruction averages over:
        //
        //   * IN SPACE, on the grid this frame traces: interleaved gradient noise (Jimenez 2014, the pattern of
        //     UE's InterleavedGradientNoise and of this engine's contact-shadow dither) over the LATTICE CELL,
        //     the quarter-res texel halved — the unit grid of the texels this frame writes (one 2x2 lattice
        //     phase, Graphic::CloudTraceGridPhase) — so the written texels a not-owned pixel's estimate blends
        //     (CloudTemporalResolve.shader TraceFiltered) are unit steps of the noise, hold well spread offsets
        //     and their errors cancel rather than add. Over the quarter-res texel itself the lattice would read
        //     the noise at a stride of two, where its step along x falls from 0.56 to 0.11 of the period and
        //     neighbouring rays start nearly together;
        //   * IN THE BLOCK: the 4x4 ordered-dither rank of the half-res pixel the ray goes through, in
        //     sixteenths — 4 x the 2x2 rank ({0, 2; 3, 1}, the table the sub-pixel walk itself is,
        //     Graphic::CloudTraceSubPixel) of its sub-pixel plus the 2x2 rank of its lattice phase, the recursive
        //     Bayer construction of Unreal's 4x4 table (VolumetricRenderTarget.cpp:314) — so the sixteen half-res
        //     pixels one lattice cell stands for sit in sixteen different sixteenths of the step, and the four
        //     one quarter-res texel stands for in four different quarters;
        //   * IN TIME, per pixel: the golden-ratio (R1) sequence over @p traceCycle, the number of times this
        //     pixel has been traced (Graphic::CloudPush::CameraPosition.w), so successive traces of one pixel
        //     fill the step evenly and any run of them averages to the step's mean. Unreal walks its 128x128x64
        //     texture by the frame index mod 8 through a four-frame sub-pixel walk, so each of its pixels sees
        //     only two of the eight slices; counting traces rather than frames is what gives every trace of a
        //     pixel a new value.
        //
        // In integers for the temporal term: 0x9E3779B9 is 2^32 / phi, and the top 24 bits of the product are
        // the R1 value exactly at any cycle count, where a float `fract(cycle * 0.618)` loses its low bits.
        // The 2x2 ordered-dither rank of a cell of a 2x2 block, {0, 2; 3, 1} read at (x, y).
        uint CloudDitherRank2x2(ivec2 cellOfBlock)
        {
            return cellOfBlock.y == 0 ? uint(cellOfBlock.x) * 2u : 3u - uint(cellOfBlock.x) * 2u;
        }

        float CloudMarchStartOffset(ivec2 latticeCell, ivec2 gridPhase, ivec2 subPixel, uint traceCycle)
        {
            float spatial  = fract(52.9829189f * fract(dot(vec2(latticeCell), vec2(0.06711056f, 0.00583715f))));
            uint  rank     = 4u * CloudDitherRank2x2(subPixel) + CloudDitherRank2x2(gridPhase);
            float temporal = float((traceCycle * 0x9E3779B9u) >> 8u) * (1.0f / 16777216.0f);
            return fract(spatial + float(rank) * (1.0f / 16.0f) + temporal);
        }

        void main()
        {
            // ONE 2x2 LATTICE PHASE OF THE TRACE GRID PER FRAME (Graphic::CloudTraceGridPhase, CLOUD-TURN-CACHE-1):
            // the dispatch is half the grid's width and half its height, and thread (x, y) marches texel
            // (2x + phase.x, 2y + phase.y), so (coord & 1) == phase on every texel written. The other three phases'
            // texels are not written this frame and CloudTemporalResolve.shader never reads them (its
            // CloudTraceTexelHoldsSample). The phase arrives as bit 0 the column's parity, bit 1 the row's.
            ivec2 size      = imageSize(CloudScatterTexture);
            int   phaseBits = int(CloudVisualize.y + 0.5f) & 3;
            ivec2 gridPhase = ivec2(phaseBits & 1, phaseBits >> 1);
            // GPU-FARFIELD-4: the ray this invocation is a segment lane of (CLOUD_MARCH_SEGMENT_LANES), and which.
            uint  segmentLane   = gl_GlobalInvocationID.x % CLOUD_MARCH_SEGMENT_LANES;
            uvec2 rayInvocation = uvec2(gl_GlobalInvocationID.x / CLOUD_MARCH_SEGMENT_LANES,
                                        gl_GlobalInvocationID.y);
            uint  laneBase      = gl_SubgroupInvocationID - segmentLane;
            ivec2 coord         = ivec2(rayInvocation) * 2 + gridPhase;
            if (coord.x >= size.x || coord.y >= size.y)
                return;

            // THE PROJECTION JITTER. The ray is reconstructed through the HALF-resolution pixel this
            // frame owns — (coord * 2 + offset) — and not through the centre of the quarter-res texel the
            // result is stored in. Clamped to the last half-res pixel because the quarter grid is the half
            // grid rounded UP: on an odd half-res extent the final column's block hangs half off the
            // image, and an unclamped uv there would reach past 1 and, with this engine's REPEAT samplers,
            // fetch the opposite edge of the aerial-perspective volume.
            ivec2 halfSize  = ivec2(max(CloudTraceParams.zw, vec2(1.0f, 1.0f)));
            ivec2 halfCoord = min(coord * 2 + ivec2(CloudTraceParams.xy), halfSize - ivec2(1, 1));

            vec2 uv  = (vec2(halfCoord) + vec2(0.5f, 0.5f)) / vec2(halfSize);
            vec2 ndc = vec2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);

            // The ray from the camera's own inverse view-projection, so it inherits whatever projection
            // the frame was drawn with. The engine is REVERSED-Z (Core/Projection.hpp): 1 is the near
            // plane and 0 the far one, which is why the two probe depths below are that way round and not
            // the other.
            vec4 nearH = InverseViewProjection * vec4(ndc.x, ndc.y, 1.0f, 1.0f);
            vec4 farH  = InverseViewProjection * vec4(ndc.x, ndc.y, 0.0f, 1.0f);
            vec3 nearP = nearH.xyz / max(nearH.w, 1e-9f);
            vec3 farP  = farH.xyz / max(farH.w, 1e-9f);
            vec3 rayDir = normalize(farP - nearP);

            // The angle one half-resolution pixel subtends along this ray, for the footprint step
            // (CloudFootprintStepKm): the ray through the next pixel column, against this one. Zero when
            // Scalability Effects.CloudFootprintSteps is off (CloudVisualize.z), which is the fixed fine step.
            float pixelAngle = 0.0f;
            if (CloudVisualize.z > 0.5f)
            {
                float ndcPixel  = 2.0f / float(halfSize.x);
                vec4  farNextH  = InverseViewProjection * vec4(ndc.x + ndcPixel, ndc.y, 0.0f, 1.0f);
                vec3  farNextP  = farNextH.xyz / max(farNextH.w, 1e-9f);
                pixelAngle      = length(normalize(farNextP - nearP) - rayDir);
            }

            CloudLayer layer = CloudUnpackLayer();

            // Planet-centre-relative kilometres: the frame in which both shells are centred on the origin.
            vec3 originKm = CloudPlanetOriginKm(CameraPosition.xyz, layer.PlanetRadiusKm);

            // The layer's chord with the authored limits — where tracing starts, and how far along the segment
            // it may run — one expression with the resolve, which classifies the same ray
            // (Common/CloudSceneDepth.glslh, CloudMarchSegment).
            vec2 segment = CloudMarchSegment(layer, originKm, rayDir, CloudMarch.z, CloudLayerGeometry.w);
            bool hasWork = CloudMarchHasWork(segment, CloudPhase.w);

            // Geometry occludes the layer. The depth target is full resolution and the ray belongs to a
            // HALF-resolution pixel, so the depth is fetched through halfCoord and not through the
            // quarter-res coord — sampling the depth at the texel the result is STORED in rather than at
            // the one the ray was CAST through would cut this frame's ray at a neighbour's geometry, and
            // the error would swing with the jitter, which is exactly the shimmer this work removes.
            //
            // The ray runs to the FARTHEST surface of the full-res block this half-res pixel covers
            // (Common/CloudSceneDepth.glslh, which says why one texel of the block flickered under thin
            // foliage). The cloud between that surface and a nearer one in the same block is removed per
            // full-res pixel by CloudUpsample.shader, which knows each pixel's own depth.
            float deviceDepth = CloudFootprintFarthestDepth(halfCoord, halfSize);

            // The guide's scene channel, resolved for EVERY pixel and not only for the ones that have
            // geometry. A sky pixel's ray is still cut somewhere — at the far plane — and giving it that
            // real distance rather than a sentinel is what lets the composite's coherence test see an
            // unbroken sky as unbroken instead of as a discontinuity against every silhouette in it.
            float farKm  = length(farP - CameraPosition.xyz) * (1.0f / CLOUD_WORLD_UNITS_PER_KM);
            float geomKm = farKm;

            // `> 0` IS THE GEOMETRY TEST under reversed-Z: a sky pixel stores 0, the far plane. Written as
            // `< 1` it would be true for every pixel including the sky, and every ray would be cut at the
            // far plane — which renders as no clouds at all, with nothing in the log.
            if (deviceDepth > 0.0f)
            {
                vec4 geomH = InverseViewProjection * vec4(ndc.x, ndc.y, deviceDepth, 1.0f);
                vec3 geomP = geomH.xyz / max(geomH.w, 1e-9f);
                geomKm     = length(geomP - CameraPosition.xyz) * (1.0f / CLOUD_WORLD_UNITS_PER_KM);
            }

            // GEOMETRY IN FRONT OF THE LAYER CUTS NOTHING (Common/CloudSceneDepth.glslh): a footprint wholly
            // nearer than the first march point holds no sample this frame. Nothing is marched for it, the
            // texel says so in the guide's .z, its .x is that first point (the nearest the cloud can be, a real
            // distance on the ray for the resolve's reprojection) and its .y the far plane — the scene distance
            // the resolve gives the same pixel, so the block's history survives the TAA jitter uncovering it.
            if (CloudFootprintOccluded(deviceDepth, geomKm, segment, hasWork))
            {
                imageStore(CloudScatterTexture, coord, vec4(0.0f, 0.0f, 0.0f, 1.0f));
                imageStore(CloudGuideTexture, coord, vec4(segment.x, farKm, 1.0f, 0.0f));
                return;
            }

            // Farther geometry cuts the ray where it stands.
            float sceneKm = geomKm;
            segment.y     = min(segment.y, geomKm);

            // A ray whose entry is beyond the cutoff is not traced at all. It bounds the cost of the
            // grazing rays that cost the most and show the least, and it is the second line of defence
            // behind the planet test: the geometry can legitimately report an entry thousands of
            // kilometres away, and nothing else in this loop would decline to march it.
            if (segment.x > max(CloudPhase.w, 0.0f))
            {
                imageStore(CloudScatterTexture, coord, vec4(0.0f, 0.0f, 0.0f, 1.0f));
                imageStore(CloudGuideTexture, coord, vec4(sceneKm, sceneKm, 0.0f, 0.0f));
                return;
            }

            if (segment.y <= segment.x)
            {
                imageStore(CloudScatterTexture, coord, vec4(0.0f, 0.0f, 0.0f, 1.0f));

                // This ray never entered the layer, so the end of its search IS the end of the ray. Both
                // guide channels therefore carry the same distance, which is what makes a whole region of
                // cloudless sky perfectly coherent to the composite and sends it down the cheap bilinear
                // path — a filter that fires everywhere quantizes the frame onto this pass's own grid.
                imageStore(CloudGuideTexture, coord, vec4(sceneKm, sceneKm, 0.0f, 0.0f));
                return;
            }

            float length_km = segment.y - segment.x;
            float stepCount = CloudStepCount(length_km, CLOUD_MIN_STEPS, max(CloudMarch.x, CLOUD_MIN_STEPS),
                                             CLOUD_DISTANCE_TO_MAX_STEPS_KM);
            int   stepTotal = int(stepCount);

            // TWO STEP SIZES, and the whole point is that they are spent in different places. The fine
            // step is Unreal's StepT — the segment over the count — and every lit sample is integrated over
            // it; the coarse one is the search stride, used only while the ray is outside cloud, where the
            // answer is exactly zero and any step size gives it correctly. On a short segment the two are
            // equal (the fine step already IS the search lattice the library is calibrated on); on a long
            // one the stride is up to CLOUD_COARSE_STEP_MULTIPLIER fine steps.
            //
            // The schedule's constants live in Common/CloudGeometry.glslh, together and next to the
            // relations they have to satisfy — they are numbers that must agree, and here they were
            // literals a hundred lines apart in a file no test can compile.
            float stepKm       = CloudFineStepKm(length_km, CLOUD_MIN_STEPS, max(CloudMarch.x, CLOUD_MIN_STEPS),
                                                 CLOUD_DISTANCE_TO_MAX_STEPS_KM);
            float coarseStepKm = CloudCoarseStepKm(length_km, stepKm, max(CloudMarch.x, CLOUD_MIN_STEPS));

            // THE ITERATION CEILING IS TWICE THE COUNT, and the count is still the cost: a lit sample
            // advances one fine step and none re-marches another's, so no ray integrates more than about
            // stepTotal of them. The second half pays for the step-backs — each entry into a body
            // re-marches (coarse - fine) of the segment — on a ray that enters many bodies and has few
            // empty strides to buy them back with; bounded by the count alone that ray ended its march
            // before the shell's far side. Every other ray leaves at segment.y first.
            int   iterationTotal = 2 * stepTotal;

            // The start offset inside the first step, so the sample planes of neighbouring pixels do not
            // line up. Without it the uniform schedule draws the layer as a set of concentric shells —
            // the banding is the step planes themselves, seen edge-on.
            //
            // The offset moves from trace to trace, so the reconstruction integrates it; a still frame of
            // this pass is therefore NOT a valid check of it — it shows one realization of the dither, not
            // what the viewer sees. Spatio-temporal low-discrepancy, not white (CloudMarchStartOffset).
            float jitter = CloudMarchStartOffset(ivec2(rayInvocation), gridPhase, ivec2(CloudTraceParams.xy),
                                                 uint(CameraPosition.w));

            CloudFieldParams params = CloudUnpackFieldParams();

            // THE DIAGNOSTIC VIEW (--cloud-visualize). Draws what the cut decided instead of the cloud, at
            // full opacity, so a frame says WHICH term made the sky look the way it does.
            //   1: at the ray's entry into the layer — R the local weather W, G the deepest profile over eight
            //      heights of the column, B 1 where the weather's remap keeps it.
            //   2: along the ray, 32 samples — R the share the remap keeps, G the largest weather step between
            //      neighbours x 8 (a seam reads bright), B the share inside a baked body (species profile
            //      > 0); R is at most B, since air is never cloud (CUT-CORE).
            //   3: at the ray's entry into the layer — which clip level of the modelling volume answers there
            //      (CloudProceduralLevelOf): level 0 red, 1 green, 2 blue, and inside a blend band the
            //      coarser level's colour mixed in by the blend weight, so the bands read as gradients; past
            //      level 2's window the far path (CloudSampleProceduralFar) grey, level 2's band blue into it.
            //   4: THE MARCH'S COST, per ray, from counters the ordinary march below keeps (GPU-CLOUD-MARCH-2) —
            //      R the view ray's field evaluations / 256 (search and fine samples; a column-probe jump is not
            //      one), G the lit samples / 64, B the light samples they scheduled / 512, so 8 B / G is the
            //      light samples per lit sample (the near march's count inside the shadow map, the full ray's
            //      outside it, both in its border band). Rays that march nothing (geometry in front of the
            //      layer, past the cutoff, no entry) keep the ordinary output.
            const int kCloudVisualizeMarchCost = 4;
            int       visualize                = int(CloudVisualize.x + 0.5f);
            if (visualize != 0 && visualize != kCloudVisualizeMarchCost)
            {
                vec3 shown = vec3(0.0f);
                if (visualize == 3)
                {
                    vec3  entryKm   = originKm + rayDir * segment.x;
                    vec2  column    = (entryKm - params.WindOffsetKm).xz;
                    int   level     = CloudProceduralLevelOf(params, column);
                    vec3  colours[3] = vec3[3](vec3(1.0f, 0.0f, 0.0f), vec3(0.0f, 1.0f, 0.0f), vec3(0.0f, 0.0f, 1.0f));
                    shown           = colours[level];
                    vec3  coarser   = level < CLOUD_PROCEDURAL_CLIP_LEVELS - 1 ? colours[level + 1] : vec3(0.5f);
                    shown           = mix(coarser, colours[level], CloudProceduralLevelWeight(params, level, column));
                }
                else if (visualize == 1)
                {
                    vec3  entryKm = originKm + rayDir * segment.x;
                    vec3  windPos = entryKm - params.WindOffsetKm;
                    // The weather is per species (WX-PAINT): each height asks its own winner's W, and
                    // R is the W at the height whose profile is the deepest; G that profile; B whether the
                    // remap keeps it.
                    float shownWeather = 0.0f;
                    float deepest      = 0.0f;
                    for (int k = 0; k < 8; ++k)
                    {
                        vec4  volume = CloudSampleProceduralVolume(params, (float(k) + 0.5f) / 8.0f, windPos);
                        int   winner = 0;
                        for (int slot = 1; slot < min(params.SpeciesCount, CLOUD_SPECIES_SLOTS); ++slot)
                            winner = volume[slot] > volume[winner] ? slot : winner;
                        if (k == 0 || volume[winner] > deepest)
                        {
                            deepest      = volume[winner];
                            shownWeather = CloudLocalWeather(params.Weather, params.LayoutPlace, params.LayoutStrength,
                                                             winner, windPos.xz);
                        }
                    }
                    shown = vec3(shownWeather, deepest, CloudCoverRemap(deepest, shownWeather) > 0.0f ? 1.0f : 0.0f);
                }
                else
                {
                    float kept = 0.0f, body = 0.0f, seam = 0.0f, previous = -1.0f;
                    for (int k = 0; k < 32; ++k)
                    {
                        float tk      = mix(segment.x, segment.y, (float(k) + 0.5f) / 32.0f);
                        vec3  p       = originKm + rayDir * tk;
                        float hf      = CloudHeightFraction(layer, p);
                        vec3  windPos = vec3(p.x, length(p) - layer.BottomRadiusKm, p.z) - params.WindOffsetKm;
                        vec4  volume  = CloudSampleProceduralVolume(params, hf, windPos);
                        int   winner  = 0;
                        for (int slot = 1; slot < min(params.SpeciesCount, CLOUD_SPECIES_SLOTS); ++slot)
                            winner = volume[slot] > volume[winner] ? slot : winner;
                        float profile = volume[winner];
                        float w       = CloudLocalWeather(params.Weather, params.LayoutPlace, params.LayoutStrength,
                                                          winner, windPos.xz);
                        kept += CloudCoverRemap(profile, w) > 0.0f ? 1.0f : 0.0f;
                        body += profile > 0.0f ? 1.0f : 0.0f;
                        if (previous >= 0.0f)
                            seam = max(seam, abs(w - previous));
                        previous = w;
                    }
                    shown = vec3(kept / 32.0f, min(seam * 8.0f, 1.0f), body / 32.0f);
                }
                float sunLuminance = dot(CloudSunColour.rgb, vec3(0.2126f, 0.7152f, 0.0722f));
                imageStore(CloudScatterTexture, coord, vec4(shown * sunLuminance * 0.03f, 0.0f));
                imageStore(CloudGuideTexture, coord, vec4(segment.x, sceneKm, 0.0f, 0.0f));
                return;
            }

            vec3  toSun        = normalize(CloudSun.xyz);
            float phase        = CloudPhaseDualLobe(dot(rayDir, toSun), CloudWind.w,
                                                    CloudPhase.x, CloudPhase.y);
            float extinction   = max(CloudMarch.w, 0.0f);
            // PER COLOUR. Clamped here as well as in the packer, on the terms every other clamp in this
            // file is: this is the side that decides what the loop does, and a channel above one makes the
            // scattering series diverge rather than converge.
            vec3  albedo       = clamp(CloudAlbedo.rgb, vec3(0.0f), vec3(1.0f));
            float lightMarchKm = max(CloudSun.w, 0.0f);
            // SIXTY-FOUR, and it must equal ECS::kCloudLightMarchMaxSamples. This clamp cannot include the
            // C++ constant, so Desert/Tests/Engine/SettingConsumers reads this line's text and fails if the
            // two ever part company — while both were the literal 16 an artist could raise the slider past
            // a ceiling the march quietly reimposed.
            int   lightSamples = int(clamp(CloudSunColour.w, 1.0f, 64.0f));
            float stopT        = clamp(CloudMarch.y, 0.0f, 1.0f);

            // The multiple-scattering series, unpacked once per pixel rather than per sample: the four
            // fields are uniform over the whole dispatch, and rebuilding the struct inside the inner loop
            // would put four loads under the hottest branch in the subsystem.
            CloudScatterSeries series;
            series.Octaves     = CloudMultiScatter.x;
            series.ScatterStep = CloudMultiScatter.y;
            series.ExtinctStep = CloudMultiScatter.z;
            series.PhaseStep   = CloudMultiScatter.w;

            // The ambient a cloud sits in, resolved once per pixel rather than per step: in the physical
            // model it is the marched full-sphere mean scaled by the artist's factor, in the artistic
            // gradient it is the dome value the packer already folded down. Neither model is a stand-in
            // for the other, which is why the choice is made from a gate rather than from whichever
            // happens to be non-zero.
            vec3 ambientRadiance = CloudAmbient.rgb;
            if (CloudAmbient.w > 0.5f)
            {
                ambientRadiance = CloudAmbient.rgb *
                                  texelFetch(DistantSkyLightTexture, ivec2(SKY_DISTANT_LIGHT_SPHERE_TEXEL, 0), 0).rgb;
            }

            vec3  luminance     = vec3(0.0f, 0.0f, 0.0f);
            float transmittance = 1.0f;

            // GPU-FARFIELD-4: THIS LANE'S STRETCH of the segment (CLOUD_MARCH_SEGMENT_LANES), with the same start
            // offset inside its first step as the whole ray had at its entry. Everything below that read the
            // segment's ends as the march's own bounds reads these; the guide's "end of search" stays segment.y.
            float laneLengthKm = (segment.y - segment.x) / float(CLOUD_MARCH_SEGMENT_LANES);
            float laneStartKm  = segment.x + laneLengthKm * float(segmentLane);
            float laneEndKm    = segment.y;
            if (segmentLane + 1u < CLOUD_MARCH_SEGMENT_LANES)
                laneEndKm = laneStartKm + laneLengthKm;
            float t             = laneStartKm + jitter * stepKm;
            // The lane's first sample. The coarse tier's step-back never lands before it (below): a lane that
            // starts inside a body would otherwise restart its fine samples at laneStartKm itself, a distance
            // with no dither in it, and that lane's samples would then sit on the same planes every frame - a
            // shell the temporal reconstruction cannot average away, drawn once per lane boundary.
            float laneFirstKm   = t;

            // Where along the ray this pixel's cloud effectively IS, weighted by how much of the ray's
            // light each sample still gets to contribute. A cloud is not at one distance, but the
            // atmosphere in front of it has to be evaluated at one — and the transmittance-weighted mean
            // is the distance at which a single evaluation is closest to the integral. Weighting by depth
            // instead would put the answer inside the far side of a thick cloud, where nothing visible
            // happens.
            float aerialWeightedT = 0.0f;
            float aerialWeightSum = 0.0f;

            // The guide's cloud channel is THE SAME WEIGHTED MEAN, Unreal's tAP (VolumetricCloud.usf:1228-1230,
            // :1493), resolved after the loop. It was the distance of the FIRST sample with any material, and on
            // a thin wisp that is one sample of one realisation of the start-offset dither: every fourth frame the
            // owned pixel's front jumped between "found at the wisp" and "end of the search" behind it, and the
            // reprojection and the composite's edge filter jumped with it (SKY-FLICKER-3). The mean moves only as
            // much as the transmittance it is weighted by.
            //
            // "NO CLOUD" BELOW 1 % OPACITY, Unreal's threshold (:1731, `GrayScaleTransmittance > 0.99`): a ray that
            // kept more than this of its light reports the end of its own search, as one that met nothing does,
            // so a trace of haze is not a silhouette for the readers that judge edges by this channel.
            const float kCloudFrontClearTransmittance = 0.99f;

            // The two-tier state. `emptyRun` counts CONSECUTIVE fine samples that found no profile; after
            // enough of them the march decides it has left the cloud and returns to coarse steps.
            bool fine     = false;
            int  emptyRun = 0;

            // BOTH TIERS JUDGE BY THE SAME QUANTITY — the un-eroded profile. This is not a detail. If the
            // coarse tier tested the profile and the fine tier tested the eroded density, then everywhere
            // the profile is positive and the erosion has cut it to zero — which is most of a procedural
            // cloudscape — the machine would drop to fine, find nothing, walk back to coarse, and land in
            // the same place. Net advance per cycle would be zero and the march would stand still while
            // burning its whole budget. CloudTwoTierCycleAdvanceKm is that net advance, and
            // Desert/Tests/Engine/CloudGeometry asserts it stays positive.
            const int kEmptyFineSamplesBeforeCoarse = CLOUD_EMPTY_FINE_SAMPLES_BEFORE_COARSE;

            // THE CONSERVATIVE PROBE OF THE COARSE TIER (CloudProceduralColumnsEmpty, CloudEmptyColumnAdvanceKm in
            // Common/CloudGeometry.glslh): before the exact search sample, the column-max image's texels of the
            // height bands the jump would cross say whether any column within the probe radius can hold cloud at
            // those heights; where none can, the march jumps the whole reach on the search lattice without
            // sampling it. The bound covers the procedural producer only, so a layer with sculpted bodies keeps
            // the exact search everywhere — their boxes are not in the image.
            bool  probeColumns       = CLOUD_AUTHORED_COUNT == 0;
            float emptyColumnAdvance = CloudEmptyColumnAdvanceKm(length(rayDir.xz), coarseStepKm);
            // The jump visits none of the lattice points before the one it lands on, so the stretch whose
            // heights the probe must bound ends one stride short of the landing point.
            float emptyColumnSkipKm  = emptyColumnAdvance - coarseStepKm;
            // The same jump in lattice points, for the grouped search's walk (CLOUD_COARSE_SEARCH_GROUP): the
            // advance is a whole number of strides by construction, so the rounding only absorbs the division's.
            int   emptyColumnStrides = max(int(emptyColumnAdvance / max(coarseStepKm, 1e-6f) + 0.5f), 1);

            // The cost census (--cloud-visualize 4): integer adds beside the fetches they count.
            int   censusFieldEvaluations = 0;
            int   censusLitSamples       = 0;
            float censusLightSamples     = 0.0f;
            int   censusColumnJumps      = 0;
            int   censusFullLightRays    = 0;
            // Left by the stretch's end or by the transmittance, not by the iteration ceiling.
            bool  laneFinished           = false;
            // Still marching. A lane that is done stays in the loop, idle, until every lane of its ray is done:
            // the lanes of one ray read each other's transmittance at the top of every iteration below.
            bool  laneActive             = true;

            for (int i = 0; i < iterationTotal; ++i)
            {
                // THE RAY STOPS WHERE ITS COMPOSED TRANSMITTANCE DOES, not where each lane's own does
                // (FPS-CLOUDS-MARCH). Lane k's light reaches the eye through P_k = prod_{j<k} T_j, the lanes in
                // front of it, and a single march - Unreal's - ends once P_k * T_k falls under the stop. Judged by
                // its local T alone a back lane kept marching, and lighting, a stretch behind a front that had
                // already gone opaque. The front's T is read WHILE it still marches, and it only falls, so the
                // product here is an upper bound on the final one and a lane stopped by it is past the stop.
                //
                // Every lane of the ray is in this loop on every iteration (the bound and the exits are per ray),
                // so each shuffle reads an active lane. A lane that stops here or below goes idle; the ray leaves
                // the loop on the iteration after its last lane did, seen through the activity read here.
                float frontT    = 1.0f;
                bool  rayActive = false;
                vec2  ownState  = vec2(transmittance, float(laneActive));
                for (uint lane = 0u; lane < CLOUD_MARCH_SEGMENT_LANES; ++lane)
                {
                    vec2 laneState = subgroupShuffle(ownState, laneBase + lane);
                    if (lane < segmentLane)
                        frontT *= laneState.x;
                    rayActive = rayActive || laneState.y > 0.5f;
                }
                if (!rayActive)
                    break;
                if (!laneActive)
                    continue;
                if (t >= laneEndKm || frontT * transmittance < stopT)
                {
                    laneFinished = true;
                    laneActive   = false;
                    continue;
                }

                if (!fine)
                {
                    // THE COARSE TIER, CLOUD_COARSE_SEARCH_GROUP LATTICE POINTS AT A TIME (FPS-CLOUDS-MARCH-3).
                    // Every fetch first, for all the points at once: the column probe and the exact profile of
                    // each, neither waiting on another point's answer. The profile is fetched even where the
                    // probe clears, because a fetch behind the probe's answer is a fetch that waits for it; a
                    // cleared point's profile is zero by the probe's own bound and the walk never reads it.
                    //
                    // The field's columns are planet x and z minus the wind (CloudSampleProceduralField), so the
                    // ray's column moves by exactly rayDir.xz per kilometre of ray — the rate the jump divides
                    // by. Its height is not linear in the ray (the shell is a sphere), so the bands the probe
                    // reads are those of the stretch's own lowest and highest point (CloudRayHeightFractionSpan),
                    // clipped to the segment the march would stop at anyway.
                    float searchT[CLOUD_COARSE_SEARCH_GROUP];
                    bool  searchCleared[CLOUD_COARSE_SEARCH_GROUP];
                    float searchProfile[CLOUD_COARSE_SEARCH_GROUP];
                    for (int j = 0; j < CLOUD_COARSE_SEARCH_GROUP; ++j)
                    {
                        float pointT   = t + float(j) * coarseStepKm;
                        vec3  pointPos = originKm + rayDir * pointT;
                        bool  cleared  = false;
                        if (probeColumns)
                        {
                            vec2 skippedHeights = CloudRayHeightFractionSpan(layer, originKm, rayDir, pointT,
                                                                             min(pointT + emptyColumnSkipKm,
                                                                                 segment.y));
                            cleared = CloudProceduralColumnsEmpty(params, pointPos.xz - params.WindOffsetKm.xz,
                                                                  CLOUD_COLUMN_PROBE_RADIUS_KM, skippedHeights.x,
                                                                  skippedHeights.y);
                        }
                        // The field takes the ALTITUDE above the layer's base, as every fetch below does.
                        vec3 pointField  = vec3(pointPos.x, length(pointPos) - layer.BottomRadiusKm, pointPos.z);
                        searchT[j]       = pointT;
                        searchCleared[j] = cleared;
                        searchProfile[j] =
                            SampleCloudField(params, CloudHeightFraction(layer, pointPos), pointField).Profile;
                    }

                    // THE WALK, which is the one-at-a-time search's own: a point the probe clears jumps
                    // emptyColumnStrides points (the reach it proved, CloudEmptyColumnAdvanceKm), one with no
                    // profile steps to the next, the first with a profile is the hit, and a point at or past the
                    // lane's end ends the walk where the old loop's head would have ended the lane. Points the
                    // walk does not land on were fetched and are not read; every one of them lies inside a reach
                    // the probe cleared or past the walk's stop, exactly the points the old search never visited.
                    int next = 0;
                    int hit  = -1;
                    for (int j = 0; j < CLOUD_COARSE_SEARCH_GROUP; ++j)
                    {
                        if (j < next)
                            continue;
                        if (searchT[j] >= laneEndKm)
                            break;
                        if (searchCleared[j])
                        {
                            next = j + emptyColumnStrides;
                            ++censusColumnJumps;
                            continue;
                        }
                        ++censusFieldEvaluations;
                        if (searchProfile[j] > 0.0f)
                        {
                            hit = j;
                            break;
                        }
                        next = j + 1;
                    }

                    // Outside cloud: keep striding, from the point the walk stopped on (a jump may land past
                    // the group; the stride count is the same either way).
                    if (hit < 0)
                    {
                        t += float(next) * coarseStepKm;
                        continue;
                    }

                    // On the first hit, drop to fine and step BACK to the first fine position past the last
                    // empty coarse sample, so the material between that sample and this one is not skipped —
                    // that gap is up to a whole coarse step of cloud and its absence reads as a flat face.
                    // Where the stride IS the fine step there is no gap: this sample is the next fine one, and
                    // it is integrated where it stands (its field is read again below, on the fine path).
                    t        = searchT[hit];
                    fine     = true;
                    emptyRun = 0;
                    // The step-back target is past the lane's first sample for every coarse sample but that first
                    // one, whose gap behind it is the lane's own dither offset: that hit is integrated where it
                    // stands, on the ray's dither, rather than re-fetched at the undithered lane start
                    // (laneFirstKm above).
                    float hitStepKm     = CloudFootprintStepKm(stepKm, coarseStepKm, t, pixelAngle);
                    float steppedBackKm = t - coarseStepKm + hitStepKm;
                    if (coarseStepKm > hitStepKm && steppedBackKm > laneFirstKm)
                    {
                        t = steppedBackKm;
                        continue;
                    }
                }

                // The step this sample is integrated over and advances by: the fine step, grown with the texel
                // footprint at this distance up to the search stride (CloudFootprintStepKm; pixelAngle 0 = fine).
                float litStepKm = CloudFootprintStepKm(stepKm, coarseStepKm, t, pixelAngle);

                vec3  samplePos      = originKm + rayDir * t;
                float heightFraction = CloudHeightFraction(layer, samplePos);

                // The field is handed the ALTITUDE above the layer's base, never the planet-relative
                // height. Two reasons, and the second is the one that bites: the noise is periodic, so a
                // y of 6363 kilometres wraps to something arbitrary rather than to something meaningful;
                // and at that magnitude float32 resolves 0.4 metres, which quantizes the vertical
                // structure of a two-kilometre layer onto a visible ladder.
                vec3 fieldPos = vec3(samplePos.x, length(samplePos) - layer.BottomRadiusKm, samplePos.z);

                CloudFieldSample field = SampleCloudField(params, heightFraction, fieldPos);
                ++censusFieldEvaluations;

                if (field.Profile <= 0.0f)
                {
                    ++emptyRun;
                    if (emptyRun >= kEmptyFineSamplesBeforeCoarse)
                    {
                        fine     = false;
                        emptyRun = 0;
                    }
                    t += litStepKm;
                    continue;
                }

                emptyRun = 0;

                {
                    float density = CloudSampleDensity(params, field, fieldPos);

                    // The near-camera fade. A camera inside the layer otherwise meets full density at
                    // arm's length, which fills the screen with a flat wall; UE fades the nearest metres
                    // for exactly this. Both distances at zero leave the density untouched.
                    if (CloudFadeParams.z > 0.0f)
                        density *= smoothstep(CloudFadeParams.w, CloudFadeParams.z, t);

                    if (density > 0.0f)
                    {
                        // The winning species' own opacity, per sample — through the MEDIUM, so a
                        // material that authors its own extinction is obeyed here and, identically, on
                        // the shadow ray, the shadow map and the occlusion volume. See the note in
                        // CloudLightOpticalDepth.
                        float sigmaT = density * extinction *
                                       CloudSampleExtinctionFactor(params, field, fieldPos);

                        // ONE shadow ray serves every scattering order. That is the whole economy of the
                        // octave approximation: the expensive part is finding how much material lies
                        // between this sample and the sun, and each order then reuses that number with its
                        // own extinction scale.
                        vec2  lightCost;
                        float opticalDepth = CloudSelfShadowOpticalDepth(layer, params, samplePos, toSun,
                                                                         lightMarchKm, lightSamples, extinction,
                                                                         lightCost);
                        ++censusLitSamples;
                        censusLightSamples += lightCost.x;
                        censusFullLightRays += int(lightCost.y > 0.5f);

                        // HOW MUCH OF THE SKY THIS SAMPLE CAN SEE, by one of two geometries.
                        //
                        // The strength is the SAME field either way — CloudPhase.z, the artist's
                        // AmbientOcclusionStrength — and it keeps one meaning, "how strongly the sky light
                        // is occluded". What the layer's flag chooses is which occluder computes it, so
                        // neither path leaves the other's parameters dead.
                        //
                        // VOLUME OFF: the sample's own Profile, i.e. its depth inside its OWN body.
                        // Authored, not fixed at full: UE carries the amount in the alpha of its albedo
                        // parameter, so it IS a dial there.
                        //
                        // VOLUME ON, WHICH IS THE DEFAULT SINCE Р12: the diffuse transmittance of
                        // everything ABOVE this column at this altitude.
                        // It REPLACES the profile term rather than multiplying it, and that is
                        // deliberate — the column integrated from the sample upward already contains the
                        // upper half of the sample's own body, so applying both would count that material
                        // twice and darken a cloud's own core for a reason nobody could find later.
                        float ambientOcclusion = CloudAmbientOcclusion(field.Profile, CloudPhase.z);
                        if (CloudFrame.x > 0.5f)
                        {
                            // The WIND-SHIFTED position, because that is the frame the volume was traced
                            // in — the same subtraction Common/CloudField.glslh makes on its own way into
                            // the modelling volume, and the reason the two land on the same texel however
                            // far the wind has run.
                            vec3 skyWindPos = vec3(fieldPos.x - params.WindOffsetKm.x,
                                                   fieldPos.y - params.WindOffsetKm.y,
                                                   fieldPos.z - params.WindOffsetKm.z);

                            vec3 skyUvw = CloudSkyOcclusionUvw(params.ProceduralLevel[CLOUD_PROCEDURAL_CLIP_LEVELS - 1].xy,
                                                               params.ProceduralLevel[CLOUD_PROCEDURAL_CLIP_LEVELS - 1].z,
                                                               heightFraction, skyWindPos);

                            // textureLod AND NOT texture, for the reason every other volume fetch in this
                            // file gives: a compute shader has no derivatives, so the implicit level of
                            // detail is undefined.
                            ambientOcclusion =
                                CloudSkyOcclusion(textureLod(CloudSkyOcclusionVolumeTexture, skyUvw, 0.0f).r,
                                                  CloudPhase.z);
                        }

                        // Wrenninge's multiple-scattering octaves, as Unreal implements them — the series
                        // itself is Common/CloudLighting.glslh's CloudMultiScatterStep, which is where its
                        // arrangement and its price are written down. It lives there rather than here so
                        // that Desert/Tests/Engine/CloudLighting can drive it as C++ and bound it against a
                        // converged Monte Carlo; inside this loop nothing could reach it.
                        //
                        // The ray's transmittance is applied HERE and not inside, because it belongs to the
                        // ray's history rather than to the medium — and keeping it out is what lets the
                        // series be bounded by the source it integrates.
                        // THE SUN THIS SAMPLE SEES. Resolved once per sample and handed to the series as
                        // its source, because the atmosphere between this point and the sun is the same
                        // atmosphere whether the light arriving is first-order or third — the octave loop
                        // inside CloudMultiScatterStep would otherwise repeat the fetch per order.
                        // THE MEDIUM'S LAST TWO OUTPUTS, taken here because both are per-SAMPLE in the
                        // Volume domain's contract even though the shipped material authors them per
                        // layer. Each default hands its argument straight back, so the two calls fold
                        // away and the loop is the loop it was.
                        ambientOcclusion = CloudSampleOcclusion(params, field, fieldPos, ambientOcclusion);

                        // THE CLOUD BOTTOM, which neither occluder above can see: both measure cloud, and what
                        // darkens a cumulus base is the GROUND under it — a sample at the bottom of the layer
                        // looks down at dark earth where one at the top looks up at open sky. Unreal's
                        // SkyLightCloudBottomOcclusion, by height in the layer (CloudLighting.glslh). Applied
                        // AFTER the medium's own occlusion so a graph that writes the pin still gets the base
                        // darkened, as the authored layer setting promises.
                        ambientOcclusion *= CloudSkyLightBottomVisibility(heightFraction, CloudAlbedo.w);
                        vec3 sampleAlbedo = CloudSampleAlbedo(params, field, fieldPos, albedo);

                        luminance += transmittance *
                                     CloudMultiScatterStep(series, CloudSunColourAt(layer, samplePos, toSun),
                                                           ambientRadiance * ambientOcclusion, opticalDepth,
                                                           phase, sigmaT, sampleAlbedo, litStepKm);

                        // EMISSION: radiance the medium ADDS, per kilometre of march, attenuated by the
                        // ray's own history and by nothing else. It is deliberately NOT multiplied by the
                        // absorption coefficient — Unreal makes the same choice for the same reason, that
                        // an artist authoring a glow wants the glow they authored and not the glow times
                        // however opaque the cloud happens to be there (VolumetricCloud.usf:1381-1384).
                        // The default medium returns a literal zero, so this line costs nothing until a
                        // graph writes the pin.
                        luminance += transmittance * CloudSampleEmissive(params, field, fieldPos) * litStepKm;

                        // Recorded BEFORE the ray is attenuated: the weight is how much this sample was
                        // able to contribute, not how much is left after it. Also the guide's front (above),
                        // and placed before the stop below so a ray stopped on transmittance keeps it.
                        aerialWeightedT += t * transmittance;
                        aerialWeightSum += transmittance;

                        // The view ray is attenuated by the medium itself, once — the octaves are orders
                        // of scattering INTO this ray, not extra material along it.
                        transmittance *= CloudBeerTransmittance(sigmaT, litStepKm);

                        // The lane's own stop, the one test above that needs no other lane; it goes idle
                        // rather than leaving, so its ray's lanes keep reading it.
                        if (transmittance < stopT)
                        {
                            laneFinished = true;
                            laneActive   = false;
                            continue;
                        }
                    }
                }

                t += litStepKm;
            }

            // GPU-FARFIELD-4: THE LANES COMPOSED FRONT TO BACK (CLOUD_MARCH_SEGMENT_LANES). Every lane of the ray
            // reaches this point (each one left the loop; nothing above returned for one lane and not another),
            // so every shuffle below reads an active lane. Each lane's own sums are weighted by the transmittance
            // of the lanes in front of it; afterwards every lane holds the whole ray's result.
            {
                vec3  composedLuminance = vec3(0.0f, 0.0f, 0.0f);
                float composedWeightedT = 0.0f;
                float composedWeightSum = 0.0f;
                float composedT         = 1.0f;
                for (uint lane = 0u; lane < CLOUD_MARCH_SEGMENT_LANES; ++lane)
                {
                    uint  source = laneBase + lane;
                    vec3  laneL  = subgroupShuffle(luminance, source);
                    float laneW  = subgroupShuffle(aerialWeightedT, source);
                    float laneS  = subgroupShuffle(aerialWeightSum, source);
                    float laneT  = subgroupShuffle(transmittance, source);
                    composedLuminance += composedT * laneL;
                    composedWeightedT += composedT * laneW;
                    composedWeightSum += composedT * laneS;
                    composedT         *= laneT;
                }
                luminance       = composedLuminance;
                aerialWeightedT = composedWeightedT;
                aerialWeightSum = composedWeightSum;
                transmittance   = composedT;
            }

            // The march counters: the per-lane ones from every lane, the per-ray ones from the first lane only.
            bool firstLane = segmentLane == 0u;
            CloudMarchCount(CLOUD_MARCH_COUNT_RAYS, uint(firstLane));
            CloudMarchCount(CLOUD_MARCH_COUNT_FIELD_SAMPLES, uint(censusFieldEvaluations));
            CloudMarchCount(CLOUD_MARCH_COUNT_LIT_SAMPLES, uint(censusLitSamples));
            CloudMarchCount(CLOUD_MARCH_COUNT_LIGHT_SAMPLES, uint(censusLightSamples + 0.5f));
            CloudMarchCount(CLOUD_MARCH_COUNT_EARLY_EXIT, uint(firstLane && transmittance < stopT));
            CloudMarchCount(CLOUD_MARCH_COUNT_CEILING_LANES, uint(!laneFinished));
            CloudMarchCount(CLOUD_MARCH_COUNT_COLUMN_JUMPS, uint(censusColumnJumps));
            CloudMarchCount(CLOUD_MARCH_COUNT_FULL_LIGHT_RAYS, uint(censusFullLightRays));

            // The census view shows the whole ray's cost: the lanes' counts summed into every lane.
            {
                int   rayFieldEvaluations = 0;
                int   rayLitSamples       = 0;
                float rayLightSamples     = 0.0f;
                for (uint lane = 0u; lane < CLOUD_MARCH_SEGMENT_LANES; ++lane)
                {
                    rayFieldEvaluations += subgroupShuffle(censusFieldEvaluations, laneBase + lane);
                    rayLitSamples       += subgroupShuffle(censusLitSamples, laneBase + lane);
                    rayLightSamples     += subgroupShuffle(censusLightSamples, laneBase + lane);
                }
                censusFieldEvaluations = rayFieldEvaluations;
                censusLitSamples       = rayLitSamples;
                censusLightSamples     = rayLightSamples;
            }

            // One lane stores the ray; the others have nothing of their own left to write.
            if (!firstLane)
                return;

            // AERIAL PERSPECTIVE. Sixty kilometres of air between the camera and a distant cloud is not
            // nothing: it scatters its own light in and attenuates what comes back, which is why a real
            // cloud on the horizon is closer to the colour of the sky than to the colour of a cloud.
            // Without this term the layer ends at the horizon as an opaque white wall, and no amount of
            // coverage, density or fade fixes it — the wall is the absence of the atmosphere, not the
            // presence of too much cloud.
            //
            // One fetch per pixel at the weighted distance above; the composition and the art-direction
            // ramp are Common/CloudAerial.glslh, shared with the panorama bake so the screen and the IBL
            // cannot drift.
            if (CloudAerial.z > 0.5f && aerialWeightSum > 0.0f)
            {
                float meanDistanceKm =
                     CloudAerialMeanDistanceKm(aerialWeightedT, aerialWeightSum, CloudAerial.y);
                float sliceUnit = SkyApSliceUnitFromDistance(meanDistanceKm, CloudAerial.x);

                // Read through the exact inverse of the fill's texel-centre remap on all three axes, so
                // the frame's edges and the volume's near plane land on written texels rather than
                // halfway between them.
                vec3 uvw = vec3(SkyUnitToTexelUv(uv.x, SKY_AP_VOLUME_WIDTH),
                                SkyUnitToTexelUv(uv.y, SKY_AP_VOLUME_HEIGHT),
                                SkyUnitToTexelUv(sliceUnit, SKY_AP_VOLUME_DEPTH));

                vec4 aerial = texture(CloudAerialPerspectiveTexture, uvw);

                luminance = CloudApplyAerialPerspective(
                     luminance, transmittance, aerial.rgb, aerial.a,
                     CloudAerialAmount(meanDistanceKm, CloudFadeParams.x, CloudFadeParams.y));
            }

            if (visualize == kCloudVisualizeMarchCost)
            {
                vec3  cost         = vec3(float(censusFieldEvaluations) / 256.0f, float(censusLitSamples) / 64.0f,
                                          censusLightSamples / 512.0f);
                float sunLuminance = dot(CloudSunColour.rgb, vec3(0.2126f, 0.7152f, 0.0722f));
                imageStore(CloudScatterTexture, coord, vec4(min(cost, vec3(1.0f)) * sunLuminance * 0.03f, 0.0f));
                imageStore(CloudGuideTexture, coord, vec4(segment.x, sceneKm, 0.0f, 0.0f));
                return;
            }

            imageStore(CloudScatterTexture, coord, vec4(luminance, clamp(transmittance, 0.0f, 1.0f)));

            // A ray that marched the whole segment and found nothing reports THE END OF ITS OWN SEARCH.
            // That is a real distance rather than a magic number, and it is the one that makes the
            // bilateral weight behave: an empty neighbour of a cloud at 3 km reports the far side of the
            // shell, several kilometres away, so the weight `1 / (dKm * 1000 + 1)` rejects it outright.
            // A large sentinel would do the same job here but not at the horizon, where a whole
            // neighbourhood of empty rays must agree with each other exactly enough to stay coherent —
            // and half precision has no room for a sentinel that is both huge and finite.
            //
            // The front is the transmittance-weighted mean distance (tAP above), UNSCALED: the aerial
            // perspective's view-distance dial (CloudAerialMeanDistanceKm) is an art direction of the haze and
            // says nothing about where the cloud is.
            bool  cloudMet     = aerialWeightSum > 0.0f && transmittance <= kCloudFrontClearTransmittance;
            float cloudFrontKm = cloudMet ? aerialWeightedT / aerialWeightSum : segment.y;
            imageStore(CloudGuideTexture, coord, vec4(cloudFrontKm, sceneKm, 0.0f, 0.0f));
        }
    }
}
