# Root: upstream KernelSU with in-tree SUSFS

v1.2 replaces SukiSU Ultra with upstream
[`tiann/KernelSU`](https://github.com/tiann/KernelSU) `main`, with SUSFS
grafted in from [`simonpunk/susfs4ksu`](https://gitlab.com/simonpunk/susfs4ksu),
`gki-android15-6.6` tree.

## Why upstream KernelSU and not KernelSU-Next

The plan for v1.2 was KernelSU-Next `dev`. It was rejected on evidence.

KernelSU-Next `dev` has **no in-tree SUSFS**. It vendors SUSFS as its own
loadable module under `kernel/susfs/` (`obj-m += susfs_guard_lkm.o`) and wires
its KSU core to call into that module - `susfs_supercall_init`,
`susfs_kstat_init`, `susfs_sus_mount_init`, `susfs_show_version` and about
ninety more symbols. None of those exist in `fs/susfs.c`, so "KernelSU-Next
with built-in SUSFS" is not a combination that branch supports. KernelSU-Next
also fails to build against the susfs4ksu patches.

Upstream `main` is the base those patches are written against. Its
`kernel/Kbuild` blob is `6c92a0130cdc`, which is exactly the pre-image recorded
in `10_enable_susfs_for_ksu.patch`, and that patch applies with **0 of 97
hunks failing**.

Vendored at commit `0c0fdb96` of `tiann/KernelSU` `main`, which is tag
`v3.3.1`, the current release (2680 commits, reported as KSU_VERSION 32680).
The driver tree is byte-identical at `v3.3.1` and at the current `main` tip
(`200818ca`, 2683 commits): between them only `manager/` and `userspace/ksud`
moved, so there was nothing to update on the kernel side.

The SUSFS patches are `susfs4ksu` branch `gki-android15-6.6`, commit
`2d11d711de80f0f749b3055f8da8f7b9682454e4` (2026-10-14), and are byte-for-byte
what that branch ships. `fs/susfs.c`, `include/linux/susfs.h` and
`include/linux/susfs_def.h` are identical between that commit and the earlier
one vendored here, so the SUSFS version did not change - only the integration
did. That revision moved 605 lines of duplicated SELinux internals out of the
driver and into `security/selinux/ss/services.c`; see "Local changes" below.

## The patches in this directory

| File | What it is |
|---|---|
| `10_enable_susfs_for_ksu.patch` | Upstream susfs4ksu. Adds the SUSFS wiring and the `CONFIG_KSU_SUSFS*` Kconfig menu to the KSU driver. Apply with `a/kernel/` rewritten to `a/drivers/kernelsu/`. |
| `50_add_susfs_in_gki-android15-6.6.patch` | Upstream susfs4ksu. The kernel-side SUSFS integration: hooks in `fs/`, `kernel/`, `mm/`, `security/selinux/hooks.c` and `security/selinux/selinuxfs.c`, plus the `my_security_*` re-implementations of the SELinux internals in `security/selinux/ss/services.c` that those hooks call. |
| `Kbuild.reference` | The driver `Kbuild` as built here, kept because one hunk of it is local: the version pin. |

Both patches are byte-for-byte what susfs4ksu ships. Neither is modified.

## How to reproduce

```bash
# 0. pin the sources
#    KernelSU : tag v3.3.1, commit 0c0fdb96. The kernel/ tree is identical at
#               the current main tip (200818ca), so either works.
#    susfs4ksu: branch gki-android15-6.6, commit 2d11d711

# 1. vendor the driver and its UAPI headers
cp -r <KernelSU 0c0fdb96>/kernel      common/drivers/kernelsu
mkdir -p common/drivers/uapi
cp <KernelSU 0c0fdb96>/uapi/*.h       common/drivers/uapi/
#    include/uapi inside the driver is a symlink to ../../uapi, which points
#    outside the repo once kernel/ lives under drivers/

# 2. SUSFS wiring into the driver
sed -e 's|a/kernel/|a/drivers/kernelsu/|g' \
    -e 's|b/kernel/|b/drivers/kernelsu/|g' \
    10_enable_susfs_for_ksu.patch > /tmp/10.patch
patch -p1 -d common < /tmp/10.patch

# 3. kernel-side SUSFS hooks
patch -p1 -d common < 50_add_susfs_in_gki-android15-6.6.patch

# 4. the patch does not create these three files - copy them in
cp <susfs4ksu>/kernel_patches/fs/susfs.c               common/fs/
cp <susfs4ksu>/kernel_patches/include/linux/susfs.h    common/include/linux/
cp <susfs4ksu>/kernel_patches/include/linux/susfs_def.h common/include/linux/

# 5. pin the version in drivers/kernelsu/Kbuild - the one local change, below
```

Then enable `CONFIG_KSU=y` and the ten `CONFIG_KSU_SUSFS*` options in
`arch/arm64/configs/gki_defconfig`.

## Local changes to the vendored driver

Exactly one is needed with the current patches.

### 1. `drivers/kernelsu/Kbuild` - the version pin

The version falls back to a sentinel when the git-derived path does not fire:

```
ccflags-y += -DKSU_VERSION=16
```

kleaf builds from a sandboxed copy of the tree with no `.git`, so the
git-derived path never runs and the manager reads 16 - which its own numbering
treats as an ancient kernel. It is pinned to the vendored revision instead:

```
KSU_VERSION ?= 32680   # 30000 + 2680 commits
```

### 2. `feature/selinux_hide.c` - obsolete with this patch revision

The **earlier** `10_enable_susfs_for_ksu.patch` carried its own copy of the
SELinux internals (`string_to_context_struct`,
`security_context_to_sid_with_policy`, ... 605 lines) in the driver, and
guarded two calls with `if (fn)`. Those names are real functions in this tree,
so the test is always true and clang refuses it:

```
error: address of function 'security_dump_masked_av_fn' will always evaluate
       to 'true' [-Werror,-Wpointer-bool-conversion]
```

The second guard was worse: its `else` branch called `context_struct_compute_av`
directly, and that symbol is `static` in `services.c`, so it would not link.
Both guards were removed.

The current revision drops those 605 lines from the driver entirely and declares
the functions in `services.c` instead (`my_security_*`). There is no `if (fn)`
left to fix, so that local change is gone with it.

### 3. `fs/exec.c` - a leftover, not a patch fix

`fs/exec.c` carried `extern bool __ksu_is_allow_uid_for_current(uid_t uid);`
from the earlier SukiSU-era integration, not from the susfs4ksu patch. The
symbol is defined in `drivers/kernelsu/policy/allowlist.c` and upstream's own
code declares and calls it in `policy/allowlist.h` and `feature/sucompat.c`;
the patch additionally declares and calls it in `fs/open.c` and `fs/stat.c`.
`fs/exec.c` neither declared it for a caller nor used it, so the stray line was
dropped. Reproducing this tree from the patches does not involve this file.

## What was verified in the built image

```
welcome to KernelSU version 32680
Linux version 6.6.144-android15-8-4k-APEX-Foundry-v1.2
strings Image | grep susfs:   -> present
```

SUSFS is in-tree: no LKM to load, no Magisk module, no `init_boot.img` patch.
