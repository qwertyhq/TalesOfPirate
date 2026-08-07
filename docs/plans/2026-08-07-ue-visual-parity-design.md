# Unreal Visual Parity — Design

**Date:** 2026-08-07  
**Branch:** `qwertyhq/codex-ue-parity`  
**Baseline:** `30b5f2a19a71a7ab25303a03ec8e36f4a30b4c6f`

## Purpose

Before replacing the original graphics with remastered assets, make the Unreal
client render the same *game state* as the original client. The first milestone
must correct character composition, the default camera profile, and material
fallbacks. It must be visually checked at the same Garner position used for the
original-client reference.

This milestone deliberately preserves the imported legacy art. It establishes a
correct, testable rendering baseline on which the remaster can be built.

## Confirmed Baseline

At character `Test195126`, map `garner`, position `(223325, 278475)`:

- the original client shows a complete, small character in a distant
  top-down/isometric view, surrounded by textured buildings, vegetation and
  paving;
- the Unreal client shows a floating oversized face, a much closer and wider
  camera, mostly grey scene objects, and a single noisy terrain texture;
- `/Game/All/0000000000/SkeletalMeshes/0000000000` is the face part, not a
  complete body, but the current character map calls it `mesh` and
  `SetBodyMesh` scales its 39 cm bounds to the 176 cm capsule;
- the original player model is modular: head/hair, face, body, gloves and shoes
  are separate skinned meshes selected by item identifiers;
- `CMD_MC_ENTERMAP` and `CMD_MC_CHABEGINSEE` already carry `ChaLookInfo`, but the
  Unreal session discards the local look and retains only `typeId` for remote
  actors;
- the current Unreal camera uses a 1400 cm spring arm, `-45°` pitch and the
  camera component's default wide FOV;
- the original default camera profile is 35 m horizontal range, 50 m vertical
  range and `32°` vertical FOV, equivalent to about 6103 cm spring-arm distance,
  `-55.0°` pitch and `54.02°` Unreal horizontal FOV at 16:9;
- the latest Garner runtime log contains fallback warnings for 1984 unique
  materials: 1972 object materials lack both Instanced Static Mesh and Nanite
  usage, and 12 terrain material instances lack Nanite usage;
- the current terrain script intentionally assigns one dominant texture per
  tile although the source format provides four weighted layers.

## Milestone Acceptance Criteria

The milestone is complete only when all of the following are true:

1. The local player is assembled from the appearance received in
   `CMD_MC_ENTERMAP`; no single face mesh is treated or scaled as a full body.
2. Remote player appearances received in `CMD_MC_CHABEGINSEE` use the same
   appearance representation and assembly path.
3. The five base player parts share one pose and remain aligned while the
   imported idle animation plays.
4. Missing optional appearance parts produce a specific warning and hide only
   that part. They must not substitute an unrelated full-height mesh.
5. The default camera derives from the legacy `35 m / 50 m / 32°` profile and
   retains that composition after movement.
6. Garner runs with zero `Default Material will be used in game` warnings for
   the materials used by its terrain and instanced scene objects.
7. A fresh screenshot at `(223325, 278475)` shows a complete small character,
   distant top-down framing, and textured scene objects.
8. `CorsairsUEEditor Mac Development` still builds successfully.

## Architecture

### 1. Preserve network appearance

Add one Unreal-facing appearance value type in `CorsairsNet`:

- synchronization type;
- character type identifier;
- hair item identifier;
- all 34 equipment item identifiers in protocol slot order;
- boat flag, so human-part assembly is never attempted for boats.

`UCorsairsSession` stores the local value from `McEnterMapData.baseInfo.look`
and exposes it with a pure getter. `FCorsairsWorldActor` stores the same value
from `McChaBeginSeeMessage.base.look`. The game module must not include or copy
packet-reader types; packet decoding remains inside `CorsairsNet`.

The local and remote rendering paths consume the same Unreal-facing type.
`CMD_MC_NOTIACTION` look-switch messages update that value and notify the
rendering layer, so changing equipment cannot leave the actor with stale parts.

### 2. Resolve appearance data independently of actors

Replace the misleading `type -> mesh` table with a generated appearance
catalog:

- animation path per character archetype;
- default part item identifiers from `characters.skin_info`;
- item identifier and character model to imported skeletal-mesh path;
- existing single-mesh fallback for non-player archetypes whose source model is
  not modular.

The generator reads `characters` and `items` from `gamedata.sqlite`. Item
identifiers are not asset names: the original client chooses
`items.module_<model + 1>` for the active character model. The generator
preserves those module strings literally and converts them to
`/Game/All/<module>/SkeletalMeshes/<module>` paths. A small C++ resolver loads
the generated JSON once, validates it, overlays non-zero server look values on
the archetype defaults, and returns a resolved five-part appearance. This logic
is kept outside `GameMode` so it can be tested without spawning a world.

For the five visual slots the resolver follows the original client's rules:

- equipped head item wins over the base hair identifier;
- non-zero server equipment wins over the archetype default;
- non-zero apparel slots 19 through 23 override their corresponding visual
  slots 0 through 4;
- a zero or unresolved optional part stays absent and is logged;
- non-player/boat records do not enter the modular-player path.

### 3. Assemble one modular Unreal character

Introduce a shared modular character base used by both the controlled player
and remote characters. It owns:

- one stable, invisible animation-driver skeletal mesh imported from the
  character's `.lab`;
- five visible follower components: head/hair, face, body, gloves and shoes.

All visible parts call UE 5.8's `SetLeaderPoseComponent` with the stable driver
and use the same relative transform. Equipment changes therefore cannot replace
the pose leader. Scaling is computed once from the combined visible bounds and
applied to the shared driver transform, never from the face bounds alone. The
imported idle animation plays only on the driver; followers do not tick an
independent pose.

The public character API becomes appearance-oriented rather than
`SetBodyMesh(path)`-oriented. `ACorsairsGameMode` loads the catalog once,
resolves the authoritative local actor received in `ENTERMAP`, and uses the
same resolver for visible remote actors and later look-switch notifications.
The controlled player derives from the shared modular character class; remote
actors no longer carry player-only camera and input components. The existing
single-mesh path remains available only for non-modular NPCs and monsters.

### 4. Reproduce the original default camera

Define the legacy profile in one place:

- horizontal offset: `3500 cm`;
- vertical offset: `5000 cm`;
- vertical FOV: `32°`;
- derived spring-arm length: `sqrt(3500² + 5000²)`, approximately `6103 cm`;
- derived pitch: `-atan2(5000, 3500)`, approximately `-55.0°`;
- initial yaw after the legacy-to-Unreal Y-axis conversion: `+90°`;
- target height: `100 cm` above the feet, or `+12 cm` relative to the center of
  the current 88 cm half-height capsule.

The player character applies the profile to the spring arm, controller and
camera component. Unreal's camera value is horizontal FOV, so at the 16:9
reference aspect ratio the client sets
`2 * atan(tan(32° / 2) * 16 / 9) = 54.0222°` and uses
`AspectRatio_MaintainYFOV`. Collision remains disabled for parity with the
original, but the acceptance screenshot must verify that nearby geometry does
not occlude the character at the much longer arm length.

The derivation is exposed as pure code and covered by an automation test so the
values cannot silently drift back to hand-tuned guesses.

The original client contains a later direction-normalization quirk that shortens
the horizontal offset after its first frame. This milestone follows the declared
`35 m / 50 m / 32°` profile, not that accidental runtime bug.

### 5. Make material usage explicit

Add an editor checker that inspects every material interface actually referenced
by Garner's hierarchical-instanced components and terrain meshes. It reports
missing `MATUSAGE_InstancedStaticMeshes` and `MATUSAGE_Nanite` separately and
fails when either is required but absent. It also distinguishes opaque/masked
materials from translucent materials: UE 5.8 does not render the latter through
Nanite by default.

Add an idempotent editor fixer that uses Unreal 5.8's exposed
`MaterialEditingLibrary.set_material_usage_override` API on the existing
material instances. Opaque/masked instances used by Garner receive Instanced
Static Mesh and Nanite overrides. Translucent instances receive the instancing
override while their owning components set `bDisallowNanite`, preserving
transparency instead of forcing an incompatible Nanite permutation. Terrain
instances receive the Nanite override. The tool recompiles affected materials,
saves only changed assets, and is run against the isolated Content snapshot.

The fixer does not replace or reparent the existing imported material instances:
doing so would broaden the change from the confirmed missing-usage failure into
a material-graph rewrite. Missing `BaseColorTexture` parameters are reported
separately and are not counted as usage-fallback failures.

Success is verified twice:

1. the editor checker reports zero missing usage flags;
2. a real Garner game launch produces zero default-material fallback warnings.

Texture-parameter presence alone is not accepted as proof because the current
checker already reports apparently valid textures while runtime rendering falls
back to grey.

## Data and Error Handling

- Generated appearance JSON is deterministic and sorted so changes are
  reviewable.
- The catalog is loaded once and reused; a missing or malformed catalog is a
  startup-level error with its full path in the log.
- Every unresolved item identifier includes the character type, slot and item
  identifier in its warning.
- Invalid human look data never falls back to the old face-as-body mapping.
- Material tools refuse to save while a graphical Unreal Editor is open.
- The original `Client/user/system.ini` remains outside all commits.

## Verification

The implementation uses test-first checks:

1. generator tests prove that Lambert resolves to five distinct default
   modules, that item-to-mesh paths come from the model-specific
   `items.module_<model + 1>` column, and that unusual module strings are not
   normalized or truncated;
2. C++ automation tests prove server-look overlay rules and exact camera
   derivation;
3. actor automation tests prove that all five parts follow the stable animation
   driver and remain connected after a body/equipment replacement;
4. the existing body checker is extended to reject a lone face as a valid
   assembled character and to validate all resolved player parts;
5. the new material-usage checker is run before the fixer to demonstrate the
   known failing baseline, then after it to prove zero missing flags;
6. build `CorsairsUEEditor Mac Development`;
7. launch Garner with the existing local stack, enter as `Test195126`, capture a
   fresh screenshot at the reference coordinates and count fallback warnings in
   the fresh runtime log.

## Out of Scope for This Milestone

- four-layer terrain weight-map generation and material blending;
- remastered PBR textures, meshes, lighting or post-processing;
- final HUD/minimap/action bar;
- authoritative movement and combat completeness;
- cross-map travel/session fixes;
- packaging and clean-checkout Content distribution;
- rectangular height-map support for the 16 affected maps.

Those items remain real work, but none should be mixed into the visual-parity
milestone. The next graphics milestone starts with four-layer terrain blending,
then replaces legacy materials and meshes on top of the corrected scene.
