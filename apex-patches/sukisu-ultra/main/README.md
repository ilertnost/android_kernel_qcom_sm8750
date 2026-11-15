# SukiSU Ultra — `main` branch

Patches for building against the **main** branch of SukiSU Ultra, which is
what a GKI 6.6 build should use.

## Why main and not builtin

| | `builtin` | `main` |
|---|---|---|
| KSU UAPI version | 2 | **5** |
| SUSFS symbols | built in | absent, needs a patch |

The UAPI version is what the manager compares. On `builtin` it is stuck at 2,
and people reported that as too old. On `main` it is 5. Verified on device:
the manager now reports `40959-5` instead of `40959-2`.

The mismatch itself is not gone and cannot be fixed from the kernel side. It
resolves when the manager updates.

## Order of application

```
1. setup.sh 42d7fda3d787b7df90fc440a50bb9c8216a3fdef
2. 10_enable_susfs_for_ksu-main.patch
3. sukisu_main_version_pin.patch   (optional, branding only)
```

## The two patches

| File | Size | What |
|---|---|---|
| `10_enable_susfs_for_ksu-main.patch` | 110 KB, 28 files | SUSFS on main. Upstream `susfs4ksu` `10_enable_susfs_for_ksu.patch` plus five fixes listed below. |
| `sukisu_main_version_pin.patch` | 1.4 KB, 1 file | Pins the version string to `42d7fda3@apexfoundry`. Optional; drop it and the build reports `unknown@unknown`. |

The kernel-side SUSFS patch is unchanged from the `builtin` flow:
`50_add_susfs_in_gki-android15-6.6.patch`, byte for byte what simonpunk
ships for `gki-android15-6.6`. Only the SukiSU-side integration differs
between the two branches.

## The five fixes, and why each was needed

All five are ours. The upstream patch predates `main` by weeks and does not
account for the changes upstream made in the meantime.

**1. `kernel/arch.h` shadowed the real one.** The `builtin` branch keeps its
architecture header at `kernel/arch.h`. On `main` it lives at
`kernel/include/arch.h`. Twelve files do `#include "arch.h"` and resolved to
the builtin copy, which does not define what they need. `su_mount_ns.c`
failed to compile with `PT_REGS_SYSCALL_PARM1` undeclared. Removing the stray
file is part of the patch set — do not carry `builtin`'s copy over.

**2. `selinux_hide.c` tested functions as pointers.**
`security_dump_masked_av_fn` and `context_struct_compute_av_fn` are plain
functions, so `if (security_dump_masked_av_fn)` is always true and the
compiler rejects it under `-Werror`. Replaced with `#ifdef
CONFIG_KSU_SUSFS` plus a fallback, which is what the patch intended.

**3. The patch drops `infra/symbol_resolver.o` but keeps its users.** It
removes the object from `Kbuild` while keeping `cpu_spoof.o` and
`uts_spoof.o`, both of which call `find_kernel_symbol_exact`. The link failed
with an undefined symbol. The object is restored.

**4. Three hunks in `core/init.c` do not apply.** They were written against a
`main` that did not have `ksu_late_loaded`, `ksu_bundled` or the x86 guard
blocks. Applied by hand: those are removed, and `susfs_init()`,
`ksu_sucompat_init()` and `ksu_setuid_hook_init()` are added. The hook
mechanism changes with this — `lsm_hook.c`, `syscall_hook_manager.c`,
`syscall_event_bridge.c` and `tp_marker.c` drop out of the build and
`setuid_hook.c` with `sucompat.c` take over. That is the upstream intent for
`main`, not something we chose.

**5. The version pin lives elsewhere.** `main` keeps version machinery in
`kernel/Kbuild`; `builtin` keeps it in `kernel/Makefile`. That is the whole
reason the pin is a separate patch here.

## Verification

Both patches applied to a clean `origin/main` with zero failures, and the
result was compared file by file against the tree that produced the tested
image:

```
28 files checked, 0 differences
```

Tested on Xiaomi 15 (dada, SM8750), GKI 6.6.139: boots, root works, SUSFS
active.