# Changelog

Newest first. Each entry states what was verified and, where relevant, what was
not.

## v1.3 (6.6.144)

- **f2fs VIP search rewritten - it could hang the kernel.** `f2fs_vip_bsearch()`
  spun forever when asked for an inode below `sorted[0]`: `hi = count - 1`
  combined with `hi = mid - 1` on unsigned ints underflows to `UINT_MAX`, so
  `(int)(hi - lo)` is -1, the `diff++` correction turns that back into 0, and
  `mid` and `lo` stay 0 forever. All of it ran with `vip->lock` held under
  `spin_lock_irqsave()`, i.e. with IRQs off, so the CPU wedged and every other
  `f2fs_iget()` blocked on the same lock. It took 16 pins to trigger: the first
  15 only land in the unsorted ring, the 16th flushes the ring into `sorted[]`,
  and from then on a lookup of an older inode hangs - which is the "panic after
  installing several DSU partitions" report. Fix by 3aTup0k (pull request #2):
  a plain `lower_bound` over `[0, count)`, the unsorted ring dropped (its
  entries were invisible to the search anyway, so the first 15 pinned nids were
  never reported as VIP), `sorted[]` grown before the lock is taken instead of
  `kvmalloc(GFP_KERNEL)` inside it, and `vip.enabled` honoured in insert and
  lookup.
- **SUSFS integration refreshed** to `susfs4ksu` `gki-android15-6.6` @
  `2d11d711` (2026-10-14), and the build instructions rewritten for the plain
  `make` + clang method. Both landed after the v1.2 tag; the SUSFS detail is in
  the v1.2 section below.
- **`CONFIG_TRIM_UNUSED_KSYMS` is now explicitly off.** No behaviour change -
  the Kconfig default is already off - but the intent is in the file now, and
  it matters because the stock vendor modules link against the full ksymtab.
- **ThinLTO is on.** v1.1 removed it on the statement that it "slowed the read
  path"; that statement was never backed by a number. v1.3 measures it instead
  of repeating it, by building one source twice - `CONFIG_LTO_CLANG_THIN=y`
  against `CONFIG_LTO_NONE=y`:

  | same source, clang r510928, 16 cores | LTO_NONE | ThinLTO | delta |
  |---|---|---|---|
  | `.text` | 17 608 704 | 17 924 096 | +315 392 (+1.79%) |
  | `.rodata` | 6 077 078 | 6 219 438 | +142 360 |
  | `.init.text` | 411 104 | 435 128 | +24 024 |
  | `Image` | 37 075 456 | 38 189 568 | +1 114 112 (+3.01%) |
  | build wall time | 4m13s | 4m56s | +43s |
  | per-file objects | ELF relocatable | LLVM IR bitcode | |

  ThinLTO makes this kernel larger, not smaller, so **no performance gain is
  claimed for it** - it is carried on the same footing as BORE. Both variants
  report which one they are: `zcat /proc/config.gz | grep LTO`.
- **BBRv3 was tried and rejected on device evidence.** The backport from
  `palazik/kernel_patches` (`common/bbrv3/0001-net-tcp-backport-BBRv3-to-android15-6.6.patch`,
  fatalcoder524, upstream commit `3bf381592fa3`) was applied and made the
  default congestion control (`CONFIG_TCP_CONG_BBR3=y`, `CONFIG_DEFAULT_BBR3=y`).
  The kernel boots, `bbr3` registers and is used. Mobile data then became
  several times worse, so the patch is reverted in full - including the ECN
  changes it makes in `net/ipv4/tcp_output.c`, which apply to every socket, not
  only to bbr3. `cubic` is the default again, as in v1.1 and v1.2; plain `bbr`
  stays available. The experiment and the two defects found in that upstream
  patch are recorded in `apex-patches/README.md`.
- **The kernel is now built for the Oryon ISA.** It carried no `-march` at all,
  so clang defaulted every translation unit to `armv8-a`. The flag is
  `-march=armv9.2-a+crypto+dotprod`, and it sits in the source
  (`arch/arm64/Makefile`), not in the build command, so a plain `make` build
  gets it. `armv9.2-a` turns on LSE atomics, CRC32, AES, SHA2, dotprod and
  i8mm, all of which the SM8750's Oryon cores have. It also defines
  `__ARM_FEATURE_SVE` and `__ARM_FEATURE_SVE2`; kernel C code is compiled with
  `-mgeneral-regs-only`, which keeps the vectoriser off, so no `z` register is
  written. The separate `-Wa,-march=armv8.5-a` was dropped - with the
  integrated assembler the architecture comes from the compiler, so it was a
  second, redundant ISA flag. `Image`: 38 189 568 -> 37 861 888 B. No
  performance claim is made; the on-device comparison is open.
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
