/* DOSBox Pure downstream VHD DOS_File adapter. GPL-2.0-or-later. */
#ifndef DOSBOX_VHD_DOS_SOURCE_H
#define DOSBOX_VHD_DOS_SOURCE_H

#include "vhd_differencing.h"

// File is DOS_File in the emulator; the small template also permits faulting
// test files without linking the emulator. Seek origins use DOS's 0/2 values.
template <class File> class VhdDOSSource : public DBPVHD::WritableSource
{
public:
	VhdDOSSource() : file(NULL), length(0), writable(false) {}
	bool Open(File* source, bool allow_write)
	{
		file = NULL; writable = false; length = 0;
		uint64_t size = 0;
		if (!source || !source->Seek64(&size, 2) || (allow_write && size > 0x7fffffffU)) return false;
		file = source; writable = allow_write; length = size;
		return true;
	}
	uint64_t Size() const override { return length; }
	bool Read(uint64_t offset, void* data, size_t bytes) override
	{
		if (!file || offset > length || bytes > length - offset) return false;
		return Transfer(offset, data, bytes, false);
	}
	bool Write(uint64_t offset, const void* data, size_t bytes) override
	{
		// memoryDrive uses signed 32-bit seek positions. Reject before narrowing,
		// and never let an overflow become a write to the start of the child.
		if (!file || !writable || offset > 0x7fffffffU || bytes > 0x7fffffffU - offset) return false;
		if (!Transfer(offset, const_cast<void*>(data), bytes, true)) return false;
		if (offset + bytes > length) length = offset + bytes;
		return true;
	}
private:
	File* file;
	uint64_t length;
	bool writable;
	bool Transfer(uint64_t offset, void* data, size_t bytes, bool writing)
	{
		uint64_t position = offset;
		if (!file->Seek64(&position, 0) || position != offset) return false;
		uint8_t* buffer = static_cast<uint8_t*>(data);
		while (bytes)
		{
			const uint16_t wanted = uint16_t(bytes > 0xffff ? 0xffff : bytes);
			uint16_t actual = wanted;
			const bool ok = writing ? file->Write(buffer, &actual) : file->Read(buffer, &actual);
			if (!ok || actual != wanted) return false;
			buffer += actual; bytes -= actual;
		}
		return true;
	}
};
#endif
