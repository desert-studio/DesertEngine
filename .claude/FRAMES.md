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
- **Диалог «Recover unsaved work?» живёт НЕ в HOME**: метка падения — `<TREE>/Editor/Resources/Assets/Scenes/Autosave/.session.lock` в ДЕРЕВЕ. Свежий HOME его не снимает (L8d: кадр целиком из диалога, а команды палитры при этом отвечали ok). Перед запуском в СВОЁМ дереве: `rm -f <TREE>/Editor/Resources/Assets/Scenes/Autosave/.session.lock`. В главном дереве (владельца) не трогать. После снимка — открой кадр целиком, уменьшенным (`ffmpeg -vf scale=1400:-1`), ДО обрезки.
- Скрипты с `C="DesertCtl --socket …"; $C …` — только под `/bin/bash`: zsh не делит `$C` на аргументы.
- **kill -9 — только СВОЙ редактор**: `pgrep -x Editor` найдёт и чужой (другой агент в другом дереве). Убивать по своей сцене/сокету: `pkill -9 -f "<TREE>/.*--scene .*<Scene>.desce"`; перед запуском ждать `until ! pgrep -x Editor`.
- В macOS нет `timeout` — не оборачивай им запуск; ограничение времени — `run_in_background` + `DesertCtl quit`.
- **НИКОГДА не снимай весь экран** (`screencapture -x` без `-l`): на нём почта, встречи и прочее владельца (SPL2
  снял именно так). Только окно редактора/заставки: id окна через `CGWindowListCopyWindowInfo` (python3 + Quartz,
  фильтр по владельцу `Editor`), затем `screencapture -x -o -l<id> файл.png` — работает, даже если окно перекрыто.
- Системный `python3` без Quartz: id окна — `osascript -l JavaScript` + `CGWindowListCopyWindowInfo`; рабочий
  скрипт окна заставки — `/private/tmp/claude-501/spl3/frames.sh`.
- ПОСМОТРИ на кадр (Read png) до того, как писать «работает».

## 1. Один кадр без взаимодействия (сцена + камера)
```
export HOME=/private/tmp/claude-501/<T>/home; mkdir -p $HOME
cd <TREE>/Editor && /Users/daniilsavcenko/.claude/tools/run_capped.sh ../build/Bin/Debug/Editor --project Desert.deproj \
  --scene Resources/Assets/Scenes/Starter.desce \
  --shot /private/tmp/claude-501/<T>/shot.png --shot-frames 90 --camera 0,200,400 --look 0,-0.3,-1 \
  > /private/tmp/claude-501/<T>/editor.log 2>&1
```
`--camera`/`--look` ОБЯЗАТЕЛЬНО со значениями. **Камера `0,200,0 --look 0,0.9,-1` на Starter видит ТОЛЬКО небо** (09-27: AL1-4, AL1-12a,
PSO1 — кадр «= dev» ничего не доказывал); для объектов Starter — `--camera 0,200,400 --look 0,-0.3,-1`; для неба —
три высоты (память check-three-elevations). **GPU-строка одного прохода:** `--gpu-profile` + `grep 'slot 0 | <pass>'`
в логе — мерить свою строку, не разницу кадров. Падение в teardown после записи PNG — известное, PNG уже записан.
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
- **Ландшафт, панель:** `bash /private/tmp/claude-501/lui2/shots.sh` (свежий HOME, LS7e_Bright, `run Landscape "Sculpt mode"`
  + `"Tool: <name>"`, shot-window). Обрезка: панель `crop=720:1400:0:180`, вьюпорт `crop=1430:1580:732:288`.
- **Моделинг (старое):** стартовый «Cube» — ПРИМИТИВ, инструментам не годится (пик отказывает «no editable mesh»).
  Сначала создать меш: `run Scene "Add shape: Cube"` или Create shape → «place at the viewport centre»;
  затем Select Elements tool, режим, `run ... "Mesh selection: pick at the viewport centre"`. Кадры P10e:
  `/private/tmp/claude-501/p10e/` (скрипт в `agent/`).

Появился новый рабочий рецепт — допиши сюда строкой в отчёте тимлиду (сам `.claude/` не правишь).

## 4. Добавлено 2026-09-27
- **DesertCtl из Bash-инструмента** — через `/bin/bash -c '…'`: инструмент запускает zsh, и `$C` не делится на аргументы.
- **Открыть ассет:** `$C run Open "<папка>/<файл>"` (путь от корня Assets), напр. `run Open "Textures/HDR/PreviewCheck.detex"`.
- **Details:** `run Entity <имя>` → `run Details "Show field: <Компонент> / <Поле>"`, `run Details "Open picker: Skybox|Static mesh|Material slot N"`;
  список появляется КАДРОМ ПОЗЖЕ — `sleep 2` перед `shot-window`. Popup выше окна уходит в окно ОС и в кадр не попадает.
- **Content Browser:** `run Panel "Open Assets"`, `run Assets "Open folder: Materials"`, `run Panel "Maximize panel: Assets"` / `"Restore panel"`.
- **Сокет на Windows:** путь ≤ 107 символов (AF_UNIX), напр. `C:/aftmp/x.sock`.
- **Если редактор вышел, а обёртка висит** — это был `footprint -p` (теперь с тайм-аутом 5 с в run_capped.sh).

## Sky bake runs (AL1-3b/3c, 09-27)
- `/private/tmp/claude-501/al1-3b/run.sh <name> <cold|warm> <look>` — SKY_HdrOrientation cold/warm run with a shot and the cache lines of the log.
- Look directions that match the reference frames `/private/tmp/claude-501/al1-3/pv-*`: horizon `0,0,-1`, zenith `0,1,0.01`, mid `0,1,-1`.
- DesertCtl may be unbuilt in an agent tree — use `DesertEngine/build/Bin/Debug/DesertCtl` from the main tree (AL1-5b, 09-27).
- `al1-3b/run.sh` waits until NO Editor runs — a foreign interactive Editor (control socket) blocks it forever; `/private/tmp/claude-501/al1-3c/run3.sh` does not wait (AL1-3c, 09-27).
- Mesh DnD through DesertCtl: `run Assets "Drop into the viewport: <path under Assets>"` (AL1-5c, 09-27).
- Pipeline build census after the splash: `/private/tmp/claude-501/pso2/measure.sh` (PSO2, 09-27).
- Budget refusal repro (CB_Red at `--view-budget-mib 250`, frames before/after): `/private/tmp/claude-501/rt2m/repro.sh [tag]` (RT2m, 09-27).
