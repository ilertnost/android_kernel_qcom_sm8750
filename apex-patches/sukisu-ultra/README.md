# SukiSU Ultra + SUSFS

## What is in this directory

| File | Purpose |
|---|---|
| `FIXES.md` | Why the branch tip needed three fixes, with the error messages. |
| `sukisu_apex_fixes.patch` | Diff against SukiSU commit `70fa0e09`: the two source fixes plus the version fallback. |
| `arch.h` | File deleted from the `builtin` branch, restored verbatim from `ebf6294e`. |
| `50_add_susfs_in_gki-android15-6.6.patch` | The kernel-side SUSFS patch, from `simonpunk/susfs4ksu` branch `gki-android15-6.6`. |

## Why SukiSU Ultra and not KernelSU-Next

KernelSU-Next keeps SUSFS out of tree, so it needs the third-party
`10_enable_susfs_for_ksu.patch` plus eight WildKernels fix patches. Those are
written against a specific KernelSU-Next commit and stop applying as soon as
the branch moves. Two attempts to apply them failed outright.

SukiSU Ultra's `builtin` branch carries its own ten `CONFIG_KSU_SUSFS_*`
symbols in `kernel/Kconfig` and handles them in `kernel/Makefile`. No external
patch is needed, so there is no version coupling to get wrong.

## Pinned commit

SukiSU Ultra has no release tag on the `builtin` branch. Every tag
(`v4.2.0`, `v4.1.3`, `v4.0.0`) lives on `main` and none of them is an ancestor
of `builtin`, so pinning to a tag would lose the GKI built-in integration
entirely. `kernel/setup.sh` accepts any ref `git checkout` resolves, so the
build pins a commit instead:

```
70fa0e092a2c81060823f8ae526eac14fdda2930
```

## Reproducing

From the kernel root (`common/`):

```sh
# 1. SukiSU Ultra, pinned
curl -LSs "https://raw.githubusercontent.com/SukiSU-Ultra/SukiSU-Ultra/builtin/kernel/setup.sh" \
  | bash -s 70fa0e092a2c81060823f8ae526eac14fdda2930

# 2. the three fixes (see FIXES.md)
cp /path/to/this/arch.h                      KernelSU/kernel/arch.h
patch -p1 -d KernelSU < /path/to/this/sukisu_apex_fixes.patch

# 3. SUSFS, kernel side
git clone --depth=1 -b gki-android15-6.6 https://gitlab.com/simonpunk/susfs4ksu.git
cp susfs4ksu/kernel_patches/fs/*                     fs/
cp susfs4ksu/kernel_patches/include/linux/*         include/linux/
cp susfs4ksu/kernel_patches/50_add_susfs_in_gki-android15-6.6.patch .
patch -p1 --forward < 50_add_susfs_in_gki-android15-6.6.patch
```

Four prepatches are required before SUSFS applies. On 6.6.139 all four already
hold, so each is a no-op, but they are checked rather than assumed:

- `fs/proc/task_mmu.c` — declare `nr_subpages` and `res` after `int ret = 0, copied = 0;`
- `fs/proc/base.c` — `#include <linux/dma-buf.h>` after `<linux/cpufreq_times.h>`
- `mm/memory.c` — `#include <linux/zswap.h>` after `<linux/sched/sysctl.h>`
- `fs/proc/task_mmu.c` — brace the `if (vma->vm_end > last_vma_end)` body

## Verify before building

```sh
grep -q "SUSFS_IS_INODE_SUS_MAP(file_inode(vma->vm_file))" fs/proc/task_mmu.c \
  || { echo "SUS_MAP guard missing"; exit 1; }
```

This is the only check in the reference workflow that can actually fail the
build. Everything else there tolerates a reject.

The build log should then report:

```
-- SukiSU-Ultra version: 40959 [v4.2.0-SukiSU-Ultra-70fa0e09@apexfoundry]
-- SukiSU-Ultra: using SUSFS_INLINE_HOOK
-- SukiSU-Ultra/compat: modern static_key_interface found
```

## Config

Only the eleven symbols SukiSU actually defines are set. The reference
workflow also writes `CONFIG_KSU_MANUAL_HOOK`, `CONFIG_KSU_SUSFS_HAS_MAGIC_MOUNT`,
`CONFIG_KSU_SUSFS_TRY_UMOUNT`, `CONFIG_KSU_SUSFS_SUS_OVERLAYFS`,
`CONFIG_KSU_SUSFS_SUS_SU` and three more `AUTO_ADD_*`. None of them exists in
SukiSU's Kconfig, and neither the SUSFS patch nor `fs/susfs.c` references
them, so kconfig drops them with a warning. They are leftovers from the
KernelSU-Next path.

## Not vendored

`KernelSU/` is 65 MB and is fetched by its own `setup.sh` at a pinned commit,
so it is gitignored here rather than committed. Reproduce with step 1 above.
