---
name: desert-engine-look
description: How a visual feature in DesertEngine is judged and closed against a reference — water, foliage, sky, lighting, materials, shadows, characters. Use when the owner says something looks wrong or is not liked, when designing or accepting anything visible, and before saying a picture is better.
---
# Look

1. **Fix the reference first.** A real frame: photo, UE scene, or a shipped AAA-class shooter's technique. It is named in the design's `## Эталон`. No reference, no work.
2. **Ask the real path.** The frame is the game view of a real scene (Bistro, the water scenes), day AND night, near and far, moving and still — never a probe scene alone.
3. **List every difference** between frame and reference: silhouette, density, albedo, normals, roughness, transmission, shadow contact, AO, GI, reflections, fog, LOD pop, temporal ghosting, aliasing, tone. Then group by cause.
4. **Prove each cause.** A view mode, a channel off, path tracer versus raster (agreement = content/material, disagreement = raster lighting), a number in linear HDR. Content causes (mesh, texture) are fixed in content, not with a constant in a shader.
5. **One design, one landing.** All causes of the complaint together.
6. **Accept side by side.** Reference | before | after, same camera, exposure and time. Every row of the list is closed or named open. Then the owner sees the same three frames.

## Traps
- A thumbnail lies: pixel-diff first. Zero VUID may mean nothing was drawn: check the mean and `did not execute`.
- `machine.json` caps quality and fakes regressions: compare with default settings.
- `--shot` needs a visible window; `--camera` needs `--shot`; a view mode persists across launches.
- One still proves nothing about temporal effects: turn the camera and compare the frames.
- Locked PC or display off, or VRAM over budget, invalidates timings.

Long polish against a bar: `desert-engine-gauntlet`. How to drive the editor and compare frames: `desert-engine-verify`.
