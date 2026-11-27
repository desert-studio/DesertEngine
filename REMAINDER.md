# REMAINDER — FIX-REFR2-NIGHT (context limit)

Brief: C:/Users/danil/.claude/studio/briefs/FIX-REFR2-NIGHT-BRIEF.md. Done here: item 2a (GPU profiling On/Off in
Scenarios/Render/NG-RT-REFRACTION.scn). Left: item 1 (day glass), item 2b (camera onto a shop window), the chain.

## Finding 1 — the night "opaque delta" is not the RT pass (measured on scn-1127-172729 frames)
Mean frame value: night_ss 36.8 -> turn_ss_a 41.1 (+16 frames, ScreenSpace) -> turn_ss_b 63.8 (ScreenSpace, no RT
pass) -> night_rt 56.4 -> turn_a 55.9 (ratio night_rt->turn_a p50 0.983). The frame brightens under ScreenSpace alone:
night_ss is captured unconverged (32 frames after PathTraced -> HardwareTraced GI; ratio spread p10 1.15 / p90 3.1 =
GI accumulation + exposure, not a uniform exposure step). Code path confirms: RayTracedTranslucency writes only its own
texture (no imageStore/atomic in GIHitLighting/GIHitLitReads/Pass_RayHit), the glass reads it only where
FrontLayerDepth == gl_FragCoord.z (Mesh/Surface/TranslucencyRefraction.glslh), AddFrameGlass draws glass only.
Scenario fix still to make: converge before every _ss/_rt capture (lock exposure as FOLIAGE-PT-TRANSMISSION.scn:9-10
does, `set <PPV id> PostProcessVolumeData.Settings.AutoExposure false` + ManualExposureEV100, restore in the tail;
and enough `frames` after the GI method switch, e.g. 256), so night_ss vs night_rt compare converged frames.

## Finding 2 — day: RT changes nothing (day_ss vs day_rt: 84 pixels differ > 25/255, none on the door glass)
So on glass the composite gets a = 0 (trace did not answer) or the depth test fails. Not yet checked:
- does the glass pass's front-layer test ever pass (DesertFrontLayerTracedReflection uses the same test — is the traced
  reflection visible on the glass?); FrontLayerDepthTexture/gl_FragCoord.z jitter or resolution mismatch;
- RayTracedTranslucency.shader:134-206: a = 0 only for empty/rough front layer or > MaxInterfaces (4) dielectric hits;
  check that translucent instances are in the raster-view TLAS (RayTracingScene.hpp kMaskTranslucent) and that the
  glass template's hit group is not the default-surface fallback (BuildReflectionHitGroups, SceneRendererFrameDeferred.cpp ~2340);
- whether the door/back windows are Translucent templates at all; the camera frames no shop window (item 2b).
