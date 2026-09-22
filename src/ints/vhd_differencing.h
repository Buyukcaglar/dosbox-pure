/*
 * DOSBox Pure downstream: experimental standard VHD differencing disk layer.
 * Distributed under the GNU General Public License, version 2 or later.
 *
 * No host paths, extraction, or ownership of backing sources here. Sources must
 * outlive their images; a mounted parent must remain immutable. Persistence and
 * strong package/parent fingerprint binding are the caller's responsibility.
 */
#ifndef DOSBOX_VHD_DIFFERENCING_H
#define DOSBOX_VHD_DIFFERENCING_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <algorithm>
#include <utility>
#include <vector>

namespace DBPVHD
{

class Source
{
public:
	virtual ~Source() {}
	virtual uint64_t Size() const = 0;
	// True means the entire request completed. Short I/O must return false.
	virtual bool Read(uint64_t offset, void* data, size_t bytes) = 0;
};

class WritableSource : public Source
{
public:
	// May extend the source; must not silently discard writes or report partial
	// writes as success. The caller supplies transaction/durability guarantees.
	virtual bool Write(uint64_t offset, const void* data, size_t bytes) = 0;
};

namespace Detail
{
static const uint32_t SectorSize = 512;
static const uint32_t Unallocated = 0xffffffffU;
static const uint32_t ChildBlockSize = 2 * 1024 * 1024;
static const uint64_t MaxVirtualSize = uint64_t(2040) * 1024 * 1024 * 1024;
static const uint32_t MaxTableEntries = 1048576; // Bound untrusted allocations.

inline uint32_t BE32(const uint8_t* p)
{
	return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
inline uint64_t BE64(const uint8_t* p) { return uint64_t(BE32(p)) << 32 | BE32(p + 4); }
inline void Put32(uint8_t* p, uint32_t n)
{
	p[0] = uint8_t(n >> 24); p[1] = uint8_t(n >> 16); p[2] = uint8_t(n >> 8); p[3] = uint8_t(n);
}
inline void Put64(uint8_t* p, uint64_t n) { Put32(p, uint32_t(n >> 32)); Put32(p + 4, uint32_t(n)); }
inline uint64_t RoundSector(uint64_t n) { return (n + 511) & ~uint64_t(511); }
inline bool Range(uint64_t offset, uint64_t bytes, uint64_t end)
{
	return offset <= end && bytes <= end - offset;
}
inline bool AllZero(const uint8_t* p, size_t bytes)
{
	for (size_t i = 0; i < bytes; i++) if (p[i]) return false;
	return true;
}
inline uint32_t Checksum(const uint8_t* p, size_t bytes, size_t checksumOffset)
{
	uint32_t sum = 0;
	for (size_t i = 0; i < bytes; i++) if (i < checksumOffset || i >= checksumOffset + 4) sum += p[i];
	return ~sum;
}

struct Layout
{
	uint8_t footer[512], header[1024];
	uint64_t virtualSize, footerOffset, tableOffset;
	uint32_t type, blockSize, bitmapSize;
	std::vector<uint32_t> table;

	Layout() : virtualSize(0), footerOffset(0), tableOffset(0), type(0), blockSize(0), bitmapSize(0)
	{
		memset(footer, 0, sizeof(footer)); memset(header, 0, sizeof(header));
	}

	const char* Parse(Source& source)
	{
		table.clear();
		const uint64_t size = source.Size();
		if (size < 512 || size % 512) return "VHD file size is not sector aligned";
		footerOffset = size - 512;
		if (!source.Read(footerOffset, footer, sizeof(footer))) return "Cannot read VHD footer";
		if (memcmp(footer, "conectix", 8) || BE32(footer + 12) != 0x10000 ||
			BE32(footer + 64) != Checksum(footer, sizeof(footer), 64)) return "Invalid VHD footer";
		if (!(BE32(footer + 8) & 2) || (BE32(footer + 8) & ~3U) || footer[84])
			return "Unsupported VHD features or saved state";
		type = BE32(footer + 60);
		if (type != 2 && type != 3 && type != 4) return "Unsupported VHD disk type";
		virtualSize = BE64(footer + 48);
		const uint64_t originalSize = BE64(footer + 40);
		if (!virtualSize || virtualSize % 512 || virtualSize > MaxVirtualSize ||
			!originalSize || originalSize % 512 || originalSize > MaxVirtualSize)
			return "Invalid or unsupported VHD virtual size";
		if (AllZero(footer + 68, 16)) return "VHD has no disk identity";
		if (type == 2)
		{
			if (BE64(footer + 16) != UINT64_MAX || footerOffset != virtualSize) return "Invalid fixed VHD extent";
			return NULL;
		}
		uint8_t leadingFooter[512];
		if (!source.Read(0, leadingFooter, sizeof(leadingFooter))) return "Cannot read VHD footer copy";
		if (memcmp(leadingFooter, footer, sizeof(footer))) return "VHD footer copies differ";
		const uint64_t headerOffset = BE64(footer + 16);
		if (headerOffset < 512 || headerOffset % 512 || !Range(headerOffset, sizeof(header), footerOffset))
			return "Invalid VHD sparse header offset";
		if (!source.Read(headerOffset, header, sizeof(header))) return "Cannot read VHD sparse header";
		if (memcmp(header, "cxsparse", 8) || BE64(header + 8) != UINT64_MAX || BE32(header + 24) != 0x10000 ||
			BE32(header + 36) != Checksum(header, sizeof(header), 36)) return "Invalid VHD sparse header";
		blockSize = BE32(header + 32);
		if (blockSize < 512 || blockSize > 32 * 1024 * 1024 || (blockSize & (blockSize - 1)))
			return "Unsupported VHD block size";
		const uint32_t entries = BE32(header + 28);
		if (!entries || entries > MaxTableEntries || entries != (virtualSize + blockSize - 1) / blockSize)
			return "Invalid or excessive VHD allocation table";
		bitmapSize = uint32_t(RoundSector((blockSize / 512 + 7) / 8));
		tableOffset = BE64(header + 16);
		const uint64_t tableBytes = RoundSector(uint64_t(entries) * 4);
		if (tableOffset % 512 || !Range(tableOffset, tableBytes, footerOffset)) return "Invalid VHD table extent";
		if (type == 3 && !AllZero(header + 40, 16)) return "Dynamic VHD unexpectedly names a parent";

		// Validate disjoint metadata and data extents before accepting any BAT entry.
		typedef std::pair<uint64_t, uint64_t> Extent;
		std::vector<Extent> extents;
		extents.push_back(Extent(0, 512));
		extents.push_back(Extent(headerOffset, headerOffset + sizeof(header)));
		extents.push_back(Extent(tableOffset, tableOffset + tableBytes));
		for (unsigned i = 0; i < 8; i++)
		{
			const uint8_t* locator = header + 576 + i * 24;
			if (AllZero(locator, 24)) continue;
			// W2ru/W2ku data-space is a byte count rounded to a sector (the
			// original specification's 'count of sectors' wording is erroneous).
			const uint32_t space = BE32(locator + 4), length = BE32(locator + 8);
			const uint64_t offset = BE64(locator + 16);
			if (type != 4 || AllZero(locator, 4) || !space || space % 512 || !length || length > space ||
				BE32(locator + 12) || offset % 512 || !Range(offset, space, footerOffset))
				return "Invalid VHD parent locator extent";
			extents.push_back(Extent(offset, offset + space));
		}
		table.resize(entries);
		uint8_t buf[512];
		for (uint32_t first = 0; first < entries; first += 128)
		{
			if (!source.Read(tableOffset + uint64_t(first) * 4, buf, sizeof(buf))) return "Cannot read VHD allocation table";
			const uint32_t count = std::min(uint32_t(128), entries - first);
			for (uint32_t i = 0; i < count; i++)
			{
				const uint32_t sector = table[first + i] = BE32(buf + i * 4);
				if (sector == Unallocated) continue;
				const uint64_t offset = uint64_t(sector) * 512, bytes = uint64_t(bitmapSize) + blockSize;
				if (!Range(offset, bytes, footerOffset)) return "VHD block is outside the file";
				extents.push_back(Extent(offset, offset + bytes));
			}
		}
		std::sort(extents.begin(), extents.end());
		for (size_t i = 1; i < extents.size(); i++)
			if (extents[i].first < extents[i - 1].second) return "VHD extents overlap";
		return NULL;
	}

	// A clear dynamic bitmap bit means zero; a clear child bit means parent.
	// Neither case permits reading stale bytes from the allocated payload.
	bool ReadOwn(Source& source, uint64_t sector, void* data, bool& present) const
	{
		present = false;
		if (sector >= virtualSize / 512) return false;
		if (type == 2) { present = true; return source.Read(sector * 512, data, 512); }
		const uint32_t block = uint32_t(sector / (blockSize / 512));
		if (table[block] == Unallocated) return true;
		const uint32_t within = uint32_t(sector % (blockSize / 512));
		const uint64_t offset = uint64_t(table[block]) * 512;
		uint8_t bit;
		if (!source.Read(offset + within / 8, &bit, 1)) return false;
		if (!(bit & (0x80 >> (within % 8)))) return true;
		present = true;
		return source.Read(offset + bitmapSize + uint64_t(within) * 512, data, 512);
	}
};
} // namespace Detail

class Parent
{
public:
	Parent() : source(NULL), timestamp(0), generation(0), error("Parent is not open") {}
	bool Open(Source& backing, uint32_t modificationTimestamp)
	{
		source = NULL;
		generation++;
		error = layout.Parse(backing);
		if (error) return false;
		if (layout.type == 4) { error = "Differencing parent chains are not supported"; return false; }
		source = &backing;
		// Seconds since 2000-01-01 UTC, from parent file/package metadata, not
		// the creation timestamp in the VHD footer.
		timestamp = modificationTimestamp;
		return true;
	}
	bool ReadSector(uint64_t sector, void* data)
	{
		if (!source) return false;
		bool present;
		if (!layout.ReadOwn(*source, sector, data, present)) { error = "Cannot read parent VHD sector"; return false; }
		if (!present) memset(data, 0, 512);
		error = NULL;
		return true;
	}
	uint64_t SectorCount() const { return source ? layout.virtualSize / 512 : 0; }
	const char* Error() const { return error; }

private:
	friend class Child;
	Parent(const Parent&) = delete;
	Parent& operator=(const Parent&) = delete;
	Detail::Layout layout;
	Source* source;
	uint32_t timestamp;
	uint64_t generation;
	const char* error;
};

class Child
{
public:
	Child() : source(NULL), parent(NULL), parentGeneration(0), error("Child is not open"), faulted(false) {}
	bool Open(WritableSource& backing, Parent& base)
	{
		source = NULL; parent = NULL; faulted = false;
		if (!base.source) return Fail("Parent is not open");
		if (static_cast<Source*>(&backing) == base.source) return Fail("Parent and child must have separate sources");
		error = layout.Parse(backing);
		if (error) return false;
		if (layout.type != 4) return Fail("Image is not a differencing VHD");
		if (layout.virtualSize != base.layout.virtualSize || memcmp(layout.header + 40, base.layout.footer + 68, 16) ||
			Detail::BE32(layout.header + 56) != base.timestamp) return Fail("Differencing VHD parent identity mismatch");
		if (!memcmp(layout.footer + 68, base.layout.footer + 68, 16)) return Fail("Child and parent UUIDs must differ");
		// Never follow untrusted locator paths. The caller explicitly binds the
		// immutable archive entry, and must also validate its strong fingerprint.
		source = &backing; parent = &base; parentGeneration = base.generation;
		return true;
	}

	bool Create(WritableSource& backing, Parent& base, const uint8_t uuid[16],
		uint32_t creationTimestamp, const char* parentFileName)
	{
		using namespace Detail;
		source = NULL; parent = NULL; faulted = false; error = NULL;
		if (!base.source) return Fail("Parent is not open");
		if (static_cast<Source*>(&backing) == base.source || backing.Size()) return Fail("New child source must be empty and separate");
		if (AllZero(uuid, 16) || !memcmp(uuid, base.layout.footer + 68, 16)) return Fail("Invalid child UUID");
		// Initial package integration uses an ASCII basename, not a host path.
		const size_t nameLength = parentFileName ? strlen(parentFileName) : 0;
		if (!nameLength || nameLength > 255 || !strcmp(parentFileName, ".") || !strcmp(parentFileName, ".."))
			return Fail("Invalid parent basename");
		for (size_t i = 0; i < nameLength; i++)
			if (uint8_t(parentFileName[i]) < 32 || uint8_t(parentFileName[i]) > 126 || strchr("\\/:*?\"<>|", parentFileName[i]))
				return Fail("Unsupported parent basename");
		const uint32_t entries = uint32_t((base.layout.virtualSize + ChildBlockSize - 1) / ChildBlockSize);
		const uint64_t tableBytes = RoundSector(uint64_t(entries) * 4), locatorOffset = 1536 + tableBytes;
		const uint64_t footerOffset = locatorOffset + 512;
		uint8_t footer[512], header[1024] = {0}, locator[512] = {0}, tableSector[512];
		memcpy(footer, base.layout.footer, sizeof(footer));
		Put32(footer + 8, 2); Put64(footer + 16, 512); Put32(footer + 24, creationTimestamp);
		memcpy(footer + 28, "dbpd", 4); Put32(footer + 32, 0x10000); memcpy(footer + 36, "Wi2k", 4);
		Put64(footer + 40, base.layout.virtualSize); Put32(footer + 60, 4); memcpy(footer + 68, uuid, 16);
		Put32(footer + 64, Checksum(footer, sizeof(footer), 64));
		memcpy(header, "cxsparse", 8); Put64(header + 8, UINT64_MAX); Put64(header + 16, 1536);
		Put32(header + 24, 0x10000); Put32(header + 28, entries); Put32(header + 32, ChildBlockSize);
		memcpy(header + 40, base.layout.footer + 68, 16); Put32(header + 56, base.timestamp);
		for (size_t i = 0; i < nameLength; i++)
		{
			header[64 + i * 2 + 1] = uint8_t(parentFileName[i]); // UTF-16BE
			locator[i * 2] = uint8_t(parentFileName[i]); // W2ru is UTF-16LE
		}
		memcpy(header + 576, "W2ru", 4); Put32(header + 580, 512);
		Put32(header + 584, uint32_t(nameLength * 2)); Put64(header + 592, locatorOffset);
		Put32(header + 36, Checksum(header, sizeof(header), 36));
		source = &backing;
		if (!Write(0, footer, sizeof(footer)) || !Write(512, header, sizeof(header))) return false;
		memset(tableSector, 0xff, sizeof(tableSector));
		for (uint64_t offset = 1536; offset < locatorOffset; offset += 512)
			if (!Write(offset, tableSector, sizeof(tableSector))) return false;
		if (!Write(locatorOffset, locator, sizeof(locator)) || !Write(footerOffset, footer, sizeof(footer))) return false;
		return Open(backing, base);
	}

	bool ReadSector(uint64_t sector, void* data)
	{
		if (!Ready(sector)) return false;
		bool present;
		if (!layout.ReadOwn(*source, sector, data, present)) return Fault("Cannot read child VHD sector");
		if (!present && !parent->ReadSector(sector, data)) return Fault("Cannot read parent VHD sector");
		error = NULL;
		return true;
	}

	bool WriteSector(uint64_t sector, const void* data)
	{
		using namespace Detail;
		if (!Ready(sector)) return false;
		uint8_t original[512];
		if (!parent->ReadSector(sector, original)) return Fault("Cannot compare parent VHD sector");
		const bool unchanged = !memcmp(original, data, sizeof(original));
		const uint32_t block = uint32_t(sector / (layout.blockSize / 512));
		const uint32_t within = uint32_t(sector % (layout.blockSize / 512));
		if (layout.table[block] == Unallocated)
		{
			if (unchanged) { error = NULL; return true; }
			if (!Allocate(block)) return false;
		}
		const uint64_t offset = uint64_t(layout.table[block]) * 512, bitmapOffset = offset + within / 8;
		uint8_t oldBits;
		if (!source->Read(bitmapOffset, &oldBits, 1)) return Fault("Cannot read child VHD bitmap");
		const uint8_t mask = uint8_t(0x80 >> (within % 8));
		const uint8_t newBits = unchanged ? uint8_t(oldBits & ~mask) : uint8_t(oldBits | mask);
		// Payload precedes the present bit: zeros over nonzero parents are real
		// overrides. Reverting to the parent clears only the bit, not the block.
		if (!unchanged && !Write(offset + layout.bitmapSize + uint64_t(within) * 512, data, 512)) return false;
		if (newBits != oldBits && !Write(bitmapOffset, &newBits, 1)) return false;
		error = NULL;
		return true;
	}
	const char* Error() const { return error; }
	bool IsFaulted() const { return faulted; }

private:
	Child(const Child&) = delete;
	Child& operator=(const Child&) = delete;
	Detail::Layout layout;
	WritableSource* source;
	Parent* parent;
	uint64_t parentGeneration;
	const char* error;
	bool faulted;
	bool Fail(const char* message) { error = message; return false; }
	bool Fault(const char* message) { faulted = true; return Fail(message); }
	bool Ready(uint64_t sector)
	{
		if (faulted) return false; // Preserve the first I/O error; require reopen.
		if (!source || !parent) return Fail("Child is not open");
		if (!parent->source || parent->generation != parentGeneration) return Fault("Parent was reopened while child was mounted");
		if (sector >= layout.virtualSize / 512) return Fail("VHD sector is outside the virtual disk");
		return true;
	}
	bool Write(uint64_t offset, const void* data, size_t bytes)
	{
		return source->Write(offset, data, bytes) || Fault("Cannot write child VHD; reopen or recover before continuing");
	}
	bool Allocate(uint32_t block)
	{
		using namespace Detail;
		const uint64_t offset = layout.footerOffset;
		if (source->Size() != offset + 512) return Fault("Child VHD source changed while mounted");
		if (offset / 512 >= Unallocated) return Fault("Child VHD allocation exceeds format limits");
		const uint64_t bytes = uint64_t(layout.bitmapSize) + layout.blockSize;
		uint8_t zeros[4096] = {0};
		// Backing memory is not a crash-safe journal. Archive checkpointing must
		// publish a complete generation; never persist a partially failed child.
		for (uint64_t written = 0; written < bytes;)
		{
			const size_t count = size_t(std::min(uint64_t(sizeof(zeros)), bytes - written));
			if (!Write(offset + written, zeros, count)) return false;
			written += count;
		}
		if (!Write(offset + bytes, layout.footer, sizeof(layout.footer))) return false;
		uint8_t entry[4]; Put32(entry, uint32_t(offset / 512));
		if (!Write(layout.tableOffset + uint64_t(block) * 4, entry, sizeof(entry))) return false;
		layout.table[block] = uint32_t(offset / 512);
		layout.footerOffset += bytes;
		return true;
	}
};
} // namespace DBPVHD
#endif
