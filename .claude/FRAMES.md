# FRAMES — как снять кадр (рецепты проверены; копируй, не ищи)

Агенты тратили 5–10 вызовов, чтобы заново найти строку запуска. Всё ниже работало 2026-09-24.
`<T>` — код твоей задачи, `<TREE>` — твоё дерево (абсолютный путь). Свои файлы — `/private/tmp/claude-501/<T>/`.

## 0. Общее
- Редактор ТОЛЬКО через `~/.claude/tools/run_capped.sh` (он сам ставит MoltenVK: VK_ICD_FILENAMES, VK_LAYER_PATH,
  DYLD_FALLBACK_LIBRARY_PATH; /bin/bash под SIP теряет DYLD_* снаружи). Один Editor на машину: `pgrep -x Editor` пусто.
- `HOME` — в scratch (не трогать `~/.desertengine/editor.json`).
- `--scene` — путь ОТНОСИТЕЛЬНО `Editor/`. Без `--scene` откроется автосейв/Starter, а не твоя сцена.
- **Не удалять `Editor/Resources/Assets/Scenes/Autosave/*`** — редактор тогда откроет другую сцену (L7j: пустой кадр).
- Бинарь должен быть свежее твоего последнего коммита кода: `stat -f %Sm <TREE>/build/Bin/Debug/Editor`.
- **HOME — СВЕЖИЙ на каждый запуск** (`/private/tmp/claude-501/<T>/home-$(date +%s)`): после падения в том же HOME
  следующий старт закрывает всё окно диалогом «Recover unsaved work?» (LUI1: 7 кадров одного диалога).
- В macOS нет `timeout` — не оборачивай им запуск; ограничение времени — `run_in_background` + `DesertCtl quit`.
- ПОСМОТРИ на кадр (Read png) до того, как писать «работает».

## 1. Один кадр без взаимодействия (сцена + камера)
```
export HOME=/private/tmp/claude-501/<T>/home; mkdir -p $HOME
cd <TREE>/Editor && /Users/daniilsavcenko/.claude/tools/run_capped.sh ../build/Bin/Debug/Editor --project Desert.deproj \
  --scene Resources/Assets/Scenes/Starter.desce \
  --shot /private/tmp/claude-501/<T>/shot.png --shot-frames 90 --camera 0,200,0 --look 0,0.9,-1 \
  > /private/tmp/claude-501/<T>/editor.log 2>&1
```
`--camera`/`--look` ОБЯЗАТЕЛЬНО со значениями. Падение в teardown после записи PNG — известное, PNG уже записан.
Облака/накопление: 90 кадров; per-frame-in-flight состояние — снимай и `--shot-frames 3`.

## 2. Сценарий (команды палитры, камера, несколько кадров) — DesertCtl
Запуск (Bash с run_in_background: true):
```
export HOME=/private/tmp/claude-501/<T>/home; mkdir -p $HOME
cd <TREE>/Editor && /Users/daniilsavcenko/.claude/tools/run_capped.sh ../build/Bin/Debug/Editor --project Desert.deproj \
  --scene Resources/Assets/Scenes/<Scene>.desce --control-socket /tmp/<T>.sock \
  > /private/tmp/claude-501/<T>/editor.log 2>&1
```
Клиент (`--wait` сам ждёт сокет и готовность — цикл опроса не нужен):
```
C="<TREE>/build/Bin/Debug/DesertCtl --socket /tmp/<T>.sock --wait 60"
$C commands | grep -i landscape          # что доступно СЕЙЧАС (список меняется по ходу)
$C run Landscape "Sculpt mode"           # <group> <label>, как в палитре; код 1 = отказ, причина в stderr
$C --subject viewport set Camera.Position -12000,12000,-12000
$C --subject viewport set Camera.Direction 0,-1,0.001
$C shot-viewport /private/tmp/claude-501/<T>/a.png   # только 3D; shot-window — весь редактор с панелями
$C state                                  # JSON: панели, выделение, документы, хвост лога
$C quit 0
```
Сетка до/после: `ffmpeg -loglevel error -y -i a.png -i b.png -filter_complex "[0]scale=1000:-1[a];[1]scale=1000:-1[b];[a][b]hstack" ab.png`

## 3. Готовые сценарии
- **Ландшафт (лепка/эрозия/Paste):** `/private/tmp/claude-501/l7j/run.sh` — холм кистью 1049, радиус 2048, 25 мазков
  Erosion, Copy/Paste на возвышенность; сцена `LS7e_Bright.desce` (неотслеживаемая, в дереве LS3), точка X=Z=-12000.
  Ловушки: сила кисти ОДНА на все инструменты; команда режима — `run Landscape "Sculpt mode"`;
  Undo сверх истории — «nothing left to undo», режим не выключает.
- **Моделинг (выделение) — РАБОЧИЙ скрипт `/private/tmp/claude-501/p10h/frames.sh`** (`P=0,150,200 bash frames.sh`):
  меш — `run Modeling "Create shape tool: Box"` + `run Modeling "Create shape: place at the viewport centre"`
  (выделяет созданное). Оверлей выделения рисуется ImGui — `shot-viewport` его НЕ видит: `shot-window` + обрезка.
  `~/.claude/...` пиши абсолютным путём: после смены HOME тильда раскрывается в scratch.
- **Моделинг TriEdit (диагональ):** `P=0,150,200 bash /private/tmp/claude-501/mui4/frames.sh`. Бокс из «place at the
  viewport centre» с камеры 0,300,600 встаёт центром в (0,150,200), НЕ в начало координат. Обрезка окна 4112x2578:
  вьюпорт `crop=1430:1580:732:288`, панель Modeling `crop=720:1440:0:180`.
- **Моделинг (старое):** стартовый «Cube» — ПРИМИТИВ, инструментам не годится (пик отказывает «no editable mesh»).
  Сначала создать меш: `run Scene "Add shape: Cube"` или Create shape → «place at the viewport centre»;
  затем Select Elements tool, режим, `run ... "Mesh selection: pick at the viewport centre"`. Кадры P10e:
  `/private/tmp/claude-501/p10e/` (скрипт в `agent/`).

Появился новый рабочий рецепт — допиши сюда строкой в отчёте тимлиду (сам `.claude/` не правишь).
