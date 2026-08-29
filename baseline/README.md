# Versioned Python Baseline Snapshot

`baseline/python` is a byte-for-byte copy of the reviewed Python 3.0 golden input and
reference JSON at the start of the C++ 4.0 migration.

The C++ build and tests must consume this local snapshot.  They must never reach into the
Mechanical-Bonding-Structure 3.0 directory.  A future baseline refresh is an explicit import
operation that records source commit, hashes and review results; it is not a live file link.

Snapshot metadata:

- Reference implementation: Mechanical-Bonding-Structure 3.0
- Snapshot date: 2026-08-24
- Case schema: 1
- Reference schema: 1
