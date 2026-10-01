/* Utility-variant patch-drive regression. GPL-2.0-or-later.
 * Link with a matching release standalone build's object files and ZillaLib.
 * Uses real memoryFile, zipDrive and patchDrive; no emulator/UI or host files.
 */
#include "dosbox.h"
#include "dos_inc.h"
#include "paging.h"
#include "../src/dos/drives.h"
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>

static unsigned checks = 0;
#define CHECK(condition) do { checks++; if (!(condition)) { \
	fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); exit(1); } } while (0)

struct Entry
{
	std::string name, bytes;
	Entry(const std::string& _name, const std::string& _bytes) : name(_name), bytes(_bytes) {}
};

static void Number(std::vector<Bit8u>& bytes, Bit32u value, unsigned count)
{
	while (count--) { bytes.push_back((Bit8u)value); value >>= 8; }
}

// Independently assemble standard stored ZIP records around synthetic strings.
static std::vector<Bit8u> Zip(const std::vector<Entry>& entries)
{
	std::vector<Bit8u> bytes, directory;
	for (const Entry& e : entries)
	{
		Bit32u crc = 0xffffffff;
		for (unsigned char ch : e.bytes)
		{
			crc ^= ch;
			for (int bit = 0; bit != 8; bit++) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
		}
		crc = ~crc;
		const Bit32u offset = (Bit32u)bytes.size(), size = (Bit32u)e.bytes.size();
		Number(bytes, 0x04034b50, 4); Number(bytes, 20, 2);
		Number(bytes, 0, 2); Number(bytes, 0, 2); Number(bytes, 0, 2); Number(bytes, 0x21, 2);
		Number(bytes, crc, 4); Number(bytes, size, 4); Number(bytes, size, 4);
		Number(bytes, (Bit32u)e.name.size(), 2); Number(bytes, 0, 2);
		bytes.insert(bytes.end(), e.name.begin(), e.name.end());
		bytes.insert(bytes.end(), e.bytes.begin(), e.bytes.end());
		Number(directory, 0x02014b50, 4); Number(directory, 20, 2); Number(directory, 20, 2);
		Number(directory, 0, 2); Number(directory, 0, 2); Number(directory, 0, 2); Number(directory, 0x21, 2);
		Number(directory, crc, 4); Number(directory, size, 4); Number(directory, size, 4);
		Number(directory, (Bit32u)e.name.size(), 2); Number(directory, 0, 2); Number(directory, 0, 2);
		Number(directory, 0, 2); Number(directory, 0, 2); Number(directory, 0, 4); Number(directory, offset, 4);
		directory.insert(directory.end(), e.name.begin(), e.name.end());
	}
	const Bit32u offset = (Bit32u)bytes.size();
	bytes.insert(bytes.end(), directory.begin(), directory.end());
	Number(bytes, 0x06054b50, 4); Number(bytes, 0, 2); Number(bytes, 0, 2);
	Number(bytes, (Bit32u)entries.size(), 2); Number(bytes, (Bit32u)entries.size(), 2);
	Number(bytes, (Bit32u)directory.size(), 4); Number(bytes, offset, 4); Number(bytes, 0, 2);
	return bytes;
}

static int Variant(const char* name)
{
	const std::string* value = patchDrive::variants.Get(name);
	CHECK(value != NULL);
	return patchDrive::variants.GetStorageIndex(value) + 1;
}

static std::string Read(DOS_Drive& drive, const char* name)
{
	std::vector<Bit8u> bytes;
	CHECK(DriveGetFileContent(&drive, name, bytes));
	return std::string(bytes.begin(), bytes.end());
}

static void CheckGameFiles(patchDrive& drive, unsigned layers)
{
	for (unsigned i = 0; i != layers; i++)
	{
		char name[20], content[40];
		sprintf(name, "LAYER%u.TXT", i);
		sprintf(content, "game layer %u", i);
		CHECK(Read(drive, name) == content);
	}
	CHECK(Read(drive, "SHARED.TXT") == "last game layer");
	CHECK(Read(drive, "ROOT.TXT") == "root file");
}

static void Exercise(unsigned layers)
{
	patchDrive::ResetVariants();
	std::vector<std::vector<Bit8u> > archives;
	for (unsigned i = 0; i != layers; i++)
	{
		char name[40], content[40];
		sprintf(name, "[GAME]/LAYER%u.TXT", i);
		sprintf(content, "game layer %u", i);
		std::vector<Entry> files;
		files.emplace_back(name, content);
		files.emplace_back("[GAME]/SHARED.TXT", i == layers - 1 ? "last game layer" : "lower game layer");
		if (i == 0)
		{
			files.emplace_back("ROOT.TXT", "root file");
			files.emplace_back("[GAME]/DOS.YML", "run_utility: false\nrun_path: GAME.COM\n");
			files.emplace_back("[TOOL]/DOS.YML", "run_utility: true\nrun_path: TOOL.COM\n");
			files.emplace_back("[OWNTOOL]/DOS.YML", "run_utility: true\nrun_path: OWN.COM\n");
			files.emplace_back("[OWNTOOL]/OWN.TXT", "utility owns files");
		}
		archives.push_back(Zip(files));
	}

	// Backing buffers outlive their memoryFile handles. patchDrive owns the
	// handles and empty underlying drives; it reads all game data from memory.
	patchDrive drive;
	Drives[2] = &drive;
	for (unsigned i = 0; i != layers; i++)
		drive.AddLayer(*new memoryDrive, true, new memoryFile(&archives[i][0], archives[i].size()), true, i == layers - 1);
	const int game = Variant("GAME"), tool = Variant("TOOL"), own = Variant("OWNTOOL");
	CHECK(patchDrive::ActivateVariant(game));
	CheckGameFiles(drive, layers);
	CHECK(patchDrive::ActivateVariant(tool));
	CHECK(patchDrive::dos_yml == "run_utility: true\nrun_path: TOOL.COM\n");
	CheckGameFiles(drive, layers);
	CHECK(!drive.FileExists("OWN.TXT"));
	CHECK(!drive.FileExists("DOS.YML"));
	// Switching away and back restores the same bytes and active utility YML.
	CHECK(patchDrive::ActivateVariant(game));
	CHECK(patchDrive::ActivateVariant(tool));
	CheckGameFiles(drive, layers);
	CHECK(patchDrive::dos_yml == "run_utility: true\nrun_path: TOOL.COM\n");
	// A utility with its own files must not inherit the preceding game variant.
	CHECK(patchDrive::ActivateVariant(own));
	CHECK(Read(drive, "OWN.TXT") == "utility owns files");
	CHECK(!drive.FileExists("LAYER0.TXT"));
	// Default root has no variant files to carry into a fileless utility.
	CHECK(patchDrive::ActivateVariant(0));
	CHECK(patchDrive::ActivateVariant(tool));
	CHECK(!drive.FileExists("LAYER0.TXT"));
	CHECK(Read(drive, "ROOT.TXT") == "root file");
	Drives[2] = NULL;
	printf("PASS: %u patch layer(s)\n", layers);
}

#ifdef _WIN32
// ZillaLib's platform object also provides main; use the wide CRT entry point.
int wmain()
#else
int main()
#endif
{
	// Filesystem enumeration stores its DOS DTA in emulated low memory.
	// Provide that scratch memory directly; CPU, audio and video never start.
	static Bit8u scratch[1024 * 1024] = {};
	MemBase = scratch;
	for (unsigned page = 0; page != sizeof(scratch) / MEM_PAGE_SIZE; page++)
	{
		paging.tlb.read[page] = scratch;
		paging.tlb.write[page] = scratch;
	}
	dos.tables.tempdta = RealMake(0x1000, 0);
	Exercise(1);
	Exercise(2);
	Exercise(3);
	patchDrive::ResetVariants();
	printf("PASS: %u checks\n", checks);
	return 0;
}
