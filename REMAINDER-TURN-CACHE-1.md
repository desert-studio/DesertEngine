# REMAINDER TURN-CACHE-1

Сделано (этот коммит): Camera.Spin теперь чистый поворот (позиция глаза побитово неизменна), а число сэмплов
карты теней облаков берётся под снапнутым центром карты, а не под сырым глазом. Без сборки и без замера:
лид собирает и прогоняет FPS-FIELD-BARN-TURN.scn (CSM и Shadows.Method=VirtualMaps). Прежний замер поворота
недействителен: глаз ездил по окружности радиусом 500 см.

## Осталось / не чинилось

1. CSM: каскады идут за взглядом по построению. ShadowCascades.hpp:364 `center = eye + viewFwd * sphereZ`,
   MeshRendererShadow.cpp:1105 `key.Add( m_CascadeCasterVP[v] )`. Поэтому MeshShadowCascade*.Static и
   TerrainShadowView в CSM на повороте честно перерисовываются (по брифу каскады не переделываем). Значит
   «TerrainShadowView ~ 0 на повороте» в CSM недостижимо без варианта (б) или без VSM.
   Попутно: CascadeShadowSignature (TerrainRenderer.cpp:1529) хеширует строку камеры целиком, а в ней есть
   DiceClip от фрустума (TerrainRenderer.cpp:221-226, LandscapeViewFootprint(viewProjection)) и DicePool.x
   (TerrainRenderer.cpp:844, смещение в пуле, оно зависит от DiceClip предыдущих тайлов). Это зависит от
   направления взгляда. Сейчас не важно, потому что CasterVP и так меняется. Станет важно, если каскады
   перестанут зависеть от взгляда.
2. VSM при ДВИЖЕНИИ (не повороте): подпись тайла ландшафта включает сырую позицию глаза DisplacementView
   (TerrainRenderer.cpp:167 записывается, :1638 хешируется). Затухание смещения следует за глазом непрерывно,
   поэтому идущая камера каждый кадр сбрасывает все страницы всех тайлов со смещением (TerrainVirtualShadow +
   VSM: Raster). Как в UE: смещение ландшафта для теней не затухает от глаза. Варианты: затухание от клетки
   глаза (DiceWindow уже привязан к клетке) или отдельное затухание для кастера. Нужно решение по архитектуре.
3. Если Clouds: ShadowMap останется на повороте после этого коммита, следующий подозреваемый —
   KeyImage -> GetResourceGeneration картинок среды (VolumetricCloudRenderer.cpp:1099, :1164), если стриминг
   текстур меняет device image при смене вида. Не проверено.
4. Трава (GRASS-TILE-1) и ScreenProbes async +2.4 мс я не разбирал. Screen probes по природе зависят от вида.
