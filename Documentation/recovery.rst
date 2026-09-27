.. SPDX-License-Identifier: GPL-2.0

Recovery notes
==============

Damaged superblocks
-------------------

If one member reports a default superblock checksum error after an unclean
shutdown, stop and preserve evidence before attempting repair. Do not run
multiple mounts or fsck processes against the same filesystem at the same time:
superblock sequence numbers are used to detect concurrent modification, and a
repair attempt can be aborted if another process writes a newer superblock.

Useful first checks are:

.. code-block:: bash

  bcachefs show-super /dev/bcachefs-member
  bcachefs show-super -l /dev/bcachefs-member

Run those checks on every available member. If the primary superblock on one
device is unreadable but another copy or member still validates and shows the
same filesystem UUID and member list, ``bcachefs recover-super`` can rewrite the
damaged member's superblock from a backup copy or from another member:

.. code-block:: bash

  bcachefs recover-super /dev/damaged-member
  bcachefs recover-super --src_device /dev/good-member --dev_idx N /dev/damaged-member

Use the second form when the damaged member's own backup copies are not usable.
``N`` is the damaged member's device index from the filesystem member list. The
command prints the superblock it found and asks for confirmation unless
``--yes`` is supplied.

After recovering a member superblock, run offline fsck before mounting
read-write:

.. code-block:: bash

  bcachefs fsck /dev/member0:/dev/member1:...

If the filesystem cannot start because a member is missing or unreadable, use
the normal degraded recovery options only after verifying which member copies
are present. Prefer a read-only/nochanges attempt while collecting evidence; use
more aggressive degraded modes only when the missing-data risk is understood.

Whenever possible, make block-device images or hardware snapshots first. A
superblock repair changes on-disk metadata, and a failed recovery attempt is
much easier to debug when the original bytes are still available.


Journal replay idempotency
--------------------------

Journal replay must tolerate a crash at every point between reading an entry,
updating a btree node, writing that node, and finally releasing the journal pin.
The durable journal entry can therefore coexist with a btree update that was
already written before the crash.

For ordinary btree keys, replay installs the recorded key with triggers disabled
(``BTREE_TRIGGER_norun``).  Reinstalling the same final key is logically
idempotent, and disabling triggers prevents replay from regenerating accounting
or other derived side effects.  The original journal sequence remains pinned
until the btree write that depends on it is durable.

Accounting keys need a stronger rule because they are deltas rather than
replacement values.  Every accounting delta receives a monotonically ordered
``bversion`` derived from its journal sequence and reservation offset.  The
accounting btree stores the newest applied version as a persistent high-water
mark.  Recovery applies a delta only when its version is newer than the
persisted version.

This gives the important crash cases:

* If the btree update was not durable, its old version is still visible and the
  delta is applied during replay.
* If the btree update was durable but its journal entry is still present, the
  persisted version is at least the delta version and replay skips it.
* Once the journal pin is released, reclaim may discard the source journal
  entry because the corresponding btree state is already durable.

The write buffer preserves the high-water-mark invariant for repeated updates
to one accounting position.  Same-position deltas are coalesced before flush,
their values are accumulated, the maximum ``bversion`` is retained, and the
earliest required journal sequence stays pinned.  Thus a persisted accounting
version represents a contiguous prefix of the deltas for that position.

The in-memory ``journal_key.overwritten`` flag is not part of this correctness
argument: it can disappear in a crash.  It only avoids redundant work within
one recovery run.  Cross-crash idempotency comes from the persisted btree
version and journal-pin ordering.

Extent ordering
~~~~~~~~~~~~~~~

The fast replay pass walks journal keys in btree/position order instead of
journal order.  This is safe for extent btrees even though range updates do
not generally commute.  The original extent transaction converts a range
overwrite into point-addressed final-state updates: for every old extent it
overwrites, it emits a replacement at that extent's original end ``bpos``.
A complete overwrite emits a deletion or whiteout there; a partial overwrite
that preserves the tail emits the right-hand fragment with the same end
``bpos``.  Front and middle fragments only add new positions.

Consequently, when journal keys are sorted, exact-position deduplication
removes stale versions of every overwritten extent before replay.  The
remaining key set describes the final non-overlapping extent layout, so its
position-ordered replay is independent of the original transaction order.

Early replay entries
~~~~~~~~~~~~~~~~~~~~

Entries handled before the btrees start also need repeatable semantics.
Btree-root, key-version usage, and I/O-clock entries replace in-memory state;
because journal entries are walked in sequence order, the newest surviving
value wins.  Journal blacklist entries are set-like: adding the same or an
overlapping range merges with the existing superblock range.  Repeating early
replay after a crash therefore converges on the same state rather than
accumulating a second side effect.
