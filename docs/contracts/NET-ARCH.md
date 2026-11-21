# NET-ARCH — multiplayer networking, built on World Partition (design, 11-21)

Owner 11-21: «Параллельно можем начать вести нетворкинг. И сразу адаптировать под WP».
This is the DESIGN only: no code, nothing built. Branch `docs/NET-ARCH` from `origin/dev` (72075de40);
revised on `docs/NET-MESH-ARCH` after the owner's answers of 11-21 (§2M server meshing, Linux-first, official
servers only, storage interface). The lead decides from it; what is still open is at the end (§11).

**Owner 11-21:** «Я не хочу иметь лимит на серверах игроков. Давай сначала Linux, наверное; свои сервера не надо,
я их буду держать только; 4) да давай» →
(1) **no player cap** — one world is many server processes (§2M); (2) **dedicated server Linux-first** (§6);
(3) **official servers only**, run by the owner — no community servers (§8.1); (4) **character storage behind an
interface**: SQLite for development, PostgreSQL for production (§2M.7).

## What the game needs (fixed, 10-09)

- RP multiplayer in the GTA5-RP / SA-MP-RP sense: **dedicated servers**, a server list, characters stored
  server-side, no save games on the client.
- **Server authority.** The client sends input and requests; the server simulates and decides.
- Own character / own vehicle: **client prediction + reconciliation**. Everyone else: **interpolation**.
  Hits: **lag compensation** on the server.
- The C++ framework (GameMode / PlayerController / Pawn / Vehicle) owns the network and replication;
  **Luau owns game logic** and reaches the network only through server/client RPCs and replicated script
  properties (§7).
- Physics stays behind `Physics::PhysicsWorld` (Desert/PhysicsCore) — Jolt is never named above it.

## What the engine has today (read 11-21, file:line)

| Part | Where | What it means for the network |
|---|---|---|
| WP runtime streamer | `Engine/Core/WorldStreamer.hpp:101` | Residency = UNION of all `StreamingSourceComponent`s (`GatherSources`, :219). Already multi-source: the server is "a streamer with N player sources". |
| Pure residency rules | `Core/Serialize/WorldPartitionResidencyExecutor.hpp`, `WorldPartitionStreamingRules.hpp` | Device-free, tested decisions — runs unchanged on a headless server. |
| Cooked cells | `Core/Serialize/WorldCells.hpp:122` (`IndexUnit::Ids`), :199 `WorldIndex` | Every cell record has a **64-bit id** listed in the cooked index, identical on every machine that has the same cook. |
| External entities | `Core/Serialize/ExternalEntities.hpp` | Record id = `Common::UUID`, its file path a function of the id alone — the id never changes while the entity exists. |
| Blocking on slow streaming | `WorldStreamer::BlocksPlay` (:152), used `Runtime/Source/RuntimeLayer.cpp:1034` | One global gate. The server cannot block for everyone (§4.4). |
| Physics | `PhysicsCore/PhysicsWorld.hpp:252` `kFixedStepSeconds = 1/60`, `Step` :312, `GetStepCount` :314 | A fixed 60 Hz step and a step counter already exist — the network tick IS this step. No SaveState/RestoreState yet. |
| Character movement | `Engine/ECS/System/CharacterMovement.hpp` | UE CMC math on Jolt `CharacterVirtual`, device-free — the shape a move-replay needs. |
| One player | `Engine/Core/Scene.hpp:492` `SetPlayerPawn`, :552 `PlayerViewPoint`; `Core/GameMode.hpp` | The scene knows ONE pawn. Becomes a list of PlayerControllers (NET-1). |
| Reflection | `Common/Core/Reflection.hpp:27` `FieldMeta`; `Scripting/ScriptSchema.hpp:28` `ScriptPropertyDecl` | Where the `Replicated` flag goes (§3.4). |
| Runtime target | `Runtime/Source/Main.cpp`, `Core/Application.hpp` | Always opens a Window + Device + Renderer. No headless target, no Linux build. |
| Networking | — | **None.** No socket, no transport in ThirdParty. |

## 1. Transport

The transport is only the pipe: connections, encryption, reliable and unreliable messages, fragmentation,
congestion control. Replication, ids, prediction and relevancy are ours and sit above an `INetTransport`
interface (UE's NetDriver/NetConnection split), so the transport can be swapped without touching them.

| | (a) Valve GameNetworkingSockets | (b) ENet | (c) own UDP layer | (d) yojimbo (+netcode, reliable) |
|---|---|---|---|---|
| Reliable + unreliable | yes; reliable ordered + unreliable messages, **lanes** with priority/weight (head-of-line separation) | yes; N channels, reliable/unreliable sequenced | whatever we write | yes; per-channel reliable-ordered / unreliable-unordered |
| Fragmentation of large messages | yes (up to 512 KB per message) | yes | to write | yes (blocks on reliable channels) |
| Congestion / bandwidth control | yes (rate-based, per connection, with stats: ping, loss, queue time) | basic throttle | to write | none (fixed send rate; we budget ourselves) |
| Encryption + auth | **yes, always on**: AES-GCM, Curve25519 key exchange, cert-based identity | **none** | to write (DTLS or libsodium) | yes: libsodium, **connect tokens** from a web backend |
| NAT / relay | ICE P2P optional; relay only through Steam (SDR) | none | to write | none (dedicated servers need none) |
| Platforms | Windows, macOS, Linux (+consoles via partners) | everything | ours | Windows, macOS, Linux |
| Licence | BSD-3 | MIT | ours | BSD-3 |
| Maintenance | Valve, active (used by CS2/Dota/Steam) | one maintainer, slow (1.3.x); IPv6 only in forks | us, forever | one author (Glenn Fiedler), low activity |
| Dependencies | protobuf (lite is enough) + a crypto backend (OpenSSL / libsodium; BCrypt on Windows) | none | none | libsodium, mbedtls |
| Diagnostics | per-connection real-time stats, simulated loss/lag/jitter built in (`FakePacketLoss/Lag`) | none | to write | network simulator built in |
| Connections per process (a gateway, §2M.2) | 1000+ proven (Steam game servers) | fine | — | designed around small max-clients (64 default) |

**Recommendation: (a) GameNetworkingSockets**, behind `INetTransport`.
- Encryption and authentication are mandatory for an RP server (accounts, characters, money): ENet would
  force us to write a DTLS layer, own UDP would force us to write the whole of (a) badly.
- Lanes give exactly what replication needs: an unreliable state lane, a reliable RPC lane, a bulk lane
  (late-join cell state, chat history) that does not head-of-line-block the others.
- Built-in lag/loss/jitter simulation = every NET slice's scenario runs under 150 ms / 2 % loss without a proxy.
- Maintained by the people who run the largest dedicated-server fleet; proven at 1000+ connections per process,
  which sets how many gateways a world needs — not how many players it holds (§2M).
- Cost: protobuf + a crypto library. Built from source through CMake like the rest of ThirdParty
  (`assimp-built-from-source` rule), protobuf-lite only. Steam relay (SDR) is NOT used — no Steamworks dependency;
  the open-source build connects by IP:port, which is what a server list needs.
- yojimbo is the honest second: its connect-token model fits a server list well, but it is near-unmaintained, has
  no congestion control and is sized for small sessions. Its **connect-token idea is kept** at the protocol
  level (§8): a backend-issued signed token, checked by the server, carried in GNS's connection handshake.

**Where UE is worse here.** UE writes its own UDP layer (UIpNetDriver + PacketHandler + an optional DTLS
handler that is off by default; most UE games ship unencrypted game traffic or bolt EOS/Steam sockets on).
We take encryption on by default from a maintained library instead of copying that layer.

## 2. Process model and tick

- **Server**: headless, authoritative, simulates at the physics fixed step (60 Hz, `kFixedStepSeconds`). "The
  server" below is the mesh of §2M — gateways + simulation servers + replication store; a single process is only
  the "mesh of one" of development. The **net frame number = the mesh's global step**, which every simulation
  server's `PhysicsWorld::GetStepCount()` is aligned to (§2M.4) — one clock for input, physics, snapshots, rewind
  and authority handover across processes.
- **Server send rate**: 30 Hz per connection by default (every second step), lowered per connection by the
  bandwidth budget and per object by priority (§3.6). A project setting, not a constant.
- **Client**: renders at any rate; simulates its own pawn at the same 60 Hz fixed step (prediction), sends input
  every step bundled redundantly (the last 3 steps per packet, so one lost packet loses no input).
- **Clock sync**: the client runs ahead of the server by RTT/2 + one jitter buffer (UE/Overwatch scheme); the
  server tells each client how full its input buffer is and the client time-dilates ±5 % to keep it at 1–2 steps.
- **Listen server is not a goal** (RP = dedicated only); PIE runs a real server process + clients (§9 NET-1),
  not UE's in-process "PIE as listen server" — one code path, so what PIE shows is what players get.

## 2M. Server meshing — one world, no player cap (owner 11-21)

Owner 11-21: «Я не хочу иметь лимит на серверах игроков». One process has a hard ceiling (one CPU socket's
step time, one NIC, one address space), so "no cap" means **one world is served by many processes**, and every
part of §3–§8 is written so it never assumes there is only one. Nothing in this document is a max-players
constant; every budget is per process or per connection and the answer to "too many" is "add a process".

### 2M.1 The options

| | (a) Sharding / instancing (EVE-style copies, WoW layers/realms) | (b) Static spatial meshing (SpatialOS-like workers on fixed WP regions, direct server↔server handover) | (c) Dynamic meshing with a replication layer (Star Citizen "Replicant", SpatialOS runtime) |
|---|---|---|---|
| Can every player meet every other? | **No** — players in different copies never see each other. A cap per copy, renamed. | Yes | Yes |
| Where state lives | in each server | in each sim server; neighbours exchange proxies peer-to-peer | in a **replication store** separate from simulation; sims and gateways subscribe |
| Client connection | to one server, reconnects on transfer | to the owning sim, reconnect or relay on border crossing | to a **gateway** for the whole session; never sees which sim owns what |
| Load follows players | no (copies are opened by count) | no — a crowd in one region overloads that region's sim | **yes** — regions split/merge, sims start/stop by load |
| Sim crash | that copy is lost | that region is lost until restarted, state since last save lost | sim is restarted, its region resumes from the store (≤ one step lost) |
| Cost | lowest | middle: N² neighbour links, handover protocol | highest: store + gateways + coordinator, one extra hop (~1 step latency inside the datacentre) |
| Path to the next column | — (a dead end) | to (c) = rewrite the state flow (peer links → store) | — |

UE 5.4+ **Multi-Server Replication** (experimental plugin) sits between (a) and (b): game servers peer to each
other through beacons, every server loads the **whole** world, actor authority is partitioned by game code, and
non-owned actors arrive as replicated proxies; a proxy net driver can front several game servers for one client.
It has no spatial partitioning, no load-driven split and no state store — every server still pays for the whole
world. **Where UE is worse:** it scales by copying the world into every server; we partition the world (WP already
has the grid) and keep one copy of state.

**Choice: (c)'s architecture from the first mesh slice, scheduled statically first (b-mode), dynamic later.**
- The expensive mistake is the state flow, not the scheduler. Star Citizen shipped static meshing first and had
  to introduce the replication layer before dynamic meshing was possible; we build the store + gateway split
  once, and "static vs dynamic" is then only the coordinator's region→sim assignment policy.
- (a) is rejected outright: it is a player cap per copy.
- (b) with peer links is rejected as a target: it is cheaper only until the first crowd, and its border
  protocol does not survive a region boundary that moves.
- Staged: NET-1..3 run "a mesh of one" (all roles in one process over in-process links — same interfaces);
  NET-MESH-1 splits the roles into processes; NET-MESH-2/3 add a second sim with static regions;
  NET-MESH-6 turns on dynamic split/merge.

### 2M.2 Process roles

| Role | Owns | Scales by |
|---|---|---|
| **Gateway** | client GNS connections, connect-token check (§8), per-connection relevancy (§3.5), priority/bandwidth (§3.6), acked baselines and delta compression (§3.4), handle tables (§3.2), routing a client's input/RPCs to the sim that currently has authority over its pawn | connections (each gateway holds a slice of the players; a client keeps one gateway for its session) |
| **Simulation server** (`DesertServer --role sim`) | authority over the replicated entities inside its **regions**: physics, `CharacterMovement`, server Luau of those entities, prediction acks, lag compensation; streams the WP cells of its regions + a border band (§4.1 per sim) | regions (space) — and in a dense cell, entities (§2M.8) |
| **Replication store** | latest state of every replicated entity (quantized fragments + change masks), the **authority map** (entity → sim, epoch; region → sim), the world delta table (§3.3), NetId block leases (§2M.6); publishes change streams to subscribers **by WP cell**; write-behind to `IServerStorage` (§2M.7) | partitioned by cell key (consistent hash of the runtime-grid cell), so a store node is never the whole world either |
| **Mesh coordinator** | region→sim assignment, starting/stopping sims and gateways, split/merge decisions (NET-MESH-6), health; control plane only, no game state | one (with a standby) |
| **Global services** (`DesertServer --role service`) | world-level server Luau that belongs to no place: economy, jobs board, chat channels, admin; talks to sims and gateways through the same declared remotes (§7) | by service |
| **Login / master** | accounts, connect tokens, the official world list (§8) | web backend, not the engine |

A "mesh of one" is all of these in one `DesertServer` process with in-process links; the interfaces
(`IReplicationStore`, `IGatewayLink`, `ISimLink`) are the same, so NET-1..3 are not thrown away.
All mesh links run over GNS too (encrypted, lanes), inside the datacentre.

### 2M.3 Region ownership by WP cells
- A **region** is an aligned block of the WP runtime grid (a node of a quadtree over the grid the plan already
  cuts, `Rules::WorldPartitionPlan`). The coordinator assigns regions to sims; static meshing = a fixed region
  table per world, dynamic = the coordinator splits a region into its four children or merges siblings.
- A sim **streams the union of the player sources inside its regions** (§4.1 restricted to its regions) plus a
  **border band**: the cells within the interaction radius of its regions' edge, loaded with their Server
  payload (§4.2) so proxies have ground and collision.
- Cell entities (stable NetIds, §3.2) are owned by the sim owning the region the cell lies in; their changed
  state (§3.3) lives in the store, so a region moving to another sim moves no state — the new owner reads it.

### 2M.4 Authority handover
- Every replicated entity has an **authority record** `(simId, epoch)` in the store. Only the holder of the
  current epoch may publish its state; a publish with an older epoch is rejected by the store.
- **Trigger**: the owner sees the entity inside a neighbour's region beyond a hysteresis margin (half a cell —
  no ping-pong on the line).
- **Protocol** (at a mesh step N): the owner publishes full state + the entity's unacked client inputs and
  in-flight Luau timers at N, then compare-and-swaps the authority record to `(newSim, epoch+1)`. The new owner
  already has the entity as a proxy (it is in its border band), so it promotes the proxy to authoritative at
  N+1 with the published state — no spawn, no gap.
- **Client side**: nothing happens. Same gateway, same NetId, same handle, same stream. The gateway routes the
  client's next inputs by the new authority record; inputs that reached the old owner after step N are
  forwarded to the new one by step number. The client's prediction (§5.1) sees an ordinary ack stream.
- Physics islands hand over as a whole (§2M.5).
- **Mesh clock**: all sims step the same global step number (mesh epoch time ÷ 1/60 s, kept by the coordinator,
  sims time-dilate like clients in §2). The net frame (§2) is this global step, so handover, input and lag
  compensation share one clock across processes.

### 2M.5 Cross-border interest and physics islands on borders
- **Sim interest**: a sim subscribes in the store to the cells of its border band; entities there that it does
  not own are **proxies** — kinematic in its `PhysicsWorld`, driven by the owner's published state, readable by
  its Luau (`FireNear`, perception), never writable. A script write to a proxy becomes a declared remote to the
  owner (§7 rules), ordered by step.
- **Client interest**: a gateway subscribes for each client to the cells in its net cull radius (§3.5)
  regardless of which sim owns them — a border is invisible to clients by construction.
- **Physics on a border**: a dynamic body touching a proxy would push it only one way. Rule: **an interacting
  physics island has one authority.** When Jolt reports a contact between bodies of different authorities in
  the border band, the island (bodies + constraints, the same unit as the vehicle resim island of §5.2) is
  handed over to one sim — deterministic choice: the sim owning the island's most bodies, ties to the lower
  sim id — at the next step. Kinematic characters (`CharacterVirtual`) against proxies need no merge.
  Until the merge (≤ 1 step) the contact is resolved by the owner treating the proxy as kinematic.
- **Lag compensation across a border** (§5.4): proxies carry the same 1 s hit-shape ring, published by their
  owner; the shooter's sim rewinds proxies like local pawns and sends the hit to the target's owner as an
  ordered request (damage is applied by the authority, never by the shooter's sim).

### 2M.6 NetId global uniqueness
- **Stable** ids = cooked record ids (§3.2) — already global, identical in every process of the mesh.
- **Dynamic** ids: the store **leases id blocks** (e.g. 65 536 ids) to each sim; a sim spawns from its block
  without asking anyone. The high-water mark is persisted through `IServerStorage`, so a restarted world never
  reuses an id. The kind bit stays; nothing in the id names a sim (authority moves, the id does not).
- Per-connection handles (§3.2) live in gateways — they are a per-connection compression, not a world limit.

### 2M.7 Persistence — `IServerStorage`
- One engine interface, two implementations: **SQLite** (development, a file next to the mesh-of-one server)
  and **PostgreSQL** (production). Owner 11-21 accepted.
- What it stores: accounts→characters (blob per slot), the world delta table (§3.3), the NetId high-water mark,
  global-service data (economy, jobs).
- Who writes: the store, **write-behind** per dirty entity/record (seconds), so sims never block on a database.
  Money/item transfers are **transactions** issued by global services with an idempotency key, committed in the
  database before the result is published — a crash can lose a footstep, never duplicate money.
- Reads: on join (character), on world start (delta table, id mark); never per frame.

### 2M.8 What is still physically bounded — density in one cell
Space can be split down to one runtime-grid cell; a crowd inside one cell cannot be split spatially further.
Two things stay bounded there, and neither becomes a refusal:
1. **Simulation of one cell.** Beyond the smallest region, authority is split **by entity**: the coordinator
   assigns the entities of an overloaded cell to several sims (each sees the others' as proxies; interacting
   physics islands still merge under one owner, §2M.5). The remaining bound is the largest single physics
   island, which is game-shaped (a pile of cars), not player-shaped.
2. **One client's downlink and render.** A client in a crowd of thousands cannot receive or draw all of them at
   full rate. The per-connection budget (§3.6) stays fixed; priority lowers the rate of the far and the
   unimportant (nearest K at 30 Hz, the rest at 2–5 Hz with reduced fragments — position + yaw + locomotion),
   and the client draws the far crowd through animation LOD/budget (owner 10-20 far-view decision). Quality
   degrades gracefully with density; nobody is turned away.

## 3. Replication model

### 3.1 What replicates
An entity replicates when it has a `NetObjectComponent` (UE `bReplicates`): its owner connection (none = server),
its class priority, its cull distance (or "the WP grid of its cell", §3.5), dormancy, and its replicated
components. A component replicates the fields its reflection marks `Replicated` (§3.4). Transform is a
dedicated, quantized fragment (position 1 mm in the cell-relative frame, rotation smallest-three 3×10 bits,
velocity) because it is most of the traffic.

### 3.2 Net ids — THE WP STABLE-ID RULE
`NetId` is 64 bits with a 1-bit kind:

- **Stable (cell / always-loaded records):** `NetId = the record's id` — the same `std::uint64_t` the cooked
  index lists (`WorldCells::IndexUnit::Ids`) and the `.deent` file is named by (`ExternalEntities::FileOf`).
  Every machine with the same cook instantiates that record with that id, so **a cell entity is never "spawned"
  by the network**: the server sends only its state, and the client applies it to the entity its own streamer
  made from the same record. The connect handshake compares the world's **cook hash** (index digest) and refuses
  a client whose cook differs, by name (UE checks package GUIDs the same way).
- **Dynamic (spawned at run time — pawns, dropped items, script spawns):** `NetId` = server-assigned counter,
  never reused within a session. The server sends a **spawn message** (prefab asset GUID + initial state +
  owner) on the reliable lane; the client makes it through `PrefabFactory` and binds the id.
- **On the wire**, each connection keeps a handle table (UE NetGUID ack, Iris `NetRefHandle`): a full 64-bit id
  is sent once, then a 16/20-bit per-connection handle. Entity references inside replicated fields
  (`EntityRef`, Luau entity refs) travel as handles; a reference to an entity the client does not have yet
  resolves when it arrives (UE's unmapped-GUID queue), never as a silent null.

### 3.3 When a stable entity stops being "as authored"
A record streamed back in is "the cell as it was authored" (`WorldStreamer.hpp:31`). With a network that is not
enough: a door opened, a barrel destroyed, a parked car driven away. The replication store (§2M.2) keeps a
**world delta table** per stable id, and it is part of the session state, not of the cell:
- **Destroyed** stable ids (UE `DestroyedStartupOrDormantActors`): the client's streamer SKIPS those records when
  it activates their cell; a client that connects late receives the list for the cells it loads.
- **Detached** stable ids: an entity that leaves its cell's ownership (possessed, picked up, moved beyond its
  cell) becomes server-owned dynamic **with the same NetId**; its record is skipped on load and it replicates as
  a spawned entity from then on. Server cell unload does not destroy it.
- **Changed** stable ids: the last replicated state is kept while the server has the cell unloaded, and is the
  baseline sent when any client next loads it — so state outlives server residency too.
Persistence of the table across restarts goes through `IServerStorage` (§2M.7).

### 3.4 Dirty tracking (push first, poll as the fallback)
- `FieldMeta` gains `Replicated` (+ optional `ReplicatedCondition`: OwnerOnly, SkipOwner, InitialOnly — UE
  `COND_*`), and a quantizer per type. `ScriptPropertyDecl` gains the same flag for Luau properties.
- **Push**: the engine's write paths for reflected components (`registry.patch` / `ComponentEvents`, Luau property
  writes) set a per-object dirty bit and a per-field change mask (Iris push model). Transform is marked by the
  physics/movement write-back that already runs once per step.
- **Poll** only where a write bypasses those paths: an object due for sending (§3.6) compares its quantized
  state with its last quantized copy. Only due objects are polled, never the world.
- Per **connection × object**, the server keeps the last ACKED change mask; a lost packet re-sends the dirty
  bits it carried (Iris), not a whole snapshot. Deltas are against the acked baseline; a new connection or a
  newly relevant object gets a full state.

### 3.5 Relevancy through the WP grid
The store indexes replicated objects **by the runtime grid cell**, and each gateway keeps the same index for its
connections' subscriptions of their position
(the same grid the plan cuts: `Rules::WorldPartitionPlan`). Each connection's relevant set each send:
1. its own pawn, its PlayerController, and anything marked AlwaysRelevant;
2. objects in grid cells within the connection's **net cull radius** of its view point — per class, default
   = the loading range of the grid the object would stream with (so the network never shows what the client
   cannot have streamed, and never hides what it has);
3. **cell entities only for cells the client reports as resident** (§4.3).
Iris does this with `NetObjectGridFilter`, a separate grid of its own; we reuse the WP grid, so relevancy and
residency are one geometry. Dormant objects (no change for N seconds, no velocity) are skipped until woken.

### 3.6 Priority and bandwidth
Per connection a byte budget per send (default 64 KB/s, project setting). Objects in the relevant set are
sorted by `priority = classPriority × distanceFactor(view → object) × (1 + timeSinceLastSent)` and written until
the budget is spent (UE prioritization; Iris `NetObjectPrioritizer`). Owned pawn and recent RPCs first.

## 4. Server streaming

### 4.1 Rule
**Each simulation server streams the UNION of the streaming sources inside its regions** — one
`StreamingSourceComponent` per pawn it has authority over (or the view point when there is no pawn, as
`GatherSources` already does), plus server-side sources (an NPC schedule, a script `RequestStreaming(pos, radius)`),
plus its border band (§2M.3). In the mesh of one that is every player. This is exactly what the streamer already
computes for N sources; NET-1 only makes the scene hold N players. Gateways stream nothing — they hold no world.

### 4.2 What the server loads of a cell
A dedicated server needs collision, gameplay and replicated records, not visuals. The **cook splits every cell
payload into a Server part and a Client part** by record content (render-only records — foliage visuals,
decals, VFX, lights, HLODs, landscape materials — are Client-only; collision, triggers, spawners,
`NetObjectComponent` records, scripts with a Server realm are Server). The server target loads no HLOD residency
and no textures. One residency plan, two payloads, chosen by the target — not two worlds.
UE does this with `bIsClientOnly` / `bIsServerOnly` actor flags and `NeedsLoadForServer`; we derive it from
the record's components at cook so nobody has to tag it by hand.

### 4.3 The client streams only around its own pawn
The client's streamer has one source: its own pawn (or its view point). It never streams around other
players. It reports **cell residency changes** to the server on the reliable lane (UE `ServerUpdateLevelVisibility`):
- cell becomes resident on the client → the server starts replicating that cell's stable entities to it
  (full state for the changed ones from §3.3, nothing for untouched ones — they are already right from the cook);
- cell unloads on the client → the server stops; the client's handles for those ids are released.

### 4.4 A replicated entity whose cell the client does not have
- A **stable** entity of a cell the client has unloaded: it is gone on the client with the cell, the server
  sees the residency report and stops — no orphan, no ghost. When the cell returns, §3.3 restores its state.
- A **dynamic** entity (another player's pawn, a car) belongs to no cell: it is relevant by distance (§3.5).
  If it stands where the client has no cell resident (e.g. a remote pawn at the edge of the net cull radius
  beyond the client's loading range), it is still replicated and drawn, but its physics is **not simulated on
  the client** (interpolated only — it never needs ground), and it is attached to the HLOD level for drawing.
  Net cull radius ≤ loading range by default makes this the exception, not the rule.
- **Server side**: a pawn whose cell is not resident on the SERVER yet (teleport, fast vehicle) is frozen
  (movement not stepped, input queued) until its cell is resident — a **per-source block**, not the global
  `BlocksPlay` gate (which stays a client-side loading overlay). UE has the same per-source behaviour
  (`bBlockOnSlowLoading` on the source); the server never stops the world for one player.

## 5. Prediction and reconciliation

### 5.1 Own character — move replay
The character is a Jolt `CharacterVirtual` driven by `CharacterMovement` (UE CMC math). It is kinematic, so it
needs no world rewind: the client keeps a ring of (step, input, resulting state); the server simulates the
same inputs, and sends back the authoritative state for the last processed step; on mismatch beyond a
tolerance the client sets that state and **replays its unacked inputs** through `CharacterMovement` against the
current world (UE `ClientAdjustPosition` + saved moves; UE 5.5 Mover has the same contract). A visual
smoothing offset hides the correction. Precondition: `CharacterMovement` is deterministic for a given input
and world on both sides (same step, same float path — clang vs MSVC: argument-order rule in memory).

### 5.2 Own vehicle — physics resimulation
A vehicle is a dynamic rigid body set; replay needs the world's state. `PhysicsWorld` gains
`SaveState(step, bodies)` / `RestoreState(step)` over Jolt `StateRecorder` for a **resim island** (the vehicle's
bodies + the bodies touching them), a 1 s ring of saved steps, and `Resimulate(fromStep, toStep, inputs)`.
On a server correction the client restores the island at the corrected step and resimulates to now (UE 5.5
Network Physics Prediction / Chaos resimulation does the same on the whole Chaos scene — we keep it to the
island because the world is a city, and a whole-scene rewind on the client is cost without gain).

### 5.3 Everyone else — interpolation
Remote pawns and vehicles are drawn 100 ms (2 snapshots + jitter, adaptive) in the past, interpolated
(Hermite with replicated velocity); short extrapolation on loss, capped at 250 ms. Animation is driven locally
from replicated locomotion variables (speed, direction, stance, aim), not replicated poses.

### 5.4 Lag compensation
The server keeps, per player pawn, a 1 s ring of hit shapes (physics-asset bodies) per step. A client hit
request carries the step it saw (its interpolation time); the server rewinds the shapes of the pawns in the
shot's path to that step, tests, restores (UE has no built-in one — every shooter writes its own; we make it
an engine service so Luau weapons get it for free). Max rewind is capped (250 ms) so high ping cannot shoot
round corners.

## 6. Dedicated headless server target

- **`DesertServer`** — a separate executable target that links Engine **without** Graphic/RHI, Window, Audio,
  ImGui (UE `UE_SERVER` build). Engine code that touches the renderer behind services (MeshService upload,
  TextureService, HLODs) gets a server implementation that loads the CPU half only (collision mesh, bounds).
  The split is compile-time (`DESERT_SERVER` define + CMake target), not a `--nullrhi` flag in the client binary:
  no Vulkan/D3D12 loader on a host, no GPU code in the server's attack surface.
- **Linux-first (owner 11-21).** Linux x86-64 is the hosting target and the first platform `DesertServer` is
  built and gated on: CMake + clang (the compiler Mac already uses, so the clang-vs-MSVC argument-order class of
  defects does not grow), libc++ static, an OCI container image per role (§2M.2). **No Linux build exists today**
  — NET-MESH-0 brings it, server-only: the Engine core without Graphic/RHI/Window/Audio/ImGui, plus the
  platform pieces Linux lacks (file/paths, threads naming, crash dumps, signals). A Linux job joins CI for
  `DesertServer` only; the client stays Windows (primary) + macOS.
- macOS and Windows builds of `DesertServer` exist for development only (PIE starts a local mesh of one); they are
  never hosted.
- Every role is the same binary with `--role gateway|sim|store|coordinator|service|all`; `all` = mesh of one.
- Console + log + an admin channel (authenticated, over the same transport, owner's tools only); no editor UI.
- The cook gains a **server cook** (§4.2): server payloads, scripts of the Server realm, no textures.
- PIE: the editor launches one `DesertServer --role all` (or a local mesh: 1 gateway + 1 store + K sims) + N client runtimes on the open level (UE "Play as Client,
  Number of Players"), each a real process, all on loopback with the GNS lag simulator configurable.

## 7. The Luau boundary

C++ owns: connections, PlayerControllers, pawn spawn/possess, replication, prediction, relevancy, streaming.
Luau owns: game rules — jobs, inventory, chat commands, economy, character creation flow.

- **Script realms**: every script asset states `Server`, `Client` or `Shared`. Server scripts are never cooked
  into the client (they hold the rules and the anti-cheat logic).
- **Remote events are declared**, not ad hoc: `Net.DefineRemote("BuyItem", { item = "string", count = "u8" })`
  in a Shared script. Arguments are validated and size-capped by the engine before the handler runs, and
  rate-limited per player — a malformed or flooding client is dropped by C++, never reaches Lua.
- Server: `Net.Server.On(name, fn(player, args))`, `Net.Server.FireClient(player, name, args)`,
  `Net.Server.FireAll(name, args)`, `Net.Server.FireNear(position, radius, name, args)` (relevancy-aware, uses
  the §3.5 grid).
- Client: `Net.Client.FireServer(name, args)`, `Net.Client.On(name, fn(args))`.
- Allowed argument types: nil, bool, integers, numbers, strings (capped), vectors, entity refs (→ NetId handles,
  §3.2), arrays/maps of those (depth- and size-capped). No functions, no userdata.
- **Replicated script properties**: a `ScriptPropertyDecl` with `Replicated` (+ condition) travels with the
  entity through §3.4 — Lua writes it on the server, clients read it; `OnRep_<Name>` fires on the client.
- RPCs go on the reliable lane; `Net.Server.FireUnreliable` exists for cosmetic events.

## 8. Sessions, server list, characters

- **Connect token** (yojimbo idea): a backend signs `{account, server, expiry}`; the client presents it in the
  GNS handshake; the server verifies the signature offline. No password ever reaches a game server.
- **World list, not a server list**: an entry is a **world** (one mesh, §2M), not a process. Gateways heartbeat
  our master with their world, load and a ping endpoint; the client's menu lists worlds with total players and
  queries the nearest gateway for ping. Player count is shown, never capped. LAN discovery by broadcast exists
  for development only.
- **Characters are game data**: the engine gives the mesh an `IServerStorage` interface (load/save a character
  blob by account + slot, async; SQLite for development, PostgreSQL for production — §2M.7) and calls Luau hooks
  `OnPlayerJoined / OnCharacterSelected / OnPlayerLeft` on the sim that gets the player's pawn.

### 8.1 Official servers only (owner 11-21) — consequences
- **No server binary leaves our infrastructure.** Server-realm scripts, server cook payloads and the store
  format are never distributed — the anti-cheat logic stays unseen, and the protocol needs no compatibility
  across third-party versions: client version = world version, a client with another cook hash is refused
  by name and sent to update.
- **Client Luau ships in the client's cook** (closes old question 4): the client never runs server-supplied
  code; Client/Shared realm scripts change with a client patch.
- **The master accepts only our worlds**: gateways and sims carry certificates issued by our CA (GNS
  cert-based identity, §1); a heartbeat or a mesh link without one is refused. No community admin plugins, no
  per-server mod loading; admin tools are ours and authenticated.
- **The mesh can assume a datacentre**: sims, store and gateways sit on one low-latency network, which is what
  makes the extra hop of §2M.1 (c) cheap.
- `SaveGame` (`Core/SaveGame.hpp`) stays for single-player tools; multiplayer never saves on the client.

## 9. Slice plan

Each slice is one result, from dev tip, with its scenario. Frames come from the client windows (and the
server's console log where named). All net scenarios run under the GNS simulator at 150 ms RTT / 2 % loss
unless said otherwise.

Order: **NET-MESH-0 → NET-1 → NET-2 → NET-3 → NET-MESH-1 → NET-MESH-2 → NET-MESH-3 → NET-4 … NET-8 →
NET-MESH-4 → NET-MESH-5 → NET-9 → NET-10 → NET-MESH-6 → NET-MESH-7.** NET-1..3 run a "mesh of one" (all roles of
§2M.2 in one process, in-process links behind the same interfaces), so nothing in them assumes one process and
nothing is rewritten when the roles split. Bot counts in scenarios are test loads, never limits.

| Slice | One result | Scenario |
|---|---|---|
| **NET-MESH-0** | First Linux build: `DesertServer` target (Engine core, no Graphic/RHI/Window/Audio/ImGui) on Linux x86-64 clang + CMake, the platform pieces Linux lacks, an OCI image, a CI job building it; also builds on macOS for PIE. Loads a cooked WP world headless and steps it. | **Тогда [mesh0_linux_server]:** the Linux CI artifact (container) runs `DesertServer --role all --world WP_Protocol --steps 600` — its log shows no device created, the cooked index loaded with the cook hash, 600 steps at 60 Hz, and the residency union of one server-side source; `ldd` of the binary lists no Vulkan/X11/Wayland library. |
| **NET-1** | GNS behind `INetTransport` + connect/handshake with cook hash + N PlayerControllers in the Scene (replaces the single `m_PlayerPawn`) + mesh-of-one roles as in-process modules (`IReplicationStore`, `IGatewayLink`, `ISimLink`) + server streaming = union of sources + dynamic pawn spawn replication + unreliable transform snapshots + plain interpolation; input → server, no prediction yet. Scenario runner learns to start a server + N clients. | **Тогда [net1_two_clients]:** `DesertServer --role all` on WP_Protocol (Linux container; log shows no device created) + 2 client windows; client A walks 20 m forward — on client B's frame A's pawn has moved 20 m along the street; client B's frame shows its own pawn where B stands; server log lists both sources in the residency union. |
| **NET-2** | Replication core: `Replicated` in `FieldMeta`, push dirty + change masks into the store, per-connection acked baselines in the gateway, quantization, handle tables, stable NetId = record id for cell entities, dynamic NetIds from leased blocks (§2M.6), reliable lane for spawn/destroy. | **Тогда [net2_door_latejoin]:** server opens a door entity of a WP_Protocol cell (console) — both clients show it open; client C joins 30 s later — its first frame shows the door open; server stat line shows the door was sent as state, never as a spawn. |
| **NET-3** | WP coupling: cell residency reports from clients, relevancy by WP grid, world delta table (destroyed / detached / changed stable ids) in the store, per-source server block. | **Тогда [net3_far_and_back]:** client A runs 1.5 km away — A's frame no longer has B's pawn, A's log shows the start cells unloaded; the server destroys a barrel at the start meanwhile; A returns — A's frame shows no barrel and B's pawn again. |
| **NET-MESH-1** | Roles split into processes: 1 gateway + 1 store + 1 sim (+ coordinator) on Linux, mesh links over GNS with CA certificates, mesh global step clock; clients connect to the gateway only. | **Тогда [mesh1_split_roles]:** the NET-1 and NET-2 scenarios pass unchanged against a 3-process Linux mesh; the gateway log shows A's inputs routed to sim 1 by the authority map; killing nothing, `ps` in the containers shows the three roles; client frames identical in content to net1_two_clients / net2_door_latejoin. |
| **NET-MESH-2** | Two sims, static regions (WP_Protocol cut along one street): region table in the coordinator, border band streaming, proxies, authority handover by epoch with input forwarding. | **Тогда [mesh2_cross_border]:** A walks across the region border while B stands on the far side — B's frames show A's pawn moving continuously (no pop, no double), A's frames show no snap and HUD "corrections" = 0; the store log shows A's authority `(sim1,e) → (sim2,e+1)` at one step; A walks back — handover again, hysteresis keeps it to one each way. |
| **NET-MESH-3** | Border physics and combat: island authority merge across sims, script writes to proxies as remotes, lag compensation on proxies with damage applied by the target's owner. | **Тогда [mesh3_border_physics]:** A (sim 1) and B (sim 2) push the same barrel standing on the border from both sides — frames show it moving by the sum of the pushes, no jitter; the store log shows the barrel island owned by one sim; A shoots B across the border at 150 ms — A's frame shows the hit marker and B's health drops on B's frame, sim 2's log applied it. |
| **NET-4** | Own-character prediction + reconciliation by move replay on `CharacterMovement`; clock sync with time dilation to the mesh step. | **Тогда [net4_predicted]:** at 150 ms / 2 % loss client A sprints and jumps a fence — frames show the pawn responding the same frame as input, no snap-back; client HUD stat "corrections" = 0 over 30 s; then the server teleports A — one correction, smoothed. |
| **NET-5** | Remote smoothing: adaptive interpolation buffer, Hermite, capped extrapolation, replicated locomotion variables driving the remote AnimGraph. | **Тогда [net5_remote_walk]:** client B's frame sequence of A walking a curve at 150 ms + 30 ms jitter shows a smooth path and a walk cycle matching speed (no foot skating, no jumps between frames). |
| **NET-6** | Lag compensation service: hit-shape history per step (published for proxies, §2M.5), rewind on hit requests, cap 250 ms; Luau `Weapons` reach it through one call. | **Тогда [net6_hit_moving]:** A fires at strafing B at 150 ms — A's frame shows the hit marker, server log confirms the hit at A's seen step; at 400 ms ping the same shot is refused by the cap. |
| **NET-7** | Luau boundary: script realms + server-only cook, `Net.DefineRemote` / On / Fire*, argument validation + rate limit (in the gateway, before any sim), replicated script properties with `OnRep`. | **Тогда [net7_lua_chat]:** A types `/me waves` — B's frame shows the chat line from A's name; a client flooding 1000 RPCs/s is dropped (gateway log names the remote); a server Lua property `JobName` set on A shows on B's nameplate. |
| **NET-8** | Sessions: connect tokens from our login backend, official world list from our master (gateway heartbeats with CA certificates), LAN discovery for development only, `IServerStorage` with SQLite (dev) and PostgreSQL (prod) + join/character hooks, disconnect/reconnect keeping the character. | **Тогда [net8_world_list]:** the client menu lists two official worlds with names, total players and ping; a gateway without our certificate is not listed (master log names it); joining one shows character select with the stored character (Postgres in the container); reconnect after a dropped connection returns to the same place. |
| **NET-MESH-4** | Persistence and recovery through the store: write-behind to `IServerStorage`, NetId high-water mark, coordinator restarts a dead sim and its region resumes from the store; transactions with idempotency keys. | **Тогда [mesh4_sim_crash]:** with A and B in sim 2's region, sim 2 is killed — both frames freeze < 2 s and resume with pawns where they stood, the door state and a dropped item intact; coordinator log shows the restart; A's money after a transfer in flight at the kill equals before+amount exactly once (Postgres row). |
| **NET-MESH-5** | Global services: `DesertServer --role service` running world-level server Luau (chat channels, economy), `FireAll` through gateways, `FireNear` through store interest across sims. | **Тогда [mesh5_global_services]:** A (sim 1, gateway 1) and C (sim 2, gateway 2) — A's global chat line appears on C's frame; a `FireNear` explosion on the border shows on clients of both sims; the service log shows one handler run per remote. |
| **NET-9** | Vehicles: `PhysicsWorld` SaveState/RestoreState/Resimulate over a resim island; vehicle prediction; a vehicle crossing a region border hands its island over (§2M.4). Depends on a vehicle component existing (none in the engine today). | **Тогда [net9_vehicle]:** A drives through a turn and across the region border at 150 ms / 2 % loss — A's frames show no wobble, B's frames show A's car smooth; a forced correction resimulates once without a visible snap; store log shows one island handover. |
| **NET-10** | Tools + per-process budgets: headless bot clients (no RHI, same input path), sim step budget and gateway bandwidth stats, a Network category in the Gameplay Debugger (relevant set, priority, bytes, corrections, owning sim, gateway). | **Тогда [net10_bots_budgets]:** client A's frame with the Network debugger category shows 256 bots (a test load) on one sim, sim step ≤ 16.6 ms, A's downstream within budget, the owning sim of each pawn under the cursor; residency union covers the bot spread. |
| **NET-MESH-6** | Dynamic meshing: coordinator splits/merges regions by sim step time, starts/stops sims; entity-sharded authority inside an overloaded cell (§2M.8); per-connection crowd LOD (nearest K full rate, rest reduced). | **Тогда [mesh6_crowd_split]:** 2000 bots converge on one square — the coordinator log shows the region split down to the cell and the cell's entities spread over several sims, every sim step ≤ 16.6 ms; client A in the crowd: frame shows the crowd drawn, A's downstream within budget, corrections ≈ 0; bots disperse — regions merge back and sims stop. |
| **NET-MESH-7** | Official deployment: container images per role on a Linux cluster (game-server orchestration, e.g. Agones on Kubernetes), coordinator asks the cluster for sims/gateways, rolling world update with forced client update by cook hash. | **Тогда [mesh7_deploy]:** a world deployed from the images on the cluster appears in the client's world list; a client with the old cook is refused with "update required"; scaling the bot load up adds sim pods (cluster log) and the world list shows one world throughout. |

NET-MESH-0 + NET-1 are the biggest pair (Linux build, server target, transport); NET-2 and NET-3 are the WP
adaptation the owner asked for "from the start"; NET-MESH-1..3 make the world multi-process before prediction
work, so prediction, lag compensation and vehicles are built against handover from their first day.

## 10. Where this differs from UE 5.5 (and why)

| UE 5.5 | Here | Why |
|---|---|---|
| Own UDP layer, DTLS optional/off | GNS, encryption always on | maintained crypto, no hand-written transport |
| Stable objects named by **path strings** (NetGUID from package path) | stable NetId = the cooked **64-bit record id** | the id already exists and is the same everywhere; no strings on the wire, no rename hazards |
| Iris `NetObjectGridFilter` = its own grid | relevancy on the **WP runtime grid** | relevancy and residency are one geometry; nothing to tune twice |
| Client/server-only actors tagged by hand (`bIsClientOnly`, `NeedsLoadForServer`) | split derived from record components at cook | nobody forgets a tag |
| Chaos resimulation of the whole scene | resim **island** around the predicted vehicle | a city's whole-scene rewind costs without gain |
| No engine lag compensation | engine service | every RP weapon needs it |
| PIE as an in-process listen server | PIE = real server process + client processes | one code path; what PIE shows ships |
| Multi-Server Replication (5.4+, experimental): peer servers, each loads the whole world, authority split by game code | mesh: gateways + sims on WP regions + replication store, authority handover by epoch | the world is partitioned once, state lives once, load follows players; no per-world cap |
| One dedicated server = one player cap (`MaxPlayers`) | no player constant anywhere; budgets per process / per connection | owner 11-21: no cap |

## 11. Decisions — closed 11-21 and still open

Closed by the owner 11-21:
- **Players per server** → no cap per world; the world is a mesh (§2M). The old "256 / 1000 / 2048" question is gone.
- **Linux server** → Linux-first hosting target, NET-MESH-0 (§6).
- **Community servers / client scripts** → official servers only; client Luau ships in the client cook (§8.1).
- **Character storage** → `IServerStorage`, SQLite for development, PostgreSQL for production (§2M.7).

Still open:
1. **Transport = Valve GameNetworkingSockets** (protobuf-lite + a crypto lib as new ThirdParty), used for client
   links and mesh links alike. Yes, or prefer yojimbo / own layer? (Not answered 11-21.)
2. **Hosting**: own hardware or a cloud provider for the Linux cluster (decides the orchestration in
   NET-MESH-7 — Agones on Kubernetes works on both; bare metal needs our own launcher), and which regions
   (one world per continent, or one world for everyone with gateways per continent — the mesh allows both).
3. **Order**: accept the mesh before prediction (NET-MESH-1..3 before NET-4), which delays the first
   "feels right" movement by those slices but means prediction is never rebuilt for handover?
