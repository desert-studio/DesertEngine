# Noticed, not this round

One gap per round. Everything else waits here with the round it was seen in.

## Seen in round 01 (r00 frames)
- [01] grass blades: matte olive, no sheen/translucency glow when back-lit; colour flat along the blade.
- [01] blade roots: ambient-only darkening mix(1, 0.45) draws dark vertical stubs on top of a sunlit floor.
- [01] specular occlusion: ComposeAmbient multiplies the whole IBL by AO, no roughness/NoV-aware specular occlusion (UE GetSpecularOcclusion).
- [01] far planes: tree line at ~100 m has the same contrast as the barn; aerial perspective barely reads (content AP scale 1).
- [01] sky light on the barn's shadowed wall flat; barn reads as a box with one value per face.
- [01] display transfer: pow(1/Gamma) instead of the sRGB OETF in SceneComposite (dark tones lifted differently than UE's sRGB encode).
- [01] FIELD_Barn runs GI method 2 (ScreenSpace) -> no traced GI; SSAO is the only ambient occlusion in this scene (GTAO change matters most here).
- [01] sun on the floor under grass: blades sub-texel in the cascades + content density; screen-space contact shadows would catch them but FIELD_Barn has ContactShadows false (content). Not chased.

## Closed / judged in rounds 04-05 (RENDER-LOOK-2)
- [01] specular occlusion -> round 04 REFUSED (invisible, <0.3/255 mean; patch kept in scratch r04_specular_occlusion.patch).
- [01] display transfer -> round 05 IMPROVED (sRGB OETF, scene v65).
- GTAO spatial/temporal filters (REMAINDER gap 3): not a round - FIELD_Barn runs TAA (GpuOrder log: "TemporalAA" after the
  composite), and no 4x4 AO noise is visible in any r04/r05 view. Closes a visible gap only on a non-TAA path (MSAA/FXAA/None).

## Seen in round 05 (r05 frames)
- [05] barn's shaded wall: one value per face still (sky light flat) - same as [01]; now darker, not more modelled.
