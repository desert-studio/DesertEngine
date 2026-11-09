# GPU-FRAME-1 — разбор GPU-кадра FPS-FIELD-BARN (scn-1102-101610)

База: origin/dev b33f3dce5. Вход: `C:/DesertEngine-scratch/FPS-FIELD-BARN/frames/scn-1102-101610/editor.log`, пять таблиц
`[Profiler] ---- per-pass breakdown` (порядок сценария: 1_static, 1_static_vis, foliage Low, foliage Epic, 2_wind).
Устройство: RTX 3070 Ti (8 GB), драйвер 591.86. Вывод 1787x657 (1.17 Мп); вход DLSS-RR 1197x440 (Quality 66.7 %,
NGX optimal 1191x438), DLSS 310.9.1, RR preset Default (снипет: Preset_F). Значения — gpu total / gpu self, мс.

## 1. Таблица GPU (5 кадров)

| проход | 1_static | 1_static_vis | foliage Low | foliage Epic | 2_wind |
|---|---|---|---|---|---|
| GPU: Frame Total | 17.17/17.17 | 17.01/17.01 | 12.43/12.43 | 17.06/17.06 | 18.40/18.40 |
| Deferred: GBuffer | 3.69/ 3.69 | 3.41/ 3.41 | 1.05/ 1.05 | 3.71/ 3.71 | 3.72/ 3.72 |
| DLSS-RR | 3.12/ 3.12 | 3.09/ 3.09 | 3.09/ 3.09 | 3.11/ 3.11 | 3.11/ 3.11 |
| Deferred: Masked Depth Prepass | 2.86/ 2.86 | 2.57/ 2.57 | 0.76/ 0.76 | 2.87/ 2.87 | 2.87/ 2.87 |
| TerrainGBuffer | 1.31/ 1.31 | 1.44/ 1.44 | 1.75/ 1.75 | 1.31/ 1.31 | 1.31/ 1.31 |
| Clouds: March | 1.29/ 1.29 | 1.22/ 1.22 | 1.25/ 1.25 | 1.29/ 1.29 | 1.48/ 1.48 |
| SSR: Classify + Trace | 0.93/ 0.93 | 0.93/ 0.93 | 1.10/ 1.10 | 0.94/ 0.94 | 0.93/ 0.93 |
| RDG Wait: graphics queue on async | 0.56/ 0.56 | 0.93/ 0.93 | 0.70/ 0.70 | 0.53/ 0.53 | 0.32/ 0.32 |
| RadianceCache: Trace | 0.59/ 0.59 | 0.62/ 0.62 | 0.41/ 0.41 | 0.47/ 0.47 | 0.47/ 0.47 |
| Deferred: Composite | 0.44/ 0.44 | 0.44/ 0.44 | 0.44/ 0.44 | 0.44/ 0.44 | 0.45/ 0.45 |
| ScreenProbes: Integrate | 0.40/ 0.40 | 0.39/ 0.39 | 0.38/ 0.38 | 0.40/ 0.40 | 0.42/ 0.42 |
| MeshShadowCascade0.3.Movable | 0.01/ 0.01 | 0.01/ 0.01 | 0.01/ 0.01 | 0.01/ 0.01 | 0.39/ 0.39 |
| MeshShadowCascade0.2.Movable | 0.01/ 0.01 | 0.01/ 0.01 | 0.01/ 0.01 | 0.01/ 0.01 | 0.39/ 0.39 |
| MeshShadowCascade0.1.Movable | 0.01/ 0.01 | 0.01/ 0.01 | 0.01/ 0.01 | 0.01/ 0.01 | 0.37/ 0.37 |
| MeshShadowCascade0.0.Movable | 0.01/ 0.01 | 0.01/ 0.01 | 0.01/ 0.01 | 0.01/ 0.01 | 0.36/ 0.36 |
| ScreenProbes: ShortRangeAO | 0.32/ 0.32 | 0.24/ 0.24 | 0.28/ 0.28 | 0.33/ 0.33 | 0.30/ 0.30 |
| Grass: Visibility Resolve | - | 0.30/ 0.30 | - | - | - |
| Grass: Visibility Raster | - | 0.30/ 0.30 | - | - | - |
| ScreenProbes: Trace Far Field | 0.27/ 0.27 | 0.27/ 0.27 | 0.27/ 0.27 | 0.28/ 0.28 | 0.29/ 0.29 |
| Landscape: Dice Pool | 0.27/ 0.27 | 0.26/ 0.26 | 0.19/ 0.19 | 0.28/ 0.28 | 0.27/ 0.27 |
| ScreenProbes: Disocclusion | 0.17/ 0.17 | 0.17/ 0.17 | 0.17/ 0.17 | 0.17/ 0.17 | 0.17/ 0.17 |
| ScreenProbes: Filter | 0.14/ 0.14 | 0.14/ 0.14 | 0.14/ 0.14 | 0.14/ 0.14 | 0.14/ 0.14 |
| ScreenProbes: Trace Near Field | 0.13/ 0.13 | 0.12/ 0.12 | 0.11/ 0.11 | 0.13/ 0.13 | 0.12/ 0.12 |
| GPU Unaccounted: graphics queue | 0.13/ 0.13 | 0.13/ 0.13 | 0.13/ 0.13 | 0.13/ 0.13 | 0.12/ 0.12 |
| HZB | 0.12/ 0.12 | 0.12/ 0.12 | 0.12/ 0.12 | 0.12/ 0.12 | 0.12/ 0.12 |
| RDG Wait: graphics queue on async | 0.12/ 0.12 | - | - | 0.09/ 0.09 | 0.10/ 0.10 |
| RDG Wait: graphics queue on async | 0.12/ 0.12 | - | 0.02/ 0.02 | 0.09/ 0.09 | 0.10/ 0.10 |
| EditorImGui | 0.11/ 0.11 | 0.11/ 0.11 | 0.11/ 0.11 | 0.11/ 0.11 | 0.11/ 0.11 |
| ScreenProbes: Trace Screen | 0.09/ 0.09 | 0.07/ 0.07 | 0.08/ 0.08 | 0.09/ 0.09 | 0.08/ 0.08 |
| RDG Wait: graphics queue on async | 0.09/ 0.09 | 0.03/ 0.03 | - | 0.07/ 0.07 | 0.08/ 0.08 |

Async-очередь (1_static): 7 сегментов, 2.494 мс в перекрытии — LightGrid 0.009, LightGridWorld 0.036,
DistanceField near-field 0.039, RadianceCache Mark..Filter 0.685, ScreenProbes Place..TraceScreen 0.449,
near field 0.142, far field..Temporal 1.134. Графика ждёт async: перед `Deferred: Reflections (SoftwareTraced)`
0.555 мс (0.93 / 0.70 / 0.53 / 0.32 в остальных), перед `SurfaceCache: Radiosity Trace (N pages)` ~0.03-0.12 мс.

Сумма графической очереди, где лежат 17.17 мс: трава+G-buffer 6.55 (из них трава ≈ 4.7: Foliage Low 1.81),
DLSS-RR 3.12, TerrainGBuffer 1.31, облака 1.37 (March+Resolve+Upsample), отражения 0.93 + ожидание 0.56,
Composite 0.44, Landscape Dice 0.27, тени FromCache 0.26 (в ветре +1.5 мс Movable-каскадов), HZB 0.12, ImGui 0.11.

## 2. Верхние проходы: наша цена, ориентир UE, причина, план

| # | проход | наша цена | ориентир UE (та же схема, то же разрешение) | причина разницы (file:line) | план |
|---|---|---|---|---|---|
| 1 | Deferred: GBuffer + Masked Depth Prepass | 3.69 + 2.86 = 6.55 (Low 1.81) | UE r.EarlyZPass masked: prepass с clip, base pass EQUAL без clip и без записи глубины | схема уже UE: `MeshRenderer.hpp:1192-1216`, `DShaderParser.hpp:111` (DESERT_SURFACE_DEPTH_EQUAL). Цена = плотность травы x quad-overdraw тонких лезвий; GrassVisibilityBuffer (табл. 2) 3.41+2.57+0.30+0.30 = 6.58 — не дешевле | вне зоны (Grass*, «Не трогать»). Владельцу травы: visibility-raster травы дешевле лишь если G-buffer из него не перерисовывается; UE 5.5 — Nanite foliage. Самый большой рычаг кадра (до −4 мс) |
| 2 | DLSS-RR | 3.12 (одинаково во всех 5) | тот же вызов NGX через NVIDIA-плагин UE: та же модель (preset Default), та же цена на пиксель вывода | лишнего в вызове нет: теги `eValidUntilPresent` — Streamline не копирует (`StreamlineRuntime.cpp:1100-1121`), alpha upscaling выключен (`:940-949`), гайды — отдельный проход 0.04. CNN-пресета у RR нет (`StreamlineRuntime.cpp:93-105`: D/E) | на ориентире, кода не меняю. Режим Quality = NVIDIA auto для вывода ≤1440p. Проверка лида: замер RR preset D против E (Scalability `DlssRRPreset*`) и сверка с таблицей NVIDIA для 3070 Ti |
| 3 | TerrainGBuffer | 1.31 | — | Terrain* | вне зоны |
| 4 | Clouds: March (+Resolve 0.03, Upsample 0.04) | 1.29 (ветер 1.48) | UE r.VolumetricRenderTarget.Mode 0: RT в половине разрешения, 1 из 4 пикселей в кадр + temporal reconstruction | схема уже UE: trace 299x110 = четверть 1197x440 (`VolumetricCloudRenderer.cpp:2276-2286`), реконструкция по истории (`:2348-2431`), двухуровневый шаг (`CloudGeometry.glslh:291-298`), MaxSteps 256 и stop T 0.005 = UE StopTracingTransmittanceThreshold (`VolumetricCloudComponent.hpp:503-509`), самотень = cloud shadow map + 8 локальных шагов (`CloudRaymarch.shader:441-470`). Уже стоит между fork и join async (`SceneRenderer.cpp:2418-2440`) | на ориентире по схеме. Число шагов и света — из ассета облаков (вид из ассетов), трогать вслепую нельзя. Дальше — только по замеру вариантов шейдера (лид) |
| 5 | SSR: Classify + Trace (Deferred: Reflections SoftwareTraced) | 0.93 | Lumen reflections под DLSS-RR: трасса в разрешении рендера, 1 spp, без денойзера; MaxRoughnessToTrace | схема та же: DownsampleFactor 1 и без денойзера под RR (`SceneRendererFrameDeferred.cpp:1881-1886`), порог шероховатости (`SSR.shader:41-45`, `SSRTrace.glslh:349`) | на ориентире |
| 6 | RDG Wait перед Deferred: Reflections | 0.56 (0.32..0.93) | UE: async-Lumen перекрывается тенями и облаками, join на композите | цепочка async от fork RadianceCache: 0.685 + 0.449 + 0.142 + 1.134 = 2.41 мс; графики в окне: HZB 0.12 + облака 1.37 + capture ≈ 1.5-1.9. По времени ожидание совпадает с концом ПОСЛЕДНЕГО async-сегмента (Integrate/ShortRangeAO/Temporal, их потребитель — композит) — вывод по длительностям, не по дампу join. Тени нельзя перенести в окно: `SurfaceCacheDirectLighting.shader` читает каскады, а SurfaceCache — вход RadianceCache | рычаг в RDG (разрезать сегмент по потребителю, `RDGCompile.cpp:870-900` FindAsyncRuns) — RDGBuilder в «Не трогать» (RDG-PARALLEL-8). Передать владельцу RDG: ожидание 0.3-0.9 мс |
| 7 | Deferred: Composite | 0.44 | UE deferred lighting полноэкранный ~0.4-0.6 при 0.5 Мп | — | на ориентире |

## 3. Итог разбора

В зоне задачи (DLSS-RR, облака, SSR, композит) все проходы уже устроены как в UE и стоят на ориентире: в них
нечего «привести к схеме UE» без смены ассетов или качества. 3.2 мс до 14 мс лежат вне зоны:
трава в G-buffer/masked prepass (≈4.7 мс; Grass*), TerrainGBuffer 1.31 (Terrain*), Movable-каскады в ветре
+1.5 мс (MeshRendererShadow*), ожидание async 0.3-0.9 мс (RDG). Пункт 2 брифа поэтому не даёт кода в
DLSS-RR/облака/SSR — любая правка там была бы снижением качества (вариант «б»), а не схемой UE.

## 4. Порядок для следующих задач (по выгоде)

1. Трава в G-buffer (Grass*): цель −2..−3 мс; замер `Grass: Visibility Raster/Resolve` против `Deferred: GBuffer`.
2. Movable-каскады в ветре (MeshRendererShadow*): UE r.Shadow.CacheWPOPrimitives / раздельный кэш статики — −1 мс в 2_wind.
3. RDG: join по первому потребителю каждого прохода, а не по концу сегмента — −0.3..−0.9 мс.
4. TerrainGBuffer (Terrain*): −0.3..−0.6 мс.
5. Облака: замер вариантов шейдера (локальная самотень 8 -> 4, sky-occlusion fetch) — только с покадровым сравнением облаков.
