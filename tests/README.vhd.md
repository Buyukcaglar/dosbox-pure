# Experimental differencing VHD disk layer

`src/ints/vhd_differencing.h` implements a standard type-4 child over an
explicitly supplied, immutable type-2 (fixed) or type-3 (dynamic) parent. This is
an experimental emulator feature enabled only by explicit `IMGMOUNT -diff`.
The codec has no host file operations.

The narrow `Source` and `WritableSource` interfaces require exact random-access
I/O. The parent interface cannot write. `vhd_dos_source.h` adapts `DOS_File`,
keeping the base inside its archive and the child in the memory-backed overlay.
Sources and the parent object must outlive their child and have exclusive
ownership while mounted. Reopening a parent invalidates existing child bindings.

Supported in this first increment:

- 512-byte sectors; creation of children with 2 MiB blocks.
- Fixed and dynamic parent logical reads, including clear bitmap bits with stale
  nonzero payload and entirely unallocated blocks.
- Parent fallback, explicit zero overrides, and clearing an override when its
  contents match the parent again. Empty allocated blocks are not yet reclaimed.
- Checksums, sector bounds, disjoint metadata/block extents, bounded allocation
  tables, parent UUID/size/modification timestamp matching, and exact I/O checks.
- Read/write failure stops child access until an explicit reopen. It never
  substitutes zeros or silently switches to nonpersistent operation on I/O error.

Deliberate limits: one child only; at most 2040 GiB virtual size, 1,048,576 BAT
entries, and 32 MiB blocks on input; matching leading/trailing sparse footers;
no saved-state flag, historical 511-byte footer or parent chains. Child creation
currently accepts an ASCII parent basename. Parent locators are validated as
metadata but never followed by this reader. UUIDs are supplied by the caller.
The codec uses 64-bit offsets; the `memoryDrive` adapter rejects writable files
or extensions above 2,147,483,647 bytes because its seeks use signed 32-bit sizes.

VHD UUID/size/timestamp checks do **not** replace a strong fingerprint of the
immutable parent. The Windows standalone host can now supply a declared identity
and verify SHA-256 through the immutable archive handle. Conversion of legacy
saves, crash-safe archive publication, guest flushes, cross-process writer
exclusion and save-state generation rules remain required integration work.
The in-memory child writer alone is not a crash-safe file writer. Do not publish
a child after a failed write; retain the last complete save generation. Child
allocation and ordinary overwrites may leave partially changed backing bytes
when an I/O error occurs.

`vhd_identity.h` defines the version-1, 512-byte `.DBI` binding record saved in
the same ZIP as the child. Its exact bytes bind package/disk IDs, canonical
parent/child names, parent SHA-256/UUID/virtual size and child UUID. It retains
the original parent timestamp for codec validation after repacking identical
bytes with a different ZIP timestamp. Declared packages reject existing unbound
children; undeclared mounts reject existing bindings. Binding files are never
silently replaced or adopted, and mounted bindings share the disk write lease.
The enclosing repository documents the manifest opt-in and runtime tests.

## Experimental mount

```text
imgmount 2 C:\BASE.VHD -t hdd -fs none -diff C:\CHILD.VHD
boot -l c
```

Use distinct root-level 8.3 `.VHD` names on one persistent ZIP union drive.
The parent must exist only in the immutable underlay. A legacy parent save is
rejected without conversion. Existing children, including empty/corrupt ones,
are validated and never reset. No host-file fallback is used. `imgmount -u 2`
releases the disk while retaining the archive drive and saved child.

Mounted names are protected against ordinary DOS writes, rename and deletion.
Successful sector writes schedule the ordinary ZIP persistence mechanism while
the child is open; a codec I/O fault disables all further overlay saving for the
session. Save states and rewind are refused until disk-generation consistency
is implemented. The existing ZIP writer is still in-place, so this interface
is for controlled testing and is not a crash-safe production persistence feature.

The child's parent timestamp is the parent's modification timestamp in seconds
since 2000-01-01 UTC, not the creation time in its footer. `W2ru` parent locator
text is UTF-16LE; the header's parent name is UTF-16BE. Locator data space is a
byte count rounded to 512 bytes, consistent with Windows' VHD implementation.

## Tests

In the enclosing standalone repository, with Visual Studio C++ tools installed:

```powershell
./tools/Test-DifferencingVhd.ps1
./tools/Test-DifferencingVhd.ps1 -AddressSanitizer
./tools/Test-DifferencingVhd.ps1 -WindowsInterop
```

Or compile this repository's test directly with a C++11-or-newer compiler:

```text
c++ -std=c++11 -Wall -Wextra -Werror tests/vhd_differencing_test.cpp -o vhd-test
```

The suite uses independently built synthetic images and a full logical-sector
oracle across create/write/reopen cycles. It covers zero overrides, sparse
garbage, bitmap/block boundaries, a 5 GiB virtual disk, invalid images, parent
mismatches and failed/partial I/O. Windows builds link `virtdisk.lib`.
The DOS adapter tests also cover split 16-bit transfers, rejected short I/O,
failed or truncated seeks, 64-bit parent reads and the writable-size ceiling.
Identity tests cover field changes, binding corruption, child substitution,
canonical names, decimal overflow and retained-timestamp reopen semantics.

`-WindowsInterop` additionally writes **generated synthetic fixtures only** into
unique directories under the test output directory and asks Windows
`OpenVirtualDisk` to open complete fixed-parent and dynamic-parent chains. It
checks type-4 metadata, virtual size, block size, sector size and parent timestamp.
It never attaches/mounts a disk and does not read or copy any Windows installation.
This proves Windows recognizes the format, not guest boot or archive persistence.
The optional check requires Windows' VHD provider; failures are reported rather
than skipped. Build products default to the root repository's ignored `work/`.
