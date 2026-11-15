Fix two build breaks on the SukiSU Ultra `builtin` branch

The `builtin` branch does not compile as published. Three separate defects
were found while integrating it into a GKI 6.6.139 tree; this patch covers
the two that are in SukiSU's own C sources. The third, a missing `arch.h`,
is handled separately because it is a deleted file rather than bad code.

1. supercall/dispatch.c: undefined identifier EVENT_SERVICES

   Commit e038c7ea ("fix(kernel): Avoid repeatedly triggering service stage",
   2026-10-04) added a `case EVENT_SERVICES:` arm to do_report_event()
   together with the `services_started` guard, but never declared
   EVENT_SERVICES in the UAPI header. The case label therefore references an
   identifier that exists nowhere in the tree:

       supercall/dispatch.c:98:10: error: use of undeclared identifier 'EVENT_SERVICES'

   Events are declared with the DECLARE macro in kernel/include/uapi/supercall.h,
   whose existing values are 1..3. The comment on ksu_report_event_cmd.event
   already reads "EVENT_POST_FS_DATA, EVENT_BOOT_COMPLETED, etc.", so the list
   is meant to be open. Declare the missing event as 4, matching the pattern.

   Behaviour is unchanged: this only makes the already-written case arm
   compile. The guard is reset on EVENT_POST_FS_DATA precisely so services
   restart after an emulated soft reboot, so arming the event here is what
   the calling code in ksud expects.

2. selinux/rules.c: redefinition of pol and old_pol

   apply_kernelsu_rules() declares `struct selinux_policy *pol, *old_pol;`
   at function scope, then declares the same two variables again inside the
   `LINUX_VERSION_CODE >= 5.10` block, this time with an initialiser:

       selinux/rules.c:162:28: error: redefinition of 'pol'
       selinux/rules.c:162:34: error: redefinition of 'old_pol'

   The inner declaration is the one that matters, since it initialises
   old_pol and is the branch that compiles on 6.6. The outer declaration is
   redundant. `db` stays at function scope because both branches assign it.

   The pre-5.10 `#else` branch was checked and uses only `db`; it never
   references pol or old_pol, so removing the outer declaration is safe for
   both branches.

Upstream, not local: both defects originate in SukiSU's own tree and are
present at the branch tip. No kernel-side file is touched.
