# Shatter in the macOS port

PhysX "enhanced" effects (glass shards, debris, smoke, steam: `NxFluid`; banners and cloth: `NxCloth`) run on
Shatter's native solvers instead of the lifted PhysX 2.8 core. Rigid bodies, joints and queries stay on the lifted core, as in upstream. Issue: prasys/RunnerRecompiled#2.

- Upstream: https://github.com/Arecsu/shatter, tag `v1.0.7`, commit `bd59b4f` (2026-10-16). MIT, see `LICENSE`.
- `third_party/sse2neon.h`: https://github.com/DLTcollab/sse2neon `v1.8.0`, MIT (licence in the file header).

## Why

Measured on an M1 Max (unattended tutorial run, 150 s, `PhysXEnhanced=True` vs `False`, one run each): with the lifted
core running the effects, 24 of 83 in-level seconds fell below 50 fps (16 below 25 fps); without effects, 2 of 114.

## What is vendored (unchanged unless listed below)

| Path | Upstream file |
|---|---|
| `solver/fastfloor.h`, `solver/fluid/{fluid.cpp,fluid.h,thread_pool.h,step_dump.h}` | same paths, unchanged |
| `shim/{coverage.cpp,coverage.h,scene_read.h}` | same paths, unchanged |
| `solver/cloth/{cloth.cpp,cloth.h,cloth_rec.h,cloth_attach.cpp,cloth_edgeshape.cpp,cloth_mesh.cpp,cloth_self.cpp,cloth_selftri.cpp,cloth_state.cpp}`, `shim/cloth_ours.h` | same paths, unchanged |
| `shim/shim.h`, `shim/fluid_internal.h`, `shim/fluid_ours.cpp`, `shim/scene_read.cpp`, `shim/cloth_ours.cpp` | same paths, local changes marked `RR port` |

Not vendored: `PhysXLoader.cpp`, `loader_core.cpp`, `fluid_wrap.cpp`, `cloth_wrap.cpp` (replaced by `port/bridge.cpp`),
the solvers' tool mains (`run_scenario.cpp`, `cloth_replay.cpp`, `step_*`), `game_hooks.cpp`
(patches exe code bytes, which do not affect lifted code), installer, harness, tools.

## Local changes

- `shim.h`: `ptr32`/`fromPtr32` map guest addresses to host pointers (`port/guest.h`), null stays null; `FnCreateFluid` /
  `FnReleaseFluid` are `GuestThis` (guest functions); loader and cloth declarations removed.
- `fluid_internal.h`: `ShimFluid::vtbl` is a 32-bit guest address (the object lives in guest memory; the game reads `vtbl`
  at +0 and `userData` at +4); `g_hostVt[]` became `hostVtable()`.
- `fluid_ours.cpp`: the vtable is built on first use in guest memory from thunks (`SHATTER_THUNK`); `<windows.h>` ->
  `win_compat.h`.
- `cloth_ours.cpp`: the game's `NxCloth` object is `{vt, userData, Hosted*}` in guest memory (upstream `{vt, Hosted*}`,
  `userData` not filled); its 92-slot vtable is built from thunks in guest memory; `NxActor` reads and the mesh's
  `saveToDesc` go through `guestThiscall` / guest scratch; `getName` returns the game's string pointer (PhysX does not copy
  names); the receive buffers (`NxMeshData` positions, normals, indices, parents) were raw casts of guest addresses and
  go through `fromPtr32`.
- `scene_read.cpp`: the call helpers call the lifted core with `guestThiscall`; class returns and out-parameters go
  through guest-stack scratch (`GuestScratch`) and are copied back; `callRet`/`callOut` take the byte count.

## Port layer (`port/`)

- `guest.h`: guest/host boundary. `SHATTER_THUNK(fn)` turns `R fn(Self, void* edx, A...)` (upstream's `__fastcall`
  spelling of `__thiscall`) into a runtime thunk: `this` from ECX, arguments from the guest stack (pointers converted,
  floats bit-cast), callee pops, result in EAX or pushed on the x87 stack. `GuestThis` / `guestThiscall` call guest code
  as `__thiscall` through `rt_call`.
- `bridge.cpp`: upstream `PhysXLoader.cpp` hooks + the `fluid_wrap.cpp` / `cloth_wrap.cpp` parts the hosted objects need
  (`NxScene` createCloth/releaseCloth; `NxPhysicsSDK::createClothMesh` hands the core a guest stream object whose 13 slots
  forward to the game's `NxStream` and record the cooked bytes, upstream's StreamTap). The runtime's
  export hook (`rt_set_export_hook`) hands the game a wrapped `NxCreatePhysicsSDK`; it patches `NxPhysicsSDK`
  (createScene, releaseScene, getHWVersion, getNbPPUs) and `NxScene` (createFluid, releaseFluid, simulate,
  fetchResults) vtables in guest memory after checking that they belong to the lifted `physxcore.dll`.
- `win_compat.h`, `neon/{emmintrin,xmmintrin}.h`: the few Windows names used; SSE intrinsics through sse2neon.

Only guest threads may call guest code. Upstream already reads the core only on the game thread (in `simulate`) and
writes the game's buffers in `fetchResults`; the fluid thread and the worker pool touch host data only.

## Measured in game (M1 Max, `PhysXEnhanced=True`, one interactive session each, same levels)

| | cloth on the lifted core (fluids native) | cloth and fluids native |
|---|---|---|
| in-level seconds below 50 fps | 20% (of 273 s) | 9% (of 553 s) |
| frames over 25 ms per minute | 175 | 41 |
| lifted PhysX core per frame, worst 600-frame average | 16.0 ms (max 94 ms) | 0.6 ms (max 31 ms) |

Native cloth: 59 cloths (10 to 1156 vertices), stepping 0.17 ms per frame on the cloth thread (worst block 0.73 ms), game
thread wait at fetch 0.06 ms on average. What remains is not physics: single frames of about 1 s and the level-start
stall (issue #1). Visual correctness not yet confirmed by the user at the time of writing.

## Checks

- Solver on arm64 (sse2neon, `-ffp-contract=off`) vs x86-64 SSE2: bit-identical particle and cloth output on all 13
  scenarios tried (7 fluid, 6 cloth) from upstream `scenarios/`.
- Switches: `ME_PC_SHATTER=0` turns the port off; `PXSHIM_CLOTH=0` leaves cloth to the lifted core. Upstream's `PXSHIM_*` environment switches apply
  (`PXSHIM_FLUID=ours|off`, `PXSHIM_THREADS=n`, `PXSHIM_REF_HZ`, `PXSHIM_DEBUG=1` for the detailed log in the runtime log).
