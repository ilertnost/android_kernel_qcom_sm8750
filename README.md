# APEX-Foundry

Custom GKI kernel for the **Xiaomi 15** (codename `dada`, SM8750 / Snapdragon 8 Elite).

Builds on the LineageOS `android_kernel_qcom_sm8750` tree at **6.6.144**
(ASB-2026-10-05, GKI branch `android15-6.6`) and runs the device's
**stock vendor modules unmodified**.

Flavour: `v1.2` — upstream KernelSU main, SUSFS, BORE, memfd ashmem shim, ADIOS, NTsync

---

## What this is

A stock Android 6.6 GKI plus a small build-level change, and nothing else.
No vendor source code is vendored in, and no module is rebuilt or replaced.

The result boots with the modules already present on the device, from
`/vendor_dlkm` (397 modules) and `/system_dlkm` (192 modules), and audio,
Bluetooth, WLAN, haptics and mobile data all work.


### Design philosophy: MISR (Make It Simple, Reliable)

Every optimization, flag and commit goes through the same audit. If it does
not produce a measurable gain, or adds complexity the platform does not
justify, it is dropped. Daily-driver stability and clean code outrank
placebo commits and attractive changelogs.

This has cost us things we wanted. `CONFIG_LD_DEAD_CODE_DATA_ELIMINATION` is
present in the defconfig and is flagged `EXPERIMENTAL` upstream, with its own
help text warning it can produce a *silently broken kernel*. It stays only
because it has been running without incident; it is on the list to remove.

It also decided the root implementation, and the reason was the only kind
MISR accepts: the build did not work.

We started on KernelSU Next and never got it to build. Fixing one error
reliably produced two more, and each fix meant another layer of patching on
top of a kernel we were already patching. In the chat people asked us to drop
it for SukiSU Ultra, and the argument on offer was that SukiSU Ultra's
Material You interface looks better. That argument we rejected, because a
nicer interface is not a reason to change a working baseline. We moved to
SukiSU Ultra anyway, and not for that reason: KernelSU Next would not
compile.

Once there we pinned SukiSU Ultra to a fixed commit and stopped tracking its
branch.

**v1.2 reverses that move, and the reason is the same kind of reason.** SUSFS
is a patch set, and it is only maintained against one root implementation at a
time. By 1.2 it had moved on: `susfs4ksu` is written against upstream
`tiann/KernelSU`, not SukiSU Ultra. So the choice is not "which manager looks
better" but "which root implementation still has a SUSFS that builds". The
answer is upstream KernelSU, which is what v1.2 uses.

KernelSU Next was re-examined for this and rejected on evidence, not on the
earlier bad experience. Its `dev` branch has no in-tree SUSFS: it vendors
SUSFS as a separate loadable module under `kernel/susfs/` and wires its KSU
core to call into it (`susfs_supercall_init`, `susfs_kstat_init`,
`susfs_show_version`, and about ninety more). None of those exist in
`fs/susfs.c`, so "KernelSU Next with built-in SUSFS" is not a combination that
branch supports. Upstream `main` is the base the patches are written against:
its `kernel/Kbuild` blob is `6c92a013`, exactly the pre-image recorded in
`10_enable_susfs_for_ksu.patch`, and that patch applies with zero failed hunks.

## Why a change was needed

Android's GKI has a KMI protection mechanism. Any **unsigned** module that
exports a symbol listed in `android/abi_gki_protected_exports_aarch64` is
refused at load time with `-EACCES`, from `kernel/module/main.c`:

```c
if (!mod->sig_ok && gki_is_module_protected_export(kernel_symbol_name(s)))
        return -EACCES;
```

Xiaomi's vendor modules are unsigned. Thirteen of them were therefore
rejected on every boot, and one of those failures took most of the device
down with it:

```
rfkill.ko          rejected -> btpower, btqca, hci_uart, bluetooth.ko fail
                             -> cfg80211: Unknown symbol rfkill_alloc
                             -> WLAN never comes up
btfm_slim_codec    rejected -> btfmcodec_dev component never registers
                             -> msm_asoc_machine_probe defers forever
                             -> no sound card at all
```

`btpower`, `hci_uart` and `bluetooth` all need `rfkill_alloc`, so the whole
Bluetooth stack went down with it.

### The fix

Disable the check at build level rather than patching it out in C:

- `BUILD.bazel` — drop `protected_exports_list`
- `modules.bzl` — `protected_modules = []`

Vendor modules remain subject to `CONFIG_MODVERSIONS` CRC verification, so
version mismatches are still caught.

## Requirements

Tested on Arch Linux. You will need:

- Android `repo` tool
- ~120 GB free disk (manifest, hermetic prebuilts, bazel cache)
- ~30 GB RAM recommended for the first full build

```bash
sudo pacman -S --needed git bc bison flex python ccache
mkdir -p ~/bin
curl -L https://storage.googleapis.com/git-repo-downloads/repo -o ~/bin/repo
chmod +x ~/bin/repo
export PATH="$HOME/bin:$PATH"
```

## Build

This tree builds with **plain `make` + clang** (no `repo`, no kleaf), the same method as [palazik/actions_oplus_sm8750](https://github.com/palazik/actions_oplus_sm8750).

### Prerequisites (Linux x86_64)

- `git`, `python3`, `make`, `bison`, `flex`, `bc`, `lz4`, `pahole`, `rsync`
- `libelf-dev`, `libssl-dev`
- AOSP clang (prebuilts/clang from this tree work)

### Clone

```bash
git clone https://github.com/ilertnost/android_kernel_qcom_sm8750.git
cd android_kernel_qcom_sm8750
```

### Build (GKI, aarch64)

```bash
export PATH=$(pwd)/prebuilts/clang/host/linux-x86/clang-r510928/bin:$PATH
export LLVM=1 LLVM_IAS=1 ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
O=out
mkdir -p $O
make O=$O gki_defconfig
make -j$(nproc) O=$O Image
```

The kernel image will be at `out/arch/arm64/boot/Image`.

Notes:
- This tree's `gki_defconfig` already includes KSU+SUSFS, BORE, KASAN/KFENCE, and the KMI relax (`CONFIG_APEX_RELAX_GKI_MODULE_KMI=y`) — no extra `.raw` files or local tweaks are needed.
- KASAN is compiled in and can be disabled at boot with `kasan=off`.



## Flashing

The released zip is an AnyKernel3 package. Flash it from recovery, or with
KernelFlasher / any AK3-compatible flasher. It writes the `boot` partition
only, via `magiskboot`.

Verify after boot:

```bash
adb shell uname -r
adb shell 'dmesg | grep -c "protected symbol"'   # expect 0
adb shell cat /proc/asound/cards                 # expect sunmtpsndcard
adb shell ls /sys/class/bluetooth/               # expect hci0
adb shell 'ip -br link | grep wlan'               # expect wlan0
```

## Root and stealth

**Upstream KernelSU** (`tiann/KernelSU`, `main`) built into the kernel, plus
**SUSFS**. No module to install and no boot image patch — `uname -r` reports
`APEX-Foundry-v1.2` and the manager talks to it directly.

v1.2 moved off SukiSU Ultra. The deciding fact is that SUSFS is a patch set and
is only kept against one root implementation at a time; by 1.2 `susfs4ksu` had
moved on to upstream KernelSU. Upstream is also the base the patch is written
against — `kernel/Kbuild` in `tiann/KernelSU` `main` is blob `6c92a013`, exactly
the pre-image recorded in `10_enable_susfs_for_ksu.patch` — so the patch applies
with zero failed hunks.

KernelSU Next, the original plan for 1.2, was rejected on evidence: its `dev`
branch has no in-tree SUSFS. It vendors SUSFS as its own loadable module and
wires the KSU core to call into it, so it cannot give "KSU plus built-in SUSFS"
at all. This is a different ground from the earlier v0.x attempt, where Next
simply would not compile.

SUSFS is grafted on in-tree by `susfs4ksu`'s `gki-android15-6.6` patches
(revision `2d11d711`, 2026-10-14), with `fs/susfs.c` and the two headers copied
in (the patch does not create them). That revision keeps the SELinux internals
in `security/selinux/ss/services.c` as `my_security_*` and leaves the driver as
the feature switch plus the `backup_sepolicy` lifecycle; the 605 lines of
duplicated internals that an earlier revision carried in
`feature/selinux_hide.c` are gone.

One local change to the vendored driver remains, documented in
`apex-patches/kernelsu/README.md`:

- `Kbuild` fell back to `KSU_VERSION=16` because kleaf builds from a sandboxed
  copy with no `.git`. The manager reads that number, and 16 means "ancient
  kernel". It is pinned to 32680 instead.

The two changes the earlier patch revision needed — the `if (fn)` guards in
`feature/selinux_hide.c` and a stray `fs/exec.c` declaration left over from the
SukiSU era — are gone with it. KernelSU itself needed no update: the driver tree
is byte-identical between tag `v3.3.1` (our vendored `0c0fdb96`) and the current
`main` tip.

The earlier SukiSU Ultra work is kept in `apex-patches/sukisu-ultra/` for
reference. It is not used by v1.2.

## Performance and Droidspaces

**Performance — 24 patches.** Memory and scheduler hot paths, `memcmp` and
`int_sqrt`, cache pressure, F2FS congestion, ext4 commit age, wakeup
behaviour, and two patches that quiet kernel log spam. All 24 carry their
measurement in `apex-patches/`; nothing is included on plausibility.

An earlier revision of the notes recorded `clear_page_16bytes_align` as
excluded, on the belief that `arch/arm64/lib/clear_page.S` differed between 6.6
and 6.7. That was wrong: the patch applies with no offset or fuzz, inserting
`.p2align 4` ahead of `SYM_FUNC_START(__pi_clear_page)`, and it is applied here.
See `apex-patches/README.md`.

**BORE** is the one scheduler patch kept without a claimed gain. It measured
neutral against its own on/off control at fixed thermal state (1829/8235 off vs
1827/8177 on), so it is carried as a control, not as a performance claim.

BORE also only published its minimum preemption slice in debugfs, which stock
Android does not mount. v1.2 adds `min_base_slice_ns` (writable) and
`base_slice_ns` (the effective value) to the BORE sysctls so a tuner can reach
them with no mount; the tick quantisation this exposes is documented in
`apex-patches/README.md`.

**Droidspaces.** The `sysvipc` KABI fix and the ghost-task NULL check, plus the
seven config options the container runtime needs (`CONFIG_SYSVIPC`,
`CONFIG_DEVTMPFS`, `CONFIG_PID_NS`, `CONFIG_POSIX_MQUEUE`, and three
`CONFIG_NETFILTER_XT_*`).

## Battery and heat: audited, nothing changed

The power, thermal and scheduler config was compared against the stock kernel's
`/proc/config.gz` over the whole `power`/`thermal`/`sched`/`rcu`/`idle` space.
Exactly three keys differ, and all three are additions here: ADIOS as the
default I/O scheduler and BORE. `HZ=250`, `NO_HZ_IDLE`, TEO+MENU, EAS,
`schedutil`, `SCHED_AUTOGROUP` off and `RCU_LAZY_DEFAULT_OFF` all already match
stock.

The QCOM cpufreq, thermal and tsens drivers (`CONFIG_QTI_THERMAL_*`,
`CONFIG_MSM_RPM_SMD`, `CONFIG_CPU_IDLE_GOV_QCOM_LPM`, `CONFIG_THERMAL_TSENS_LEGACY`)
are deliberately off, as they are in stock: on SM8750 those live in the vendor
modules. Enabling them in the kernel would fight the stock `.ko`.

**No kernel-side battery or heat win is claimed, because none was measured.**
The full audit, and the runtime experiments that could still produce one, are in
`findings/POWER_THERMAL_AUDIT.md`. Runtime governor tuning is the honest place
to look, not kernel patches.

## Versioning

Three maturity stages, named for what changes between them:

| Flavour | Meaning |
|---|---|
| `v0.1 (Prototype)` | Working model, rough edges expected. |
| `v0.2 (Experimental)` | Features on trial, behaviour may change. |
| `v1.0 (Tempered)` | Hardened by testing, for daily use. |
| `v1.1` | Adds BORE, the memfd ashmem shim, SukiSU Ultra main. See `CHANGELOG.md`. |
| `v1.2` | Narrows the EROFS LZ4 inplace margin to `>> 6`, and replaces SukiSU Ultra with upstream KernelSU main, SUSFS still in-tree. See `CHANGELOG.md`. |

## Known cosmetic issue

None that the kernel controls. v1.1 reported a version the manager disagreed
with because it pinned a SukiSU commit rather than the branch tip. v1.2 reports
`KSU_VERSION 32680` from upstream KernelSU's own numbering, which is what the
upstream manager expects.

## Branch naming

The branch is named after the Android platform the kernel targets, using the
LineageOS convention, not after the GKI branch it is developed on. The two are
different things:

- GKI source branch: `common-android15-6.6`. This records that Google developed
  the 6.6 kernel train during the Android 15 timeframe. It says nothing about
  which Android the kernel runs on. Its two parts do reach `uname -r` as
  `android15-8`, but as AOSP identifiers, not as a claim about the running
  system: `android15` is the workspace `BRANCH` and `8` is the GKI KMI
  generation.
- Target platform: Android 16, hence `lineage-23.2`. The patches taken from
  the community build workflows are written against 23.0-23.2, and Android 16
  is what has been verified on device.

Kernel version string stays `APEX-Foundry-v1.2`; the two numbering schemes are
independent and never mixed.

## Layout

| Branch | Contents |
|---|---|
| `lineage-23.2` | Current. KMI fix, optimizations, Droidspaces, upstream KernelSU, SUSFS. |
| `apex-foundry-v0.1-prot` | The working base, before optional features. |
| `android15-6.6-dada` | Full upstream GKI history, kept for reference. |

## Status

Verified on device: audio, Bluetooth, WLAN, charging and Droidspaces all work.
v1.2 is reported working on device by the maintainer, with the EROFS oracle
(`md5sum /system/lib64/libminikin.so` = `3b3f0ed6...`) holding.

## Acknowledgements

- LineageOS, for `android_kernel_qcom_sm8750` (branch `lineage-22.2`), which
  is the base this tree is rebased on
- AnyKernel3 by osm0sis
- The community GKI build workflows this approach follows
- The optimization and Droidspaces patch authors, whose patches are recorded
  verbatim in `apex-patches/`

## Licence

Kernel sources remain GPL-2.0 as per the upstream tree. The AnyKernel3
installer in the flashable zip is GPL-3.0, see `LICENSE` in that package.