/* DOSBox Pure downstream VHD package identity. GPL-2.0-or-later. */
#ifndef DOSBOX_VHD_IDENTITY_H
#define DOSBOX_VHD_IDENTITY_H
#include "vhd_differencing.h"
#include <string>

namespace DBPVHD
{
struct Identity
{
	std::string package, disk, parent, child;
	uint8_t sha256[32] = {}, parentUuid[16] = {};
	uint64_t virtualSize = 0;
	enum { BindingSize = 512, TimestampOffset = 296 };

	static bool Name(const std::string& name)
	{
		if (name.size() < 5 || name.size() > 12 || name.substr(name.size() - 4) != ".VHD") return false;
		for (size_t i = 0; i < name.size() - 4; ++i)
			if (!(name[i] >= 'A' && name[i] <= 'Z') && !(name[i] >= '0' && name[i] <= '9') && name[i] != '_' && name[i] != '-') return false;
		return true;
	}
	static bool Token(const std::string& value, size_t maximum, bool dots = false)
	{
		if (value.empty() || value.size() > maximum) return false;
		for (char c : value)
			if (!(c >= 'A' && c <= 'Z') && !(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9') && c != '_' && c != '-' && !(dots && c == '.')) return false;
		return true;
	}
	bool Valid() const
	{
		return Token(package, 128, true) && Token(disk, 64) && Name(parent) && Name(child) && parent != child &&
			virtualSize && !(virtualSize % 512) && virtualSize <= UINT64_C(2040) * 1024 * 1024 * 1024 &&
			!Detail::AllZero(parentUuid, 16) && !Detail::AllZero(sha256, 32);
	}
	static bool Hex(const char* text, uint8_t* output, size_t bytes)
	{
		if (!text || strlen(text) != bytes * 2) return false;
		for (size_t i = 0; i < bytes * 2; ++i)
		{
			const char c = text[i];
			const int n = (c >= '0' && c <= '9' ? c - '0' : (c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1));
			if (n < 0) return false;
			if (!(i % 2)) output[i / 2] = uint8_t(n << 4); else output[i / 2] |= uint8_t(n);
		}
		return true;
	}
	static bool Size(const char* text, uint64_t& value)
	{
		value = 0;
		if (!text || !*text || *text == '0') return false;
		for (; *text; ++text)
		{
			if (*text < '0' || *text > '9' || value > (UINT64_MAX - uint64_t(*text - '0')) / 10) return false;
			value = value * 10 + uint64_t(*text - '0');
		}
		return true;
	}
	bool Binding(uint8_t out[BindingSize], const uint8_t childUuid[16], uint32_t timestamp) const
	{
		if (!Valid() || Detail::AllZero(childUuid, 16) || !memcmp(childUuid, parentUuid, 16)) return false;
		memset(out, 0, BindingSize); memcpy(out, "DBPVDI01", 8);
		memcpy(out + 8, package.data(), package.size()); memcpy(out + 136, disk.data(), disk.size());
		memcpy(out + 200, parent.data(), parent.size()); memcpy(out + 212, child.data(), child.size());
		memcpy(out + 224, sha256, 32); memcpy(out + 256, parentUuid, 16);
		Detail::Put64(out + 272, virtualSize); memcpy(out + 280, childUuid, 16);
		Detail::Put32(out + TimestampOffset, timestamp);
		return true;
	}
	bool Matches(const uint8_t stored[BindingSize], const uint8_t childUuid[16]) const
	{
		uint8_t expected[BindingSize];
		return Binding(expected, childUuid, Detail::BE32(stored + TimestampOffset)) && !memcmp(expected, stored, BindingSize);
	}
};
}
#endif
