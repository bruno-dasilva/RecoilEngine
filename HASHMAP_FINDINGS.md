# Hash Container Findings

Running log of what we learn about `spring::unordered_map` / `spring::unordered_set`
(and friends) usage and performance in RecoilEngine. New findings go at the bottom.

Related docs:
- `KEY_DECISIONS.md` — instrumentation design decisions
- `coding-agents/profiling/` — profiling guides
- `results/` — fightertest runtime data
- `test/microbenchmarking.txt` — microbenchmarking methodology notes

---

## 1. Where hash containers live in the engine (static survey)

**Date:** 2026-04-10
**Source:** grep across `rts/` excluding `rts/lib/`.
**Build flag:** none — this is a static survey, not runtime data.

### 1.1 Aggregate declaration counts

| Type                           | Occurrences | Files |
|--------------------------------|------------:|------:|
| `spring::unordered_map`        |         203 |   104 |
| `spring::unsynced_map`         |          68 |    27 |
| `spring::unordered_set`        |          65 |    40 |
| `spring::unsynced_set`         |          16 |     8 |
| `std::unordered_map` (non-lib) |         ~21 |    15 |
| `std::unordered_set` (non-lib) |          ~5 |     5 |
| `spring::unsynced_multimap`    |           0 |     0 |

Takeaway: `spring::` wrappers dominate (~350 uses) vs raw `std::` (~26). The
`std::` holdouts are clustered in the GL/Lua/Rml boundary — most sites are
already flowing through the engine wrapper and will be covered by the
`SPRING_HASH_INSTRUMENTATION` build.

### 1.2 Per-subsystem breakdown (spring::\* only)

| Subsystem | Map uses | Set uses | Notes |
|-----------|--------:|--------:|-------|
| `Sim/` (Path, Units, Misc, Features, Projectiles, MoveTypes) | ~65 | ~15 | Gameplay hot path |
| `Rendering/` (Fonts, Models, Shaders, Textures, GL, Env) | ~55 | ~15 | Per-frame draw |
| `Game/` (Setup, UI, Players) | ~40 | ~15 | Mostly load-time + UI |
| `System/` (VFS, TimeProfiler, Config, Sound, creg, Net) | ~40 | ~7 | Infra |
| `Lua/` | ~25 | ~8 | Lua bridge caches |
| `Net/`, `ExternalAI/` | ~5 | ~1 | Networking / AI glue |

### 1.3 Likely hot per-frame sites (candidates for runtime profiling)

These are the static call sites most likely to dominate runtime hash operations,
based on what they hold and how often their surrounding code runs.

> **See §2.2 for the actual worst offenders from the instrumented run.** Most
> of the guesses below held up (MoveMath, CobEngine, ModelDrawerData, Shader
> uniforms, QTPFS) but several were wrong: HAPFS path maps, Font caches, Lua
> contexts, BuilderCaches, and LuaHandle context sets did not show up in the
> top offenders. Two real hot sites were missed here:
> `rts/System/TimeProfiler.cpp:27` (refCounters) and
> `rts/System/FreeListMap.h:122`.

**Simulation — pathfinding**
- `rts/Sim/Path/QTPFS/PathManager.h:112-119` — `PathTypeMap`, `PathTraceMap`,
  `SharedPathMap`, `PartialSharedPathMap` (uint/PathHashType → entity/exec).
- `rts/Sim/Path/HAPFS/PathManager.h:278` — `pathMap` (uint → MultiPath).
- `rts/Sim/Path/HAPFS/PathCache.h:82` — `cachedPaths` (uint64 → CacheItem).
- `rts/Sim/Path/HAPFS/PathFlowMap.hpp:52` — double-buffered `indices[2]`
  (`unordered_set<uint>`) rebuilt every flow update.
- `rts/Sim/MoveTypes/MoveMath/MoveMath.cpp:396` — per-thread `blockMaps`
  (`CSolidObject*` → `BlockType`) used during move-checks.

**Simulation — object/ID management**
- `rts/Sim/Misc/SimObjectIDPool.h:47-49` — `poolIDs` / `freeIDs` / `tempIDs`
  (uint32 → uint32); every unit/feature/projectile spawn hits these.
- `rts/Sim/Misc/LosHandler.h:173` — `instanceHashes` (int → vector of LOS
  instances); queried by LOS updates.
- `rts/Sim/Features/FeatureHandler.h:88` — `activeFeatureIDs` (set<int>).
- `rts/Sim/Units/UnitHandler.h:105` — `builderCAIs` (uint → CBuilderCAI*).
- `rts/Sim/Units/CommandAI/BuilderCaches.{h,cpp}` — `reclaimers`,
  `featureReclaimers`, `resurrecters` (sets of unit IDs) scanned by build AI.
- `rts/Sim/Units/CommandAI/CommandAI.h:158` — `commandDeathDependences`
  (`unsynced_set<CObject*>`).
- `rts/Sim/Units/Scripts/CobEngine.h:118,127` — `threadInstances` and
  `deferredCallins` (int → COB thread state); hit every COB tick.

**Rendering — fonts & glyph caches**
- `rts/Rendering/Fonts/CFontTexture.cpp:85-94` — `fontFaceCache`,
  `fontMemCache`, `invalidFonts`, `pinnedRecentFonts`.
- `rts/Rendering/Fonts/CFontTexture.h:201-208` — per-font `glyphs`
  (char32 → GlyphInfo), `kerningDynamic`, `glyphNameToIdx`.

**Rendering — model drawer, shaders, atlases**
- `rts/Rendering/Common/ModelDrawerData.h:83-84` — per-model
  `scTransMemAllocMap` and `lastSyncedFrameUpload` (touched every draw).
- `rts/Rendering/Shaders/Shader.h:321-325` — `UniformStates` (fast_hash),
  `luaTextures`, `attribLocations`, `outputLocations`.
- `rts/Rendering/Shaders/ShaderHandler.h:18-20,75` — `ProgramObjMap`,
  `ProgramTable`, per-program binary `cache`.
- `rts/Rendering/Textures/IAtlasAllocator.h:145` +
  `rts/Rendering/Textures/TextureAtlas.h:188-190` — name → entry, name → atlas
  tex, ptr → name.
- `rts/Rendering/Textures/S3OTextureHandler.h:66-68` — `TextureCache`,
  `BitmapCache`, `TextureTable`.

**Lua / scripting boundary**
- `rts/Lua/LuaHandle.cpp:74-76` — `SYNCED_LUAHANDLE_CONTEXTS` /
  `UNSYNCED_LUAHANDLE_CONTEXTS` (sets of context pointers).
- `rts/Lua/LuaMaterial.h:282` — `objectUniforms[2]` (int → array of
  `LuaMatUniform`); touched per-object per-frame when Lua materials active.
- `rts/Lua/LuaRulesParams.h:38` — `Params` (string → Param) read/written from
  gadgets.
- `rts/Lua/LuaUtils.cpp:46-160` — per-invocation `alreadyCopied` maps in
  `CopyPushData`/`CopyPushTable`; short-lived but called frequently during
  cross-state copies.

**Load-time only (likely cold at runtime)**
- `rts/Game/GameSetup.h:224-237` — 10 setup maps parsed once at game start.
- `rts/Sim/Units/UnitDefHandler.h:72-73`, `rts/Sim/Features/FeatureDefHandler.h:58`,
  `rts/Sim/Weapons/WeaponDefHandler.h:41` — def-name → ID tables filled during
  mod load, then mostly read.
- `rts/System/FileSystem/ArchiveScanner.{cpp,h}` — 5+ maps used during VFS
  scan/cache.
- `rts/Sim/Units/Scripts/{Cob,Lua}ScriptNames.{cpp,h}` — static script-name
  tables built once at init.

### 1.4 Key/value type patterns

- **Integer-keyed (uint32/int/uint64)** — dominant for runtime/gameplay state:
  unit/feature/projectile IDs, path entity IDs, LOS instance indices. Most
  promising targets for alternative hash policies (trivial hash); open-addressing
  is already in place in `SpringHashMap`.
- **`std::string`-keyed** — dominant for def/script/param/archive/atlas maps.
  Populated at load time and then mostly read; cost is in string hashing/copies
  at insert time rather than steady-state find().
- **Pointer-keyed (`CObject*`, `CWorldObject*`, `CUnit*`, `CFeature*`)** —
  dependency/listener tables and renderer per-object state. **Runtime data
  (§2.3 #2) shows this is the biggest distribution problem in the engine:**
  7 of the top 10 offenders are pointer-keyed, with peak probe lengths of
  10-41 under load. Aligned heap pointers have zero low bits, and the default
  hash does not shift them out before bucketing. `Shader.cpp:222` (which uses
  `fast_hash`) is the only pointer/int-like hot site with sane probe lengths.
- **Tuple/struct keys with custom hash** — `glExtra.h` sphere cache,
  `Shader.h` `fast_hash`, `GroundDecalHandler` `float4Hash`. Rare but worth
  watching: a bad custom hash can dominate probe length.

### 1.5 Open questions — resolved by §2

- **Which candidate sites dominate at runtime?** Answered in §2.2. Top 10 is
  led by `ModelsMemStorage.h:156` (alone ≈45% of pathfinding hash time),
  followed by the `ModelDrawerData.h:100` template (4 containers, ~30%
  combined), `MoveMath` blockMaps, and `CobEngine`.
- **Are the load-time maps truly cold?** Yes — confirmed in §2.3 #5.
  `ArchiveScanner.cpp:395`, `UnitToolTipMap`, and `SimObjectIDPool` are all
  populated once and never queried on the hot path. `unitDefIDs` is touched
  (~1.4M find-hits under pathfinding) but total cost is under 1 ms. Safe to
  ignore these for tuning.
- **Any sites with bad probe distribution?** Yes — many. `CobEngine.h:25`
  peaks at **1656**, `LosHandler.h:103` at 49-64, `MoveMath.cpp:403` and
  `ModelsMemStorage.h:156` at 40-41, QTPFS `PathManager.cpp:171` at 38-46.
  Root cause is a mix of tombstone accumulation and the pointer-hash problem
  called out in §1.4 — see §2.3 points #2 and #3.

---

## 2. Worst offenders — runtime stats from fightertest

**Date:** 2026-04-10
**Source:** `HashContainerRegistry::PrintAllStats()` dumps below — two
fightertest scenarios, "regular" and "pathfinding".
**Build flag:** `-DHASH_INSTRUMENTATION=ON`.

### 2.1 Scenario totals

| Scenario    | find-hit ops | find-hit ms | insert ops | insert ms | rehash ms | total ms (all ops) |
|-------------|-------------:|------------:|-----------:|----------:|----------:|-------------------:|
| Regular     |        80.3M |        3012 |      10.1M |      1067 |        10 |              ~4255 |
| Pathfinding |       307.8M |       19595 |      29.3M |      2347 |        16 |             ~22576 |

Pathfinding spends **~5.3× more** time in hash containers than the regular run,
and per-op find-hit cost rises from 37 → 64 ns — probe length grows with
population. Regular: 81 active containers (63261 skipped <1000 ops);
pathfinding: 83 active (144666 skipped).

### 2.2 Top 10 offenders (by total time across both runs)

| # | Container — source                                                                         | Regular ms | Pathfinding ms | Notes |
|--:|--------------------------------------------------------------------------------------------|-----------:|---------------:|-------|
|  1 | `map<CWorldObject*, size_t>` — `rts/Rendering/Models/ModelsMemStorage.h:156`                |       1102 |     **10183**  | 25.9M → 83M find-hits. Peak probe 40, probe histogram heavy across 4-7 / 8-15 / 16+ buckets (`▒▒▓` / `▓▒▒▓▓▓`). **Single container ≈ 45% of all pathfinding hash time. #1 hotspot by a wide margin.** |
|  2 | `map<CSolidObject*, BlockType>` — `rts/Sim/MoveTypes/MoveMath/MoveMath.cpp:403` (×16 threads) |        182 |      **4414**  | 147M find-hits + 21M misses + 21M inserts in pathfinding; `insert == find-miss` so entries are never evicted. Peak probe 41. |
|  3 | `map<CFeature const*, int>` — `rts/Rendering/Common/ModelDrawerData.h:100`                    |        224 |      **2715**  | 26.4M find-hits, peak probe 35-42, probe histogram `░░░▒▒█` — most hits fall in the ≥16-probe bucket. |
|  4 | `map<CUnit const*, int>` — `rts/Rendering/Common/ModelDrawerData.h:100`                       |        321 |       1751     | 18.6M find-hits, peak probe 10. Same source line as #3/#6/#7. |
|  5 | `map<int, CCobThread>` — `rts/Sim/Units/Scripts/CobEngine.h:25`                                |        727 |        647     | **Tombstone apocalypse:** 45% → **84%** tombstones, peak probe **1656** in pathfinding. 666K erases / 671K inserts per run — heavy churn with no effective rehash. |
|  6 | `map<CUnit const*, ScopedTransformMemAlloc>` — `rts/Rendering/Common/ModelDrawerData.h:100`    |        450 |        879     | Insert-bound: 2.3M / 2.5M inserts per run (441-833 ms in insert time alone). |
|  7 | `map<CFeature const*, ScopedTransformMemAlloc>` — `rts/Rendering/Common/ModelDrawerData.h:100` |        190 |        686     | Insert-bound: 1.0M / 2.4M inserts per run. |
|  8 | `map<QTPFS::PathHashType, int>` — `rts/Sim/Path/QTPFS/PathManager.cpp:171`                    |          0 |        405     | Active only under pathfinding load. 2.8M find-hits + 621K misses, load 50-57%, **43-49% tombstones**, peak probe 38-46. |
|  9 | `map<unsigned int, Shader::UniformState>` — `rts/Rendering/Shaders/Shader.cpp:222`             |        252 |        140     | 12.8M / 6.6M find-hits. Already uses `fast_hash`; peak probe only 11 — **volume, not distribution**, is the cost. |
| 10 | `map<unsigned int, int>` — `rts/System/TimeProfiler.cpp:27` (`refCounters`)                    |        241 |        160     | 12.3M / 7.7M find-hits. Called from the TimeProfiler hot path itself. |

**Just outside the top 10:**
- `map<int, size_t>` — `rts/System/FreeListMap.h:122` — 264 / 120 ms, heavy churn
  (2.1M inserts + 1.1M erases in the regular run).
- `map<int, vector<SLosInstance*>>` — `rts/Sim/Misc/LosHandler.h:103` — 26 / 182 ms,
  **peak probe 49-64** in pathfinding (bad distribution on int LOS indices).

### 2.3 Roll-up by key/value shape

Every instrumented container grouped by a coarse key→value shape (`ptr*` =
object pointer key; `scalar` = int/uint/enum/size_t/char/small pointer;
`struct` = larger struct or `vector<T>`). Each table's totals tie out to the
aggregate footer of its run.

#### Regular fightertest (total 4255 ms)

| # | Key → value shape                       | ms   |    %  | Dominant containers |
|--:|-----------------------------------------|-----:|------:|---------------------|
| 1 | `Map<ptr*, scalar>`                     | 1828 |  43 % | ModelsMemStorage (CWorldObject*→size_t, 1102 ms), ModelDrawerData (CUnit*/CFeature*→int, 544 ms), MoveMath blockMaps ×16 (182 ms) |
| 2 | `Map<int, struct>`                      | 1117 |  26 % | CobEngine (int→CCobThread, 727 ms), Shader (uint→UniformState, 252 ms), TimeProfiler (uint→TimeRecord, 110 ms), LosHandler (int→vector<SLosInstance*>, 26 ms) |
| 3 | `Map<ptr*, struct>`                     |  641 |  15 % | ModelDrawerData (CUnit*/CFeature*→ScopedTransformMemAlloc, 450 + 190 ms) |
| 4 | `Map<int, scalar>`                      |  521 |  12 % | FreeListMap (int→size_t, 264 ms), TimeProfiler refCounters (uint→int, 241 ms), SimObjectIDPool ×7 (11 ms), QTPFS PathManager (0.4 ms) |
| 5 | `Map<string, struct/ptr>`               |   94 | 2.2 % | ShaderHandler ProgramObjMap, LuaUnitDefs DataElement, S3OTextureHandler caches |
| 6 | `Map<func_ptr, *>` / `Map<*, func_ptr>` |   28 | 0.7 % | creg Lua state (SerializeLuaState) |
| 7 | `Map<string, scalar>`                   |   20 | 0.5 % | Def name→ID tables, CategoryHandler, DamageArrayHandler |
| 8 | `Set<int>`                              |    3 | 0.1 % | FeatureHandler, BuilderCaches |
| 9 | Other (misc <1 ms)                      |    1 |<0.1 % | — |

#### Pathfinding fightertest (total 22576 ms)

| # | Key → value shape                       |    ms |    %  | Dominant containers |
|--:|-----------------------------------------|------:|------:|---------------------|
| 1 | `Map<ptr*, scalar>`                     | 19064 |  84 % | **ModelsMemStorage (10183 ms, alone 45 % of the run)**, MoveMath blockMaps ×16 (4414 ms), ModelDrawerData CFeature*→int (2715 ms), ModelDrawerData CUnit*→int (1751 ms) |
| 2 | `Map<ptr*, struct>`                     |  1565 |   7 % | ModelDrawerData ScopedTransformMemAlloc (879 + 686 ms) |
| 3 | `Map<int, struct>`                      |  1057 |   5 % | CobEngine (647 ms), LosHandler (182 ms), Shader UniformState (140 ms), TimeProfiler TimeRecord (83 ms) |
| 4 | `Map<int, scalar>`                      |   720 |   3 % | QTPFS PathManager (PathHashType→int, 405 ms), TimeProfiler refCounters (160 ms), FreeListMap (120 ms), SimObjectIDPool ×7 (16 ms) |
| 5 | `Map<string, struct/ptr>`               |    73 | 0.3 % | LuaUnitDefs DataElement, ShaderHandler ProgramObjMap, S3OTextureHandler caches |
| 6 | `Map<string, scalar>`                   |    61 | 0.3 % | UnitDefHandler/FeatureDefHandler/WeaponDefHandler name→ID tables |
| 7 | `Map<func_ptr, *>` / `Map<*, func_ptr>` |    27 | 0.1 % | creg Lua state (SerializeLuaState) |
| 8 | `Set<int>`                              |     7 |<0.1 % | FeatureHandler, BuilderCaches, Player, SelectedUnits |
| 9 | Other (misc <1 ms)                      |     1 |<0.1 % | — |

#### Headlines

- **Pointer-keyed dominates in both runs, but pathfinding amplifies it hard:**
  `Map<ptr*, *>` is **58 %** of regular (2469 / 4255 ms) and jumps to **91 %**
  of pathfinding (20629 / 22576 ms). The pathfinding signature is "pointer
  hash path gets hammered."
- **Int-keyed maps stay roughly flat in absolute terms** (1638 → 1777 ms) but
  drop from **38 % to 8 %** as pointer-keyed traffic balloons around them.
- **String-keyed maps never cross 3 %** in either run — confirms again that
  the ~45 % of declarations using string keys are overwhelmingly load-time
  work, not runtime work.
- **QTPFS PathManager is scenario-gated** — 0.4 ms regular → 405 ms
  pathfinding, a 1000× increase — so it's visible only in scenario 2.
  Contrast with CobEngine which is high in both.
- **Reordering between runs is informative:** `Map<ptr*, struct>` overtakes
  `Map<int, struct>` in pathfinding (rank 3 → 2), because
  `ScopedTransformMemAlloc` insert churn scales with per-frame object count
  while `CCobThread`/`UniformState` traffic is more steady-state.

This reframes the optimization problem: **the real lever is the pointer-key
hash path**, especially under pathfinding load, not the string-interning /
lookup-table maps that dominate the static survey in §1.

### 2.4 Patterns behind the worst offenders

1. **`ModelDrawerData.h:100` is a single source line with four distinct
   instantiations** (`scTransMemAllocMap` and `lastSyncedFrameUpload` for both
   `CUnit` and `CFeature`). Together they account for **~1.2 s regular and
   ~6.0 s pathfinding** — roughly 30% of all hash-container time. Whatever
   fix lands at this line pays out 4×.

2. **Pointer-keyed maps dominate** — 7 of the top 10 offenders key on
   `CObject*` / `CWorldObject*` / `CUnit*` / `CFeature*`. Aligned heap pointers
   have zero low bits, so the default hash can cluster entries and blow up
   probe chains (peak probe 10-41 at these sites). Candidate fix: shift-mix
   the pointer, or route through `fast_hash` as `Shader.cpp:222` already does.

3. **Tombstone accumulation is the single biggest secondary cost.**
   `CobEngine.h:25` reaches 84% tombstones + 1656-probe spike; `QTPFS
   PathManager.cpp:171` reaches 43-49% tombstones; `MoveMath.cpp:403` has
   `insert == find-miss` (every miss creates a lingering entry). `SpringHashMap`
   rehashes on load factor but not on tombstone ratio — that is the common
   failure mode. Candidate fix: trigger rehash when
   `tombstones / buckets` exceeds a threshold, or clear these maps between
   frames where the use pattern allows.

4. **Pathfinding amplifies MoveMath blockMaps ~24×** (182 → 4414 ms). There are
   16 per-thread copies (`std::array<..., ThreadPool::MAX_THREADS>`); each one
   sees millions of ops during pathfinding. These are short-lived scratch
   tables — a flat small open-addressed table sized to the thread's live set
   might beat a general hash map here.

5. **The load-time maps flagged in §1.3 are confirmed cold at runtime.**
   `ArchiveScanner.cpp:395` (17236 entries, 0 find-hits post-load),
   `UnitToolTipMap` (20012 entries, 0 find-hits), `SimObjectIDPool`
   (insert-only). Safe to ignore for runtime tuning.

### 2.5 Priority targets

1. **`ModelsMemStorage.h:156`** — fix first. Biggest single lever, ~10× larger
   than anything else under pathfinding load.
2. **`ModelDrawerData.h:100` template** — one source fix covers 4 containers.
3. **Tombstone-triggered rehash** in `SpringHashMap` — unblocks `CobEngine`,
   `QTPFS PathManager`, `FreeListMap`, `MoveMath` blockMaps in one go.
4. **Pointer hash distribution** — verify `fast_hash` vs default for all
   pointer-keyed offenders (microbenchmark already in place).
5. **Re-run both fightertests** after each change for comparable numbers.

### 2.6 Codepath classification — sim frame vs render

**Date:** 2026-04-10

Each hot container classified by which codepath hits it at runtime:

- **SIM** — hit per tick inside `CGame::SimFrame` (deterministic, latency-critical)
- **SIM-SPAWN** — hit from sim only on object lifecycle (spawn/death), not per-tick
- **RENDER** — hit per draw frame from `CGame::UpdateUnsynced` / `CWorldDrawer::Update` / `CGame::Draw`
- **BOTH** — hit from both paths (profiler/Lua-def infrastructure)
- **LOAD** — populated at startup, cold at steady state
- **UI** — input/config/UI event path

#### Top 10 + just-outside with verdict

| # | Container | Source | ms (regular / pf) | Verdict | Call chain |
|--:|-----------|--------|------------------:|---------|------------|
|  1 | `map<CWorldObject*, size_t>` | `Rendering/Models/ModelsMemStorage.h:156` | 1102 / **10183** | **RENDER** | `UpdateUnsynced` → `CUnit/FeatureDrawerData::Update` (MT `for_mt_chunk`) → `UpdateObjectUniforms` (`ModelDrawerData.h:206`). Sim touches only via one-shot `Add/DelObject` on lifecycle events. |
|  2 | `map<CSolidObject*, BlockType>` ×16 | `Sim/MoveTypes/MoveMath/MoveMath.cpp:403` | 182 / **4414** | **SIM** | `SimFrame` → `unitHandler.Update()` / `pathManager->Update()` → `CMoveMath::RangeIsBlockedHashedMt` (`.cpp:410, 433`). Per-thread scratch tables. |
|  3 | `map<CFeature const*, int>` | `Rendering/Common/ModelDrawerData.h:100` | 224 / 2715 | **RENDER** | Same chain as #1 (feature side, `lastSyncedFrameUpload`). |
|  4 | `map<CUnit const*, int>` | `Rendering/Common/ModelDrawerData.h:100` | 321 / 1751 | **RENDER** | Same chain as #1 (unit side). |
|  5 | `map<int, CCobThread>` | `Sim/Units/Scripts/CobEngine.h:25` | **727** / 647 | **SIM** | `SimFrame` → `unitScriptEngine->Tick()` (`Game.cpp:1779`) → `CCobEngine::Tick` → `TickRunningThreads` → `GetThread` / `RemoveThread`. |
|  6 | `map<CUnit const*, ScopedTransformMemAlloc>` | `Rendering/Common/ModelDrawerData.h:100` | 450 / 879 | **RENDER** | Same chain as #1. Insert-bound. |
|  7 | `map<CFeature const*, ScopedTransformMemAlloc>` | `Rendering/Common/ModelDrawerData.h:100` | 190 / 686 | **RENDER** | Same chain as #1. Insert-bound. |
|  8 | `map<QTPFS::PathHashType, int>` | `Sim/Path/QTPFS/PathManager.cpp:171` | 0 / 405 | **SIM** | `SimFrame` → `pathManager->Update()` (`Game.cpp:1767`) → `QTPFS::PathManager::ExecuteQueuedSearches` (`.cpp:834`) → `pathCache.SharedPathMap/PartialSharedPathMap` (`PathManager.h:116-119`). Note: line 171 is the ctor; the hot containers are at the header. |
|  9 | `map<unsigned int, UniformState>` | `Rendering/Shaders/Shader.cpp:222` | 252 / 140 | **RENDER** | Per-shader-bind uniform lookup throughout `CGame::Draw`. Already uses `fast_hash`. |
| 10 | `map<unsigned int, int>` refCounters | `System/TimeProfiler.cpp:27` | 241 / 160 | **BOTH** | Every `SCOPED_TIMER` constructor/destructor, sim and render paths. |
|  — | `map<int, size_t>` | `System/FreeListMap.h:122` | 264 / 120 | **SIM** | Only hot instantiation: `spring::FreeListMapCompact<CProjectile*, int> projectiles[2]` at `Sim/Projectiles/ProjectileHandler.h:116`. `SimFrame` → `projectileHandler.Update()` (`Game.cpp:1768`). |
|  — | `map<int, vector<SLosInstance*>>` | `Sim/Misc/LosHandler.h:103` | 26 / 182 | **SIM** | `SimFrame` → `losHandler->Update()` (`Game.cpp:1785`) → `ILosType::Update/AddUnit/UnrefInstance`. Also from `CUnit::SlowUpdate` when units change LOS cell. Peak probe 49-64. |

#### Broader coverage

**SIM (per-tick)**

| Container | Source | Entry point |
|-----------|--------|-------------|
| `MoveMath` blockMaps ×16 | `Sim/MoveTypes/MoveMath/MoveMath.cpp:403` | `unitHandler.Update()` / `pathManager->Update()` → `RangeIsBlockedHashedMt` |
| `CobEngine` threadInstances / deferredCallins | `Sim/Units/Scripts/CobEngine.h:25, :118` | `unitScriptEngine->Tick()` (`Game.cpp:1779`) |
| `QTPFS` pathCache (SharedPathMap/PartialSharedPathMap) | `Sim/Path/QTPFS/PathManager.h:116-119` | `pathManager->Update()` → `ExecuteQueuedSearches` |
| `LosHandler` instanceHashes | `Sim/Misc/LosHandler.h:103` | `losHandler->Update()` (`Game.cpp:1785`) |
| `ProjectileHandler` FreeListMapCompact (synced) | `Sim/Projectiles/ProjectileHandler.h:116` | `projectileHandler.Update()` (`Game.cpp:1768`) |
| `FeatureHandler::activeFeatureIDs` | `Sim/Features/FeatureHandler.h:88` | `featureHandler.UpdatePreFrame()` (`Game.cpp:1749`) + `featureHandler.Update()` → `UpdateFeature` |
| `CommandAI` commandDeathDependences (`unsynced_set` — name is misleading, written from sim) | `Sim/Units/CommandAI/CommandAI.h:158` | `unitHandler.Update()` → `CUnit::SlowUpdate` → `Add/DeleteDeathDependence` |

**SIM-SPAWN (lifecycle only, not per-tick)**

| Container | Source | Notes |
|-----------|--------|-------|
| `SimObjectIDPool` poolIDs / freeIDs / tempIDs ×7 | `Sim/Misc/SimObjectIDPool.h:14` | Spawn/death only; instrumentation confirms 0 find-hits. |
| `ExplosionGenerator` aliases / expGenHashIdentMap | `Sim/Projectiles/ExplosionGenerator.h:45, :92` | First-time-per-tag load only; runtime `GenExplosion` uses integer id into a vector. |

**RENDER**

| Container | Source |
|-----------|--------|
| `ModelUniformsStorage::objectsMap` | `Rendering/Models/ModelsMemStorage.h:156` |
| `ModelDrawerData` 4× instantiations | `Rendering/Common/ModelDrawerData.h:100` |
| `Shader::UniformStates` | `Rendering/Shaders/Shader.cpp:222` |
| `IconHandler` iconsMap | `Rendering/IconHandler.h:73` |
| `NamedTextures` texInfoMap | `Rendering/Textures/NamedTextures.cpp:22` |

**BOTH (hit from sim and render)**

| Container | Source | Notes |
|-----------|--------|-------|
| `TimeProfiler` refCounters / profiles | `System/TimeProfiler.cpp:27, :123` | Every `SCOPED_TIMER` in sim and render. |
| `LuaUnitDefs/WeaponDefs/FeatureDefs` paramMap | `Lua/LuaUnitDefs.cpp:29`, `LuaWeaponDefs.cpp:27`, `LuaFeatureDefs.cpp:20` | Function-local static shared by synced gadgets (sim stack via `eventHandler.GameFrame`) and unsynced widgets (draw stack). Same container object hit from both. |
| `LuaHandleSynced` gameParams | `Lua/LuaHandleSynced.cpp:57` | Writes are sim (synced Lua `SetGameRulesParam`); reads happen from both paths. |

**LOAD (cold post-startup)**

`ArchiveScanner.cpp:395`, `IArchive.cpp:10`, `UnitDefHandler.h:18`, `FeatureDefHandler.h:18`, `WeaponDefHandler.h:17`, `DamageArrayHandler.h:14`, `DefinitionTag.cpp:20`, `CobFile/CobFileHandler` script tables, `S3OTextureHandler` string caches, `TextureAtlas` files/textures, `SerializeLuaState.cpp:68-69` (creg, save/load only), `CategoryHandler.h:13`.

**UI / input / config**

`KeyBindings.h:16`, `ConfigHandler.cpp:83`.

#### Sim-focused priority list

Narrowing §2.5 to containers that actually affect sim tick latency:

1. **`MoveMath.cpp:403` blockMaps** — #2 overall, pure sim, 24× pathfinding amplifier. 16 per-thread scratch tables with `insert == find-miss` accumulation and peak probe 41. Candidates: tombstone-triggered rehash, pointer-hash shift-mix, or a small flat table sized per thread's live set.
2. **`CobEngine.h:25` threadInstances** — 84% tombstones, peak probe **1656** under pathfinding. Tombstone-triggered rehash in `SpringHashMap` fixes this without per-site changes.
3. **`QTPFS` pathCache** — scenario-gated (0 → 405 ms), 43-49% tombstones. Same rehash fix as #2.
4. **`FreeListMap.h:122`** via `ProjectileHandler.h:116` — churn-heavy (2.1M inserts + 1.1M erases regular run). Same rehash fix class.
5. **`LosHandler.h:103`** — peak probe 49-64, integer-key distribution issue. Candidate: better int hash, or sorted flat structure given its iteration pattern.

**Shared lever:** tombstone-triggered rehash in `SpringHashMap` addresses items 2, 3, and 4 simultaneously (matching §2.5 item 3 in the overall list).

**Deliberately deprioritised for sim focus:** items #1, #3, #4, #6, #7, #9 from the top-10 are all RENDER path — they dominate absolute time but do not gate sim tick latency. Address after sim containers are handled (or in a parallel track).

---

# Outputs from instrumentation
## regular fightertest
```
[HashContainerStats] 63342 instrumented containers
type                                                                                       | source                                             |    size | buckets |  load |  tomb |     find-hit |    find-miss |  inserts | erases |  iters |   rh | maxP | probes
                                                                                                                                                                        1|2|3|4-7|8-15|16+  ░=1-10% ▒=10-30% ▓=30-55% █=55%+
synced map<CUnit const*, ScopedTransformMemAlloc>                                          | rts/Rendering/Common/ModelDrawerData.h:100         |    1303 |  524288 |  0.2% |  0.0% |         6669 |            0 |  2288662 |   6669 |      0 |    1 |    0 | █     
synced map<CFeature const*, ScopedTransformMemAlloc>                                       | rts/Rendering/Common/ModelDrawerData.h:100         |     307 |  524288 |  0.1% |  0.1% |         7151 |            0 |  1045894 |   7151 |      0 |    1 |    0 | █     
unsynced map<std::string, int (*)(lua_State*)>                                             | rts/System/creg/SerializeLuaState.cpp:68           |  115245 |  262144 | 44.0% |  0.0% |            0 |            0 |   115254 |      0 |      0 |   17 |   15 |  n/a 
synced map<unsigned int, unsigned int>                                                     | rts/Sim/Misc/SimObjectIDPool.h:14                  |   32000 |   65536 | 48.8% |  0.0% |            0 |            0 |    38542 |      0 |      0 |    1 |    0 |  n/a 
synced map<unsigned int, unsigned int>                                                     | rts/Sim/Misc/SimObjectIDPool.h:14                  |   24542 |   65536 | 37.4% | 11.4% |            0 |            0 |    32000 |   7458 |   7458 |    1 |    0 |  n/a 
synced map<unsigned int, unsigned int>                                                     | rts/Sim/Misc/SimObjectIDPool.h:14                  |    6542 |   65536 | 10.0% |  0.0% |            0 |            0 |     6542 |      0 |      0 |    1 |    0 |  n/a 
synced set<int>                                                                            | rts/Sim/Features/FeatureHandler.h:47               |     307 |   65536 |  0.5% | 10.9% |         7151 |            0 |     7458 |   7151 |   2029 |    1 |    0 | █     
synced map<std::string, CArchiveScanner::FileInfo>                                         | rts/System/FileSystem/ArchiveScanner.cpp:395       |   17236 |   65536 | 26.3% |  0.0% |            0 |            0 |    17236 |      0 |      3 |    1 |    0 |  n/a 
synced map<unsigned int, unsigned int>                                                     | rts/Sim/Misc/SimObjectIDPool.h:14                  |   32000 |   65536 | 48.8% |  0.0% |            0 |            0 |    38669 |      0 |      0 |    1 |    0 |  n/a 
synced map<unsigned int, unsigned int>                                                     | rts/Sim/Misc/SimObjectIDPool.h:14                  |   24028 |   65536 | 36.7% | 12.2% |            0 |            0 |    32000 |   7972 |   7972 |    1 |    0 |  n/a 
synced map<unsigned int, unsigned int>                                                     | rts/Sim/Misc/SimObjectIDPool.h:14                  |    6669 |   65536 | 10.2% |  0.0% |            0 |            0 |     6669 |      0 |      0 |    1 |    0 |  n/a 
synced map<std::string, unsigned int>                                                      | rts/System/FileSystem/Archives/IArchive.cpp:10     |   17945 |   32768 | 54.8% |  0.0% |        15373 |            0 |    17945 |      0 |      0 |   14 |   35 | █▒░░  
synced map<int, unsigned long>                                                             | rts/System/FreeListMap.h:122                       |    5804 |   16384 | 35.4% |  2.5% |       800241 |            0 |  1606037 | 800241 |      0 |   13 |    0 | █     
synced map<int, std::string>                                                               | rts/Sim/Units/UnitToolTipMap.hpp:8                 |    7972 |   16384 | 48.7% |  0.0% |            0 |            0 |    10453 |      0 |      0 |    6 |    9 |  n/a 
synced map<int, CCobThread>                                                                | rts/Sim/Units/Scripts/CobEngine.h:25               |    4468 |    8192 | 54.5% | 45.5% |      4719659 |       229684 |   671153 | 666685 |      0 |    2 |  506 | ▒░░░░▓
synced map<std::string, unsigned long>                                                     | rts/Rendering/IconHandler.h:73                     |    1964 |    4096 | 47.9% |  0.0% |        13926 |         1964 |     1964 |      0 |      1 |   11 |   10 | █░    
synced map<CWorldObject*, unsigned long>                                                   | rts/Rendering/Models/ModelsMemStorage.h:156        |    1611 |    4096 | 39.3% | 60.7% |     25909683 |            0 |    15431 |  13820 |      0 |   11 |   40 | ░░░▒▒▓
synced map<int, std::vector<SLosInstance*, std::allocator<SLosInstance*> >>                | rts/Sim/Misc/LosHandler.h:103                      |    2240 |    4096 | 54.7% |  0.0% |       121184 |         2240 |     2325 |      0 |      0 |   11 |   41 | █▒░░░ 
synced map<std::string, int>                                                               | rts/Sim/Misc/DamageArrayHandler.h:14               |    1427 |    4096 | 34.8% |  0.0% |         4079 |         1552 |     1427 |      0 |      0 |   11 |   11 | █░    
synced map<std::string, int>                                                               | rts/Sim/Features/FeatureDefHandler.h:18            |    1010 |    2048 | 49.3% |  0.0% |        10065 |         1012 |     1010 |      0 |      1 |    1 |    8 | █░    
synced map<std::string, int>                                                               | rts/Sim/Weapons/WeaponDefHandler.h:17              |     784 |    2048 | 38.3% |  0.0% |         2278 |           35 |      784 |      0 |      0 |    1 |    9 | █▒░░  
synced map<CUnit const*, int>                                                              | rts/Rendering/Common/ModelDrawerData.h:100         |    1303 |    2048 | 63.6% |  5.1% |      5619558 |            0 |     7972 |   6669 |      0 |   10 |   10 | ▒▒▒▓▓ 
synced map<int, unsigned long>                                                             | rts/System/FreeListMap.h:122                       |     444 |    2048 | 21.7% | 78.3% |       424774 |            0 |   565852 | 282905 |      0 |   10 |   32 | █▒░░  
unsynced map<int (*)(lua_State*), std::string>                                             | rts/System/creg/SerializeLuaState.cpp:69           |     871 |    2048 | 42.5% |  0.0% |       114383 |          871 |   116131 |      0 |      0 |   10 |   14 | ▒▒░▓▒ 
synced map<CFeature const*, int>                                                           | rts/Rendering/Common/ModelDrawerData.h:100         |     307 |    2048 | 15.0% | 33.4% |      4946407 |            0 |     7458 |   7151 |      0 |   10 |   42 |  ░ ░▒█
synced map<std::string, LuaRulesParams::Param>                                             | rts/Lua/LuaHandleSynced.cpp:57                     |     650 |    1024 | 63.5% |  0.0% |        46534 |          115 |      651 |      1 |      0 |    9 |   23 |  █    
synced map<int, std::vector<SLosInstance*, std::allocator<SLosInstance*> >>                | rts/Sim/Misc/LosHandler.h:103                      |     605 |    1024 | 59.1% |  0.0% |        69930 |          605 |     5041 |      0 |      0 |    9 |   16 | █▒░░  
synced map<std::string, int>                                                               | rts/Sim/Units/UnitDefHandler.h:18                  |     581 |    1024 | 56.7% |  0.0% |       204473 |            0 |      581 |      0 |      1 |    1 |    9 | █▒░░░ 
synced map<unsigned int, unsigned int>                                                     | rts/Sim/Projectiles/ExplosionGenerator.h:45        |     355 |    1024 | 34.7% |  0.0% |       147154 |          356 |      355 |      0 |      0 |    9 |   17 | ▒▓▒   
synced map<std::string, AtlasedTexture>                                                    | rts/Rendering/Textures/TextureAtlas.cpp:28         |       0 |     512 |  0.0% |  0.0% |            0 |         4125 |        0 |      0 |      0 |    1 |    0 |  n/a 
synced map<std::string, unsigned long>                                                     | rts/Sim/Units/Scripts/CobFileHandler.h:11          |       3 |     512 |  0.6% |  0.0% |         7968 |            3 |        3 |      0 |      0 |    1 |    0 | █     
synced map<std::string, DefTagMetaData const*>                                             | rts/Sim/Misc/DefinitionTag.cpp:20                  |     182 |     512 | 35.5% |  0.0% |       138335 |         2945 |      182 |      0 |      0 |    8 |    3 | █▒░░  
synced map<std::string, DataElement>                                                       | rts/Lua/LuaUnitDefs.cpp:29                         |     255 |     512 | 49.8% |  0.0% |       750530 |        17643 |     1275 |      0 |      0 |   40 |    9 | █░░   
unsynced map<std::string, std::vector<std::string, std::allocator<std::string > >>         | rts/Game/UI/KeyBindings.h:16                       |     272 |     512 | 53.1% |  0.0% |           20 |            0 |      989 |      0 |      0 |    4 |   14 | █▒░   
synced map<std::string, unsigned long>                                                     | rts/Rendering/Textures/NamedTextures.cpp:22        |       1 |     256 |  0.4% |  0.4% |       113115 |           23 |        4 |      1 |      0 |    1 |    0 | █     
synced map<std::string, DataElement>                                                       | rts/Lua/LuaWeaponDefs.cpp:27                       |     131 |     256 | 51.2% |  0.0% |       151400 |           32 |      655 |      0 |      0 |   35 |    7 | █▒    
synced map<int, int>                                                                       | rts/System/UnorderedMap.hpp:75                     |     131 |     256 | 51.2% |  0.0% |         3549 |          131 |      131 |      0 |      0 |    1 |   79 | █     
synced map<unsigned int, CTimeProfiler::TimeRecord>                                        | rts/System/TimeProfiler.cpp:123                    |      11 |     256 |  4.3% |  0.0% |      5668407 |           11 |      798 |      0 |    158 |    1 |    0 | █     
synced map<CSolidObject*, Bitwise::BitwiseEnum<CMoveMath::BlockTypes>>                     | rts/Sim/MoveTypes/MoveMath/MoveMath.cpp:403        |       1 |     128 |  0.8% |  0.0% |       165388 |        94637 |    94637 |      0 |      0 |    4 |   15 | ▓▒▒▒░ 
synced map<CSolidObject*, Bitwise::BitwiseEnum<CMoveMath::BlockTypes>>                     | rts/Sim/MoveTypes/MoveMath/MoveMath.cpp:403        |       2 |     128 |  1.6% |  0.0% |       155629 |       109496 |   109496 |      0 |      0 |    4 |   13 | █▒▒▒░ 
unsynced map<std::string, std::vector<ConfigHandlerImpl::NamedConfigNotifyCallback, std::allocator<ConfigHandlerImpl::NamedConf | rts/System/Config/ConfigHandler.cpp:83             |      72 |     128 | 56.2% |  0.0% |            1 |         3776 |       98 |      0 |      0 |    6 |    8 | █     
synced map<CSolidObject*, Bitwise::BitwiseEnum<CMoveMath::BlockTypes>>                     | rts/Sim/MoveTypes/MoveMath/MoveMath.cpp:403        |       2 |     128 |  1.6% |  0.0% |       499669 |       139668 |   139668 |      0 |      0 |    4 |   15 | ▓▒▒▒░ 
synced map<CSolidObject*, Bitwise::BitwiseEnum<CMoveMath::BlockTypes>>                     | rts/Sim/MoveTypes/MoveMath/MoveMath.cpp:403        |       1 |     128 |  0.8% |  0.0% |       551316 |       148283 |   148283 |      0 |      0 |    4 |   15 | ▓▒▒▒░ 
synced map<CSolidObject*, Bitwise::BitwiseEnum<CMoveMath::BlockTypes>>                     | rts/Sim/MoveTypes/MoveMath/MoveMath.cpp:403        |       2 |     128 |  1.6% |  0.0% |       277672 |       101112 |   101112 |      0 |      0 |    4 |   13 | ▓▒▒▒░ 
synced map<CSolidObject*, Bitwise::BitwiseEnum<CMoveMath::BlockTypes>>                     | rts/Sim/MoveTypes/MoveMath/MoveMath.cpp:403        |       2 |     128 |  1.6% |  0.0% |       146028 |        90279 |    90279 |      0 |      0 |    4 |   12 | ▓▒▒▒░ 
synced map<CSolidObject*, Bitwise::BitwiseEnum<CMoveMath::BlockTypes>>                     | rts/Sim/MoveTypes/MoveMath/MoveMath.cpp:403        |       1 |     128 |  0.8% |  0.0% |       144809 |        90286 |    90286 |      0 |      0 |    4 |   12 | ▓▒▒▒░ 
synced map<std::string, DataElement>                                                       | rts/Lua/LuaFeatureDefs.cpp:20                      |      45 |     128 | 35.2% |  0.0% |        62535 |         1396 |      225 |      0 |      0 |   30 |    4 | █░    
synced map<CSolidObject*, Bitwise::BitwiseEnum<CMoveMath::BlockTypes>>                     | rts/Sim/MoveTypes/MoveMath/MoveMath.cpp:403        |       1 |     128 |  0.8% |  0.0% |       139717 |        94931 |    94931 |      0 |      0 |    4 |   13 | ▓▒▒▒░ 
synced map<CSolidObject*, Bitwise::BitwiseEnum<CMoveMath::BlockTypes>>                     | rts/Sim/MoveTypes/MoveMath/MoveMath.cpp:403        |       2 |     128 |  1.6% |  0.0% |       205658 |        99827 |    99827 |      0 |      0 |    4 |   13 | ▓▒▒▒  
synced map<CSolidObject*, Bitwise::BitwiseEnum<CMoveMath::BlockTypes>>                     | rts/Sim/MoveTypes/MoveMath/MoveMath.cpp:403        |       1 |     128 |  0.8% |  0.0% |       165694 |       103519 |   103519 |      0 |      0 |    4 |   12 | █▒▒▒░ 
synced map<CSolidObject*, Bitwise::BitwiseEnum<CMoveMath::BlockTypes>>                     | rts/Sim/MoveTypes/MoveMath/MoveMath.cpp:403        |       1 |     128 |  0.8% |  0.0% |       199673 |       106578 |   106578 |      0 |      0 |    4 |   13 | █▒▒▒  
synced map<CSolidObject*, Bitwise::BitwiseEnum<CMoveMath::BlockTypes>>                     | rts/Sim/MoveTypes/MoveMath/MoveMath.cpp:403        |       2 |     128 |  1.6% |  0.0% |       193702 |       118515 |   118515 |      0 |      0 |    4 |   12 | █▒▒▒░ 
synced map<CSolidObject*, Bitwise::BitwiseEnum<CMoveMath::BlockTypes>>                     | rts/Sim/MoveTypes/MoveMath/MoveMath.cpp:403        |       2 |     128 |  1.6% |  0.0% |       219988 |       125050 |   125050 |      0 |      0 |    4 |   12 | █▒▒▒  
synced map<CSolidObject*, Bitwise::BitwiseEnum<CMoveMath::BlockTypes>>                     | rts/Sim/MoveTypes/MoveMath/MoveMath.cpp:403        |       3 |     128 |  2.3% |  0.0% |       324336 |       161615 |   161615 |      0 |      0 |    4 |   14 | ▓▒▒▒░ 
synced map<CSolidObject*, Bitwise::BitwiseEnum<CMoveMath::BlockTypes>>                     | rts/Sim/MoveTypes/MoveMath/MoveMath.cpp:403        |       2 |     128 |  1.6% |  0.0% |       411026 |       201606 |   201606 |      0 |      0 |    4 |   14 | ▓▒▒▒░ 
synced map<CSolidObject*, Bitwise::BitwiseEnum<CMoveMath::BlockTypes>>                     | rts/Sim/MoveTypes/MoveMath/MoveMath.cpp:403        |       2 |     128 |  1.6% |  0.0% |       517084 |       193800 |   193800 |      0 |      0 |    4 |   13 | ▓▒▒▒  
synced map<unsigned int, int>                                                              | rts/System/TimeProfiler.cpp:27                     |      77 |     128 | 60.2% |  0.0% |     12295318 |           77 |       77 |      0 |      0 |    6 |    8 | █░    
unsynced map<unsigned int, Shader::UniformState>                                           | rts/Rendering/Shaders/Shader.cpp:222               |      71 |     128 | 55.5% |  0.0% |     12816222 |           71 |       71 |      0 |      0 |    6 |   11 | █░    
unsynced map<std::string, CBitmap>                                                         | rts/Rendering/Textures/S3OTextureHandler.h:16      |       2 |     128 |  1.6% | 91.4% |         4514 |         2238 |     2238 |   2236 |      0 |    6 |    7 | █▒░░░ 
unsynced map<std::string, CS3OTextureHandler::CachedS3OTex>                                | rts/Rendering/Textures/S3OTextureHandler.h:16      |      81 |     128 | 63.3% |  0.0% |         4435 |           81 |     4518 |      0 |      0 |    6 |    7 | █▒░░░ 
synced map<std::string, unsigned int>                                                      | rts/Sim/Misc/CategoryHandler.h:13                  |      23 |      64 | 35.9% |  0.0% |         6193 |           23 |     6216 |      0 | 181517 |    1 |    1 | █░    
synced map<std::string, DefTagMetaData const*>                                             | rts/Sim/Misc/DefinitionTag.cpp:20                  |      40 |      64 | 62.5% |  0.0% |         3672 |        21210 |       40 |      0 |      0 |    5 |    5 | █░    
synced map<std::string, DefTagMetaData const*>                                             | rts/Sim/Misc/DefinitionTag.cpp:20                  |      36 |      64 | 56.2% |  0.0% |         2732 |           73 |       36 |      0 |      0 |    5 |    5 | ▓█ ░  
synced map<std::string, std::string>                                                       | rts/Sim/Projectiles/ExplosionGenerator.h:29        |      31 |      64 | 48.4% |  0.0% |            0 |         3276 |       31 |      0 |      0 |    5 |    1 |  n/a 
synced map<std::string, int>                                                               | rts/Sim/Units/Scripts/CobFile.h:20                 |      22 |      64 | 34.4% |  0.0% |            1 |         3930 |        0 |      0 |      0 |    0 |    2 | █     
synced map<std::string, int>                                                               | rts/Sim/Units/Scripts/CobFile.h:20                 |      20 |      32 | 62.5% |  0.0% |            1 |         4040 |        0 |      0 |      0 |    0 |    8 |     █ 
unsynced map<std::string, bool>                                                            | rts/Rendering/Shaders/ShaderStates.h:228           |      18 |      32 | 56.2% |  0.0% |        27658 |           18 |    27676 |      0 |      0 |    4 |    1 | ▓█    
synced map<std::string, LuaRulesParams::Param>                                             | rts/Sim/Misc/Team.cpp:64                           |      17 |      32 | 53.1% |  0.0% |         6218 |            1 |      289 |      0 |      2 |    4 |    1 | █     
synced map<int, int>                                                                       | rts/Game/Game.h:43                                 |       6 |      16 | 37.5% |  0.0% |         2018 |            2 |        2 |      0 |      0 |    1 |    0 | █     
synced map<QTPFS::PathHashType, int>                                                       | rts/Sim/Path/QTPFS/PathManager.cpp:171             |       2 |      16 | 12.5% | 37.5% |           65 |        11230 |       28 |     18 |      0 |    3 |    4 | █░░▒  
unsynced map<std::string, emilib::HashMap<std::string, Shader::IProgramObject*, std::hash<std::string >, emilib::HashMapEqualTo | rts/Rendering/Shaders/ShaderHandler.h:16           |       9 |      16 | 56.2% |  0.0% |       339144 |            1 |  1017445 |      1 |      0 |    3 |    3 | █     
unsynced map<std::string, Shader::IProgramObject*>                                         | rts/System/SpringHashMap.hpp:567                   |       4 |       8 | 50.0% |  0.0% |       339139 |            3 |   339143 |      0 |      0 |    2 |    1 | █▒    
synced map<int, CGame::PlayerTrafficInfo>                                                  | rts/Game/Game.cpp:225                              |       3 |       8 | 37.5% |  0.0% |         2244 |            4 |        3 |      0 |      0 |    2 |    2 | █     
synced map<unsigned char, GameParticipant::ClientLinkData>                                 | rts/Net/GameParticipant.cpp:10                     |       1 |       8 | 12.5% | 12.5% |        81855 |            0 |    79453 |      1 |  74113 |    2 |    0 | █     
unsynced map<unsigned long, unsigned int>                                                  | rts/Rendering/Textures/S3OTextureHandler.h:16      |       1 |       4 | 25.0% |  0.0% |         1117 |            1 |        1 |      0 |      0 |    1 |    0 | █     
synced set<int>                                                                            | rts/Game/SelectedUnitsHandler.h:20                 |       0 |       4 |  0.0% |  0.0% |         7101 |        76846 |        1 |      0 |  39073 |    1 |    0 | █     
synced map<int, unsigned int>                                                              | rts/Net/GameParticipant.cpp:10                     |       0 |       4 |  0.0% | 100.0% |         6057 |        96126 |     2019 |   2019 |      0 |    1 |    0 | █     
unsynced map<int, int>                                                                     | rts/Game/UI/Groups/GroupHandler.cpp:37             |       0 |       0 |  0.0% |  0.0% |            0 |        18358 |        0 |   7214 |      0 |    0 |    0 |  n/a 
synced map<unsigned int, QTPFS::PathSearchTrace::Execution*>                               | rts/Sim/Path/QTPFS/PathManager.cpp:171             |       0 |       0 |  0.0% |  0.0% |            0 |         9778 |        0 |      0 |      0 |    0 |    0 |  n/a 
unsynced map<int, int>                                                                     | rts/Game/UI/Groups/GroupHandler.cpp:37             |       0 |       0 |  0.0% |  0.0% |            0 |        18891 |        0 |   7425 |      0 |    0 |    0 |  n/a 
synced map<int, std::vector<int, std::allocator<int> >>                                    | rts/Sim/Misc/GeometricObjects.h:27                 |       0 |       0 |  0.0% |  0.0% |            0 |         2020 |        0 |      0 |      0 |    0 |    0 |  n/a 
[HashContainerStats] 81 active, 63261 skipped (<1000 ops)

[HashContainerStats] Top 20 containers by total time (ns/op):
  type                                                                             | source                                                  |  total-ms |   find-hit |  find-miss |     insert |      erase |     rehash
  synced map<CWorldObject*, unsigned long>                                         | rts/Rendering/Models/ModelsMemStorage.h:156             |  1101.87ms |         42 |          - |        122 |          0 |       2401
  synced map<int, CCobThread>                                                      | rts/Sim/Units/Scripts/CobEngine.h:25                    |   726.93ms |         84 |        486 |        320 |          0 |      87069
  synced map<CUnit const*, ScopedTransformMemAlloc>                                | rts/Rendering/Common/ModelDrawerData.h:100              |   450.25ms |        142 |          - |        192 |       1147 |     122221
  synced map<CUnit const*, int>                                                    | rts/Rendering/Common/ModelDrawerData.h:100              |   320.48ms |         56 |          - |         91 |         95 |       1114
  unsynced map<unsigned int, Shader::UniformState>                                 | rts/Rendering/Shaders/Shader.cpp:222                    |   251.50ms |         19 |         60 |        216 |          - |       1604
  synced map<unsigned int, int>                                                    | rts/System/TimeProfiler.cpp:27                          |   240.56ms |         19 |         28 |         83 |          - |        327
  synced map<CFeature const*, int>                                                 | rts/Rendering/Common/ModelDrawerData.h:100              |   223.53ms |         44 |          - |        126 |        105 |       1637
  synced map<int, unsigned long>                                                   | rts/System/FreeListMap.h:122                            |   196.83ms |        175 |          - |         34 |          0 |       1799
  synced map<CFeature const*, ScopedTransformMemAlloc>                             | rts/Rendering/Common/ModelDrawerData.h:100              |   190.48ms |        174 |          - |        175 |        814 |      30478
  synced map<unsigned int, CTimeProfiler::TimeRecord>                              | rts/System/TimeProfiler.cpp:123                         |   110.37ms |         19 |         81 |         77 |          - |       2444
  synced map<int, unsigned long>                                                   | rts/System/FreeListMap.h:122                            |    66.99ms |         60 |          - |         72 |          0 |       1351
  unsynced map<std::string, emilib::HashMap<std::string, Shader::IProgramObject*, std::hash<std::string >, emilib::HashMapEqualTo | rts/Rendering/Shaders/ShaderHandler.h:16                |    35.00ms |         30 |        280 |         24 |        260 |       1777
  unsynced map<std::string, int (*)(lua_State*)>                                   | rts/System/creg/SerializeLuaState.cpp:68                |    23.62ms |          - |          - |        131 |          - |     499487
  synced map<CSolidObject*, Bitwise::BitwiseEnum<CMoveMath::BlockTypes>>           | rts/Sim/MoveTypes/MoveMath/MoveMath.cpp:403             |    21.86ms |         23 |         16 |         33 |          - |        851
  synced map<CSolidObject*, Bitwise::BitwiseEnum<CMoveMath::BlockTypes>>           | rts/Sim/MoveTypes/MoveMath/MoveMath.cpp:403             |    20.10ms |         24 |         18 |         27 |          - |        954
  unsynced map<std::string, Shader::IProgramObject*>                               | rts/System/SpringHashMap.hpp:567                        |    18.67ms |         29 |         50 |         25 |          - |        495
  synced map<std::string, DataElement>                                             | rts/Lua/LuaUnitDefs.cpp:29                              |    18.24ms |         23 |         21 |        126 |          - |       1937
  synced map<int, std::vector<SLosInstance*, std::allocator<SLosInstance*> >>      | rts/Sim/Misc/LosHandler.h:103                           |    18.00ms |        144 |         98 |         81 |          - |       4597
  synced map<CSolidObject*, Bitwise::BitwiseEnum<CMoveMath::BlockTypes>>           | rts/Sim/MoveTypes/MoveMath/MoveMath.cpp:403             |    17.44ms |         23 |         13 |         25 |          - |        827
  synced map<CSolidObject*, Bitwise::BitwiseEnum<CMoveMath::BlockTypes>>           | rts/Sim/MoveTypes/MoveMath/MoveMath.cpp:403             |    16.51ms |         21 |         16 |         26 |          - |        796

[HashContainerStats] Aggregated by type (42 types):
  type                                                                                       | count |     size |     find-hit |    find-miss |    inserts |   erases |  iters |    find-ms |  insert-ms |   total-ms
  synced map<CWorldObject*, unsigned long>                                                   |     1 |     1611 |     25909683 |            0 |      15431 |    13820 |      0 |   1100.0ms |      1.9ms |   1101.9ms
  unsynced map<unsigned int, Shader::UniformState>                                           |     1 |       71 |     12816222 |           71 |         71 |        0 |      0 |    251.5ms |      0.0ms |    251.5ms
  synced map<unsigned int, int>                                                              |     1 |       77 |     12295318 |           77 |         77 |        0 |      0 |    240.5ms |      0.0ms |    240.6ms
  synced map<CSolidObject*, Bitwise::BitwiseEnum<CMoveMath::BlockTypes>>                     |    16 |       27 |      4317389 |      1979202 |    1979202 |        0 |      0 |    126.3ms |     56.2ms |    182.5ms
  synced map<int, CCobThread>                                                                |     1 |     4468 |      4719659 |       229684 |     671153 |   666685 |      0 |    511.7ms |    215.1ms |    726.9ms
  synced map<unsigned int, CTimeProfiler::TimeRecord>                                        |     1 |       11 |      5668407 |           11 |        798 |        0 |    158 |    110.3ms |      0.1ms |    110.4ms
  synced map<CUnit const*, int>                                                              |     1 |     1303 |      5619558 |            0 |       7972 |     6669 |      0 |    319.1ms |      0.7ms |    320.5ms
  synced map<CFeature const*, int>                                                           |     1 |      307 |      4946407 |            0 |       7458 |     7151 |      0 |    221.8ms |      0.9ms |    223.5ms
  synced map<int, unsigned long>                                                             |     2 |     6248 |      1225015 |            0 |    2171889 |  1083146 |      0 |    166.3ms |     97.4ms |    263.8ms
  synced map<CUnit const*, ScopedTransformMemAlloc>                                          |     1 |     1303 |         6669 |            0 |    2288662 |     6669 |      0 |      1.0ms |    441.5ms |    450.2ms
  unsynced map<std::string, emilib::HashMap<std::string, Shader::IProgramObject*, std::hash<std::string >, emilib::HashMapEqualTo |     1 |        9 |       339144 |            1 |    1017445 |        1 |      0 |     10.2ms |     24.8ms |     35.0ms
  synced map<CFeature const*, ScopedTransformMemAlloc>                                       |     1 |      307 |         7151 |            0 |    1045894 |     7151 |      0 |      1.2ms |    183.4ms |    190.5ms
  synced map<std::string, DataElement>                                                       |     3 |      431 |       964465 |        19071 |       2155 |        0 |      0 |     22.6ms |      0.2ms |     22.9ms
  unsynced map<std::string, Shader::IProgramObject*>                                         |     1 |        4 |       339139 |            3 |     339143 |        0 |      0 |     10.2ms |      8.5ms |     18.7ms
  synced map<unsigned int, unsigned int>                                                     |     7 |   126136 |       147154 |          356 |     154777 |    15430 |  15430 |      3.8ms |      7.6ms |     11.4ms
  synced map<unsigned char, GameParticipant::ClientLinkData>                                 |     1 |        1 |        81857 |            0 |      79455 |        1 |  74115 |      1.6ms |      1.9ms |      3.5ms
  synced map<std::string, int>                                                               |     6 |     3844 |       220897 |        10569 |       3802 |        0 |      2 |      6.4ms |      0.2ms |      6.6ms
  unsynced map<int (*)(lua_State*), std::string>                                             |     1 |      871 |       114383 |          871 |     116131 |        0 |      0 |      2.4ms |      2.5ms |      4.8ms
  synced map<std::string, unsigned int>                                                      |     2 |    17968 |        21566 |           23 |      24161 |        0 | 181517 |      2.7ms |      1.4ms |      4.7ms
  synced map<int, std::vector<SLosInstance*, std::allocator<SLosInstance*> >>                |     2 |     2845 |       191114 |         2845 |       7366 |        0 |      0 |     25.6ms |      0.7ms |     26.3ms
  synced map<std::string, DefTagMetaData const*>                                             |     3 |      258 |       144739 |        24228 |        258 |        0 |      0 |      3.8ms |      0.0ms |      3.9ms
  synced set<int>                                                                            |     2 |      307 |        14252 |        76846 |       7459 |     7151 |  41102 |      0.8ms |      0.9ms |      2.7ms
  synced map<std::string, unsigned long>                                                     |     3 |     1968 |       135009 |         1990 |       1971 |        1 |      1 |      5.0ms |      0.1ms |      5.2ms
  unsynced map<std::string, int (*)(lua_State*)>                                             |     1 |   115245 |            0 |            0 |     115254 |        0 |      0 |      0.0ms |     15.1ms |     23.6ms
  synced map<int, unsigned int>                                                              |     1 |        0 |         6057 |        96134 |       2019 |     2019 |      0 |      0.2ms |      0.4ms |      0.7ms
  unsynced map<std::string, bool>                                                            |     1 |       18 |        27658 |           18 |      27676 |        0 |      0 |      2.7ms |      1.3ms |      3.9ms
  synced map<std::string, LuaRulesParams::Param>                                             |     2 |      667 |        52752 |          116 |        940 |        1 |      2 |      2.8ms |      0.1ms |      2.9ms
  unsynced map<int, int>                                                                     |     2 |        0 |            0 |        37249 |          0 |    14639 |      0 |      0.0ms |      0.0ms |      0.5ms
  synced map<std::string, CArchiveScanner::FileInfo>                                         |     1 |    17236 |            0 |            0 |      17236 |        0 |      3 |      0.0ms |      1.4ms |      1.5ms
  synced map<QTPFS::PathHashType, int>                                                       |     1 |        2 |           65 |        11230 |         28 |       18 |      0 |      0.4ms |      0.0ms |      0.4ms
  unsynced map<std::string, CBitmap>                                                         |     1 |        2 |         4514 |         2238 |       2238 |     2236 |      0 |      0.3ms |      0.1ms |      7.8ms
  synced map<int, std::string>                                                               |     1 |     7972 |            0 |            0 |      10453 |        0 |      0 |      0.0ms |      2.4ms |      2.5ms
  synced map<unsigned int, QTPFS::PathSearchTrace::Execution*>                               |     1 |        0 |            0 |         9778 |          0 |        0 |      0 |      0.0ms |      0.0ms |      0.0ms
  unsynced map<std::string, CS3OTextureHandler::CachedS3OTex>                                |     1 |       81 |         4435 |           81 |       4518 |        0 |      0 |      0.5ms |      0.2ms |      0.7ms
  synced map<int, int>                                                                       |     2 |      137 |         5567 |          133 |        133 |        0 |      0 |      0.6ms |      0.0ms |      0.6ms
  synced map<std::string, AtlasedTexture>                                                    |     1 |        0 |            0 |         4125 |          0 |        0 |      0 |      0.0ms |      0.0ms |      0.0ms
  unsynced map<std::string, std::vector<ConfigHandlerImpl::NamedConfigNotifyCallback, std::allocator<ConfigHandlerImpl::NamedConf |     1 |       72 |            1 |         3776 |         98 |        0 |      0 |      0.6ms |      0.0ms |      0.6ms
  synced map<std::string, std::string>                                                       |     1 |       31 |            0 |         3276 |         31 |        0 |      0 |      0.1ms |      0.0ms |      0.1ms
  synced map<int, CGame::PlayerTrafficInfo>                                                  |     1 |        3 |         2244 |            4 |          3 |        0 |      0 |      0.7ms |      0.0ms |      0.7ms
  synced map<int, std::vector<int, std::allocator<int> >>                                    |     1 |        0 |            0 |         2020 |          0 |        0 |      0 |      0.0ms |      0.0ms |      0.0ms
  unsynced map<unsigned long, unsigned int>                                                  |     1 |        1 |         1117 |            1 |          1 |        0 |      0 |      0.0ms |      0.0ms |      0.0ms
  unsynced map<std::string, std::vector<std::string, std::allocator<std::string > >>         |     1 |      272 |           20 |            0 |        989 |        0 |      0 |      0.0ms |      0.1ms |      0.1ms
[HashContainerStats] Aggregate timing:
  find_hit:  80349628 ops, 3012.279 ms total (37 ns/op avg)
  find_miss: 2516031 ops, 141.279 ms total (56 ns/op avg)
  insert:    10124348 ops, 1067.133 ms total (105 ns/op avg)
  erase:     1832788 ops, 23.948 ms total (13 ns/op avg)
  rehash:    432 ops, 10.283 ms total (23804 ns/op avg)
  iterate:   312331 begin() calls
```

## pathfinding fightertest
```
[HashContainerStats] 144749 instrumented containers
type                                                                                       | source                                             |    size | buckets |  load |  tomb |     find-hit |    find-miss |  inserts | erases |  iters |   rh | maxP | probes
                                                                                                                                                                        1|2|3|4-7|8-15|16+  ░=1-10% ▒=10-30% ▓=30-55% █=55%+
synced map<CUnit const*, ScopedTransformMemAlloc>                                          | rts/Rendering/Common/ModelDrawerData.h:100         |      58 |  524288 |  0.0% |  3.3% |        19954 |            0 |  2546162 |  19954 |      0 |    1 |    0 | █     
synced map<CFeature const*, ScopedTransformMemAlloc>                                       | rts/Rendering/Common/ModelDrawerData.h:100         |    7205 |  524288 |  1.4% |  0.6% |         6330 |            0 |  2435999 |   6330 |      0 |    1 |    1 | █▒    
unsynced map<std::string, int (*)(lua_State*)>                                             | rts/System/creg/SerializeLuaState.cpp:68           |  116325 |  262144 | 44.4% |  0.0% |            0 |            0 |   116334 |      0 |      0 |   17 |   15 |  n/a 
synced map<unsigned int, unsigned int>                                                     | rts/Sim/Misc/SimObjectIDPool.h:14                  |   11988 |   65536 | 18.3% | 30.5% |            0 |            0 |    32000 |  20012 |  20012 |    1 |    0 |  n/a 
synced map<int, std::vector<SLosInstance*, std::allocator<SLosInstance*> >>                | rts/Sim/Misc/LosHandler.h:103                      |   36560 |   65536 | 55.8% |  0.0% |       474170 |        36560 |    88796 |      0 |      0 |   15 |   64 | █▒░░░ 
synced map<unsigned int, unsigned int>                                                     | rts/Sim/Misc/SimObjectIDPool.h:14                  |    6000 |   65536 |  9.2% |  0.0% |            0 |            0 |     6000 |      0 |      0 |    1 |    0 |  n/a 
synced map<unsigned int, unsigned int>                                                     | rts/Sim/Misc/SimObjectIDPool.h:14                  |   32000 |   65536 | 48.8% |  0.0% |            0 |            0 |    38000 |      0 |      0 |    1 |    0 |  n/a 
synced map<unsigned int, unsigned int>                                                     | rts/Sim/Misc/SimObjectIDPool.h:14                  |   18452 |   65536 | 28.2% | 20.7% |            0 |            0 |    32000 |  13548 |  13548 |    1 |    0 |  n/a 
synced map<unsigned int, unsigned int>                                                     | rts/Sim/Misc/SimObjectIDPool.h:14                  |   19954 |   65536 | 30.4% |  0.0% |            0 |            0 |    19954 |      0 |      0 |    1 |    0 |  n/a 
synced set<int>                                                                            | rts/Sim/Features/FeatureHandler.h:47               |    7218 |   65536 | 11.0% |  9.7% |         6330 |            0 |    13548 |   6330 |   2029 |    1 |    0 | █     
synced map<unsigned int, unsigned int>                                                     | rts/Sim/Misc/SimObjectIDPool.h:14                  |   32000 |   65536 | 48.8% |  0.0% |            0 |            0 |    51954 |      0 |      0 |    1 |    0 |  n/a 
synced map<int, CCobThread>                                                                | rts/Sim/Units/Scripts/CobEngine.h:25               |     216 |   65536 |  0.3% | 84.0% |      6280937 |        59046 |   175419 | 175203 |      0 |    5 | 1656 | █░░░░▒
synced map<CWorldObject*, unsigned long>                                                   | rts/Rendering/Models/ModelsMemStorage.h:156        |    7264 |   65536 | 11.1% | 29.6% |     83069112 |            0 |    33548 |  26284 |      0 |   15 |   40 | ▓▒▒▓▓▓
synced map<std::string, CArchiveScanner::FileInfo>                                         | rts/System/FileSystem/ArchiveScanner.cpp:395       |   17236 |   65536 | 26.3% |  0.0% |            0 |            0 |    17236 |      0 |      3 |    1 |    0 |  n/a 
synced map<int, std::string>                                                               | rts/Sim/Units/UnitToolTipMap.hpp:8                 |   20012 |   32768 | 61.1% |  0.0% |            0 |            0 |    20376 |      0 |      0 |    7 |    9 |  n/a 
synced map<std::string, unsigned int>                                                      | rts/System/FileSystem/Archives/IArchive.cpp:10     |   17945 |   32768 | 54.8% |  0.0% |        15370 |            0 |    17945 |      0 |      0 |   14 |   35 | █▒░░  
synced map<CUnit const*, int>                                                              | rts/Rendering/Common/ModelDrawerData.h:100         |      58 |   32768 |  0.2% | 54.1% |     18624044 |            0 |    20012 |  19954 |      0 |   14 |   10 | ▒▒▒▓░ 
synced map<unsigned int, CBuilderCAI*>                                                     | rts/Sim/Units/UnitHandler.h:22                     |      58 |   32768 |  0.2% | 57.5% |        19954 |            0 |    20012 |  19954 |      0 |   14 |   36 | █     
synced map<int, unsigned long>                                                             | rts/System/FreeListMap.h:122                       |   15832 |   32768 | 48.3% |  0.1% |       263677 |            0 |   543132 | 263677 |      0 |   14 |    0 | █     
synced map<int, std::vector<SLosInstance*, std::allocator<SLosInstance*> >>                | rts/Sim/Misc/LosHandler.h:103                      |   11795 |   32768 | 36.0% |  0.0% |       209462 |        11795 |    32943 |      0 |      0 |   14 |   49 | █▒░░░ 
synced map<CFeature const*, int>                                                           | rts/Rendering/Common/ModelDrawerData.h:100         |    7205 |   16384 | 44.0% | 12.3% |     26379594 |            0 |    13535 |   6330 |      0 |   13 |   35 | ░░░▒▒█
synced map<QTPFS::PathHashType, int>                                                       | rts/Sim/Path/QTPFS/PathManager.cpp:171             |    4146 |    8192 | 50.6% | 49.4% |      1253008 |       507421 |   554041 | 276720 |      0 |   12 |   38 | █▒░▒░ 
synced map<QTPFS::PathHashType, int>                                                       | rts/Sim/Path/QTPFS/PathManager.cpp:171             |    2331 |    4096 | 56.9% | 43.1% |      1547561 |       114275 |   553640 | 111944 |      0 |   11 |   46 | █▒░▒░ 
synced map<std::string, unsigned long>                                                     | rts/Rendering/IconHandler.h:73                     |    1964 |    4096 | 47.9% |  0.0% |        56543 |         1964 |     1964 |      0 |      1 |   11 |   10 | █     
synced map<std::string, LuaRulesParams::Param>                                             | rts/Lua/LuaHandleSynced.cpp:57                     |    1526 |    4096 | 37.3% |  0.0% |         5831 |          115 |     1527 |      1 |      0 |   11 |    9 | █     
synced map<std::string, int>                                                               | rts/Sim/Misc/DamageArrayHandler.h:14               |    1427 |    4096 | 34.8% |  0.0% |         4079 |         1552 |     1427 |      0 |      0 |   11 |   11 | █░    
synced map<std::string, int>                                                               | rts/Sim/Weapons/WeaponDefHandler.h:17              |     784 |    2048 | 38.3% |  0.0% |         2278 |           35 |      784 |      0 |      0 |    1 |    9 | █▒░░  
unsynced map<int (*)(lua_State*), std::string>                                             | rts/System/creg/SerializeLuaState.cpp:69           |     871 |    2048 | 42.5% |  0.0% |       115463 |          871 |   117211 |      0 |      0 |   10 |   14 | ▒▒░▓▒ 
synced map<std::string, int>                                                               | rts/Sim/Features/FeatureDefHandler.h:18            |    1037 |    2048 | 50.6% |  0.0% |        30861 |         1039 |     1037 |      0 |      1 |    1 |    9 | █░    
synced map<unsigned int, unsigned int>                                                     | rts/Sim/Projectiles/ExplosionGenerator.h:45        |     355 |    1024 | 34.7% |  0.0% |       380839 |          356 |      355 |      0 |      0 |    9 |   17 | █▒▒   
synced map<std::string, int>                                                               | rts/Sim/Units/UnitDefHandler.h:18                  |     581 |    1024 | 56.7% |  0.0% |      1405073 |            0 |      581 |      0 |      1 |    1 |    9 | █░    
synced map<std::string, DataElement>                                                       | rts/Lua/LuaUnitDefs.cpp:29                         |     255 |     512 | 49.8% |  0.0% |       917266 |        17643 |     1275 |      0 |      0 |   40 |    9 | █░░   
unsynced map<std::string, std::vector<std::string, std::allocator<std::string > >>         | rts/Game/UI/KeyBindings.h:16                       |     272 |     512 | 53.1% |  0.0% |           20 |            0 |      989 |      0 |      0 |    4 |   14 | █▒░   
synced map<std::string, AtlasedTexture>                                                    | rts/Rendering/Textures/TextureAtlas.cpp:28         |       0 |     512 |  0.0% |  0.0% |            0 |         4129 |        0 |      0 |      0 |    1 |    0 |  n/a 
synced map<std::string, DefTagMetaData const*>                                             | rts/Sim/Misc/DefinitionTag.cpp:20                  |     182 |     512 | 35.5% |  0.0% |       138335 |         2945 |      182 |      0 |      0 |    8 |    3 | █▒░░  
synced map<std::string, unsigned long>                                                     | rts/Sim/Units/Scripts/CobFileHandler.h:11          |       3 |     512 |  0.6% |  0.0% |        20008 |            3 |        3 |      0 |      0 |    1 |    0 | █     
synced map<CSolidObject*, Bitwise::BitwiseEnum<CMoveMath::BlockTypes>>                     | rts/Sim/MoveTypes/MoveMath/MoveMath.cpp:403        |       2 |     256 |  0.8% |  0.0% |      8506372 |      1189452 |  1189452 |      0 |      0 |    5 |   41 | █▒░░░ 
synced map<CSolidObject*, Bitwise::BitwiseEnum<CMoveMath::BlockTypes>>                     | rts/Sim/MoveTypes/MoveMath/MoveMath.cpp:403        |       0 |     256 |  0.0% |  0.0% |      8692370 |      1212895 |  1212895 |      0 |      0 |    5 |   41 | █▒░░░ 
synced map<CSolidObject*, Bitwise::BitwiseEnum<CMoveMath::BlockTypes>>                     | rts/Sim/MoveTypes/MoveMath/MoveMath.cpp:403        |       0 |     256 |  0.0% |  0.0% |      9631221 |      1272386 |  1272386 |      0 |      0 |    5 |   41 | █▒░░░ 
synced map<CSolidObject*, Bitwise::BitwiseEnum<CMoveMath::BlockTypes>>                     | rts/Sim/MoveTypes/MoveMath/MoveMath.cpp:403        |       2 |     256 |  0.8% |  0.0% |      8525167 |      1118022 |  1118022 |      0 |      0 |    5 |   41 | █▒░░░░
synced map<CSolidObject*, Bitwise::BitwiseEnum<CMoveMath::BlockTypes>>                     | rts/Sim/MoveTypes/MoveMath/MoveMath.cpp:403        |       2 |     256 |  0.8% |  0.0% |      9400713 |      1321024 |  1321024 |      0 |      0 |    5 |   41 | █▒░░░ 
synced map<CSolidObject*, Bitwise::BitwiseEnum<CMoveMath::BlockTypes>>                     | rts/Sim/MoveTypes/MoveMath/MoveMath.cpp:403        |       2 |     256 |  0.8% |  0.0% |      8334699 |      1159489 |  1159489 |      0 |      0 |    5 |   41 | █▒░░░ 
synced map<CSolidObject*, Bitwise::BitwiseEnum<CMoveMath::BlockTypes>>                     | rts/Sim/MoveTypes/MoveMath/MoveMath.cpp:403        |       2 |     256 |  0.8% |  0.0% |      7821966 |      1112565 |  1112565 |      0 |      0 |    5 |   41 | █▒░░░ 
synced map<CSolidObject*, Bitwise::BitwiseEnum<CMoveMath::BlockTypes>>                     | rts/Sim/MoveTypes/MoveMath/MoveMath.cpp:403        |       2 |     256 |  0.8% |  0.0% |      8233361 |      1128930 |  1128930 |      0 |      0 |    5 |   41 | █▒░░░░
synced map<CSolidObject*, Bitwise::BitwiseEnum<CMoveMath::BlockTypes>>                     | rts/Sim/MoveTypes/MoveMath/MoveMath.cpp:403        |       2 |     256 |  0.8% |  0.0% |     13295600 |      1848379 |  1848379 |      0 |      0 |    5 |   41 | █▒░░░ 
synced map<CSolidObject*, Bitwise::BitwiseEnum<CMoveMath::BlockTypes>>                     | rts/Sim/MoveTypes/MoveMath/MoveMath.cpp:403        |       0 |     256 |  0.0% |  0.0% |      8525663 |      1136144 |  1136144 |      0 |      0 |    5 |   41 | █▒░░░░
synced map<CSolidObject*, Bitwise::BitwiseEnum<CMoveMath::BlockTypes>>                     | rts/Sim/MoveTypes/MoveMath/MoveMath.cpp:403        |       0 |     256 |  0.0% |  0.0% |      8058185 |      1148826 |  1148826 |      0 |      0 |    5 |   41 | █▒░░░ 
synced map<CSolidObject*, Bitwise::BitwiseEnum<CMoveMath::BlockTypes>>                     | rts/Sim/MoveTypes/MoveMath/MoveMath.cpp:403        |       0 |     256 |  0.0% |  0.0% |      7721362 |      1131084 |  1131084 |      0 |      0 |    5 |   41 | █▒░░░ 
synced map<std::string, DataElement>                                                       | rts/Lua/LuaWeaponDefs.cpp:27                       |     131 |     256 | 51.2% |  0.0% |       151400 |           32 |      655 |      0 |      0 |   35 |    7 | █▒    
synced map<unsigned int, CTimeProfiler::TimeRecord>                                        | rts/System/TimeProfiler.cpp:123                    |      11 |     256 |  4.3% |  0.0% |      3966163 |           11 |     1403 |      0 |    276 |    1 |    0 | █     
synced map<std::string, unsigned long>                                                     | rts/Rendering/Textures/NamedTextures.cpp:22        |       1 |     256 |  0.4% |  0.4% |        41131 |           23 |        4 |      1 |      0 |    1 |    0 | █     
unsynced map<std::string, CS3OTextureHandler::CachedS3OTex>                                | rts/Rendering/Textures/S3OTextureHandler.h:16      |      91 |     256 | 35.5% |  0.0% |         4533 |           91 |     4626 |      0 |      0 |    7 |    8 | █▒░░░ 
unsynced map<std::string, CBitmap>                                                         | rts/Rendering/Textures/S3OTextureHandler.h:16      |       2 |     256 |  0.8% | 67.6% |         4622 |         2292 |     2292 |   2290 |      0 |    7 |    8 | █▒░░░ 
synced map<std::string, DataElement>                                                       | rts/Lua/LuaFeatureDefs.cpp:20                      |      45 |     128 | 35.2% |  0.0% |        72666 |         1423 |      225 |      0 |      0 |   30 |    4 | █░    
synced map<CSolidObject*, Bitwise::BitwiseEnum<CMoveMath::BlockTypes>>                     | rts/Sim/MoveTypes/MoveMath/MoveMath.cpp:403        |       2 |     128 |  1.6% |  0.0% |     12001568 |      1959738 |  1959738 |      0 |      0 |    4 |   41 | █▒░░░ 
synced map<CSolidObject*, Bitwise::BitwiseEnum<CMoveMath::BlockTypes>>                     | rts/Sim/MoveTypes/MoveMath/MoveMath.cpp:403        |       0 |     128 |  0.0% |  0.0% |     12254732 |      2016499 |  2016499 |      0 |      0 |    4 |   41 | █▒░░░ 
synced map<CSolidObject*, Bitwise::BitwiseEnum<CMoveMath::BlockTypes>>                     | rts/Sim/MoveTypes/MoveMath/MoveMath.cpp:403        |       0 |     128 |  0.0% |  0.0% |      7839396 |      1130846 |  1130846 |      0 |      0 |    4 |   41 | █▒░░░ 
unsynced map<unsigned int, Shader::UniformState>                                           | rts/Rendering/Shaders/Shader.cpp:222               |      71 |     128 | 55.5% |  0.0% |      6656118 |           71 |       71 |      0 |      0 |    6 |   11 | █░ ░  
synced map<unsigned int, int>                                                              | rts/System/TimeProfiler.cpp:27                     |      77 |     128 | 60.2% |  0.0% |      7671028 |           77 |       77 |      0 |      0 |    6 |    8 | █     
synced map<CSolidObject*, Bitwise::BitwiseEnum<CMoveMath::BlockTypes>>                     | rts/Sim/MoveTypes/MoveMath/MoveMath.cpp:403        |       2 |     128 |  1.6% |  0.0% |      8522944 |      1205088 |  1205088 |      0 |      0 |    4 |   41 | █▒░░░ 
synced map<std::string, DefTagMetaData const*>                                             | rts/Sim/Misc/DefinitionTag.cpp:20                  |      40 |      64 | 62.5% |  0.0% |         3672 |        21210 |       40 |      0 |      0 |    5 |    5 | █░    
synced map<std::string, DefTagMetaData const*>                                             | rts/Sim/Misc/DefinitionTag.cpp:20                  |      36 |      64 | 56.2% |  0.0% |         2732 |           73 |       36 |      0 |      0 |    5 |    5 | ▓█ ░  
synced map<std::string, unsigned int>                                                      | rts/Sim/Misc/CategoryHandler.h:13                  |      23 |      64 | 35.9% |  0.0% |         6193 |           23 |     6216 |      0 | 149637 |    1 |    1 | █░    
synced map<std::string, std::string>                                                       | rts/Sim/Projectiles/ExplosionGenerator.h:29        |      31 |      64 | 48.4% |  0.0% |            0 |         3279 |       31 |      0 |      0 |    5 |    1 |  n/a 
unsynced map<std::string, bool>                                                            | rts/Rendering/Shaders/ShaderStates.h:228           |      18 |      32 | 56.2% |  0.0% |        62238 |           18 |    62256 |      0 |      0 |    4 |    1 | ▓█    
synced map<int, int>                                                                       | rts/Game/Game.h:43                                 |       7 |      16 | 43.8% |  0.0% |         2068 |            3 |        3 |      0 |      0 |    1 |    1 | █░    
synced map<int, int>                                                                       | rts/Game/Game.h:43                                 |       6 |      16 | 37.5% |  0.0% |        20345 |            1 |        1 |      0 |      0 |    1 |    1 | █     
unsynced map<std::string, emilib::HashMap<std::string, Shader::IProgramObject*, std::hash<std::string >, emilib::HashMapEqualTo | rts/Rendering/Shaders/ShaderHandler.h:16           |       9 |      16 | 56.2% |  0.0% |       123192 |            1 |   369589 |      1 |      0 |    3 |    3 | █     
synced map<std::string, LuaRulesParams::Param>                                             | rts/Sim/Misc/Team.cpp:64                           |       7 |      16 | 43.8% |  0.0% |        26228 |            1 |      279 |      0 |      2 |    3 |    1 | █     
unsynced map<std::string, Shader::IProgramObject*>                                         | rts/System/SpringHashMap.hpp:567                   |       4 |       8 | 50.0% |  0.0% |       123187 |            3 |   123191 |      0 |      0 |    2 |    1 | █▒    
synced map<int, CGame::PlayerTrafficInfo>                                                  | rts/Game/Game.cpp:225                              |       3 |       8 | 37.5% |  0.0% |        22454 |            4 |        3 |      0 |      0 |    2 |    2 | █     
synced set<int>                                                                            | rts/Game/Players/Player.cpp:44                     |       1 |       8 | 12.5% |  0.0% |        40021 |            0 |        4 |      0 |      0 |    1 |    0 | █     
synced map<unsigned char, GameParticipant::ClientLinkData>                                 | rts/Net/GameParticipant.cpp:10                     |       1 |       8 | 12.5% | 12.5% |       163503 |            0 |   140913 |      1 | 132633 |    2 |    0 | █     
unsynced map<unsigned long, unsigned int>                                                  | rts/Rendering/Textures/S3OTextureHandler.h:16      |       1 |       4 | 25.0% |  0.0% |         1144 |            1 |        1 |      0 |      0 |    1 |    0 | █     
synced map<int, unsigned int>                                                              | rts/Net/GameParticipant.cpp:10                     |       0 |       4 |  0.0% | 100.0% |         6057 |       287698 |     2019 |   2019 |      0 |    1 |    0 | █     
synced set<int>                                                                            | rts/Game/SelectedUnitsHandler.h:20                 |       0 |       4 |  0.0% |  0.0% |          809 |       127068 |        1 |      0 |   4461 |    1 |    0 | █     
synced map<unsigned int, QTPFS::PathSearchTrace::Execution*>                               | rts/Sim/Path/QTPFS/PathManager.cpp:171             |       0 |       0 |  0.0% |  0.0% |            0 |       200913 |        0 |      0 |      0 |    0 |    0 |  n/a 
unsynced map<int, int>                                                                     | rts/Game/UI/Groups/GroupHandler.cpp:37             |       0 |       0 |  0.0% |  0.0% |            0 |        40021 |        0 |  20010 |      0 |    0 |    0 |  n/a 
unsynced map<int, int>                                                                     | rts/Game/UI/Groups/GroupHandler.cpp:37             |       0 |       0 |  0.0% |  0.0% |            0 |        49913 |        0 |  19954 |      0 |    0 |    0 |  n/a 
synced map<int, std::vector<int, std::allocator<int> >>                                    | rts/Sim/Misc/GeometricObjects.h:27                 |       0 |       0 |  0.0% |  0.0% |            0 |         2020 |        0 |      0 |      0 |    0 |    0 |  n/a 
synced set<int>                                                                            | rts/Sim/Units/CommandAI/BuilderCaches.cpp:8        |       0 |       0 |  0.0% |  0.0% |            0 |        19954 |        0 |  19954 |      0 |    0 |    0 |  n/a 
synced set<int>                                                                            | rts/Sim/Units/CommandAI/BuilderCaches.cpp:9        |       0 |       0 |  0.0% |  0.0% |            0 |        19954 |        0 |  19954 |      0 |    0 |    0 |  n/a 
synced set<int>                                                                            | rts/Sim/Units/CommandAI/BuilderCaches.cpp:10       |       0 |       0 |  0.0% |  0.0% |            0 |        19954 |        0 |  19954 |      0 |    0 |    0 |  n/a 
[HashContainerStats] 83 active, 144666 skipped (<1000 ops)

[HashContainerStats] Top 20 containers by total time (ns/op):
  type                                                                             | source                                                  |  total-ms |   find-hit |  find-miss |     insert |      erase |     rehash
  synced map<CWorldObject*, unsigned long>                                         | rts/Rendering/Models/ModelsMemStorage.h:156             | 10183.39ms |        122 |          - |        119 |          0 |      29631
  synced map<CFeature const*, int>                                                 | rts/Rendering/Common/ModelDrawerData.h:100              |  2715.39ms |        102 |          - |        130 |        349 |       7556
  synced map<CUnit const*, int>                                                    | rts/Rendering/Common/ModelDrawerData.h:100              |  1751.23ms |         93 |          - |        172 |        189 |      14362
  synced map<CUnit const*, ScopedTransformMemAlloc>                                | rts/Rendering/Common/ModelDrawerData.h:100              |   878.88ms |        171 |          - |        327 |       2110 |     104648
  synced map<CFeature const*, ScopedTransformMemAlloc>                             | rts/Rendering/Common/ModelDrawerData.h:100              |   686.31ms |        373 |          - |        273 |       2741 |      91001
  synced map<int, CCobThread>                                                      | rts/Sim/Units/Scripts/CobEngine.h:25                    |   647.43ms |         83 |        704 |        432 |          0 |     611775
  synced map<CSolidObject*, Bitwise::BitwiseEnum<CMoveMath::BlockTypes>>           | rts/Sim/MoveTypes/MoveMath/MoveMath.cpp:403             |   407.71ms |         26 |         20 |         28 |          - |       6843
  synced map<CSolidObject*, Bitwise::BitwiseEnum<CMoveMath::BlockTypes>>           | rts/Sim/MoveTypes/MoveMath/MoveMath.cpp:403             |   374.67ms |         22 |         19 |         30 |          - |       1410
  synced map<CSolidObject*, Bitwise::BitwiseEnum<CMoveMath::BlockTypes>>           | rts/Sim/MoveTypes/MoveMath/MoveMath.cpp:403             |   362.64ms |         21 |         16 |         25 |          - |       3628
  synced map<CSolidObject*, Bitwise::BitwiseEnum<CMoveMath::BlockTypes>>           | rts/Sim/MoveTypes/MoveMath/MoveMath.cpp:403             |   308.01ms |         25 |         19 |         31 |          - |       3648
  synced map<CSolidObject*, Bitwise::BitwiseEnum<CMoveMath::BlockTypes>>           | rts/Sim/MoveTypes/MoveMath/MoveMath.cpp:403             |   281.23ms |         26 |         18 |         30 |          - |       1717
  synced map<CSolidObject*, Bitwise::BitwiseEnum<CMoveMath::BlockTypes>>           | rts/Sim/MoveTypes/MoveMath/MoveMath.cpp:403             |   276.06ms |         25 |         19 |         29 |          - |       2564
  synced map<CSolidObject*, Bitwise::BitwiseEnum<CMoveMath::BlockTypes>>           | rts/Sim/MoveTypes/MoveMath/MoveMath.cpp:403             |   271.28ms |         26 |         19 |         30 |          - |       5065
  synced map<CSolidObject*, Bitwise::BitwiseEnum<CMoveMath::BlockTypes>>           | rts/Sim/MoveTypes/MoveMath/MoveMath.cpp:403             |   271.15ms |         27 |         19 |         29 |          - |       3370
  synced map<CSolidObject*, Bitwise::BitwiseEnum<CMoveMath::BlockTypes>>           | rts/Sim/MoveTypes/MoveMath/MoveMath.cpp:403             |   254.37ms |         21 |         15 |         23 |          - |       6039
  synced map<CSolidObject*, Bitwise::BitwiseEnum<CMoveMath::BlockTypes>>           | rts/Sim/MoveTypes/MoveMath/MoveMath.cpp:403             |   247.32ms |         22 |         16 |         27 |          - |       3051
  synced map<QTPFS::PathHashType, int>                                             | rts/Sim/Path/QTPFS/PathManager.cpp:171                  |   242.93ms |         93 |        135 |         72 |         61 |       7234
  synced map<CSolidObject*, Bitwise::BitwiseEnum<CMoveMath::BlockTypes>>           | rts/Sim/MoveTypes/MoveMath/MoveMath.cpp:403             |   233.16ms |         21 |         16 |         27 |          - |       3739
  synced map<CSolidObject*, Bitwise::BitwiseEnum<CMoveMath::BlockTypes>>           | rts/Sim/MoveTypes/MoveMath/MoveMath.cpp:403             |   231.42ms |         21 |         15 |         24 |          - |       4562
  synced map<CSolidObject*, Bitwise::BitwiseEnum<CMoveMath::BlockTypes>>           | rts/Sim/MoveTypes/MoveMath/MoveMath.cpp:403             |   230.76ms |         21 |         15 |         25 |          - |       4695

[HashContainerStats] Aggregated by type (42 types):
  type                                                                                       | count |     size |     find-hit |    find-miss |    inserts |   erases |  iters |    find-ms |  insert-ms |   total-ms
  synced map<CSolidObject*, Bitwise::BitwiseEnum<CMoveMath::BlockTypes>>                     |    16 |       18 |    147365319 |     21091367 |   21091367 |        0 |      0 |   3829.5ms |    584.5ms |   4414.3ms
  synced map<CWorldObject*, unsigned long>                                                   |     1 |     7264 |     83069112 |            0 |      33548 |    26284 |      0 |  10178.9ms |      4.0ms |  10183.4ms
  synced map<CFeature const*, int>                                                           |     1 |     7205 |     26379594 |            0 |      13535 |     6330 |      0 |   2711.3ms |      1.8ms |   2715.4ms
  synced map<CUnit const*, int>                                                              |     1 |       58 |     18624044 |            0 |      20012 |    19954 |      0 |   1743.8ms |      3.5ms |   1751.2ms
  synced map<unsigned int, int>                                                              |     1 |       77 |      7671028 |           77 |         77 |        0 |      0 |    159.5ms |      0.0ms |    159.5ms
  synced map<int, CCobThread>                                                                |     1 |      216 |      6280937 |        59046 |     175419 |   175203 |      0 |    568.5ms |     75.8ms |    647.4ms
  unsynced map<unsigned int, Shader::UniformState>                                           |     1 |       71 |      6656118 |           71 |         71 |        0 |      0 |    139.7ms |      0.0ms |    139.7ms
  synced map<QTPFS::PathHashType, int>                                                       |     2 |     6477 |      2800569 |       621696 |    1107681 |   388664 |      0 |    318.9ms |     62.2ms |    405.0ms
  synced map<unsigned int, CTimeProfiler::TimeRecord>                                        |     1 |       11 |      3966163 |           11 |       1403 |        0 |    276 |     82.6ms |      0.1ms |     82.7ms
  synced map<CUnit const*, ScopedTransformMemAlloc>                                          |     1 |       58 |        19954 |            0 |    2546162 |    19954 |      0 |      3.4ms |    833.2ms |    878.9ms
  synced map<CFeature const*, ScopedTransformMemAlloc>                                       |     1 |     7205 |         6330 |            0 |    2435999 |     6330 |      0 |      2.4ms |    666.5ms |    686.3ms
  synced map<std::string, int>                                                               |     4 |     3829 |      1442291 |         2626 |       3829 |        0 |      2 |     40.4ms |      0.2ms |     40.7ms
  synced map<std::string, DataElement>                                                       |     3 |      431 |      1141332 |        19098 |       2155 |        0 |      0 |     27.1ms |      0.2ms |     27.3ms
  synced map<int, unsigned long>                                                             |     1 |    15832 |       263677 |            0 |     543132 |   263677 |      0 |     69.6ms |     50.5ms |    120.2ms
  synced map<int, std::vector<SLosInstance*, std::allocator<SLosInstance*> >>                |     2 |    48355 |       683632 |        48355 |     121739 |        0 |      0 |    174.1ms |      6.3ms |    181.7ms
  synced map<unsigned int, unsigned int>                                                     |     7 |   120749 |       380839 |          356 |     180263 |    33560 |  33560 |      8.4ms |      7.4ms |     15.9ms
  unsynced map<std::string, emilib::HashMap<std::string, Shader::IProgramObject*, std::hash<std::string >, emilib::HashMapEqualTo |     1 |        9 |       123192 |            1 |     369589 |        1 |      0 |      6.6ms |     10.0ms |     16.6ms
  synced map<unsigned char, GameParticipant::ClientLinkData>                                 |     1 |        1 |       163508 |            0 |     140918 |        1 | 132638 |      3.5ms |      4.0ms |      7.5ms
  synced set<int>                                                                            |     6 |     7219 |        47160 |       186930 |      13553 |    66192 |   6490 |      2.7ms |      1.1ms |      7.3ms
  synced map<int, unsigned int>                                                              |     1 |        0 |         6057 |       287988 |       2019 |     2019 |      0 |      0.3ms |      0.8ms |      1.3ms
  unsynced map<std::string, Shader::IProgramObject*>                                         |     1 |        4 |       123187 |            3 |     123191 |        0 |      0 |      6.5ms |      4.8ms |     11.4ms
  unsynced map<int (*)(lua_State*), std::string>                                             |     1 |      871 |       115463 |          871 |     117211 |        0 |      0 |      2.5ms |      2.6ms |      5.1ms
  synced map<unsigned int, QTPFS::PathSearchTrace::Execution*>                               |     1 |        0 |            0 |       200913 |          0 |        0 |      0 |      0.0ms |      0.0ms |      0.0ms
  synced map<std::string, unsigned int>                                                      |     2 |    17968 |        21563 |           23 |      24161 |        0 | 149637 |      2.7ms |      1.5ms |      4.8ms
  synced map<std::string, DefTagMetaData const*>                                             |     3 |      258 |       144739 |        24228 |        258 |        0 |      0 |      3.8ms |      0.0ms |      3.9ms
  unsynced map<int, int>                                                                     |     2 |        0 |            0 |        89934 |          0 |    39964 |      0 |      0.0ms |      0.0ms |      0.8ms
  unsynced map<std::string, bool>                                                            |     1 |       18 |        62238 |           18 |      62256 |        0 |      0 |      5.4ms |      2.5ms |      7.9ms
  synced map<std::string, unsigned long>                                                     |     3 |     1968 |       117682 |         1990 |       1971 |        1 |      1 |      7.3ms |      0.1ms |      7.5ms
  unsynced map<std::string, int (*)(lua_State*)>                                             |     1 |   116325 |            0 |            0 |     116334 |        0 |      0 |      0.0ms |     14.3ms |     22.0ms
  synced map<unsigned int, CBuilderCAI*>                                                     |     1 |       58 |        19954 |            0 |      20012 |    19954 |      0 |      2.5ms |      3.4ms |      9.5ms
  synced map<std::string, LuaRulesParams::Param>                                             |     2 |     1533 |        32059 |          116 |       1806 |        1 |      2 |      2.5ms |      0.2ms |      2.8ms
  synced map<int, CGame::PlayerTrafficInfo>                                                  |     1 |        3 |        22454 |            4 |          3 |        0 |      0 |      1.3ms |      0.0ms |      1.3ms
  synced map<int, int>                                                                       |     2 |       13 |        22413 |            4 |          4 |        0 |      0 |      1.3ms |      0.0ms |      1.3ms
  synced map<int, std::string>                                                               |     1 |    20012 |            0 |            0 |      20376 |        0 |      0 |      0.0ms |      4.2ms |      4.6ms
  synced map<std::string, CArchiveScanner::FileInfo>                                         |     1 |    17236 |            0 |            0 |      17236 |        0 |      3 |      0.0ms |      1.5ms |      1.5ms
  unsynced map<std::string, CBitmap>                                                         |     1 |        2 |         4622 |         2292 |       2292 |     2290 |      0 |      0.3ms |      0.1ms |      8.2ms
  unsynced map<std::string, CS3OTextureHandler::CachedS3OTex>                                |     1 |       91 |         4533 |           91 |       4626 |        0 |      0 |      0.6ms |      0.3ms |      0.8ms
  synced map<std::string, AtlasedTexture>                                                    |     1 |        0 |            0 |         4129 |          0 |        0 |      0 |      0.0ms |      0.0ms |      0.0ms
  synced map<std::string, std::string>                                                       |     1 |       31 |            0 |         3279 |         31 |        0 |      0 |      0.1ms |      0.0ms |      0.1ms
  synced map<int, std::vector<int, std::allocator<int> >>                                    |     1 |        0 |            0 |         2020 |          0 |        0 |      0 |      0.0ms |      0.0ms |      0.0ms
  unsynced map<unsigned long, unsigned int>                                                  |     1 |        1 |         1144 |            1 |          1 |        0 |      0 |      0.0ms |      0.0ms |      0.0ms
  unsynced map<std::string, std::vector<std::string, std::allocator<std::string > >>         |     1 |      272 |           20 |            0 |        989 |        0 |      0 |      0.0ms |      0.0ms |      0.1ms
[HashContainerStats] Aggregate timing:
  find_hit:  307782928 ops, 19595.524 ms total (64 ns/op avg)
  find_miss: 22647596 ops, 512.564 ms total (23 ns/op avg)
  insert:    29315230 ops, 2347.475 ms total (80 ns/op avg)
  erase:     1070379 ops, 104.843 ms total (98 ns/op avg)
  rehash:    491 ops, 15.541 ms total (31651 ns/op avg)
  iterate:   322610 begin() calls
```