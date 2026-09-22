/* Standard VHD differencing tests. GPL-2.0-or-later. No emulator or host disks. */
#include "../src/ints/vhd_differencing.h"
#include "../src/ints/vhd_dos_source.h"
#include "../src/ints/vhd_identity.h"
#include <stdio.h>
#include <stdlib.h>
#include <string>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <initguid.h>
#include <virtdisk.h>
#pragma comment(lib, "virtdisk.lib")
#endif

static unsigned checks = 0;
#define CHECK(condition) do { checks++; if (!(condition)) { \
	fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); exit(1); } } while (0)

struct Memory : DBPVHD::WritableSource
{
	std::vector<uint8_t> bytes;
	size_t reads = 0, writes = 0, failRead = SIZE_MAX, failWrite = SIZE_MAX;
	uint64_t Size() const override { return bytes.size(); }
	bool Read(uint64_t offset, void* data, size_t count) override
	{
		reads++;
		if (offset > bytes.size() || count > bytes.size() - offset) return false;
		if (reads == failRead)
		{
			if (count > 1) memcpy(data, &bytes[size_t(offset)], count / 2);
			return false; // Deliberate short read, including partially modified buffer.
		}
		memcpy(data, &bytes[size_t(offset)], count);
		return true;
	}
	bool Write(uint64_t offset, const void* data, size_t count) override
	{
		writes++;
		const bool fail = writes == failWrite;
		const size_t actual = fail ? count / 2 : count;
		if (offset > 64 * 1024 * 1024 || actual > 64 * 1024 * 1024 - offset) return false;
		if (offset + actual > bytes.size()) bytes.resize(size_t(offset) + actual);
		if (actual) memcpy(&bytes[size_t(offset)], data, actual);
		return !fail;
	}
};

// Fixture construction deliberately does not use the codec's helpers. Offsets,
// endian encodings, sparse garbage and expected sectors are specified here.
static void be(std::vector<uint8_t>& b, size_t offset, uint64_t n, size_t count)
{
	for (size_t i = 0; i < count; i++) { b[offset + count - 1 - i] = uint8_t(n); n >>= 8; }
}
static uint64_t number(const std::vector<uint8_t>& b, size_t offset, size_t count)
{
	uint64_t n = 0;
	for (size_t i = 0; i < count; i++) n = n * 256 + b[offset + i];
	return n;
}
static void checksum(std::vector<uint8_t>& b, size_t offset, size_t count, size_t field)
{
	be(b, offset + field, 0, 4);
	uint32_t sum = 0;
	for (size_t i = 0; i < count; i++) sum += b[offset + i];
	be(b, offset + field, uint64_t(uint32_t(~sum)), 4);
}
static const uint64_t VirtualBytes = 4 * 1024 * 1024 + 512;
static const uint64_t Sectors = VirtualBytes / 512;
static const uint32_t ParentTimestamp = 12345;
static const uint8_t ChildId[16] = {0x91, 2, 3, 4, 5, 6, 0x47, 8, 0x89, 10, 11, 12, 13, 14, 15, 16};

static Memory fixture(bool dynamic)
{
	Memory m;
	const size_t blockSize = 2 * 1024 * 1024, blockExtent = 512 + blockSize;
	const size_t end = dynamic ? 2048 + 2 * blockExtent : size_t(VirtualBytes);
	m.bytes.resize(end + 512);
	memcpy(&m.bytes[end], "conectix", 8);
	be(m.bytes, end + 8, 2, 4); be(m.bytes, end + 12, 0x10000, 4);
	be(m.bytes, end + 16, dynamic ? 512 : UINT64_MAX, 8);
	be(m.bytes, end + 24, 456, 4); // Creation time intentionally differs from mtime.
	memcpy(&m.bytes[end + 28], "test", 4);
	be(m.bytes, end + 32, 0x10000, 4); memcpy(&m.bytes[end + 36], "Wi2k", 4);
	be(m.bytes, end + 40, VirtualBytes, 8); be(m.bytes, end + 48, VirtualBytes, 8);
	be(m.bytes, end + 56, 8, 2); m.bytes[end + 58] = 16; m.bytes[end + 59] = 63;
	be(m.bytes, end + 60, dynamic ? 3 : 2, 4);
	for (size_t i = 0; i < 16; i++) m.bytes[end + 68 + i] = uint8_t(i + 1);
	checksum(m.bytes, end, 512, 64);
	if (dynamic)
	{
		memcpy(&m.bytes[0], &m.bytes[end], 512);
		memcpy(&m.bytes[512], "cxsparse", 8);
		be(m.bytes, 520, UINT64_MAX, 8); be(m.bytes, 528, 1536, 8);
		be(m.bytes, 536, 0x10000, 4); be(m.bytes, 540, 3, 4); be(m.bytes, 544, blockSize, 4);
		checksum(m.bytes, 512, 1024, 36);
		memset(&m.bytes[1536], 0xff, 512);
		be(m.bytes, 1536, 4, 4); be(m.bytes, 1544, (2048 + blockExtent) / 512, 4);
		m.bytes[2048] = 0x80; // Only sector zero is present in this block.
		memset(&m.bytes[2560], 0x11, 512);
		memset(&m.bytes[2560 + 3 * 512], 0x55, 512); // Garbage beneath a clear bit.
		m.bytes[2048 + blockExtent] = 0x80;
		memset(&m.bytes[2560 + blockExtent], 0x66, 512);
	}
	else
	{
		memset(&m.bytes[0], 0x11, 512);
		memset(&m.bytes[size_t(VirtualBytes) - 512], 0x66, 512);
	}
	return m;
}

static void repairFooter(Memory& m)
{
	checksum(m.bytes, 0, 512, 64);
	memcpy(&m.bytes[m.bytes.size() - 512], &m.bytes[0], 512);
}
static void parentReads()
{
	for (unsigned dynamic = 0; dynamic < 2; dynamic++)
	{
		Memory m = fixture(dynamic != 0);
		const std::vector<uint8_t> untouched = m.bytes;
		DBPVHD::Parent p;
		CHECK(p.Open(m, ParentTimestamp)); CHECK(p.SectorCount() == Sectors);
		uint8_t data[512];
		CHECK(p.ReadSector(0, data)); CHECK(data[0] == 0x11 && data[511] == 0x11);
		CHECK(p.ReadSector(3, data)); CHECK(DBPVHD::Detail::AllZero(data, sizeof(data)));
		CHECK(p.ReadSector(4096, data)); CHECK(DBPVHD::Detail::AllZero(data, sizeof(data)));
		CHECK(p.ReadSector(Sectors - 1, data)); CHECK(data[0] == 0x66);
		CHECK(!p.ReadSector(Sectors, data)); CHECK(!p.ReadSector(UINT64_MAX, data));
		CHECK(m.bytes == untouched); CHECK(m.writes == 0);
		m.failRead = m.reads + 1;
		CHECK(!p.ReadSector(0, data)); CHECK(p.Error() != NULL);
		CHECK(p.ReadSector(0, data)); CHECK(data[0] == 0x11); // No poisoned cache.
	}
	Memory good = fixture(true);
	DBPVHD::Parent p; CHECK(p.Open(good, ParentTimestamp));
	for (size_t failure = 1; failure <= good.reads; failure++)
	{
		Memory broken = good; broken.reads = 0; broken.failRead = failure;
		DBPVHD::Parent q; CHECK(!q.Open(broken, ParentTimestamp)); CHECK(q.SectorCount() == 0);
	}
}

static void childRoundTrip(bool dynamic)
{
	Memory original = fixture(dynamic), saved;
	const std::vector<uint8_t> untouched = original.bytes;
	DBPVHD::Parent p; CHECK(p.Open(original, ParentTimestamp));
	DBPVHD::Child c; CHECK(c.Create(saved, p, ChildId, 98765, "WIN98.VHD"));
	CHECK(saved.Size() == 3072);
	CHECK(number(saved.bytes, 60, 4) == 4); CHECK(number(saved.bytes, 568, 4) == ParentTimestamp);
	CHECK(!memcmp(&saved.bytes[552], &original.bytes[original.bytes.size() - 512 + 68], 16));
	CHECK(!memcmp(&saved.bytes[1088], "W2ru", 4));
	CHECK(number(saved.bytes, 1092, 4) == 512); CHECK(number(saved.bytes, 1096, 4) == 18);
	CHECK(saved.bytes[577] == 'W' && saved.bytes[2048] == 'W' && saved.bytes[2049] == 0);
	uint8_t data[512], zero[512] = {0}, same[512]; memset(same, 0x11, 512);
	CHECK(c.ReadSector(0, data)); CHECK(!memcmp(data, same, 512));
	CHECK(c.WriteSector(0, same)); CHECK(saved.Size() == 3072);
	CHECK(c.WriteSector(0, zero)); CHECK(c.ReadSector(0, data)); CHECK(!memcmp(data, zero, 512));
	const size_t block = size_t(number(saved.bytes, 1536, 4)) * 512;
	CHECK(saved.bytes[block] == 0x80); CHECK(saved.Size() == 3072 + 512 + 2 * 1024 * 1024);
	CHECK(c.WriteSector(0, same)); CHECK(saved.bytes[block] == 0);
	CHECK(c.ReadSector(0, data)); CHECK(!memcmp(data, same, 512));
	CHECK(!c.WriteSector(Sectors, same)); CHECK(!c.IsFaulted());

	std::vector<uint8_t> expected(size_t(VirtualBytes), 0);
	memset(&expected[0], 0x11, 512); memset(&expected[expected.size() - 512], 0x66, 512);
	const uint64_t boundaries[] = {0, 7, 8, 4095, 4096, 8191, 8192};
	for (size_t i = 0; i < sizeof(boundaries) / sizeof(boundaries[0]); i++)
	{
		memset(data, int(i + 0x70), 512);
		CHECK(c.WriteSector(boundaries[i], data)); memcpy(&expected[size_t(boundaries[i]) * 512], data, 512);
	}
	uint32_t rng = 0xcafebabe;
	for (unsigned i = 0; i < 300; i++)
	{
		rng = rng * 1664525U + 1013904223U;
		const uint64_t sector = rng % Sectors;
		memset(data, (i % 3) ? int(rng >> 24) : 0, 512);
		CHECK(c.WriteSector(sector, data)); memcpy(&expected[size_t(sector) * 512], data, 512);
	}
	DBPVHD::Child reopened; CHECK(reopened.Open(saved, p));
	for (uint64_t sector = 0; sector < Sectors; sector++)
	{
		CHECK(reopened.ReadSector(sector, data)); CHECK(!memcmp(data, &expected[size_t(sector) * 512], 512));
	}
	CHECK(original.bytes == untouched); CHECK(original.writes == 0);
	DBPVHD::Parent chain; CHECK(!chain.Open(saved, ParentTimestamp));
	CHECK(!c.Create(original, p, ChildId, 0, "WIN98.VHD"));
	CHECK(original.bytes == untouched);
}

static void invalidImages()
{
	Memory original = fixture(true); DBPVHD::Parent p; CHECK(p.Open(original, ParentTimestamp));
	Memory saved; DBPVHD::Child c; CHECK(c.Create(saved, p, ChildId, 98765, "WIN98.VHD"));
	for (unsigned problem = 0; problem < 15; problem++)
	{
		Memory bad = saved;
		switch (problem)
		{
		case 0: bad.bytes[100] ^= 1; break; // Invalid checksum/copy.
		case 1: be(bad.bytes, 48, 0, 8); repairFooter(bad); break;
		case 2: be(bad.bytes, 16, UINT64_MAX, 8); repairFooter(bad); break;
		case 3: be(bad.bytes, 544, 0, 4); break; // Zero block size.
		case 4: be(bad.bytes, 544, 513, 4); break;
		case 5: be(bad.bytes, 540, 0xffffffffU, 4); break;
		case 6: be(bad.bytes, 528, 512, 8); break; // Table overlaps header.
		case 7: be(bad.bytes, 1536, 0xfffffffeU, 4); break;
		case 8: be(bad.bytes, 1104, 512, 8); break; // Locator overlaps header.
		case 9: be(bad.bytes, 1092, 1, 4); break;
		case 10: be(bad.bytes, 1096, 513, 4); break;
		case 11: bad.bytes[552] ^= 1; break; // Wrong parent UUID.
		case 12: be(bad.bytes, 568, ParentTimestamp + 1, 4); break;
		case 13: be(bad.bytes, 48, VirtualBytes + 512, 8); repairFooter(bad); break;
		case 14: bad.bytes.resize(bad.bytes.size() - 1); break;
		}
		checksum(bad.bytes, 512, 1024, 36);
		DBPVHD::Child broken; CHECK(!broken.Open(bad, p)); CHECK(broken.Error() != NULL);
		uint8_t sector[512]; CHECK(!broken.ReadSector(0, sector));
	}
	uint8_t data[512]; memset(data, 0xab, 512);
	CHECK(c.WriteSector(1, data)); CHECK(c.WriteSector(4096, data));
	Memory overlap = saved;
	memcpy(&overlap.bytes[1540], &overlap.bytes[1536], 4);
	DBPVHD::Child broken; CHECK(!broken.Open(overlap, p));
	Memory truncated = saved; truncated.bytes.erase(truncated.bytes.end() - 1024, truncated.bytes.end() - 512);
	CHECK(!broken.Open(truncated, p));
	Memory wrongParent = fixture(false); wrongParent.bytes[wrongParent.bytes.size() - 512 + 68] ^= 1;
	checksum(wrongParent.bytes, wrongParent.bytes.size() - 512, 512, 64);
	DBPVHD::Parent other; CHECK(other.Open(wrongParent, ParentTimestamp)); CHECK(!broken.Open(saved, other));
	CHECK(other.Open(original, ParentTimestamp + 1)); CHECK(!broken.Open(saved, other));
	Memory empty; CHECK(!broken.Create(empty, p, ChildId, 1, "../WIN98.VHD")); CHECK(empty.Size() == 0);
	CHECK(!broken.Create(empty, p, ChildId, 1, "..")); CHECK(empty.Size() == 0);
}

static void ioFailures()
{
	Memory original = fixture(true); DBPVHD::Parent p; CHECK(p.Open(original, ParentTimestamp));
	Memory seed; DBPVHD::Child baseline; CHECK(baseline.Create(seed, p, ChildId, 98765, "WIN98.VHD"));
	for (size_t i = 1; i <= seed.writes; i++)
	{
		Memory broken; broken.failWrite = i;
		DBPVHD::Child c; CHECK(!c.Create(broken, p, ChildId, 98765, "WIN98.VHD")); CHECK(c.IsFaulted());
	}
	for (size_t i = 1; i <= seed.reads; i++)
	{
		Memory broken = seed; broken.reads = 0; broken.failRead = i;
		DBPVHD::Child c; CHECK(!c.Open(broken, p));
	}
	uint8_t data[512]; memset(data, 0xac, 512);
	Memory success = seed; success.writes = 0;
	DBPVHD::Child c; CHECK(c.Open(success, p)); CHECK(c.WriteSector(0, data));
	const size_t failures[] = {1, 256, success.writes - 4, success.writes - 3, success.writes - 2, success.writes - 1, success.writes};
	for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); i++)
	{
		Memory broken = seed; broken.writes = 0; broken.failWrite = failures[i];
		DBPVHD::Child d; CHECK(d.Open(broken, p)); CHECK(!d.WriteSector(0, data)); CHECK(d.IsFaulted());
		const size_t writes = broken.writes;
		CHECK(!d.WriteSector(1, data)); CHECK(!d.ReadSector(0, data)); CHECK(broken.writes == writes);
	}
	Memory broken = success; DBPVHD::Child d; CHECK(d.Open(broken, p));
	broken.failRead = broken.reads + 1; CHECK(!d.ReadSector(0, data)); CHECK(d.IsFaulted());
	CHECK(d.Open(broken, p)); broken.failRead = broken.reads + 2;
	CHECK(!d.ReadSector(0, data)); CHECK(d.IsFaulted()); // Payload short read.
	CHECK(d.Open(broken, p)); original.failRead = original.reads + 1;
	CHECK(!d.ReadSector(3, data)); CHECK(d.IsFaulted()); // Parent fallback bitmap failure.
	original.failRead = SIZE_MAX; CHECK(d.Open(broken, p));
	original.failRead = original.reads + 1; const size_t writes = broken.writes;
	CHECK(!d.WriteSector(1, data)); CHECK(d.IsFaulted()); CHECK(broken.writes == writes);
	CHECK(original.writes == 0);
}

static void largeVirtualDisk()
{
	const uint64_t largeSize = uint64_t(5) * 1024 * 1024 * 1024;
	Memory smallFixture = fixture(true), large;
	large.bytes.resize(12288);
	memcpy(&large.bytes[0], &smallFixture.bytes[0], 1536);
	be(large.bytes, 40, largeSize, 8); be(large.bytes, 48, largeSize, 8); repairFooter(large);
	be(large.bytes, 540, 2560, 4); checksum(large.bytes, 512, 1024, 36);
	memset(&large.bytes[1536], 0xff, 10240);
	DBPVHD::Parent p; CHECK(p.Open(large, ParentTimestamp));
	Memory saved; DBPVHD::Child c; CHECK(c.Create(saved, p, ChildId, 1, "LARGE.VHD"));
	uint8_t sector[512], readBack[512]; memset(sector, 0x98, 512);
	CHECK(c.WriteSector(largeSize / 512 - 1, sector));
	DBPVHD::Child again; CHECK(again.Open(saved, p));
	CHECK(again.ReadSector(largeSize / 512 - 1, readBack)); CHECK(!memcmp(sector, readBack, 512));
	CHECK(saved.Size() < 3 * 1024 * 1024); CHECK(large.writes == 0);
	CHECK(p.Open(large, ParentTimestamp)); // Explicit remount invalidates old bindings.
	CHECK(!again.ReadSector(0, readBack)); CHECK(again.IsFaulted());
}

static void dosFileAdapter()
{
	struct File
	{
		uint64_t length = 200000, position = 0;
		unsigned reads = 0, writes = 0;
		bool failSeek = false, narrowSeek = false, shortRead = false, shortWrite = false;
		bool Seek64(uint64_t* offset, uint32_t origin)
		{
			if (failSeek) return false;
			position = origin == 2 ? length : *offset;
			if (narrowSeek) position = uint32_t(position);
			*offset = position;
			return true;
		}
		bool Read(uint8_t* data, uint16_t* bytes)
		{
			reads++;
			if (shortRead) *bytes /= 2;
			memset(data, 0x5a, *bytes); position += *bytes;
			return true;
		}
		bool Write(uint8_t*, uint16_t* bytes)
		{
			writes++;
			if (shortWrite) *bytes /= 2;
			position += *bytes;
			if (position > length) length = position;
			return true;
		}
	} file;
	VhdDOSSource<File> reader, writer;
	std::vector<uint8_t> data(70000);
	CHECK(reader.Open(&file, false)); CHECK(reader.Read(500, data.data(), data.size()));
	CHECK(file.reads == 2 && file.position == 70500 && data.back() == 0x5a);
	CHECK(!reader.Write(0, data.data(), 1)); CHECK(file.writes == 0);
	CHECK(!reader.Read(UINT64_MAX, data.data(), 1));
	CHECK(!reader.Read(199999, data.data(), 2));
	file.shortRead = true; CHECK(!reader.Read(0, data.data(), 512)); file.shortRead = false;
	file.failSeek = true; CHECK(!reader.Read(0, data.data(), 512)); file.failSeek = false;
	CHECK(writer.Open(&file, true)); CHECK(writer.Write(200000, data.data(), data.size()));
	CHECK(file.writes == 2 && writer.Size() == 270000);
	CHECK(!writer.Write(0x80000000ULL, data.data(), 1));
	CHECK(!writer.Write(0x7fffffffULL, data.data(), 1)); CHECK(file.writes == 2);
	file.shortWrite = true; CHECK(!writer.Write(0, data.data(), 512));
	file.length = uint64_t(6) * 1024 * 1024 * 1024;
	CHECK(!writer.Open(&file, true)); CHECK(!writer.Write(0, data.data(), 1)); CHECK(reader.Open(&file, false));
	CHECK(reader.Read(0x100000000ULL, data.data(), 512)); CHECK(file.position == 0x100000200ULL);
	file.narrowSeek = true; const unsigned reads = file.reads;
	CHECK(!reader.Read(0x100000000ULL, data.data(), 512)); CHECK(file.reads == reads);
}

#ifdef _WIN32
static void winCheck(DWORD status, const char* operation)
{
	if (status != ERROR_SUCCESS) fprintf(stderr, "%s: Windows error %lu (0x%08lx)\n", operation, status, status);
	CHECK(status == ERROR_SUCCESS);
}
static void writeSynthetic(const std::wstring& path, const Memory& image, bool parent)
{
	HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
	CHECK(file != INVALID_HANDLE_VALUE);
	DWORD written = 0;
	CHECK(WriteFile(file, image.bytes.data(), DWORD(image.bytes.size()), &written, NULL));
	CHECK(written == image.bytes.size());
	if (parent)
	{
		// FILETIME epoch 1601 -> VHD timestamp epoch 2000. Set the parent mtime
		// independently so Windows must resolve the timestamp stored in the child.
		ULARGE_INTEGER timestamp;
		timestamp.QuadPart = 125911584000000000ULL + uint64_t(ParentTimestamp) * 10000000;
		FILETIME modified = {timestamp.LowPart, timestamp.HighPart};
		CHECK(SetFileTime(file, NULL, NULL, &modified));
	}
	CHECK(CloseHandle(file));
}
static void windowsInterop()
{
	wchar_t cwd[32768];
	const DWORD count = GetCurrentDirectoryW(32768, cwd);
	CHECK(count > 0 && count < 32768);
	const std::wstring directory = std::wstring(cwd) + L"\\interop-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
	CHECK(CreateDirectoryW(directory.c_str(), NULL));
	for (unsigned dynamic = 0; dynamic < 2; dynamic++)
	{
		const std::wstring caseDirectory = directory + (dynamic ? L"\\dynamic" : L"\\fixed");
		CHECK(CreateDirectoryW(caseDirectory.c_str(), NULL));
		Memory original = fixture(dynamic != 0), saved;
		DBPVHD::Parent p; CHECK(p.Open(original, ParentTimestamp));
		DBPVHD::Child c; CHECK(c.Create(saved, p, ChildId, 98765, "PARENT.VHD"));
		uint8_t zero[512] = {0}; CHECK(c.WriteSector(0, zero));
		writeSynthetic(caseDirectory + L"\\PARENT.VHD", original, true);
		writeSynthetic(caseDirectory + L"\\CHILD.VHD", saved, false);
		VIRTUAL_STORAGE_TYPE storage = {VIRTUAL_STORAGE_TYPE_DEVICE_VHD, VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT};
		OPEN_VIRTUAL_DISK_PARAMETERS parameters = {};
		parameters.Version = OPEN_VIRTUAL_DISK_VERSION_1;
		parameters.Version1.RWDepth = OPEN_VIRTUAL_DISK_RW_DEPTH_DEFAULT;
		HANDLE disk = INVALID_HANDLE_VALUE;
		// Read metadata with the complete chain; never attach/mount the disk.
		winCheck(OpenVirtualDisk(&storage, (caseDirectory + L"\\CHILD.VHD").c_str(), VIRTUAL_DISK_ACCESS_GET_INFO,
			OPEN_VIRTUAL_DISK_FLAG_NONE, &parameters, &disk), "OpenVirtualDisk parent chain");
		GET_VIRTUAL_DISK_INFO info = {};
		ULONG infoBytes = sizeof(info);
		info.Version = GET_VIRTUAL_DISK_INFO_SIZE;
		winCheck(GetVirtualDiskInformation(disk, &infoBytes, &info, NULL), "GetVirtualDiskInformation size");
		CHECK(info.Size.VirtualSize == VirtualBytes); CHECK(info.Size.BlockSize == 2 * 1024 * 1024);
		CHECK(info.Size.SectorSize == 512);
		infoBytes = sizeof(info); info.Version = GET_VIRTUAL_DISK_INFO_PROVIDER_SUBTYPE;
		winCheck(GetVirtualDiskInformation(disk, &infoBytes, &info, NULL), "GetVirtualDiskInformation subtype");
		CHECK(info.ProviderSubtype == 4);
		infoBytes = sizeof(info); info.Version = GET_VIRTUAL_DISK_INFO_PARENT_TIMESTAMP;
		winCheck(GetVirtualDiskInformation(disk, &infoBytes, &info, NULL), "GetVirtualDiskInformation parent timestamp");
		CHECK(info.ParentTimestamp == ParentTimestamp);
		struct { GET_VIRTUAL_DISK_INFO info; wchar_t extraPathSpace[32768]; } location = {};
		infoBytes = sizeof(location); location.info.Version = GET_VIRTUAL_DISK_INFO_PARENT_LOCATION;
		winCheck(GetVirtualDiskInformation(disk, &infoBytes, &location.info, NULL), "GetVirtualDiskInformation parent resolution");
		CHECK(location.info.ParentLocation.ParentResolved);
		CHECK(wcsstr(location.info.ParentLocation.ParentLocationBuffer, L"PARENT.VHD") != NULL);
		CHECK(CloseHandle(disk));
	}
	printf("PASS: Windows opened both synthetic parent chains and recognized type-4 metadata (no disks attached)\n");
}
#endif

static void identityBinding()
{
	Memory original = fixture(true), saved;
	DBPVHD::Identity id;
	id.package = "org.example.win98"; id.disk = "os"; id.parent = "BASE.VHD"; id.child = "CHILD.VHD";
	id.virtualSize = VirtualBytes; memcpy(id.parentUuid, &original.bytes[68], 16); memset(id.sha256, 0xab, 32);
	CHECK(id.Valid());
	uint8_t binding[512]; CHECK(id.Binding(binding, ChildId, ParentTimestamp)); CHECK(id.Matches(binding, ChildId));
	CHECK(!id.Matches(binding, id.parentUuid));
	for (unsigned i = 0; i < sizeof(binding); ++i)
	{
		if (i >= DBPVHD::Identity::TimestampOffset && i < DBPVHD::Identity::TimestampOffset + 4) continue;
		binding[i] ^= 1; CHECK(!id.Matches(binding, ChildId)); binding[i] ^= 1;
	}
	DBPVHD::Identity changed = id;
	changed.package += "2"; CHECK(!changed.Matches(binding, ChildId)); changed = id;
	changed.disk += "2"; CHECK(!changed.Matches(binding, ChildId)); changed = id;
	changed.parent = "OTHER.VHD"; CHECK(!changed.Matches(binding, ChildId)); changed = id;
	changed.child = "OTHER.VHD"; CHECK(!changed.Matches(binding, ChildId)); changed = id;
	changed.virtualSize += 512; CHECK(!changed.Matches(binding, ChildId));
	CHECK(!DBPVHD::Identity::Name("BASE.VHD.")); CHECK(!DBPVHD::Identity::Name("..\\B.VHD"));
	CHECK(!DBPVHD::Identity::Name("base.vhd")); CHECK(DBPVHD::Identity::Name("LONGNAME.VHD"));
	uint64_t size; CHECK(DBPVHD::Identity::Size("2190433320960", size)); CHECK(size == UINT64_C(2190433320960));
	CHECK(!DBPVHD::Identity::Size("18446744073709551616", size)); CHECK(!DBPVHD::Identity::Size("123x", size));
	CHECK(!DBPVHD::Identity::Size("01", size)); CHECK(!DBPVHD::Identity::Size("", size));
	uint8_t hex[2]; CHECK(DBPVHD::Identity::Hex("ab09", hex, 2)); CHECK(hex[0] == 0xab && hex[1] == 9);
	CHECK(!DBPVHD::Identity::Hex("AB09", hex, 2)); CHECK(!DBPVHD::Identity::Hex("a", hex, 2));
	// The retained timestamp is checked against the child header by the codec.
	DBPVHD::Parent p; CHECK(p.Open(original, ParentTimestamp));
	DBPVHD::Child child; CHECK(child.Create(saved, p, ChildId, 98765, "BASE.VHD"));
	DBPVHD::Parent repacked; CHECK(repacked.Open(original, ParentTimestamp + 1));
	DBPVHD::Child reopened; CHECK(!reopened.Open(saved, repacked));
	CHECK(repacked.Open(original, DBPVHD::Detail::BE32(binding + DBPVHD::Identity::TimestampOffset)));
	CHECK(reopened.Open(saved, repacked));
}

int main(int argc, char** argv)
{
	parentReads(); childRoundTrip(false); childRoundTrip(true); invalidImages(); ioFailures(); largeVirtualDisk(); dosFileAdapter(); identityBinding();
#ifdef _WIN32
	if (argc == 2 && !strcmp(argv[1], "--windows-interop")) windowsInterop();
	else CHECK(argc == 1);
#else
	(void)argc; (void)argv;
#endif
	printf("PASS: %u checks; fixed/dynamic parents, child round trips, corruption and I/O faults\n", checks);
	return 0;
}
