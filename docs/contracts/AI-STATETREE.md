# AI-STATETREE contract: NPC decisions (StateTree), perception (sight and hearing), tasks in Luau

Status: HEADER CONTRACT, waiting for the owner's "yes". Nothing is implemented. The headers are the API that
the implementation cards (section 5) must deliver unchanged, or change only with a new owner "yes".
Card: Notion "Задачи" AI-STATETREE. Analysis: Notion "Вики" AI-STATETREE (10-11).

## 1. Headers

| Header | What it declares | UE counterpart |
| --- | --- | --- |
| `Engine/AI/StateTree/StateTreeTypes.hpp` | `StateIndex`/`NodeIndex`, `RunStatus`, `StateType`, `SelectionBehavior`, `TransitionTrigger`/`Target`/`Priority`, `ConditionOperand`, `StateTreeEvent` | `StateTreeTypes.h` (`EStateTreeRunStatus`, `EStateTreeStateType`, `EStateTreeStateSelectionBehavior`, `EStateTreeTransitionTrigger`, `EStateTreeTransitionType`, `EStateTreeTransitionPriority`), `StateTreeEvents.h` `FStateTreeEvent` |
| `Engine/AI/StateTree/StateTree.hpp` | Authored tree `StateTreeDesc`/`StateDesc`/`TransitionDesc`/`NodeDesc`/`PropertyBinding`; compiled tree `CompiledStateTree` (`CompactState`/`CompactTransition`/`CompactNode`/`CompactBinding`); `Compile`, `ReadStateTree`/`WriteStateTree` | `UStateTreeEditorData`, `UStateTreeState`, `FStateTreeTransition`, `FStateTreeEditorNode`; `FCompactStateTreeState`, `FCompactStateTransition` (StateTreeTypes.h:668); `FStateTreeCompiler` |
| `Engine/AI/StateTree/StateTreeNodes.hpp` | `IStateTreeTask`, `IStateTreeCondition`, `IStateTreeEvaluator`, `NodeContext`, `TransitionInfo`, `NodeRegistry`, `RegisterBuiltinNodes` | `FStateTreeTaskBase` (StateTreeTaskBase.h:19), `FStateTreeConditionBase` (StateTreeConditionBase.h:21), `FStateTreeEvaluatorBase` (StateTreeEvaluatorBase.h:18), `FStateTreeTransitionResult` |
| `Engine/AI/StateTree/StateTreeExecution.hpp` | `StateTreeExecution` (Start/Tick/SendEvent/Stop/ShouldTick/ActiveStates/Trace), `ExecutionSetup`, `ExecutionTraceEntry` | `FStateTreeExecutionContext` (StateTreeExecutionContext.h:329), `FStateTreeInstanceData`, `FStateTreeTraceEvent` |
| `Engine/AI/StateTree/StateTreeLuau.hpp` | Luau node module format, `LuauNodeSchema`, `IStateTreeScriptHost` (implemented by `Scripting::ScriptEngine`) | `UStateTreeTaskBlueprintBase`, `UStateTreeConditionBlueprintBase`, `UStateTreeEvaluatorBlueprintBase` |
| `Engine/Assets/StateTreeAsset.hpp` | `StateTreeAsset` (`.dstatetree`): authored + compiled tree, revision, `MarkEdited` recompiles | `UStateTree` (StateTree.h:133) |
| `Engine/AI/StateTreeComponent.hpp` | `StateTreeComponent` / `StateTreeData` (asset, StartOnPlay, RestartOnFinish, parameter overrides, transient execution) | `UStateTreeComponent` (StateTreeComponent.h:39), `UStateTreeAIComponent` (StateTreeAIComponent.h:16), `FStateTreeReference` |
| `Engine/AI/Perception/PerceptionTypes.hpp` | `Sense`, `AffiliationFilter`, `SightConfig`, `HearingConfig`, `DamageConfig`, `Stimulus`, `NoiseEvent`, `DamageEvent` | `FAISenseAffiliationFilter`, `UAISenseConfig_Sight` (AISenseConfig_Sight.h:18), `UAISenseConfig_Hearing`, `UAISenseConfig_Damage`, `FAIStimulus`, `FAINoiseEvent`, `FAIDamageEvent` |
| `Engine/AI/AIPerceptionComponent.hpp` | `AIPerceptionComponent` (senses, team, transient known stimuli), `AIPerceptionStimuliSourceComponent` | `UAIPerceptionComponent` (AIPerceptionComponent.h:208), `UAIPerceptionStimuliSourceComponent` (AIPerceptionStimuliSourceComponent.h:15) |
| `Engine/AI/AISystem.hpp` | `AISystem` (perception, then decisions), `ReportNoiseEvent`, `ReportDamageEvent`, `SendEvent`, `StartTree`/`StopTree`/`StopAll`, `AISystemSettings::MaxSightTracesPerFrame` | `UAIPerceptionSystem` (Tick, MaxTracesPerTick), `UAISense_Hearing::ReportNoiseEvent` (AISense_Hearing.h:64), `UAISense_Damage::ReportDamageEvent`, `UStateTreeComponent::SendStateTreeEvent` |

Dependency headers declared by this contract (declarations only; the named card implements them, nothing links a call before it lands):
- `Engine/GameplayTags/GameplayTag.hpp` - GameplayTag / GameplayTagContainer / registry (TAGS card, Q5).
- `Engine/AI/AIController.hpp` - AIControllerComponent (controller entity, transient Pawn), AIPawnComponent (on the pawn: controller prefab + AutoPossessAI), `AI::Possess` / `Unpossess` / `ControllerOf` (AI-CONTROLLER card, Q4). `StateTreeComponent.hpp` includes it.
- `Engine/Physics/CollisionProfiles.hpp` - trace channels `EngineTraceChannels::kVisibility / kCamera / kSight`, `CollisionChannelId`, `CollisionProfiles::ResolveChannel` / `QueryResponse`; `Engine/Physics/PhysicsWorld.hpp` - `CastRay(origin, direction, maxDistance, traceChannel)` (CHANNELS card, Q8; it removes the channel-less CastRay).

## 2. What it is built on (existing code)

- Luau: `Scripting/Luau/LuauRuntime.hpp` (one VM, one sandbox per slot, compile-once cache, watchdog,
  `Load`/`Reload`/`Defines`/`Call`/`CallWithEntity`), `LuauEntityRef`, `Scripting/ScriptSchema.hpp`
  (`ReadScriptSchema`, `ScriptPropertyDecl`), `Scripting/ScriptProperty.hpp` (`ScriptProperty`). Luau node
  parameters are declared with the same `Properties = { ... }` table entity scripts use. The Details panel
  already edits that.
- Host: `Scripting/ScriptEngine.hpp` owns the world's runtime, debugger and hot reload. It implements
  `IStateTreeScriptHost`, so AI scripts get breakpoints and reload with no second runtime.
- Assets: `Assets/AssetBase.hpp`, following the `Assets/AnimGraphAsset.hpp` pattern: shared parsed object,
  revision owned by the asset, static `Save`.
- Components: the `COMPONENT(Key, Block(Data), Run(ActorsAndUI))` and `REFLECT`/`PROPERTY` markers
  (`CoreReflection/ReflectionMacros.hpp`), scanned by DesertHeaderTool. Example: `ECS/WindSourceComponent.hpp`.
- Systems: `ECS/System/System.hpp`. Traces: `Core/Scene.hpp` `Scene::Raycast(ray, hit, accept)`. Parallel
  traces: `Common/Core/JobSystem.hpp` `ParallelFor`.
- Model of a state machine: `Animation/Graph/AnimGraph.hpp` (Condition/Transition) and `AnimGraphValidation.hpp`
  (GraphWarning). These were used as a pattern, not as code: StateTree has hierarchy, tasks with results, and
  events.

Not in the engine (checked 10-21 on int/w1021): navigation (no nav mesh, Recast/Detour or path following),
Controller/Pawn/AIController, GameplayTag, physics overlap/sweep or collision channels, and a server/client
split.

## 3. Where this engine differs from UE

1. **The authored and compiled trees are separate, and only the authored one is stored.** UE serializes
   the compact arrays into the asset and keeps the editor data beside them. Here the compact arrays are
   derived on load and on every edit. One source of truth, so a stale compiled tree cannot ship.
2. **Nodes are shared, per-NPC state is instance data (Q6).** As in UE, node objects belong to one
   compiled tree (`StateTreeNodeSet`) and are shared by every NPC running it; each NPC's state lives in
   `INodeInstanceData` owned by its `StateTreeExecution`. One Luau environment per script class, the
   per-NPC state is the script's `InstanceData()`. Crowds and traffic are still a separate system
   (CITY-TRAFFIC).
3. **Luau instead of Blueprint.** A script declares its kind (`Node = "Task"`), its parameters
   (`Properties`) and its outputs (`Outputs`). A missing required function is a COMPILE error of the tree,
   not a runtime surprise. A script error fails the task and is never treated as Running.
4. **The tree lives on the AI Controller (Q4).** The controller and the pawn are two entities
   (`AIController.hpp`); `NodeContext` carries `Controller` and `Pawn`. Perception sits on the controller,
   the eye point comes from the pawn.
5. **Senses are fixed members, not an array of config objects.** Sight, Hearing and Damage are
   `AIPerceptionData` members with `Enabled`. This gives a plain reflected block and no instanced-object
   editor. A new sense is a new member together with its system.
6. **Nothing is perceivable by default.** UE auto-registers every pawn as a sight source. Here only an
   entity with `AIPerceptionStimuliSourceComponent` is a source, so there is no hidden global rule.
7. **Events and stimuli are `GameplayTag`s (Q5).** `StateTreeEvent::Tag`, transitions and noise/damage
   tags use the registered tag type (`Engine/GameplayTags/GameplayTag.hpp`), with `FGameplayTag::MatchesTag`
   prefix semantics; no strings.
8. **Perception and decisions run in one system, in a fixed order** (perception, then trees, then
   movement). UE runs these as separate tick groups. Here the order is part of the API: a tree always sees
   this frame's perception.
9. **Teams are two integers** (`Team` on the perceiver and on the source) until a team system exists. This
   replaces UE `IGenericTeamAgentInterface` / `FGenericTeamId`.

## 4. Behaviour the implementation must keep (UE semantics)

- Tick order: evaluators and global tasks; then active tasks root to leaf; then transitions from the leaf
  up, where the highest priority wins and ties go to the deepest state; then ExitState leaf-first and
  EnterState root-first. A completion inside EnterState re-evaluates in the same tick, at most
  `kMaxTransitionsPerTick` times. Going over that limit stops the tree with an error.
- ExitState is called for every task that was entered, whatever it returned.
- Scheduled tick: a tree with no ticking task, no OnTick or delayed transition, and no pending event is
  not ticked.
- Sight hysteresis: a target is gained inside SightRadius and lost outside LoseSightRadius. A lost stimulus
  is remembered for MaxAge. Traces are capped per frame across all NPCs and shared round-robin.
- Noise comes from gameplay (`ReportNoiseEvent`), never from the audio engine. Its range is
  listener HearingRange × Loudness, capped by MaxRange.

## 5. Implementation cards (from the 10-11 analysis)

AI-ST-ASSET (Compile, Read/Write, StateTreeAsset, AssetTypeID::StateTree and its AssetHandleStability entry,
serializing StateTreeComponent parameter overrides). AI-ST-EXEC (StateTreeExecution, NodeRegistry, Wait,
Succeed, Fail and SendEvent nodes). AI-ST-LUAU (IStateTreeScriptHost on ScriptEngine, the Luau node adaptors,
and the `ai.*` / `tree` / `perception` script globals). AI-CONTROLLER. AI-ST-EDITOR. AI-TASK-MOVE (needs
NAV-MESH / NAV-FOLLOW). AI-PERC-SIGHT, AI-PERC-HEAR, AI-DEBUG. Order: ASSET → EXEC → (LUAU ∥ CONTROLLER ∥
EDITOR) → (MOVE ∥ SIGHT) → HEAR → DEBUG.

## 6. REMAINDER: not in this contract, on purpose

- Utility and weighted state selection (UE 5.5 `TrySelectChildrenWithHighestUtility`). A new
  `SelectionBehavior` value can be added later without breaking anything.
- Mass StateTree (crowds and traffic): a separate system.
- SmartObjects (open question 3).
- Behavior Tree (open question 1).

## 7. Lead decisions 10-22 (owner: "the decision is yours, what matters is that the architecture is right")

- **Q1** Only StateTree; no Behavior Tree (two homes for one logic).
- **Q2** The architecture does not depend on the number: tick LOD (rate by distance / significance, `AISystemSettings::LodBands`) + batched processing; the budget for measurements is 1000 NPC per server (`BudgetNpcs`).
- **Q3** SmartObjects are a separate system; StateTree only queries and claims slots (UE).
- **Q4** The tree lives on the AI controller (UE UStateTreeAIComponent), not on the NPC body: `StateTreeComponent` and `AIPerceptionComponent` sit on the controller entity (`AIController.hpp`), the eye point is the possessed pawn's. Controller and navigation come first.
- **Q5** Real GameplayTags first (hierarchical, registered, interned); events, transitions and perception tags use `GameplayTag`, never strings.
- **Q6** One Luau environment per task script class; per-NPC state is instance data (`StateTreeNodeSet` shared per compiled tree + `INodeInstanceData` per NPC, UE instanced data).
- **Q7** Server only, enforced at registration; a client sees only the replicated result.
- **Q8** A separate trace channel Sight: glass lets sight through; foliage blocks it only with a flag on the foliage type (`BlocksSight`, default false, dense = true).

Module: Engine/AI is in module Engine today (catch-all row of cmake/DesertModuleTable.cmake). A separate AI module (UE AIModule / StateTreeModule) is the correct layering and is done in AI-ST-ASSET together with its build.
