# Root: upstream KernelSU with in-tree SUSFS

v1.2 replaced SukiSU Ultra with upstream
[`tiann/KernelSU`](https://github.com/tiann/KernelSU) `main`, with SUSFS
grafted in from [`simonpunk/susfs4ksu`](https://gitlab.com/simonpunk/susfs4ksu),
`gki-android15-6.6` tree.

v1.4 moves both to their current revisions: KernelSU `main` at `6d31a613` and
the SUSFS patches at `4a4cb1b`, which is the susfs4ksu commit that syncs its
patch set with the official KernelSU `main`. The previous pair (KernelSU
`0c0fdb96` = tag v3.3.1, susfs4ksu `2d11d711`) is what v1.2..v1.3 shipped.

## Why upstream KernelSU and not KernelSU-Next

The plan for v1.2 was KernelSU-Next `dev`. It was rejected on evidence.

KernelSU-Next `dev` has **no in-tree SUSFS**. It vendors SUSFS as its own
loadable module under `kernel/susfs/` (`obj-m += susfs_guard_lkm.o`) and wires
its KSU core to call into that module - `susfs_supercall_init`,
`susfs_kstat_init`, `susfs_sus_mount_init`, `susfs_show_version` and about
ninety more symbols. None of those exist in `fs/susfs.c`, so "KernelSU-Next
with built-in SUSFS" is not a combination that branch supports. KernelSU-Next
also fails to build against the susfs4ksu patches.

## The pinned revisions

Upstream `main` is the base those patches are written against. At `6d31a613`
its `kernel/Kbuild` blob is `ee5af0ba`, which is exactly the pre-image recorded
in `10_enable_susfs_for_ksu.patch`, so that patch applies with **no rejects**.

Vendored at commit `6d31a613` of `tiann/KernelSU` `main` (2702 commits,
reported as KSU_VERSION 32702).

The SUSFS patches are `susfs4ksu` branch `gki-android15-6.6`, commit
`4a4cb1b2` ("KernelSU: Sync with the official KernelSU main repo"), and are
byte-for-byte what that branch ships. Compared with the revision vendored in
v1.2..v1.3 (`2d11d711`), the kernel-side patch changed as follows:

- SUSFS hooks now cooperate with KernelSU's `preinit` feature: `fs/exec.c`
  gained `ksu_setup_preinit_file()` / `ksu_preinit_handle_post_execveat()`
  calls, all under `#ifndef KSU_NO_PREINIT`;
- the 605 lines of duplicated SELinux internals are no longer added to
  `security/selinux/ss/services.c`; the driver calls `my_setprocattr()` from
  `security/selinux/hooks.c` instead, so `services.c` is no longer patched at
  all;
- one hunk carries a post-6.6 context line (`vma_data_pages()` instead of
  `vma_pages()`); this tree is 6.6, so the 6.6 spelling is kept and that hunk
  is applied with one line of fuzz.

`fs/susfs.c`, `include/linux/susfs.h` and `include/linux/susfs_def.h` are
copied in from the same commit and are byte-identical to it.

The UAPI headers moved with KernelSU: `drivers/uapi/supercall.h` is at
`KERNEL_SU_UAPI_VERSION = 6` (`KSU_IOCTL_LOOKUP_SYMBOLS`,
`KSU_GET_INFO_FLAG_PREINIT`, preinit/modules.rc under `/metadata/vold/ksu`).
The other five headers (`app_profile.h`, `feature.h`, `ksu.h`, `selinux.h`,
`sulog.h`) are unchanged from v1.4.

## The patches in this directory

| File | What it is |
|---|---|
| `10_enable_susfs_for_ksu.patch` | Upstream susfs4ksu. Adds the SUSFS wiring and the `CONFIG_KSU_SUSFS*` Kconfig menu to the KSU driver. Apply with `a/kernel/` rewritten to `a/drivers/kernelsu/`. |
| `50_add_susfs_in_gki-android15-6.6.patch` | Upstream susfs4ksu. The kernel-side SUSFS integration: hooks in `fs/`, `kernel/`, `mm/`, `security/selinux/hooks.c` and `security/selinux/selinuxfs.c`. |
| `Kbuild.reference` | The driver `Kbuild` as built here, kept because one hunk of it is local: the version pin. |

Both patches are byte-for-byte what susfs4ksu ships. Neither is modified.

## How to reproduce

```bash
# 0. pin the sources
#    KernelSU : main @ 6d31a613 (2702 commits)
#    susfs4ksu: branch gki-android15-6.6 @ 4a4cb1b

# 1. vendor the driver and its UAPI headers
cp -r <KernelSU 6d31a613>/kernel      common/drivers/kernelsu
mkdir -p common/drivers/uapi
cp <KernelSU 6d31a613>/uapi/*.h       common/drivers/uapi/
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

Exactly one is needed.

### 1. `drivers/kernelsu/Kbuild` - the version pin

The version falls back to a sentinel when the git-derived path does not fire:

```
ccflags-y += -DKSU_VERSION=16
```

The driver lives inside the kernel repository here, so `GIT_ROOT` equals the
kernel root and the git-derived path never runs: the manager would read 16,
which its own numbering treats as an ancient kernel. It is pinned to the
vendored revision instead:

```
KSU_VERSION ?= 32702   # 30000 + 2702 commits
```

### 2. `feature/selinux_hide.c`, `fs/exec.c` - fixes from the v1.2 era, now moot

The old `10_enable_susfs_for_ksu.patch` carried its own copy of the SELinux
internals and guarded two calls with `if (fn)`; clang rejected the guard with
`-Werror,-Wpointer-bool-conversion` and the `else` branch called a `static`
symbol. That copy is gone from the patch, so the guard fix is gone with it.

The stray `extern bool __ksu_is_allow_uid_for_current(uid_t uid);` in
`fs/exec.c` came from the SukiSU-era integration, not from the patch; it is
still not added by the current patch revision.

## What was verified in the built image

```
welcome to KernelSU version 32702
Linux version 6.6.144-4k-APEX-Foundry-v1.4
strings Image | grep susfs:   -> present
```

SUSFS is in-tree: no LKM to load, no Magisk module, no `init_boot.img` patch.
