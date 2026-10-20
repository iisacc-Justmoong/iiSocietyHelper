# Host adoption journal

`Replica::bindHost` prepares the detach-journal insert once for the whole
adoption plan, then rebinds each path. It also avoids constructing a one-element
path list for every top-level entry. Large mirror adoptions no longer parse and
prepare the same SQL statement for each of up to 250,000 entries.

The existing replica lock, single transaction, path confinement, item limit,
recovery-directory layout and rollback behavior are preserved. The statement is
finished before metadata updates and commit. Adoption remains serialized with
other changes to that replica; independent replicas keep independent locks.

Host-binding tests include multi-entry recovery and repeated binding, in addition
to the existing authority and bootstrap cases.

Installed synchronizer, namespace and on-demand scenario suites use the same
600-second total execution budget as their source-build counterparts. These
suites perform many independent scenarios and bulk transfers; their internal
response-time, non-blocking and cancellation assertions are unchanged. The
other installed checks retain their existing 90-second suite budget.
