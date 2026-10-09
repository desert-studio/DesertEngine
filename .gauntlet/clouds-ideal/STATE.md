# clouds-ideal

Started 2026-10-09T15:01:09Z. Goal: Clouds_Demo/Showcase read as real cumulus: flat dark bases, cauliflower heads, soft wispy edges, no grain/sparkle/popcorn

| round | lens | gap | outcome | note |
|---|---|---|---|---|
| 01 | silhouette | sub-head grain and loose flecks on the outline | improved | Detail Strength default 0.85->0.65 (schema CloudRaymarch.shader:70 + mirror CloudMaterialValues.hpp:97) |
| 02 | light | micro-shadow stipple from the fine erosion octave | improved | CLOUD_DETAIL_HF_WEIGHT 0.5->0.3 (CloudField.glslh:227) |
| 03 | scale | undersides as bright as lit flanks — bodies read small | refused | MultiScatterOcclusion 0.25->0.4847 dims the whole body, gradient unchanged, Showcase greys; reverted |
| 04 | coherence | bases as bright as tops: sky light not occluded toward the cloud bottom | improved | UE SkyLightCloudBottomOcclusion 0.5 as material param in u_CloudAlbedo.w; Showcase base/top 0.49->0.42, tops -5%; Demo neutral (shape, not light) |
