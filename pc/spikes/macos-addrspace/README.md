# macOS arm64 guest address space spike

Standalone experiment for the PC runtime's 4 GiB guest reservation. It has no runtime dependencies.

## Build and run

Run from the repository root on Apple Silicon / macOS 15.8 with Homebrew LLVM installed:

```sh
/opt/homebrew/opt/llvm/bin/clang -std=c11 -O2 -Wall -Wextra -Werror -pthread \
  pc/spikes/macos-addrspace/addrspace_spike.c \
  -Wl,-stack_size,0x4000000 \
  -o /tmp/macos-addrspace-spike
/tmp/macos-addrspace-spike
```

The linker flag sets the main executable thread's stack to 64 MiB. The pthread case separately sets a 64 MiB worker stack with `pthread_attr_setstacksize`. The program prints `PASS`/`FAIL` per runtime check and reports the host page size, signal number, fault address, and arm64 PC.

## Observed run

Machine: Apple Silicon arm64, Darwin 24.6.0, macOS 15.8; Homebrew clang 23.1.4. Host page size was 16,384 bytes. Output:

```text
host_page=16384 guest_page=4096
PASS: 4 GiB PROT_NONE MAP_PRIVATE|MAP_ANON reservation (reserved 0x100000000 bytes)
FAIL: 4 GiB reservation at fixed low address (requested 0x100000000, got 0x300000000; hint ignored)
FAIL: 4 KB guest commit protection isolation (mprotect 4 KB: Invalid argument; host page=16384; kernel protection granularity is host page)
FAIL: decommit zero-on-recommit semantics (MADV_DONTNEED recommit-zero=no; MADV_FREE immediate-zero=no (lazy, not guaranteed))
PASS: SA_SIGINFO PROT_NONE fault, mprotect and resume (signal=10 (SIGBUS), si_addr=0x1028cc000, arm64 PC=0x102870968, resumed=yes)
PASS: 64 MB pthread stack (pthread_attr_setstacksize(67108864)=ok; create/join=ok; worker touched stack=yes)
main_thread_stack_link_flag: build with -Wl,-stack_size,0x4000000 (64 MiB); see README
```

The exact `si_addr` and PC vary per run because the anonymous mapping is randomized. The probe uses a non-fixed mmap hint for `0x100000000`; the kernel moved it to `0x300000000`. It does not use `MAP_FIXED`, which could replace an existing mapping. This demonstrates that a caller cannot assume a low fixed base is available. Guest pointers encoded as `base + u32_offset` still work as host pointers when the chosen base is above 4 GiB, provided all runtime host-pointer calculations use a sufficiently wide type (`uintptr_t`/`void *`) and only guest addresses remain 32-bit offsets. A 4 GiB contiguous mapping itself succeeded.

## Findings and runtime recommendation

* **Reservation:** A 4 GiB private anonymous `PROT_NONE` mapping succeeds without `MAP_NORESERVE`. The Linux `MAP_NORESERVE` flag is not part of the macOS contract and should not be copied into the Darwin implementation. Treat `mmap(NULL, size, ...)` as an arbitrary host base; no low-address placement is required by the base-plus-u32 guest pointer model.
* **Commit granularity:** With 16 KiB host pages, `mprotect(base + 4096, 4096, RW)` failed with `EINVAL`. Protection changes are host-page granular, so a four-page guest group cannot independently be reserved/committed/protected at 4 KiB granularity. Track guest commit state per 4 KiB page for guest API answers, plus host protection/ownership state per 16 KiB group. When any guest subpage in a 16 KiB group is committed, make the entire aligned host page RW; keep other guest subpages logically uncommitted in metadata. This makes neighboring uncommitted guest subpages physically accessible, so enforce guest access restrictions in software if strict Windows `PAGE_NOACCESS` behavior is required; host `mprotect` alone cannot represent it.
* **Decommit and zeroing:** This run did not observe zeroes after either `MADV_DONTNEED` or `MADV_FREE`, so neither advice alone meets the guest's zero-on-recommit contract on this configuration. A robust implementation should zero the entire rounded 16 KiB host page when a guest commit transitions from no committed subpages to some committed subpages (or when decommit retires the last committed subpage), before exposing it as RW. `MADV_FREE` is lazy and must not be used as the source of zeroing semantics. For full-group decommit, protect the host page with `PROT_NONE`; for a partial-group decommit, retain RW for the still-committed neighbors and zero the decommitted 4 KiB bytes before any later recommit. The commit metadata must ensure recommit cannot expose stale bytes.
* **Faults:** On this macOS 15.8 arm64 run, the `PROT_NONE` read raised signal 10 (`SIGBUS`), not `SIGSEGV`; a `SA_SIGINFO` handler that `mprotect`ed the aligned host page RW returned and the load resumed. Install handlers for both `SIGBUS` and `SIGSEGV`. `si->si_addr` contains the faulting host address. In arm64 macOS `ucontext_t`, the saved thread state is reached through `uc->uc_mcontext->__ss`; the faulting instruction address is `uc->uc_mcontext->__ss.__pc`. The handler can inspect that PC for diagnostics or runtime recovery policy. These fields are Darwin arm64-specific and should be guarded accordingly. The sample demonstrates resuming a retriable protection fault, not a general-purpose async-signal-safe recovery design.
* **Threads/stacks:** A pthread with a 64 MiB stack was created and joined successfully. Set worker stack size in the pthread attributes. The tested linker option `-Wl,-stack_size,0x4000000` requests a 64 MiB main-thread stack for this executable.

### Concrete `rt_host_macos.c` direction

Use `mmap(NULL, 0x100000000, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0)` and keep the returned pointer as `uintptr_t`/host pointer; do not require a fixed base or `MAP_NORESERVE`. Round host protection operations to 16 KiB boundaries and maintain a count or bitmap of the four 4 KiB guest pages in each host page. Commit should zero newly exposed guest bytes and make a host page RW if needed. Decommit should zero the decommitted guest bytes, preserve RW while any sibling guest page remains committed, and apply `PROT_NONE` only when all four are decommitted. Install `SA_SIGINFO` handlers for both `SIGBUS` and `SIGSEGV`, record `si_addr` and arm64 `__pc`, and use an alternate signal stack. Configure new pthread stacks with attributes; add the link stack-size flag for the main thread if the runtime needs that capacity.

The test exercises OS behavior directly. It does not change `pc/runtime` or claim that software bookkeeping for partial 4 KiB permissions is already implemented.
