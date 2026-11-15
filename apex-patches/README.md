# Patch provenance

These are the exact patch sources applied in this branch. They are included so
every source change in the tree can be traced back to an upstream patch instead
of being taken on trust.

## optimizations/

24 patches from `wildkernels_patches/common/`, maintained and curated by
[palazik](https://github.com/palazik). They are not maintained here and carry
their original authors in the `Signed-off-by:` trailers.

An earlier revision of this file recorded `clear_page_16bytes_align.patch` as
excluded because `arch/arm64/lib/clear_page.S` was believed to differ between
6.6 and 6.7. That was incorrect: the patch applies cleanly to this tree with no
offset or fuzz, inserting `.p2align 4` ahead of
`SYM_FUNC_START(__pi_clear_page)`. All 24 are applied.

## ntsync/

`ntsync_base.patch` and `ntsync_compat_android15-6.6.patch`, from
[palazik/kernel_patches](https://github.com/palazik/kernel_patches).
NTsync emulates Windows NT synchronization primitives and is required by Wine
and Proton. Driver by Elizabeth Figura (CodeWeavers), GPL-2.0. Enabled with
`CONFIG_NTSYNC=y`.

## droidspaces/

Two compatibility fixes from `wildkernels_patches/common/droidspaces/`:

- `fix_sysvipc_kabi_6_7_8.patch`
- `0001-Return-ghost-task-if-task-is-null-and-is-requested-b.patch`

The matching `CONFIG_` options were added to `arch/arm64/configs/gki_defconfig`
in the same commit that applies the source changes.

## adios/

The ADIOS I/O scheduler, two patches. Predicts a device class per I/O rather
than applying one static policy. Enabled with `CONFIG_MQ_IOSCHED_ADIOS=y`.

## unicode/

Empty UTF-8 decomposition fix. Without it, cursor state is corrupted in F2FS
normalization.

## sukisu-ultra/

**Legacy. Used through v1.1 only; v1.2 does not use this.** Kept for reference.

Root and stealth for SukiSU Ultra. `50_add_susfs_in_gki-android15-6.6.patch` is
the kernel-side SUSFS integration for the `builtin` branch, byte for byte what
[simonpunk/susfs4ksu](https://gitlab.com/simonpunk/susfs4ksu) ships for
`gki-android15-6.6`. `sukisu_apex_fixes.patch` carries source fixes and a
version pin.

### sukisu-ultra/main/

What v1.1 built against: SukiSU Ultra's `main` branch. `main` reports KSU UAPI 5
where `builtin` is stuck at 2, and 2 was what people reported as too old. It
does not build SUSFS in, so SUSFS came from here instead.

`10_enable_susfs_for_ksu-main.patch` is upstream `susfs4ksu` plus five
corrections, each explained in that directory's README. Both patches apply to a
clean `origin/main` with no failures, and the result was compared file by file
against the tree that produced the tested image: 28 files, no differences.

## kernelsu/

**This is what v1.2 uses.** The driver is upstream
[`tiann/KernelSU`](https://github.com/tiann/KernelSU) `main`, with SUSFS grafted
in from `susfs4ksu`'s `gki-android15-6.6` tree.

The swap was forced by SUSFS, not by taste: it is a patch set and is only kept
against one root implementation at a time, and by v1.2 `susfs4ksu` had moved to
upstream KernelSU. Upstream is also the base the patch records as its pre-image
(`kernel/Kbuild` blob `6c92a013`), so `10_enable_susfs_for_ksu.patch` applies
with 0 of 97 hunks failing.

KernelSU-Next `dev`, the original plan, was rejected on evidence: it has no
in-tree SUSFS. It vendors SUSFS as its own loadable module and the KSU core
calls into it, so it cannot provide "KSU plus built-in SUSFS".

Both susfs4ksu patches, the reproduction steps, and the three build fixes -
`selinux_hide.c` (a `-Werror,-Wpointer-bool-conversion` on tests around real
functions, plus an `else` branch calling a `static` symbol), the `Kbuild`
version fallback (kleaf has no `.git`, so the manager read the sentinel 16), and
the SukiSU-only `__ksu_is_allow_uid_for_current` declaration in `fs/exec.c` -
are documented in `kernelsu/README.md`.

## isa/

`0001_target_oryon_isa.patch` sets `-march=armv9.2-a+crypto+dotprod` in
`build.config.gki`. The kernel previously carried no `-march` at all, so clang
defaulted every translation unit to `armv8-a`.

The same value is set as `KBUILD_CFLAGS` in `arch/arm64/Makefile`, which is what
the plain `make` build in `README.md` reads. With the integrated assembler
(`LLVM_IAS=1`) the architecture comes from the compiler, so the separate
`-Wa,-march=armv8.5-a` is not passed.

`armv9.2-a` defines `__ARM_FEATURE_SVE` and `__ARM_FEATURE_SVE2`. Kernel C code
is compiled with `-mgeneral-regs-only`, which keeps the vectoriser off: with
that flag a float loop compiles without SVE instructions, so no `z` register is
written.

## kmi/

`0002_relax_gki_module_kmi.patch` adds `CONFIG_APEX_RELAX_GKI_MODULE_KMI`,
which lets unsigned modules import any exported symbol instead of only those on
the generated unprotected list. It is a Kconfig switch, visible in `.config`,
and reverting is one line in `gki_defconfig`.

Ten stock modules were refused at every boot: `bluetooth`, `hci_uart`,
`btbcm`, `btqca`, `btsdio`, `hidp`, `rfcomm`, `rtl8150`, `9pnet_fd` and
`ptp_kvm`. The first nine now load. `ptp_kvm` cannot work on a phone without a
hypervisor and still fails, which is correct.

Not caused by anything in this repository. The KMI lists arrived with the AOSP
6.6.139 base and no commit since has touched them. The stock modules are built
for 6.6.118 and the 6.6.139 list no longer carries every symbol they import.

## versioning/

`0003_derive_version_from_kconfig.patch` restores the upstream
`scripts/setlocalversion` and moves the version to `CONFIG_LOCALVERSION`.

The last line of `setlocalversion` had been replaced with a hardcoded `echo`,
so the reported version bore no relation to what was built, and it spelled out
`maybe-dirty` unconditionally. On a clean tree that was a false claim.

Worth being precise about where `maybe-dirty` came from: without
`--config=stamp`, kleaf's `_write_localversion` in `stamp.bzl` writes exactly
that into a `localversion` file. Our builds do pass `--config=stamp`. The
hardcoded `echo` asserted it regardless of both.

## combined/

`apex-foundry-all.patch` is the whole change set as one diff against a clean
AOSP `android15-6.6` 6.6.139 tree. 188 files. Verify it with:

```bash
git apply --check apex-patches/combined/apex-foundry-all.patch
```

It predates the patches above. Regenerate it if you need it current; as it
stands it does not include `bore/sysctl_min_base_slice.patch` either.
## bore/

`bore.patch` — the Burst-Oriented Response Enhancer CPU scheduler, from
[palazik/actions_oplus_sm8750](https://github.com/palazik/actions_oplus_sm8750/blob/master/patches/bore.patch)
(BORE 5.9.7, Masahito Suzuki). It originated on the LineageOS QCOM sm8750 base
and was carried along by the rebase.

**Status: applied.** `CONFIG_SCHED_BORE=y`, re-enabled in commit `ae5ddacaaea3`
("sched: re-enable BORE, and fix a null-check in the memfd ashmem shim"). The
tree carries it: `kernel/sched/bore.c` is byte for byte what `bore.patch`
produces. That was checked with a script, not assumed.

It is a one-way port. Reverse-applying `bore.patch` does not work cleanly:
`kernel/sched/core.c` diverges, and by the record above three hunks were
finished by hand (`fair.c` tunable-scaling block, `core.c` `sched_bore_init()`
call block, and `include/linux/sched/bore.h`, which the local tree had modified
to guard `BORE_MIN_BASE_SLICE_NS` behind `#ifndef`).

### sysctl_min_base_slice.patch — our delta, not from palazik

BORE publishes every knob at `/proc/sys/kernel/` via
`register_sysctl_init("kernel", sched_bore_sysctls)` - except the minimum
preemption slice, which only ever existed in debugfs
(`/sys/kernel/debug/sched/min_base_slice_ns`, `kernel/sched/debug.c`). Stock
Android does not mount debugfs, so userspace cannot reach it. That is the whole
reason this delta exists.

It adds two entries to `sched_bore_sysctls[]`:

- `min_base_slice_ns` (0644) - the request. Its handler calls
  `sched_update_min_base_slice()`; without that, writing the variable would not
  take effect until the next recompute, which is exactly what the debugfs file
  does.
- `base_slice_ns` (0444) - the value the scheduler actually uses.

The second one keeps the first honest. BORE rounds the request up to a whole
tick:

    sysctl_sched_base_slice = nsecs_per_tick *
        max(1, DIV_ROUND_UP(sysctl_sched_min_base_slice, nsecs_per_tick))

and at `CONFIG_HZ=250` a tick is 4 ms. So the request is quantised to 4 ms and
cannot go below it: 2 ms and 4 ms both produce a 4 ms base slice, and only a
request above 4 ms changes anything. `base_slice_ns` exposes that, so a tuner can
show the effective value instead of the requested one.

It applies on top of a tree that already carries `bore.patch`.

## erofs-margin/

**This is the fix for the HyperOS 4 boot crash.**

`0001-erofs-fix-the-LZ4-inplace-decompression-margin.patch` (commit
ca252d6aebd3). Our tree used the upstream constant

    #define LZ4_DECOMPRESS_INPLACE_MARGIN(srcsize)  (((srcsize) >> 8) + 32)

in `fs/erofs/decompressor.c`, while the stock HyperOS 4 kernel has it as a
function with a `__setup` boot parameter:

    z_erofs_get_lz4_decompress_inplace_margin

`z_erofs_lz4_handle_overlap()` uses it to decide whether LZ4 may decompress
inplace - output written into the same buffer, overwriting compressed source
bytes that have not been read yet - or must take the safe `docopy` path into a
per-CPU buffer. With the smaller upstream constant we take the inplace path too
often, and LZ4 overwrites bytes it has not consumed.

Measured on the device, with the STOCK vendor_boot:

    before: md5sum /system/lib64/libminikin.so = bf99ed78bc649a2ef945f2c576a94b41
    after:  md5sum /system/lib64/libminikin.so = 3b3f0ed661cc9a5ffcfdcd4dc866ea86  (stock value)

and the device boots. The 2409 corrupted bytes were confined to offsets
0x754fd..0x75fcd, inside .eh_frame/.eh_frame_hdr, and the corruption is what
crashed `minikin::FontFamily::FontFamily` and put system_server into a crashloop.

**The value `~0U` in the patch is a diagnostic value, not the final fix.** It
forces every pcluster through the copy path, which costs EROFS read
performance. The proper fix is to port the stock function with its boot
parameter; the exact value it uses has not been read off the running stock
kernel yet, so the patch says so instead of inventing one.

Note the patch is standalone - it applies to `LineageOS/android_kernel_qcom_sm8750`
at ca252d6aebd3, and to any tree carrying the same
`fs/erofs/decompressor.c`.

### v1.2 narrows the margin to `>> 6`

v1.1 used `(srcsize >> 2) + 32` (commit 87685b4af1cb). It booted, but it is
wider than it needs to be, so it forces more clusters down the `docopy` path
than necessary and costs read performance. v1.2 uses `(srcsize >> 6) + 32`
(commit 95279db1c1aa).

The safe band is now bounded on both sides by measurement:

| Margin | Result |
|---|---|
| `(srcsize >> 8) + 32` | upstream value; corrupts `/system/lib64/libminikin.so` (md5 `bf99ed78...`), device crashloops |
| `(srcsize >> 2) + 32` | v1.1; boots, oracle reads `3b3f0ed6...`, but wider than needed |
| `(srcsize >> 6) + 32` | v1.2; boots, oracle reads `3b3f0ed6...` |

The oracle is `adb shell su 0 md5sum /system/lib64/libminikin.so`, whose correct
value is `3b3f0ed661cc9a5ffcfdcd4dc866ea86`, measured on the stock vendor_boot
(md5 `49193512e0e1ef61d0ab9efbdcc53524`). `>> 6` is the most inplace use per
pcluster measured safe, and nothing beyond that is claimed. The build of `>> 6`
was confirmed booting on device by the maintainer.

Everything from `>> 8` through `>> 6` is still a guess at the *stock* value.
Stock's `fs/erofs/decompressor.c` has this as a function with a `__setup` boot
parameter (`z_erofs_get_lz4_decompress_inplace_margin`) rather than a constant,
and that function has not been read off the running stock kernel. Porting it is
the proper end state; until then these are measured constants, not the stock
ones.

## bbrv3 (applied, then reverted - not carried)

BBRv3 was backported from `palazik/kernel_patches`
(`common/bbrv3/0001-net-tcp-backport-BBRv3-to-android15-6.6.patch`,
fatalcoder524, upstream commit `3bf381592fa3`) and made the default congestion
control for one build. It boots and `bbr3` registers and is used, but on device
it made mobile data several times worse, so it is reverted in full - including
the ECN changes it carries in `net/ipv4/tcp_output.c`, which are not
bbr3-only. No patch files are carried here; this note exists so the experiment
is not repeated blindly.

Two defects in that patch were found while it was applied, worth knowing if it
is ever revisited: `0001` writes the comment `/* Caller made space either
from:->` with a stray `->`, and four lines carry trailing whitespace or a space
before a tab.

## thinlto (config only, no patch)

`CONFIG_LTO_CLANG_THIN=y` replaces `CONFIG_LTO_NONE=y` in
`arch/arm64/configs/gki_defconfig`. There is no patch to vend, but it belongs
here because it changes code generation for the whole tree. v1.1 and v1.2
shipped `LTO_NONE` on the grounds that ThinLTO "slowed the read path"; that
statement was never backed by a measurement, so v1.3 measures it instead of
repeating it. Numbers are in CHANGELOG 1.3; it is carried without a claimed
gain.
