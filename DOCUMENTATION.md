# Where things are written down

**Owner's decision, 2026-09-23, in two parts:**

1. Task tracking moves to **Notion**. `Docs/` stops being the queue.
2. **`Docs/` does not go into git.** It is frozen, then deleted.

This file is tracked so that agents and future sessions read the rule rather than reconstruct it.

## The four homes

| what | home | who reads it |
|---|---|---|
| queue and status | **Notion → Задачи** | owner, lead |
| why a thing exists, and why something was refused | **Notion → Спеки** | owner, lead |
| the reasoning an implementer needs | **commit messages** | agents, git, permanently |
| evidence frames, UE reference source | local archive, tracked by nothing | nobody, until needed |

## Why reference material does NOT go into git, and why that is survivable

`Docs/` is 989 MB over 139 non-image files and `git ls-files Docs` returns **0**. An agent's
worktree therefore contains **zero** files under it. That bit already cost us: a brief told an
agent to read `Docs/Sky/N7_HANDOVER.md`, a path that exists in no worktree.

The owner's answer is not to track it. The answer is that **a commit message already carries
reasoning better than a document does** — the N7 commits carried the measurement table, the
argument that separated two suspects, and the reason the fix was shared, and they were better
than the handover note written about them. Commit messages are in git, are read by agents, and
survive the deletion of `Docs/`.

What reaches an agent, then, is: the **brief written into its worktree**, and the **commit
history**. Nothing else is guaranteed to.

## The one thing that must be lifted before `Docs/` dies

Ten files under `Docs/` hold **measured refusals**; four hold decisions waiting on the owner.
Those do not survive as titles. A refusal is only worth keeping if it carries its number and its
return condition, because otherwise the question gets re-opened from memory — which happened the
same day this file was written: mip streaming was reported as "ready to start" when
`Docs/World/07_mip_streaming.md` had closed it at **0.41 %** (texture pixels 5,636,608 bytes of
1,382,105,088 held by the engine).

They are lifted into **Спеки**, because a refusal is a decision and that is what Спеки are for.

## What must never be uploaded or committed

`Docs/Clouds/UEReference/Source/**` is Epic Games source carrying
`// Copyright Epic Games, Inc. All Rights Reserved.` It is a local reading copy: not into git,
not into a cloud workspace. The shot directories stay out for size — they are machine output and
can be re-shot.

## The rule while the migration is unfinished

1. **New work items are created in Notion only.** The roadmap files under `Docs/` are frozen:
   read them, never extend them.
2. **A Notion card is not an argument.** A number on a card names its provenance in
   "Чем доказано" — a suite, a census, a frame, or a commit.
3. **Briefs never cite a `Docs/` path** unless the lead copied that file into the agent's
   worktree in the same breath — and it is then read-only and uncommittable.
4. **A debt is migrated when its card carries the same reasoning the document did.** Copying a
   title across is not migration; it is losing the argument and keeping the label.

## What is left to lift

- `Docs/UI_ROADMAP.md` — 26 open items
- `Docs/CUBEGRID_TODO.md` — 8 remaining sections
- `Docs/EDITOR_DETAILS_ROADMAP.md` — the open "risks / decisions" section
- 10 files holding measured refusals, 4 holding open owner decisions

When this list is empty, the roadmap files are **deleted**, not left behind as a second answer.
