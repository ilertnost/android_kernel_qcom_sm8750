# Changelog

Newest first. Each entry states what was verified and, where relevant, what was
not.

## v1.3 (6.6.144)

- **BBRv3 was tried and rejected on device evidence.** The backport from
  `palazik/kernel_patches` (`common/bbrv3/0001-net-tcp-backport-BBRv3-to-android15-6.6.patch`,
  fatalcoder524, upstream commit `3bf381592fa3`) was applied and made the
  default congestion control (`CONFIG_TCP_CONG_BBR3=y`, `CONFIG_DEFAULT_BBR3=y`).
  The kernel boots, `bbr3` registers and is used. Mobile data then became
  several times worse, so the patch is reverted in full - including the ECN
  changes it makes in `net/ipv4/tcp_output.c`, which apply to every socket, not
  only to bbr3. `cubic` is the default again, as in v1.1 and v1.2. The
  experiment and the two defects found in that upstream patch are recorded in
  `apex-patches/README.md`.
- **ThinLTO enabled, and measured this time.** v1.1 removed it on the statement
  that it "slowed the read path"; that statement was never backed by a number.
  v1.3 builds one source twice - `CONFIG_LTO_CLANG_THIN=y` and
  `CONFIG_LTO_NONE=y` - and puts the difference in the table instead of
  repeating the claim.

  | same source, clang r510928, 16 cores | LTO_NONE | ThinLTO | delta |
  |---|---|---|---|
  | `.text` | 17 608 704 | 17 924 096 | +315 392 (+1.79%) |
  | `.rodata` | 6 077 078 | 6 219 438 | +142 360 |
  | `.init.text` | 411 104 | 435 128 | +24 024 |
  | `Image` | 37 075 456 | 38 189 568 | +1 114 112 (+3.01%) |
  | build wall time | 4m13s | 4m56s | +43s |
  | per-file objects | ELF relocatable | LLVM IR bitcode | |

  ThinLTO makes this kernel larger, not smaller. Whether that costs anything at
  runtime is what the device comparison decides; both variants say which one
  they are, with `zcat /proc/config.gz | grep LTO`.
- Still 6.6.144, still KASAN compiled in and off via `kasan=off`, still all 24
  optimization patches, still BORE.

Root: upstream KernelSU `main` + SUSFS, built in. No `init_boot.img` patching.

## v1.2 (6.6.144)

- **EROFS LZ4 inplace margin narrowed from `(srcsize >> 2) + 32` to
  `(srcsize >> 6) + 32`** (`fs/erofs/decompressor.c`, commit 95279db1c1aa).
  v1.1's value booted but was wider than needed, forcing more clusters down the
  `docopy` path than necessary. The safe band is now bounded on both sides by
  measurement: `>> 8` corrupts `/system/lib64/libminikin.so`, `>> 6` boots it.
  Same oracle as before: `md5sum /system/lib64/libminikin.so` reads
  `3b3f0ed661cc9a5ffcfdcd4dc866ea86` on the stock vendor_boot. Confirmed booting
  on device by the maintainer.
- **Root solution replaced: SukiSU Ultra is gone.** The driver is now upstream
  [`tiann/KernelSU`](https://github.com/tiann/KernelSU) `main` (2680 commits,
  KSU_VERSION 32680), with SUSFS grafted in by the
  [`susfs4ksu`](https://gitlab.com/simonpunk/susfs4ksu) patches for
  `gki-android15-6.6`. The reason is that SUSFS is a patch set and is only kept
  against one root implementation at a time, and by v1.2 `susfs4ksu` targets
  upstream KernelSU, not SukiSU Ultra. Upstream is also the base those patches
  record as their pre-image (`kernel/Kbuild` blob `6c92a013`), so
  `10_enable_susfs_for_ksu.patch` applies with 0 of 97 hunks failing.
- **SUSFS integration refreshed to the current `susfs4ksu` revision**
  (`gki-android15-6.6` @ `2d11d711`, 2026-10-14). The SUSFS code itself
  (`fs/susfs.c` and the two headers) is unchanged between revisions; what moved
  is where the hiding lives. The 605 lines of duplicated SELinux internals that
  the earlier revision carried in the driver's `feature/selinux_hide.c` are now
  in `security/selinux/ss/services.c` as `my_security_*`, called from
  `security/selinux/hooks.c` and `selinuxfs.c`, and the driver is back to the
  feature switch plus the `backup_sepolicy` lifecycle. Both patches are
  byte-for-byte what that branch ships. KernelSU needed no update: its driver
  tree is byte-identical between tag `v3.3.1` (the release, our vendored
  `0c0fdb96`, 2680 commits) and the current `main` tip (`200818ca`, 2683
  commits); only `manager/` and `userspace/ksud` moved on.
- **KernelSU-Next `dev` was the original plan and was rejected on evidence,**
  not on the earlier bad experience: it has no in-tree SUSFS. It vendors SUSFS
  as its own loadable module and wires the KSU core to call into it, so
  "KernelSU-Next with built-in SUSFS" is not a combination that branch
  supports.
- **KSU_VERSION pinned to 32680.** kleaf builds from a sandboxed copy of the
  tree with no `.git`, so the git-derived version never ran and the manager read
  the fallback sentinel `16`, i.e. "ancient kernel".
- **SUSFS stays in-tree.** No LKM to load, no Magisk module, no `init_boot.img`
  patch. The ten `CONFIG_KSU_SUSFS*` options are enabled in `gki_defconfig`.
- Still 6.6.144, still KASAN compiled in and off via `kasan=off`, still all 24
  optimization patches.
- **Battery and heat audited, nothing changed.** The power/thermal/scheduler
  config was diffed against stock's `/proc/config.gz` over the whole
  `power`/`thermal`/`sched`/`rcu`/`idle` space: exactly three keys differ, all
  additions here (ADIOS as the default I/O scheduler, and BORE). No kernel-side
  battery or heat win is claimed, because none was measured. The audit is in
  `findings/POWER_THERMAL_AUDIT.md`.
- **`min_base_slice_ns` and `base_slice_ns` are now sysctls.** BORE exposed every
  knob under `/proc/sys/kernel/` via `register_sysctl_init` except the minimum
  preemption slice, which only ever existed in debugfs - and stock Android does
  not mount debugfs, so nothing in userspace could reach it. Both are now in
  `sched_bore_sysctls[]` (`apex-patches/bore/sysctl_min_base_slice.patch`).
  `min_base_slice_ns` (0644) calls `sched_update_min_base_slice()` on write,
  exactly as the debugfs file did. `base_slice_ns` (0444) is what makes the knob
  honest: BORE rounds the request up to a whole tick, and at `CONFIG_HZ=250` a
  tick is 4 ms, so 2 ms and 4 ms both give a 4 ms base slice and only a request
  above 4 ms changes anything.

Root: upstream KernelSU `main` + SUSFS, built in. No `init_boot.img` patching.

## v1.1 (6.6.144)

- **Rebased onto LineageOS `android_kernel_qcom_sm8750`** (6.6.144,
  ASB-2026-10-05) instead of a vanilla GKI 6.6.139. The base already carries the
  QCOM/sm8750 tree this device uses, so vendor code is not guessed at.
- **Fixed the HyperOS 4 boot crash.** `/system/lib64/libminikin.so` was read
  2409 bytes short (offsets `0x754fd..0x75fcd`, inside `.eh_frame_hdr`), which
  crashed `minikin::FontFamily::FontFamily` and put `system_server` into a
  crashloop. Cause: the LZ4 inplace decompression margin in EROFS -
  upstream's `((srcsize) >> 8) + 32` lets LZ4 overwrite source bytes it has not
  read yet. Changed to `((srcsize) >> 2) + 32`, found by measurement, not by
  reading the stock code.
- **LTO disabled again.** 1.0 enabled ThinLTO to match stock's `+lto`, but that
  slowed the read path for no correctness gain, so v1.1 matches the stock
  choice.
- **f2fs MAIN/EXT hybrid UFS support ported** - the one F2FS config option stock
  had that was still missing.
- **DTS sources for dada vendored into the tree,** so DTB/DTBO no longer has to
  come from the firmware partition.
- **memfd ashmem shim:** fixed a NULL dereference when a dentry has no name.
- `apex-patches/` carried in the kernel tree, with a checker that proves the
  patches are applied by looking for the lines they add.
- 24 optimization patches, all of them.

Root: SukiSU Ultra built in, SUSFS built in - no `init_boot.img` patching.

## v1.0 (6.6.139)

First release built to be a daily driver, on a vanilla AOSP `android15-6.6`
6.6.139 base. It enabled ThinLTO to match stock, took DTB/DTBO from the
firmware partition, and used SukiSU Ultra's `builtin` branch (SUSFS built in).
It shipped with the upstream EROFS margin, which is what v1.1 fixed.
