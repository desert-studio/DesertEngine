# Project templates

One folder per template, and **adding a template is dropping a folder in** — nothing is compiled,
nothing is rebuilt, and no list anywhere names them. The launcher scans this directory at start
(`Templates/*/template.json`) and draws a card for every manifest that parses; a folder whose
manifest does not parse becomes a message on the New Project screen carrying the parser's own
words, never a silent skip.

```
Templates/<Id>/
    template.json         the manifest — Common::Project::TemplateManifest, in desert-shared
    Media/Thumbnail.png   optional, 512x288 (16:9); absent = the card draws its placeholder
    Payload/              copied BYTE FOR BYTE into the new project root
```

`<Id>` is the folder name and is deliberately not repeated inside the manifest.

## The manifest

| field | meaning |
| --- | --- |
| `DisplayName` | the card's title. The only field a manifest has to carry. |
| `Description` | one or two lines under the title |
| `Category` | `""` = no category tabs. Tabs arrive as data, when there are enough templates to need them. |
| `SortKey` | ascending; ties break on `DisplayName`. This is how `Blank` sorts first without being alphabetically first. |
| `DefaultScene` | project-relative, e.g. `Assets/Scenes/Main.desce`. **The payload must contain this file** — the launcher refuses a create whose template names a scene it does not ship, rather than handing the Editor a project that cannot open. |

## No substitutions

`Payload/` is copied verbatim. A `.desce` is JSON full of GUID references to materials, and a
textual replacement inside one is a way to break a reference, not a way to personalise a scene. The
project's name lives in exactly one generated file, the `.deproj`.

## What is not here yet

A **First Person** template — a floor, a light and a player entity with a camera driven by a Lua
controller — has to be AUTHORED in the Editor and dropped into `Templates/FirstPerson/Payload/`.
That is content work with its own verification (play it, and shoot it from three elevations), not
launcher work, and it is tracked separately.

**Starter** is that kind of content: a test playground (sky and sun, ground, lit calibration rows,
glass, an emissive probe, shadow casters, fill lights, a camera) whose `Assets/Scenes/Main.desce` and
16 materials were authored by the Editor and are shipped as files. The Editor no longer builds any
scene in code; with nothing to open it opens the engine's Basic level template
(`Editor/Resources/Engine/Maps/Templates/Basic.desce`) as an untitled scene.
