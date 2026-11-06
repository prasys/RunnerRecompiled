# TODO 1: lost GPU device on level load: destroy margins (start here)

Status: root cause found, a fix that works in testing exists as a work-in-progress patch, and the margin still has
to be tuned. This file is the first item of the next session; `CLAUDE.md` and `pc/MACOS-PORT-PLAN.md` ("Still open",
item 1) point here.

## Symptom

In about a third to half of level loads (the unattended repro below dies near frame 2630-2830), MoltenVK reports
`MTLCommandBuffer execution failed (code 9): Invalid Resource`. Every later submit then fails with
`VK_ERROR_DEVICE_LOST`. The game keeps counting frames, but the window freezes.

## Root cause (measured)

- With `MTL_DEBUG_LAYER=1 MTL_DEBUG_LAYER_ERROR_MODE=nslog DXVK_DEBUG=capture`, Metal names the object:
  "being destroyed while still required to be alive by the command buffer". Seen so far:
  - the game's 1280x720 `D3D9Format::A16B16G16R16F` render-target texture;
  - a 64 MB DXVK memory chunk (`Global buffer (N)`).
- Before the fix, 5 of 5 validation runs showed this.
- A backtrace (temporary logging in DXVK) showed who frees what:
  - images are freed by DXVK's `dxvk-queue` thread once the command list's timeline value is reached;
  - chunks are freed by `DxvkMemoryAllocator::performTimedTasks` (empty-chunk cleanup) on the `dxvk-submit` thread.
- MoltenVK 1.4.2 source (read, not modified):
  - On macOS 15 it uses one Metal residency set for all buffers and textures (`MVKDevice.mm`:
    `residencySets = mvkOSVersionIsAtLeast(15.0, ...)`).
  - Its command buffers are therefore created with `retainedReferences = NO` (`MVKQueue.mm`, `getActiveMTLCommandBuffer`:
    `needsRetain` is false when a residency set exists).
  - `vkDestroyImage`, `vkDestroyBuffer` and `vkFreeMemory` remove the object from the set, commit the set, and release the
    Metal object at once (`MVKDevice.h` `removeResidency`).
  - Timeline semaphores are Metal events signalled from the GPU, which happens before Metal considers the command buffer
    complete.
  - Net effect: Vulkan allows the destroy at that point, but Metal does not.

## What was tried

| DXVK build (`work/dxvk-lost`) | Plain level-load runs that lost the device |
|---|---|
| Patched DXVK as committed (`work/dxvk-textfix`) | about 1/3 to 1/2 (session 2) |
| Per-command-list completion fence | (validation only) texture case gone; chunk case still 2 of 3 |
| Deferred destroy until earlier submissions completed (margin 0) | **4 of 6** |
| Deferred destroy + `DXVK_DEFER_MARGIN=200` | **0 of 5** (all ran past frame 6000) |

`MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS=1` crashes at startup, so it can't be used as a workaround.

### The WIP patch

The patch is `docs/todo/dxvk-defer-destroy-wip.patch`. It applies with `patch -p1` on top of
`pc/dxvk/dxvk-3.1.1-macos.patch`. What it does:

- Adds `dxvk_deferred_destroy.h`.
- Routes the image, buffer, view and memory destroys in `dxvk_memory.cpp` through the deferral.
- Adds a fence-only `vkQueueSubmit2` after each command list. The `dxvk-queue` thread waits on it, then releases destroys
  whose request came at least `margin` submissions before the completed one.
- Adds the option `dxvk.deferResourceDestroy`. Its default (Auto) turns the deferral on only on MoltenVK.
- The margin comes from the env var `DXVK_DEFER_MARGIN` (default 0). That variable is test-only and must not ship as is.

## Open question

Why is margin 0 not enough? There are two candidates:

1. **Out-of-order completion.** Metal completes the empty fence command buffer before earlier command buffers have
   finished, so "fence signalled" does not mean "everything before it is done". If so, a small margin (2-8) is
   enough, and a better fix is to fence every submission and wait for all of them.
2. **Use after the destroy request.** Something references the resource after DXVK asked to destroy it: a stale
   descriptor or argument-buffer entry, or MoltenVK internal state. The margin only hides that. If so, the
   validation layer should still flag it with a large margin. In that case, find the user and file it upstream.

## Next steps

1. **Margin sweep.**
   - Run `DXVK_DEFER_MARGIN` = 2 and 16, five plain runs each. Also do three validation runs at margin 200 and see
     whether any "being destroyed" message remains.
   - Commands: build `work/dxvk-lost` (or apply the WIP patch to a fresh copy), then repeat the repro below with
     `ME_PC_DXVK=<that dylib>`. Copy the dylib elsewhere before rebuilding, because a rebuild under a running game
     swaps the file.
2. **Turn it into a fix.**
   - Replace the env var with either a principled default (e.g. a fixed margin, or "N frames") or a fence per
     submission.
   - Remove the env var, fold the result into `pc/dxvk/dxvk-3.1.1-macos.patch`, and update the plan, `AGENTS.md` and this
     file.
3. **Upstream report.**
   - To MoltenVK: unretained command buffers plus immediate release on destroy, with residency sets.
   - To DXVK, only if candidate 2 turns out to be true.

## Unattended repro (no user needed)

```
ME_PC_DXVK=<dylib> DXVK_LOG_PATH=<out> ME_PC_MOUSE_CAPTURE=0 ME_PC_ASSUME_FOCUS=1 MVK_CONFIG_LOG_LEVEL=2 \
ME_PC_AUTO_KEYS=32,36,40,44,48 work/pc-recomp-textfix/bin-macos/me_pc work/pc/Binaries work/pc-deps work/pc-user
```

- Stop after about 120 s, or as soon as `mvk-error` appears in the log.
- Count `mvk-error` / `Invalid Resource` per run.
- For validation, add `MTL_DEBUG_LAYER=1 MTL_DEBUG_LAYER_ERROR_MODE=nslog DXVK_DEBUG=capture`. With validation on, use
  later key times, e.g. `36,44,52,60,68`, because loading is slower.
- Logs of these runs are in `work/frames-textfix/{lost-*,fix-*,defer-*,margin-*}` (not committed).
