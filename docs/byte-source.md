# Random-access image sources

`src/storage/ByteSource.h` separates encoded image bytes from codecs and HTTP.
The first implementation, `LocalFileSource`, owns one POSIX file descriptor;
The [S3 adapter](s3-byte-source.md) implements the same contract through AWS SDK
for C++; the local adapter requires no external package.

## Contract and ownership

- `info()` returns immutable `SourceInfo`: backend-qualified identity, opaque
  version validator, and byte size. These are storage identities, not public URLs.
- `read_at(offset, destination)` fills the caller's `std::span<std::byte>` up to
  the captured EOF and returns the number of bytes written. Empty buffers and
  offsets at/beyond EOF return zero. Short transport reads must be completed by
  the implementation; only reaching the captured EOF permits a short result.
- Reads do not share a cursor. Concurrent reads are safe with separate buffers.
  The owner must keep the source alive until all readers finish; destruction may
  not race reads. A source must not silently reopen a newer version.
- `ByteSourceError` distinguishes invalid arguments, missing sources, I/O failure
  and detected source changes from codec errors. Allocation failures may propagate.
  Discard buffer contents after any exception.

A typical local operation is:

```cpp
#include "storage/LocalFileSource.h"
#include "imgformats/TiffReader.h"

const sipi::LocalFileSource source(path);
const auto metadata = sipi::read_tiff_metadata(source);
// Resolve the requested geometry against metadata before decoding.
const auto pixels = sipi::read_tiff_region(source, region);
```

The source owns its handle; readers borrow it, and returned metadata/pixels own
all their data. The concrete source need not be heap-allocated. A future backend
can implement the same interface without changing the TIFF public API.

## Local consistency

Opening captures `fstat` information from the actual descriptor. Identity uses
its device and inode; the version fingerprint uses size plus nanosecond mtime and
ctime. This is a practical validator under the repository update contract, not
proof of identical content or a portable cross-host content identifier.

`pread` fills requested ranges without changing a shared file position, retries
interrupted reads and reports premature EOF as a source change. Only regular
files are accepted; opening a FIFO does not block. RAII closes the descriptor
on normal destruction and constructor failure. The implementation targets macOS
and POSIX Linux; this change was tested on macOS.

Publish replacement files with atomic rename. An already opened source continues
reading the old file even after replacement or unlink; a subsequent open sees
the new file. **Do not modify repository files in place.** Holding a descriptor
does not create a snapshot of in-place writes; such changes are not generally
detected. The stat fingerprint is captured once, not refreshed per read.

Path containment remains the responsibility of `SourceResolver`. Its canonical
path check and the subsequent open are not atomic: directories and symlinks must
remain trusted. This interface does not remove that authorization boundary.

## TIFF and HTTP integration

`TIFFClientOpenExt` reads through per-handle callbacks, each with its own cursor.
Mapping and writes are disabled. Codec handles borrow the source and never close
it. Callback exceptions are captured and rethrown after returning from libtiff;
no C++ exception crosses the C boundary. Existing TIFF memory limits remain.

After preflight and path resolution, `IiifWorker` opens one `LocalFileSource` and
uses it for the magic probe, metadata/geometry planning and pixel decoding.
Missing sources at open produce 404; other storage failures produce 500
`source_error`. Codec errors retain their separate mapping. Public responses
expose neither paths nor source identities. JPEG delivery is otherwise unchanged.

## Verification and next step

`byte-source` tests cover range/EOF semantics, concurrent reads, invalid sources,
atomic replacement/unlink, truncation, sparse BigTIFF offsets beyond 4 GiB, and
metadata/pixel consistency. A test-only memory source verifies backend independence
and injected storage/allocation failures through TIFF callbacks. Existing TIFF,
worker and HTTP suites verify that delivery still works.

The S3 adapter now binds ranges to a VersionId or ETag condition and is tested
against MinIO. Typed local/remote selection is integrated through trusted
configuration/preflight; URL prefixes remain outside default storage mapping. Whole-operation cancellation and range reuse remain open.
No source-range cache or result cache is implemented.
