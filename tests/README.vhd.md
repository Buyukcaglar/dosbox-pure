# Experimental differencing VHD disk layer

`src/ints/vhd_differencing.h` implements a standard type-4 child over an
explicitly supplied, immutable type-2 (fixed) or type-3 (dynamic) parent. This is
an internal foundation, not an enabled emulator feature. It has no host file
operations and is not yet connected to `imageDisk`, archive mounting or saves.

The narrow `Source` and `WritableSource` interfaces require exact random-access
I/O. The parent interface cannot write. The eventual adapter should use
`DOS_File`, keeping the base inside its archive and the child in the memory-backed
overlay. Sources and the parent object must outlive their child and have exclusive
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
The codec uses 64-bit offsets; the future `memoryDrive` adapter still needs its
own physical-size limit because that existing implementation uses 32-bit sizes.

VHD UUID/size/timestamp checks do **not** replace a strong fingerprint of the
immutable parent. Package fingerprint binding, metadata opt-in, conversion of
legacy saves, crash-safe archive publication, guest flushes, concurrent writer
exclusion and save-state generation rules remain required integration work.
The in-memory child writer alone is not a crash-safe file writer. Do not publish
a child after a failed write; retain the last complete save generation. Child
allocation and ordinary overwrites may leave partially changed backing bytes
when an I/O error occurs.

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

`-WindowsInterop` additionally writes **generated synthetic fixtures only** into
unique directories under the test output directory and asks Windows
`OpenVirtualDisk` to open complete fixed-parent and dynamic-parent chains. It
checks type-4 metadata, virtual size, block size, sector size and parent timestamp.
It never attaches/mounts a disk and does not read or copy any Windows installation.
This proves Windows recognizes the format, not guest boot or archive persistence.
The optional check requires Windows' VHD provider; failures are reported rather
than skipped. Build products default to the root repository's ignored `work/`.
