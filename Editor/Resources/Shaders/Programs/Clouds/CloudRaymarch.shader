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
    // resolved the material — so the march's own GLSL below reads the same `u_Cloud*` fields it always
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

    Properties
    {
        CloudType CloudType1 ("Cloud Type 1", Category("Cloud Types"), Timing(Rebake), Tooltip("Which kind of cloud this layer is made of - drag a .decloudtype from the Content Browser or double-click one there to edit it. The type carries its own base and top in kilometres, its own family of vertical profiles, its own edge character, its own density and its own placement scale, and the shell the march intersects is COMPUTED from all of them together. A layer may carry up to four; they OVERLAP rather than divide the sky between them, so a low deck and a tall tower can stand in the same place. All four empty uses the engine's built-in cumulus congestus."))
        CloudType CloudType2 ("Cloud Type 2", Category("Cloud Types"), Timing(Rebake), Tooltip("A second kind of cloud in the same sky. Its placement field is its OWN - its own scale, its own stretch along the wind, its own patches - so it is not a share of the first type's sky but an independent one laid over it. Where two types meet, the deeper body wins and keeps its own edge and density."))
        CloudType CloudType3 ("Cloud Type 3", Category("Cloud Types"), Timing(Rebake), Tooltip("A third kind of cloud. Costs one noise fetch per sample only at altitudes its own base and top actually reach - a cirrus at eight kilometres is free everywhere a cumulus lives."))
        CloudType CloudType4 ("Cloud Type 4", Category("Cloud Types"), Timing(Rebake), Tooltip("The fourth and last kind. Four is the ceiling because a type owns one channel of the vertical profile table, which is how Unreal arranges it too."))

        Float Coverage ("Coverage", Range(0.0, 1.0), Category("Weather"), Timing(Rebake), Tooltip("What fraction of the sky has cloud in it. It decides how many cells of the cloud lattice carry a cloud, so lowering it opens clear gaps rather than thinning everything.")) = 0.45
        Float CoverageContrast ("Coverage Contrast", Range(0.1, 4.0), Category("Weather"), Timing(Rebake), Tooltip("Sharpness of the transition from clear to cloudy, as the WIDTH of the ramp from an empty cell to a full one. Above 1 the ramp narrows and the sky is decisively cloud or decisively clear; below 1 it widens and the cloud sizes spread out, which is what a broken deck looks like.")) = 1.0
        Float WeatherTileSize ("Weather Tile Size (cm)", Range(200000.0, 8000000.0), Category("Weather"), Timing(Rebake), Tooltip("World size the cloud lattice is measured against, in centimetres (1200000 = 12 km). One cell is a QUARTER of it, and a cell carries Cloud Density clouds on average - which is also what decides whether there is any cloud overhead at all: a cell much larger than the layer altitude cannot fit one above the camera, and the zenith comes out empty however high the coverage is set.")) = 1200000.0
        Int Seed ("Seed", Range(0, 65535), Category("Weather"), Timing(Rebake), Tooltip("Which sky. Two layers with different seeds have unrelated clouds; the same seed always gives the same sky, in the same places, for the same settings.")) = 1

        Float PlacementDensity ("Cloud Density", Range(0.25, 8.0), Category("Placement"), Timing(Rebake), Tooltip("How many clouds a lattice cell carries on average - a whole number is drawn per cell with this mean, so 'exactly one per cell' stops being a property of the sky. It does NOT change how much cloud there is: each cloud is narrowed as this rises, so the matter is redistributed rather than added and Coverage keeps meaning what it says. Raise it for many small clouds at the same cover, lower it for few large ones. On its own it makes the lattice MORE visible, not less - Cloud Scatter is what removes that.")) = 1.75
        Float PlacementScatter ("Cloud Scatter", Range(0.0, 4.0), Category("Placement"), Timing(Rebake), Tooltip("How far a cloud may wander from its lattice site, measured in CELLS - at 1 it may sit anywhere in a cell-wide box around its site and so crosses into its neighbours' ground. THIS IS THE KNOB THAT REMOVES THE GRID: at 0 the sky is a lattice with a wobble, and the measured lattice bump falls by six times between 0 and 1. It is not free - independently placed clouds overlap where a lattice keeps them apart, so the sky covers a few points less than Coverage says at 0 and a few points more when this is returned to zero.")) = 1.0
        Float PlacementSizeVariety ("Cloud Size Variety", Range(0.0, 1.0), Category("Placement"), Timing(Rebake), Tooltip("How much cloud sizes differ from each other. At 0 every cloud in the sky is the size its cell's coverage says, which is the second half of what reads as a pattern; at 1 the widest is about four times the narrowest. The draw is uniform in AREA rather than in width, so the mean cloud covers the same ground at every setting and Coverage does not move with it. A smaller cloud is also a flatter one, which is what a field of cumulus looks like.")) = 0.75
        Float PatchTileSize ("Weather Patch Size (cm)", Range(500000.0, 20000000.0), Category("Placement"), Timing(Rebake), Tooltip("World size over which the sky's BUSY and CLEAR regions alternate, in centimetres - the scale of a weather system rather than of a cloud. Bounded from BELOW at three lattice cells and raised to it silently if set finer, because a modulation whose period is near a cell's decides cells one at a time and reads as a checkerboard rather than as weather. Above Region Size it stops buying anything: the sky already repeats at that distance, so a longer period cannot complete a cycle before the repetition does.")) = 2100000.0
        Float PatchStrength ("Weather Patch Strength", Range(0.0, 1.0), Category("Placement"), Timing(Rebake), Tooltip("How hard the sky is divided into busy and clear regions. At 0 the coverage is the same everywhere and the sky is uniformly occupied - which is what 'the whole sky is cloud' describes; at 1 a patch can reach nearly solid and its neighbour nearly empty. It is symmetric about Coverage, so it moves cloud around the sky rather than adding or removing it.")) = 0.60

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
        Float ExtinctionScale ("Extinction Scale (/km)", Range(0.5, 60.0), Category("Detail"), Timing(Immediate), Tooltip("How strongly the medium absorbs and scatters, per kilometre at full density. This is what makes a cloud opaque rather than merely visible. The cloud type multiplies it: ice at a quarter of a cumulus, a storm at a third above it. Eight is the approximation's own calibration, not physics - a real cumulus extinguishes at roughly 45/km, and Р18 measured that raising it moves five of six protocol points by less than 0.01 while re-authoring every scene (D-32).")) = 8.0

        Color3 ScatteringAlbedo ("Scattering Albedo", Category("Lighting"), Timing(Immediate), Tooltip("Fraction of extinguished light that is scattered rather than absorbed, PER COLOUR. Water droplets barely absorb at all and absorb every wavelength alike, which is why a cloud is white and why the default is a neutral 0.98 - keep it grey for water. Tinting it is how the medium itself becomes something other than water: a warm cast for dust or smoke, a cold one for ice haze. It is the MEDIUM's colour and not a filter over the frame, so the tint compounds through multiple scattering exactly as it would in the real thing, and the deep interior of a body ends up further from white than its edge. Values much below 1 in every channel read as smoke.")) = (0.98, 0.98, 0.98)
        Float PhaseG ("Phase G", Range(-0.9, 0.9), Category("Lighting"), Timing(Immediate), Tooltip("Asymmetry of the Henyey-Greenstein phase function. Positive scatters forward, which is what puts the bright rim on a cloud you are looking at through the sun.")) = 0.8
        Float PhaseGBackward ("Phase G Backward", Range(-0.9, 0.9), Category("Lighting"), Timing(Immediate), Tooltip("Asymmetry of the SECOND phase lobe. Near zero it is almost isotropic, which is what carries the body of the cloud while the first lobe carries the bright rim against the sun. One lobe cannot do both: strong enough for the rim leaves the body black, weak enough for the body loses the rim.")) = 0.1667
        Float PhaseBlend ("Phase Blend", Range(0.0, 1.0), Category("Lighting"), Timing(Immediate), Tooltip("How much of the second lobe is mixed in. UE's shipped instance weights it toward the BODY at 0.575, so more than half the answer is the near-isotropic lobe and the sharp one is a highlight on top.")) = 0.575
        Float AmbientOcclusionStrength ("Ambient Occlusion Strength", Range(0.0, 1.0), Category("Lighting"), Timing(Immediate), Tooltip("How strongly the sky light reaching a sample is occluded. Which occluder is measured is Sky Occlusion Volume's choice (on the component); this is how much of it is applied, either way. At 0 the core of a three-kilometre cumulus is lit as brightly as a wisp on its edge, which reads as a flat white cut-out.")) = 1.0
        Int MultiScatterOctaves ("Multiple Scattering Octaves", Range(1, 3), Category("Lighting"), Timing(Immediate), Tooltip("How many scattering orders are approximated. ONE IS SINGLE SCATTERING, and a cloud lit by single scattering alone is physically grey: the light that makes a real cloud white has bounced many times inside it. Two or three is where it starts to look like weather. The ceiling of 3 is ECS::kCloudMultiScatterMaxOctaves; the CloudMaterialSchema census pins the two numbers together.")) = 3
        Float MultiScatterContribution ("Multiple Scattering Contribution", Range(0.0, 1.0), Category("Lighting"), Timing(Immediate), Tooltip("How much each successive scattering order contributes. The factor is SQUARED at every octave, so the series falls away quickly and the third order is already a small correction.")) = 0.667
        Float MultiScatterOcclusion ("Multiple Scattering Occlusion", Range(0.0, 1.0), Category("Lighting"), Timing(Immediate), Tooltip("How much less each successive order is absorbed. This is what lets light that has already scattered reach the core of a cloud that the direct beam never gets into - the reason a thick cumulus glows rather than going black. 0.4847 (the cube root of the similarity factor) puts the third octave exactly on the medium's diffusion length; 0.25 is the shipped calibration at 8/km (D-32).")) = 0.25
        Float MultiScatterEccentricity ("Multiple Scattering Eccentricity", Range(0.0, 1.0), Category("Lighting"), Timing(Immediate), Tooltip("How much directionality each successive order keeps. Light that has bounced many times has forgotten where it came from, so the higher orders blend toward an isotropic phase.")) = 0.18
        ShaderProgram Medium ("Cloud Medium", Category("Medium"), Timing(Immediate), Tooltip("WHAT A CLOUD IS at a point in space - the density, the extinction, the albedo, the emission and the sky occlusion of the medium itself, authored as a node graph. Create one with New Shader Graph > Cloud Medium in the Content Browser, then drop it here. EMPTY IS THE NORMAL STATE and means the engine's own medium, which is what every scene drew before this slot existed and costs exactly the same. A graph with nothing wired into its Volume Output is that same medium again, so authoring starts from the sky you already have rather than from nothing. Note what the graph CANNOT reach: about half of the values in this window are inputs to a CPU bake that has already run by the time the march samples anything, and they are marked as such - a graph reads only the ones that are still in scope, and the palette offers no way to name the others - Cloud Material Param lists exactly those and nothing else."))

        Color3 AmbientScale ("Ambient Scale", Category("Lighting"), Timing(Immediate), Tooltip("Scales the sky's ambient contribution to the clouds. White is the full contribution; black lights them by the sun alone and leaves their shadowed sides black. Lowering it is NOT how the clouds get their form back: deleting it is the largest single knockout in the subsystem (D-25, D-32), and the contrast that appears when it goes is the sun's, uncovered - the frame loses half its light and goes warm.")) = (1.0, 1.0, 1.0)
    }

    Compute
    {
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
        // ONE of the four — u_CloudTrace.xy says which. The offset is applied to the RAY, not to the
        // store: the texel is written where it always was, but the ray it holds passes through the
        // half-res pixel the frame owns. That is Unreal's HackAddTemporalAAProjectionJitter, minus the
        // half that removes an existing TAA jitter, because this engine has none to remove.
        //
        // Four frames therefore cover every half-res pixel exactly once, and the reconstruction is what
        // turns four quarter-res traces into one half-res image instead of one blurred one.
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
        layout(binding = 0, rgba16f) restrict writeonly uniform image2D u_CloudScatter;

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
        layout(binding = 6, rgba16f) restrict writeonly uniform image2D u_CloudGuide;

        // The scene depth attachment, presented to this dispatch by ComputeImageBeginRead and handed back
        // afterwards. Point-sampled with texelFetch, never filtered: a filtered depth across a silhouette
        // averages foreground and background into a distance where nothing is.
        Uniform(2) sampler2D u_SceneDepth;

        // The noise volume, now an ASSET rather than a bake: four named channels from the Nubis deck
        // (p.96) — R,G Curly-Alligator LF/HF, B,A Alligator LF/HF — generated on the CPU by
        // Engine::Assets::CloudNoiseVolumeGenerator from the same Common/CloudNoise.glslh this shader
        // includes, and shipped as a .dcnv file. REPEAT and LINEAR,
        // which every volume sampler in this engine is; the volume was baked to tile exactly under that
        // assumption.
        Uniform(3) sampler3D u_CloudNoise;

        // AND THREE MORE OF THEM, because a layer carries four cloud types and a type names its own
        // volume. Separate bindings and not an array: this engine's reflection refuses an array of
        // descriptors in so many words (VulkanShaderReflection.cpp, "arrays of descriptors are not
        // supported — declare separate bindings"). ALWAYS BOUND, all four, on the terms every other
        // sampler here is bound on — Graphic::ResolveCloudNoiseVolumes fills the slots a layer does not
        // need with slot 0's image, so an unused one is a second descriptor onto bytes that are already
        // resident rather than a hole in the set.
        Uniform(10) sampler3D u_CloudNoise1;
        Uniform(11) sampler3D u_CloudNoise2;
        Uniform(12) sampler3D u_CloudNoise3;

        // THE FOUR-WAY SELECT, and it is a chain of compares because a sampler is not indexable in this
        // dialect. It costs the wave every branch the wave takes — which is why the CPU DEDUPLICATES the
        // slots before sending them: eight of the nine shipped types name no volume of their own, so the
        // ordinary sky sends 0 for every species, `slot` is uniform, and this is one fetch exactly as it
        // was before a type could name a volume at all.
        vec4 CloudFetchNoise( int slot, vec3 p )
        {
            if ( slot == 1 )
                return texture( u_CloudNoise1, p );
            if ( slot == 2 )
                return texture( u_CloudNoise2, p );
            if ( slot == 3 )
                return texture( u_CloudNoise3, p );
            return texture( u_CloudNoise, p );
        }

        // The sky's DISTANT SKY LIGHT: one texel holding the average radiance of the whole sky, marched
        // this frame by Programs/Sky/SkyDistantLight.shader. It is the physical model's ambient, and a
        // cloud is lit from every direction at once, so the full-sphere mean is the right quantity — the
        // same one the height fog reads. ALWAYS BOUND, even when it will not be read: a declared sampler
        // with no image is an INVALID descriptor set rather than an unused one, and this engine's compute
        // path answers that by silently skipping the whole dispatch — which would lose the clouds with
        // nothing in the log. u_CloudAmbient.w decides whether it is sampled.
        Uniform(4) sampler2D u_DistantSkyLight;

        // The sky's CAMERA AERIAL-PERSPECTIVE volume, 32x32x16 froxels of pre-integrated atmosphere
        // (Programs/Sky/SkyAerialPerspectiveLut.shader). Bound on the same terms as the texel above and
        // read only when u_CloudAerial.z says the volume exists.
        Uniform(5) sampler3D u_CloudAerialPerspective;

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
        Uniform(7) sampler3D u_CloudModelling;

        // THE SCULPTED HERO-CLOUD BODY — slot A of the seam. 128 x 64 x 128 RGBA8 of dimensional
        // profile, detail type, density scale and cutout envelope, loaded from a `.dcmv` and uploaded by
        // Runtime::CloudModellingService. ALWAYS BOUND, exactly like the two above and for exactly the
        // same reason: a declared sampler with no image is an INVALID descriptor set rather than an
        // unused one, and this backend answers an invalid set by silently skipping the dispatch — the
        // clouds would vanish with nothing in the log. When the scene has no hero cloud the renderer
        // binds the engine's fallback volume and the instance count is zero, so nothing reads it.
        Uniform(9) sampler3D u_CloudAuthoredAtlas;

        // THE SKY-LIGHT OCCLUSION VOLUME — 128 x 16 x 128 RGBA16F over the procedural modelling volume's
        // own region, .r holding the diffuse transmittance of everything ABOVE that column at that
        // altitude, written this frame by Programs/Clouds/CloudSkyOcclusionVolume.shader.
        //
        // IT IS THE QUANTITY u_CloudPhase.z's OTHER OCCLUDER CANNOT EXPRESS. CloudAmbientOcclusion is a
        // function of the sample's own Profile — its depth inside its OWN body — so a sample under three
        // kilometres of congestus receives exactly what a sample under clear sky receives. See
        // Common/CloudLighting.glslh, which owns both the maths and the addressing, and
        // Docs/Clouds/DIAGNOSIS_CARTOON.md §1, which is the measurement that asked for this.
        //
        // ALWAYS BOUND, fallback included, on the terms every sampler here is bound on: a declared sampler
        // with no image is an INVALID descriptor set, and this backend answers one by silently skipping
        // the dispatch. u_CloudFrame.x decides whether it is read.
        Uniform(13) sampler3D u_CloudSkyOcclusionVolume;

        // THE ATMOSPHERE'S TRANSMITTANCE LUT — 256x64 RGBA16F of what survives the trip from a point in
        // the atmosphere to space along a given zenith angle, marched by
        // Programs/Sky/SkyTransmittanceLut.shader in Bruneton's (r, mu) mapping and read here through
        // Common/SkyScattering.glslh's SkySunAtAltitude — the exact inverse of the mapping the LUT was
        // written under, clamped to the texel centres it was written at.
        //
        // WHAT IT IS FOR. u_CloudSunColour is ONE colour for the whole shell. Unreal's default computes it
        // as `SunOuterSpaceIlluminance * T(groundLevel)` and so does this engine, which means a deck four
        // kilometres up is lit through four kilometres of air it is standing above. With u_CloudFrame.y
        // raised, the packer sends the OUTER-SPACE illuminance instead and this pass applies T at the
        // sample's own altitude and its own local zenith — the absolute form, with no ratio and therefore
        // no division by a transmittance that reaches zero at a low sun.
        //
        // ALWAYS BOUND, fallback included, on the terms every sampler above is bound on.
        Uniform(14) sampler2D u_CloudSunTransmittanceLut;

        // The seam's three callbacks. Declared here, next to the samplers, because Common/CloudField.glslh
        // must stay free of samplers to remain compilable as C++ by its tests.
        #define CLOUD_SAMPLE_NOISE(s, p) CloudFetchNoise((s), (p))
        // textureLod for the reason the authored atlas gives below, and for the same reason.
        #define CLOUD_SAMPLE_MODELLING(p) textureLod(u_CloudModelling, (p), 0.0f)
        // textureLod AND NOT texture: a compute shader has no derivatives, so the implicit level of
        // detail is undefined. The volume has one level, so every implementation happens to pick it — but
        // "happens to" is the state three other sites in this engine were found in.
        #define CLOUD_SAMPLE_AUTHORED(p) textureLod(u_CloudAuthoredAtlas, (p), 0.0f)

        // Slot A's instance list. Included BEFORE the seam, because the seam's authored producer reads
        // the block this declares and GLSL has no forward declarations.
        #define CLOUD_AUTHORED_BUFFER_BINDING 8
        #include <Common/CloudAuthored.glslh>

        #include <Common/CloudField.glslh>
        #include <Common/CloudParams.glslh>

        PushConstant CloudPush
        {
            mat4 u_InverseViewProjection;
            vec4 u_CameraPosition; // xyz = camera position in world units, w = frame index
            // xy = this frame's sub-pixel inside the 2x2 block of HALF-resolution pixels each quarter-res
            //      texel covers, each 0 or 1. zw = the HALF-resolution grid's size in pixels, which this
            //      pass cannot derive: imageSize() reports the quarter-res target it writes, and the two
            //      round-ups that produced it are not invertible on an odd viewport.
            vec4 u_CloudTrace;
            // WHAT THIS FRAME'S RESOURCES ARE, as opposed to what the artist asked for. Graphic::CloudPush
            // ::Frame, member for member.
            //
            // x = 1 when the sky-light occlusion volume was written for THIS frame and must be read. Its
            //     region and side are NOT here: the volume shares the modelling volume's frame exactly, so
            //     u_CloudRegion already carries both and a second copy would be one fact on the wire twice.
            // y = 1 when u_CloudSunTransmittanceLut holds this frame's atmosphere AND the layer asked for
            //     per-sample sun transmittance. It also says WHAT u_CloudSunColour.rgb IS: the sun's
            //     outer-space illuminance when 1, the ground-level product when 0.
            // z, w = the atmosphere shell's bottom and top radii, kilometres from the planet centre.
            //     Written whatever y says, read only when y is 1.
            vec4 u_CloudFrame;
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
            if (u_CloudFrame.y < 0.5f)
                return u_CloudSunColour.rgb;

            float sunZenithCos = clamp(dot(CloudLocalUp(positionKm), toSun), -1.0f, 1.0f);

            // textureSize() and not a literal 256x64, so the clamp cannot drift from the image the
            // renderer actually bound.
            SkySunAtPoint sun = SkySunAtAltitude(u_CloudFrame.z, u_CloudFrame.w,
                                                 CloudAltitudeKm(layer, positionKm), sunZenithCos,
                                                 vec2(textureSize(u_CloudSunTransmittanceLut, 0)));

            if (sun.PlanetShadow <= 0.0f)
                return vec3(0.0f, 0.0f, 0.0f);

            return u_CloudSunColour.rgb *
                   (sun.PlanetShadow * textureLod(u_CloudSunTransmittanceLut, sun.Uv, 0.0f).rgb);
        }

        // The sun-transmittance quadrature is Common/CloudField.glslh's — see the note there. It moved out
        // of this file when the sky's environment bake began lighting the same field, because a second copy
        // of it beside this one is the mirror that drifts.

        void main()
        {
            ivec2 size  = imageSize(u_CloudScatter);
            ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
            if (coord.x >= size.x || coord.y >= size.y)
                return;

            // THE PROJECTION JITTER. The ray is reconstructed through the HALF-resolution pixel this
            // frame owns — (coord * 2 + offset) — and not through the centre of the quarter-res texel the
            // result is stored in. Clamped to the last half-res pixel because the quarter grid is the half
            // grid rounded UP: on an odd half-res extent the final column's block hangs half off the
            // image, and an unclamped uv there would reach past 1 and, with this engine's REPEAT samplers,
            // fetch the opposite edge of the aerial-perspective volume.
            ivec2 halfSize  = ivec2(max(u_CloudTrace.zw, vec2(1.0f, 1.0f)));
            ivec2 halfCoord = min(coord * 2 + ivec2(u_CloudTrace.xy), halfSize - ivec2(1, 1));

            vec2 uv  = (vec2(halfCoord) + vec2(0.5f, 0.5f)) / vec2(halfSize);
            vec2 ndc = vec2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);

            // The ray from the camera's own inverse view-projection, so it inherits whatever projection
            // the frame was drawn with. The engine is REVERSED-Z (Core/Projection.hpp): 1 is the near
            // plane and 0 the far one, which is why the two probe depths below are that way round and not
            // the other.
            vec4 nearH = u_InverseViewProjection * vec4(ndc.x, ndc.y, 1.0f, 1.0f);
            vec4 farH  = u_InverseViewProjection * vec4(ndc.x, ndc.y, 0.0f, 1.0f);
            vec3 nearP = nearH.xyz / max(nearH.w, 1e-9f);
            vec3 farP  = farH.xyz / max(farH.w, 1e-9f);
            vec3 rayDir = normalize(farP - nearP);

            CloudLayer layer = CloudUnpackLayer();

            // Planet-centre-relative kilometres: the frame in which both shells are centred on the origin.
            vec3 cameraKm = u_CameraPosition.xyz * (1.0f / CLOUD_WORLD_UNITS_PER_KM);
            vec3 originKm = vec3(cameraKm.x, cameraKm.y + layer.PlanetRadiusKm, cameraKm.z);

            vec2 segment = CloudLayerIntersect(layer, originKm, rayDir);

            // Geometry occludes the layer. The depth target is full resolution and the ray belongs to a
            // HALF-resolution pixel, so the depth is fetched through halfCoord and not through the
            // quarter-res coord — sampling the depth at the texel the result is STORED in rather than at
            // the one the ray was CAST through would cut this frame's ray at a neighbour's geometry, and
            // the error would swing with the jitter, which is exactly the shimmer this work removes.
            //
            // One full-res texel out of the 2x2 block stands for all four, and it is the TOP-LEFT one, not
            // the centre: `(halfCoord * depthSize) / halfSize` truncates. The two readings differ exactly
            // on a silhouette, where the choice decides whether this pixel's cloud is cut at the mountain
            // or behind it; the guide's bilateral weights hide most of the residual.
            ivec2 depthSize  = textureSize(u_SceneDepth, 0);
            ivec2 depthCoord = clamp((halfCoord * depthSize) / halfSize, ivec2(0, 0), depthSize - ivec2(1, 1));
            float deviceDepth = texelFetch(u_SceneDepth, depthCoord, 0).r;

            // The guide's scene channel, resolved for EVERY pixel and not only for the ones that have
            // geometry. A sky pixel's ray is still cut somewhere — at the far plane — and giving it that
            // real distance rather than a sentinel is what lets the composite's coherence test see an
            // unbroken sky as unbroken instead of as a discontinuity against every silhouette in it.
            float sceneKm = length(farP - u_CameraPosition.xyz) * (1.0f / CLOUD_WORLD_UNITS_PER_KM);

            // `> 0` IS THE GEOMETRY TEST under reversed-Z: a sky pixel stores 0, the far plane. Written as
            // `< 1` it would be true for every pixel including the sky, and every ray would be cut at the
            // far plane — which renders as no clouds at all, with nothing in the log.
            if (deviceDepth > 0.0f)
            {
                vec4 geomH = u_InverseViewProjection * vec4(ndc.x, ndc.y, deviceDepth, 1.0f);
                vec3 geomP = geomH.xyz / max(geomH.w, 1e-9f);
                float geomKm = length(geomP - u_CameraPosition.xyz) * (1.0f / CLOUD_WORLD_UNITS_PER_KM);
                sceneKm   = geomKm;
                segment.y = min(segment.y, geomKm);
            }

            // The authored limits: where tracing starts, and how far along the segment it may run. The
            // distance is measured FROM THE LAYER ENTRY, not from the camera — Unreal's
            // DistanceFromCloudLayerEntryPoint mode — because a limit measured from the camera would cut
            // the layer short at a fixed radius and put a visible circular edge in the sky.
            segment.x = max(segment.x, u_CloudMarch.z);
            segment.y = min(segment.y, segment.x + max(u_CloudLayer.w, 0.0f));

            // A ray whose entry is beyond the cutoff is not traced at all. It bounds the cost of the
            // grazing rays that cost the most and show the least, and it is the second line of defence
            // behind the planet test: the geometry can legitimately report an entry thousands of
            // kilometres away, and nothing else in this loop would decline to march it.
            if (segment.x > max(u_CloudPhase.w, 0.0f))
            {
                imageStore(u_CloudScatter, coord, vec4(0.0f, 0.0f, 0.0f, 1.0f));
                imageStore(u_CloudGuide, coord, vec4(sceneKm, sceneKm, 0.0f, 0.0f));
                return;
            }

            if (segment.y <= segment.x)
            {
                imageStore(u_CloudScatter, coord, vec4(0.0f, 0.0f, 0.0f, 1.0f));

                // This ray never entered the layer, so the end of its search IS the end of the ray. Both
                // guide channels therefore carry the same distance, which is what makes a whole region of
                // cloudless sky perfectly coherent to the composite and sends it down the cheap bilinear
                // path — a filter that fires everywhere quantizes the frame onto this pass's own grid.
                imageStore(u_CloudGuide, coord, vec4(sceneKm, sceneKm, 0.0f, 0.0f));
                return;
            }

            float length_km = segment.y - segment.x;
            float stepCount = CloudStepCount(length_km, CLOUD_MIN_STEPS, max(u_CloudMarch.x, CLOUD_MIN_STEPS),
                                             CLOUD_DISTANCE_TO_MAX_STEPS_KM);
            int   stepTotal = int(stepCount);

            // TWO STEP SIZES, and the whole point is that they are spent in different places. The fine
            // step is the finest the march ever goes; the coarse one is a multiple of it and is used only
            // while the ray is outside cloud, where the answer is exactly zero and any step size gives it
            // correctly.
            //
            // The budget therefore stops being a resolution and becomes a resolution WHERE IT MATTERS: a
            // ray that crosses mostly empty sky finishes in a fraction of the iterations, and a ray inside
            // cloud gets to spend all of them on the part that has something in it.
            //
            // The multiplier and its partner constant live in Common/CloudGeometry.glslh, together and
            // next to the relation they have to satisfy — they are two numbers that must agree, and here
            // they were two literals a hundred lines apart in a file no test can compile.
            float stepKm       = CloudFineStepKm(length_km, CLOUD_MIN_STEPS, max(u_CloudMarch.x, CLOUD_MIN_STEPS),
                                                 CLOUD_DISTANCE_TO_MAX_STEPS_KM);
            float coarseStepKm = CloudCoarseStepKm(stepKm);

            // The start offset inside the first step, so the sample planes of neighbouring pixels do not
            // line up. Without it the uniform schedule draws the layer as a set of concentric shells —
            // the banding is the step planes themselves, seen edge-on.
            //
            // The hash includes the frame index, so the pattern moves and the eye integrates it. That is
            // the whole reason a still frame of this pass is NOT a valid check of it: a fixed camera and a
            // fixed frame index show one realization of the dither, not what the viewer sees.
            // Hashed on the HALF-res pixel, not on the quarter-res texel: two frames of the same block
            // trace different half-res pixels, so hashing halfCoord gives them different start offsets as
            // well as different rays. Hashing coord would give the whole 2x2 block one offset per frame,
            // and the temporal reconstruction would then average four samples that share their bias.
            uint  jitterHash = CloudHashCell(uint(halfCoord.x), uint(halfCoord.y),
                                             uint(u_CameraPosition.w), 0x51ED270Bu);
            float jitter     = float(jitterHash & 0xFFFFu) * (1.0f / 65536.0f);

            CloudFieldParams params = CloudUnpackFieldParams();

            vec3  toSun        = normalize(u_CloudSun.xyz);
            float phase        = CloudPhaseDualLobe(dot(rayDir, toSun), u_CloudWind.w,
                                                    u_CloudPhase.x, u_CloudPhase.y);
            float extinction   = max(u_CloudMarch.w, 0.0f);
            // PER COLOUR. Clamped here as well as in the packer, on the terms every other clamp in this
            // file is: this is the side that decides what the loop does, and a channel above one makes the
            // scattering series diverge rather than converge.
            vec3  albedo       = clamp(u_CloudAlbedo.rgb, vec3(0.0f), vec3(1.0f));
            float lightMarchKm = max(u_CloudSun.w, 0.0f);
            // SIXTY-FOUR, and it must equal ECS::kCloudLightMarchMaxSamples. This clamp cannot include the
            // C++ constant, so Desert/Tests/Engine/SettingConsumers reads this line's text and fails if the
            // two ever part company — while both were the literal 16 an artist could raise the slider past
            // a ceiling the march quietly reimposed.
            int   lightSamples = int(clamp(u_CloudSunColour.w, 1.0f, 64.0f));
            float stopT        = clamp(u_CloudMarch.y, 0.0f, 1.0f);

            // The multiple-scattering series, unpacked once per pixel rather than per sample: the four
            // fields are uniform over the whole dispatch, and rebuilding the struct inside the inner loop
            // would put four loads under the hottest branch in the subsystem.
            CloudScatterSeries series;
            series.Octaves     = u_CloudMultiScatter.x;
            series.ScatterStep = u_CloudMultiScatter.y;
            series.ExtinctStep = u_CloudMultiScatter.z;
            series.PhaseStep   = u_CloudMultiScatter.w;

            // The ambient a cloud sits in, resolved once per pixel rather than per step: in the physical
            // model it is the marched full-sphere mean scaled by the artist's factor, in the artistic
            // gradient it is the dome value the packer already folded down. Neither model is a stand-in
            // for the other, which is why the choice is made from a gate rather than from whichever
            // happens to be non-zero.
            vec3 ambientRadiance = u_CloudAmbient.rgb;
            if (u_CloudAmbient.w > 0.5f)
            {
                ambientRadiance = u_CloudAmbient.rgb *
                                  texelFetch(u_DistantSkyLight, ivec2(SKY_DISTANT_LIGHT_SPHERE_TEXEL, 0), 0).rgb;
            }

            vec3  luminance     = vec3(0.0f, 0.0f, 0.0f);
            float transmittance = 1.0f;
            float t             = segment.x + jitter * stepKm;

            // Where along the ray this pixel's cloud effectively IS, weighted by how much of the ray's
            // light each sample still gets to contribute. A cloud is not at one distance, but the
            // atmosphere in front of it has to be evaluated at one — and the transmittance-weighted mean
            // is the distance at which a single evaluation is closest to the integral. Weighting by depth
            // instead would put the answer inside the far side of a thick cloud, where nothing visible
            // happens.
            float aerialWeightedT = 0.0f;
            float aerialWeightSum = 0.0f;

            // The guide's cloud channel: the distance of the FIRST sample that had any material in it.
            // Negative means "not found yet" — a distance can never be negative here, so the sentinel
            // cannot collide with an answer. It is resolved to a real distance after the loop, on every
            // path, because the composite compares this number across four texels and a sentinel that
            // leaked out of one of them would read as a silhouette where there is none.
            float cloudFrontKm = -1.0f;

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

            for (int i = 0; i < stepTotal; ++i)
            {
                if (t >= segment.y)
                    break;

                vec3  samplePos      = originKm + rayDir * t;
                float heightFraction = CloudHeightFraction(layer, samplePos);

                // The field is handed the ALTITUDE above the layer's base, never the planet-relative
                // height. Two reasons, and the second is the one that bites: the noise is periodic, so a
                // y of 6363 kilometres wraps to something arbitrary rather than to something meaningful;
                // and at that magnitude float32 resolves 0.4 metres, which quantizes the vertical
                // structure of a two-kilometre layer onto a visible ladder.
                vec3 fieldPos = vec3(samplePos.x, length(samplePos) - layer.BottomRadiusKm, samplePos.z);

                CloudFieldSample field = SampleCloudField(params, heightFraction, fieldPos);

                if (!fine)
                {
                    // Outside cloud: keep striding. On the first hit, step BACK one coarse step and drop
                    // to fine, so the material between the last coarse sample and this one is not skipped
                    // — that gap is a whole coarse step of cloud and its absence reads as a flat face.
                    if (field.Profile > 0.0f)
                    {
                        fine     = true;
                        emptyRun = 0;
                        t        = max(segment.x, t - coarseStepKm);
                        continue;
                    }

                    t += coarseStepKm;
                    continue;
                }

                if (field.Profile <= 0.0f)
                {
                    ++emptyRun;
                    if (emptyRun >= kEmptyFineSamplesBeforeCoarse)
                    {
                        fine     = false;
                        emptyRun = 0;
                    }
                    t += stepKm;
                    continue;
                }

                emptyRun = 0;

                {
                    float density = CloudSampleDensity(params, field, fieldPos);

                    // The near-camera fade. A camera inside the layer otherwise meets full density at
                    // arm's length, which fills the screen with a flat wall; UE fades the nearest metres
                    // for exactly this. Both distances at zero leave the density untouched.
                    if (u_CloudFade.z > 0.0f)
                        density *= smoothstep(u_CloudFade.w, u_CloudFade.z, t);

                    if (density > 0.0f)
                    {
                        // Recorded at the first hit and never again: the front of the cloud, not its
                        // middle. Placed before the lighting so an early break on transmittance still
                        // leaves the guide with the distance the ray actually met material at.
                        if (cloudFrontKm < 0.0f)
                            cloudFrontKm = t;

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
                        float opticalDepth = CloudLightOpticalDepth(layer, params, samplePos, toSun,
                                                                    lightMarchKm, lightSamples, extinction);

                        // HOW MUCH OF THE SKY THIS SAMPLE CAN SEE, by one of two geometries.
                        //
                        // The strength is the SAME field either way — u_CloudPhase.z, the artist's
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
                        float ambientOcclusion = CloudAmbientOcclusion(field.Profile, u_CloudPhase.z);
                        if (u_CloudFrame.x > 0.5f)
                        {
                            // The WIND-SHIFTED position, because that is the frame the volume was traced
                            // in — the same subtraction Common/CloudField.glslh makes on its own way into
                            // the modelling volume, and the reason the two land on the same texel however
                            // far the wind has run.
                            vec3 skyWindPos = vec3(fieldPos.x - params.WindOffsetKm.x,
                                                   fieldPos.y - params.WindOffsetKm.y,
                                                   fieldPos.z - params.WindOffsetKm.z);

                            vec3 skyUvw = CloudSkyOcclusionUvw(params.RegionOriginKm, params.InvRegionSizeKm,
                                                               heightFraction, skyWindPos);

                            // textureLod AND NOT texture, for the reason every other volume fetch in this
                            // file gives: a compute shader has no derivatives, so the implicit level of
                            // detail is undefined.
                            ambientOcclusion =
                                CloudSkyOcclusion(textureLod(u_CloudSkyOcclusionVolume, skyUvw, 0.0f).r,
                                                  u_CloudPhase.z);
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
                        vec3 sampleAlbedo = CloudSampleAlbedo(params, field, fieldPos, albedo);

                        luminance += transmittance *
                                     CloudMultiScatterStep(series, CloudSunColourAt(layer, samplePos, toSun),
                                                           ambientRadiance * ambientOcclusion, opticalDepth,
                                                           phase, sigmaT, sampleAlbedo, stepKm);

                        // EMISSION: radiance the medium ADDS, per kilometre of march, attenuated by the
                        // ray's own history and by nothing else. It is deliberately NOT multiplied by the
                        // absorption coefficient — Unreal makes the same choice for the same reason, that
                        // an artist authoring a glow wants the glow they authored and not the glow times
                        // however opaque the cloud happens to be there (VolumetricCloud.usf:1381-1384).
                        // The default medium returns a literal zero, so this line costs nothing until a
                        // graph writes the pin.
                        luminance += transmittance * CloudSampleEmissive(params, field, fieldPos) * stepKm;

                        // Recorded BEFORE the ray is attenuated: the weight is how much this sample was
                        // able to contribute, not how much is left after it.
                        aerialWeightedT += t * transmittance;
                        aerialWeightSum += transmittance;

                        // The view ray is attenuated by the medium itself, once — the octaves are orders
                        // of scattering INTO this ray, not extra material along it.
                        transmittance *= CloudBeerTransmittance(sigmaT, stepKm);

                        if (transmittance < stopT)
                            break;
                    }
                }

                t += stepKm;
            }

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
            if (u_CloudAerial.z > 0.5f && aerialWeightSum > 0.0f)
            {
                float meanDistanceKm =
                     CloudAerialMeanDistanceKm(aerialWeightedT, aerialWeightSum, u_CloudAerial.y);
                float sliceUnit = SkyApSliceUnitFromDistance(meanDistanceKm, u_CloudAerial.x);

                // Read through the exact inverse of the fill's texel-centre remap on all three axes, so
                // the frame's edges and the volume's near plane land on written texels rather than
                // halfway between them.
                vec3 uvw = vec3(SkyUnitToTexelUv(uv.x, SKY_AP_VOLUME_WIDTH),
                                SkyUnitToTexelUv(uv.y, SKY_AP_VOLUME_HEIGHT),
                                SkyUnitToTexelUv(sliceUnit, SKY_AP_VOLUME_DEPTH));

                vec4 aerial = texture(u_CloudAerialPerspective, uvw);

                luminance = CloudApplyAerialPerspective(
                     luminance, transmittance, aerial.rgb, aerial.a,
                     CloudAerialAmount(meanDistanceKm, u_CloudFade.x, u_CloudFade.y));
            }

            imageStore(u_CloudScatter, coord, vec4(luminance, clamp(transmittance, 0.0f, 1.0f)));

            // A ray that marched the whole segment and found nothing reports THE END OF ITS OWN SEARCH.
            // That is a real distance rather than a magic number, and it is the one that makes the
            // bilateral weight behave: an empty neighbour of a cloud at 3 km reports the far side of the
            // shell, several kilometres away, so the weight `1 / (dKm * 1000 + 1)` rejects it outright.
            // A large sentinel would do the same job here but not at the horizon, where a whole
            // neighbourhood of empty rays must agree with each other exactly enough to stay coherent —
            // and half precision has no room for a sentinel that is both huge and finite.
            imageStore(u_CloudGuide, coord,
                       vec4(cloudFrontKm < 0.0f ? segment.y : cloudFrontKm, sceneKm, 0.0f, 0.0f));
        }
    }
}
