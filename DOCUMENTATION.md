# Where things are written down

**Owner's decision, 2026-09-23: task tracking moves to Notion.** `Docs/` stops being the queue.
It does not stop existing, and this file says exactly which part moves and which does not — with
the measurements that decided it, so nobody re-opens the question from memory.

## The three kinds of content in `Docs/`, measured

`Docs/` is 989 MB over 139 non-image files, and **`git ls-files Docs` returns 0** — none of it is
tracked. That single fact is what this policy is about.

| kind | size | where it goes | why |
|---|---|---|---|
| **open work** — roadmaps, remaining-lists | tiny | **Notion** | that is what a queue is for |
| **mechanism, research, measured refusals** | 106 `.md`, 4.6 MB | **into git** | an agent must be able to read it, and an agent can read neither `Docs/` nor Notion |
| **evidence frames and UE reference source** | ~985 MB | **stays untracked** | bulk, and Epic's copyright |

## Why the middle row is NOT "move it to Notion too"

On the day this policy was written, a brief told an agent to read `Docs/Sky/N7_HANDOVER.md`. That
path does not exist in any worktree — `Docs/` is gitignored, so an agent's checkout has **zero**
files under it. The agent could not have followed the brief.

**Notion has the same property, and worse**: an agent cannot reach it at all. Moving reference
material there would make the invisibility permanent instead of fixing it. So reference goes into
the repository, where the people and the agents who need it actually are.

The same day, a second instance of the same defect: I reported mip streaming as "ready to start"
when `Docs/World/07_mip_streaming.md` had closed it with a **measured refusal** — texture pixels
are 5 636 608 bytes of 1 382 105 088, **0.41 %** of what the engine holds. A decision nobody can
read is a decision that gets contradicted.

Ten files under `Docs/` hold measured refusals; four hold decisions waiting on the owner. Those
are the highest-value rows to lift, not the roadmaps.

## What does NOT go into git, and must not

`Docs/Clouds/UEReference/Source/**` is Epic Games source, carrying
`// Copyright Epic Games, Inc. All Rights Reserved.` It is a local reading copy. Do not commit it
and do not upload it to a cloud workspace. The screenshots and per-task shot directories stay out
for size, not licence — they are machine output and can be re-shot.

## The rule while the transition is unfinished

1. **New work items are created in Notion, never in a `Docs/*.md` list.** The roadmap files are
   frozen: read them, do not extend them.
2. **A Notion card is not an argument.** If it states a number, the number's provenance lives in
   a commit message or a tracked document, and the card names it in "Чем доказано".
3. **Briefs never cite a `Docs/` path** unless the file has been copied into that agent's
   worktree in the same breath — and then it is read-only and uncommittable.
4. A debt is migrated when its card carries the same *reasoning* the document did. Copying a
   title across is not migration; it is losing the argument and keeping the label.

## What is left to move

- `Docs/UI_ROADMAP.md` — 26 open items
- `Docs/CUBEGRID_TODO.md` — 8 remaining sections
- `Docs/EDITOR_DETAILS_ROADMAP.md` — the open "risks / decisions" section
- 10 files holding measured refusals, 4 holding open owner decisions

Until that list is empty, **both places are live** and Notion is authoritative for *status*,
`Docs/` for *reasoning*. When it is empty, this section says so and the roadmap files are deleted
rather than left to rot as a second answer.
