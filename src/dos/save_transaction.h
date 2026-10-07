/* Downstream save-generation publication. GPL-2.0-or-later, as drive_union.cpp. */
#ifndef DBP_SAVE_TRANSACTION_H
#define DBP_SAVE_TRANSACTION_H

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdint.h>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <algorithm>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>
#endif

namespace DBPSave
{
inline uint64_t ClockMs()
{
	return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
#if defined(_WIN32)
inline std::wstring Wide(const std::string& path)
{
	int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.c_str(), -1, NULL, 0);
	if (!count) return std::wstring();
	std::wstring result((size_t)count, L'\0');
	MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.c_str(), -1, &result[0], count);
	return result;
}
#endif
inline FILE* Open(const std::string& path, const char* mode)
{
#if defined(_WIN32)
	std::wstring p = Wide(path), m;
	for (const char* c = mode; *c; ++c) m += (wchar_t)*c;
	return p.empty() ? NULL : _wfopen(p.c_str(), m.c_str());
#else
	return fopen(path.c_str(), mode);
#endif
}
inline bool Exists(const std::string& path)
{
#if defined(_WIN32)
	std::wstring p = Wide(path);
	return !p.empty() && GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
#else
	return access(path.c_str(), F_OK) == 0;
#endif
}
inline bool Remove(const std::string& path)
{
#if defined(_WIN32)
	std::wstring p = Wide(path);
	return !Exists(path) || (!p.empty() && DeleteFileW(p.c_str()));
#else
	return !Exists(path) || unlink(path.c_str()) == 0;
#endif
}
inline bool Flush(FILE* file)
{
	if (fflush(file)) return false;
#if defined(_WIN32)
	return FlushFileBuffers((HANDLE)_get_osfhandle(_fileno(file))) != 0;
#else
	return fsync(fileno(file)) == 0;
#endif
}
inline bool FlushPath(const std::string& path)
{
	FILE* file = Open(path, "rb+");
	if (!file) return false;
	bool ok = Flush(file);
	return fclose(file) == 0 && ok;
}
inline bool Replace(const std::string& source, const std::string& target)
{
#if defined(_WIN32)
	std::wstring s = Wide(source), t = Wide(target);
	return !s.empty() && !t.empty() && MoveFileExW(s.c_str(), t.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
#else
	if (rename(source.c_str(), target.c_str())) return false;
	std::string parent = target.substr(0, target.find_last_of('/'));
	int fd = open(parent.c_str(), O_RDONLY | O_DIRECTORY);
	if (fd < 0) return false;
	bool ok = fsync(fd) == 0;
	close(fd);
	return ok;
#endif
}

// Failure injection is enabled only by an explicit isolated test-root prefix.
// No package metadata, UI setting or ordinary save enables it.
inline bool TestEnabled(const std::string& save)
{
	const char* root = getenv("DBP_TEST_SAVE_ROOT");
	if (!root || !*root) return false;
	std::string a = save, b = root;
	for (size_t i = 0; i < a.size(); ++i) { if (a[i] == '\\') a[i] = '/'; if (a[i] >= 'A' && a[i] <= 'Z') a[i] += 'a' - 'A'; }
	for (size_t i = 0; i < b.size(); ++i) { if (b[i] == '\\') b[i] = '/'; if (b[i] >= 'A' && b[i] <= 'Z') b[i] += 'a' - 'A'; }
	if (b.empty()) return false;
	if (b.back() != '/') b += '/';
	return a.compare(0, b.size(), b) == 0;
}
inline bool Fault(const std::string& save, const char* step)
{
	const char* wanted = getenv("DBP_TEST_SAVE_FAULT");
	if (!wanted || !TestEnabled(save) || strcmp(wanted, step)) return false;
	const char* once = getenv("DBP_TEST_SAVE_FAULT_ONCE");
	if (once && !strcmp(once, "1"))
	{
		static std::vector<std::string> used;
		std::string token = save + ":" + step;
		if (std::find(used.begin(), used.end(), token) != used.end()) return false;
		used.push_back(token);
	}
	return true;
}
inline void Stage(const std::string& save, const char* step)
{
	const char* wanted = getenv("DBP_TEST_SAVE_PAUSE");
	if (!wanted || !TestEnabled(save) || strcmp(wanted, step)) return;
	FILE* marker = Open(save + ".test-stage", "wb");
	if (marker) { fwrite(step, strlen(step), 1, marker); Flush(marker); fclose(marker); }
	for (;;) std::this_thread::sleep_for(std::chrono::milliseconds(100));
}
inline bool Write(FILE* file, const void* bytes, size_t count, const std::string& save)
{
	if (!count) return true;
	if (Fault(save, "short_write")) { fwrite(bytes, 1, count - 1, file); return false; }
	return fwrite(bytes, 1, count, file) == count;
}

inline uint16_t LE16(const unsigned char* p) { return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }
inline uint32_t LE32(const unsigned char* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
inline uint32_t CRC(const unsigned char* bytes, size_t count, uint32_t crc = 0)
{
	static uint32_t table[256];
	static bool initialized = false;
	if (!initialized)
	{
		for (uint32_t n = 0; n < 256; ++n) { uint32_t c = n; for (int k = 0; k < 8; ++k) c = (c >> 1) ^ ((c & 1) ? 0xedb88320U : 0); table[n] = c; }
		initialized = true;
	}
	crc = ~crc;
	for (size_t n = 0; n < count; ++n) crc = table[(crc ^ bytes[n]) & 255] ^ (crc >> 8);
	return ~crc;
}
inline bool Seek(FILE* file, uint64_t position)
{
#if defined(_WIN32)
	return _fseeki64(file, (__int64)position, SEEK_SET) == 0;
#else
	return fseeko(file, (off_t)position, SEEK_SET) == 0;
#endif
}
inline uint64_t Size(FILE* file)
{
#if defined(_WIN32)
	if (_fseeki64(file, 0, SEEK_END)) return UINT64_MAX;
	__int64 size = _ftelli64(file);
#else
	if (fseeko(file, 0, SEEK_END)) return UINT64_MAX;
	off_t size = ftello(file);
#endif
	return size < 0 ? UINT64_MAX : (uint64_t)size;
}
inline bool SafeName(std::string& name)
{
	if (name.empty() || name.size() > 260 || name[0] == '/' || name[0] == '\\') return false;
	size_t component = 0;
	for (size_t i = 0; i < name.size(); ++i)
	{
		unsigned char c = (unsigned char)name[i];
		if (!c || c == ':' || c == '\\' || c < 32) return false;
		if (c >= 'a' && c <= 'z') name[i] -= 'a' - 'A';
		if (c != '/') continue;
		std::string part = name.substr(component, i - component);
		if (part.empty() || part == "." || part == "..") return false;
		component = i + 1;
	}
	std::string last = name.substr(component);
	return last != "." && last != "..";
}

typedef bool (*DeflateCheck)(const std::string&, const std::string&, uint32_t, uint32_t);
// Validate complete structure and every entry CRC before recovery promotion.
// Stored saves are checked here; the existing archive reader checks deflate.
inline bool Validate(const std::string& path, DeflateCheck deflate = NULL)
{
	FILE* file = Open(path, "rb");
	if (!file) return false;
	bool ok = true;
	unsigned char end[22];
	uint64_t size = Size(file);
	if (size < sizeof(end) || size > UINT32_MAX || !Seek(file, size - sizeof(end)) || fread(end, 1, sizeof(end), file) != sizeof(end)) ok = false;
	uint32_t directory = ok ? LE32(end + 16) : 0, directory_size = ok ? LE32(end + 12) : 0;
	uint16_t count = ok ? LE16(end + 10) : 0;
	if (ok && (LE32(end) != 0x06054b50 || LE16(end + 4) || LE16(end + 6) || LE16(end + 8) != count || LE16(end + 20) || (uint64_t)directory + directory_size != size - 22)) ok = false;
	uint64_t central = directory, local = 0;
	std::vector<std::string> names;
	unsigned char buffer[65536];
	for (uint32_t i = 0; ok && i < count; ++i)
	{
		unsigned char cd[46], lf[30];
		if (central + sizeof(cd) > size - 22 || !Seek(file, central) || fread(cd, 1, sizeof(cd), file) != sizeof(cd)) { ok = false; break; }
		uint16_t name_size = LE16(cd + 28);
		uint32_t data_size = LE32(cd + 24), compressed_size = LE32(cd + 20);
		uint16_t method = LE16(cd + 10), extra_size = LE16(cd + 30), comment_size = LE16(cd + 32);
		if (LE32(cd) != 0x02014b50 || (LE16(cd + 8) & ~0x800) || (method != 0 && method != 8) || LE16(cd + 34) || (method == 0 && compressed_size != data_size) || LE32(cd + 42) != local || !name_size || name_size > 260 || central + 46 + name_size + extra_size + comment_size > size - 22 || local + 30 + name_size + compressed_size > directory) { ok = false; break; }
		std::string name(name_size, '\0');
		if (fread(&name[0], 1, name_size, file) != name_size || !Seek(file, local) || fread(lf, 1, sizeof(lf), file) != sizeof(lf) || LE32(lf) != 0x04034b50 || memcmp(lf + 4, cd + 6, 24)) { ok = false; break; }
		uint16_t local_extra = LE16(lf + 28);
		if (local + 30 + name_size + local_extra + compressed_size > directory) { ok = false; break; }
		std::string local_name(name_size, '\0');
		if (fread(&local_name[0], 1, name_size, file) != name_size || name != local_name || !SafeName(name) || std::find(names.begin(), names.end(), name) != names.end() || (name.back() == '/' && data_size)) { ok = false; break; }
		names.push_back(name);
		if (!Seek(file, local + 30 + name_size + local_extra)) { ok = false; break; }
		if (method == 8)
		{
			if (!deflate || !deflate(path, name, data_size, LE32(cd + 16))) ok = false;
			local += 30 + name_size + local_extra + (uint64_t)compressed_size;
			central += 46 + name_size + extra_size + comment_size;
			continue;
		}
		uint32_t remaining = data_size, crc = 0;
		while (remaining)
		{
			size_t step = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
			if (fread(buffer, 1, step, file) != step) { ok = false; break; }
			crc = CRC(buffer, step, crc); remaining -= (uint32_t)step;
		}
		if (crc != LE32(cd + 16)) ok = false;
		local += 30 + name_size + local_extra + (uint64_t)data_size;
		central += 46 + name_size + extra_size + comment_size;
	}
	if (local != directory || central != size - 22) ok = false;
	return fclose(file) == 0 && ok;
}
inline bool CopyDurable(const std::string& source, const std::string& target, const std::string& test_save = std::string())
{
	FILE *in = Open(source, "rb"), *out = in ? Open(target, "wb") : NULL;
	if (!out) { if (in) fclose(in); return false; }
	if (!test_save.empty()) Stage(test_save, "backup_open");
	unsigned char buffer[65536];
	bool ok = true;
	for (;;)
	{
		size_t count = fread(buffer, 1, sizeof(buffer), in);
		if (count && fwrite(buffer, 1, count, out) != count) { ok = false; break; }
		if (count != sizeof(buffer)) { if (ferror(in)) ok = false; break; }
	}
	if (!test_save.empty()) Stage(test_save, "backup_write");
	if ((!test_save.empty() && Fault(test_save, "backup_flush")) || !Flush(out)) ok = false;
	if (ok && !test_save.empty()) Stage(test_save, "backup_flush");
	if (fclose(in)) ok = false;
	if (fclose(out)) ok = false;
	return ok;
}

class Transaction
{
	std::string save;
	bool (*validator)(const std::string&);
	bool (*recovery_validator)(const std::string&, void*);
	void* recovery_context;
#if defined(_WIN32)
	HANDLE lock;
#else
	int lock;
#endif
	Transaction(const Transaction&);
	Transaction& operator=(const Transaction&);
public:
	Transaction() : validator(NULL), recovery_validator(NULL), recovery_context(NULL), lock(
#if defined(_WIN32)
		INVALID_HANDLE_VALUE
#else
		-1
#endif
	) {}
	~Transaction()
	{
#if defined(_WIN32)
		if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
#else
		if (lock >= 0) close(lock);
#endif
	}
	bool Acquire(const std::string& path, bool (*check)(const std::string&) = NULL, bool (*recovery_check)(const std::string&, void*) = NULL, void* context = NULL)
	{
		save = path;
		validator = check;
		recovery_validator = recovery_check; recovery_context = context;
		// Host flush/lock primitives cannot provide durability for an opaque
		// libretro storage URI. Refuse explicitly rather than mixing VFS FILE
		// objects with native file descriptors or silently weakening guarantees.
		if (path.find("://") != std::string::npos) return false;
#if defined(_WIN32)
		std::wstring p = Wide(save + ".lock");
		if (!p.empty()) lock = CreateFileW(p.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		return lock != INVALID_HANDLE_VALUE;
#else
		lock = open((save + ".lock").c_str(), O_RDWR | O_CREAT, 0600);
		if (lock < 0) return false;
		if (!flock(lock, LOCK_EX | LOCK_NB)) return true;
		close(lock); lock = -1; return false;
#endif
	}
	bool Recover(bool& recovered)
	{
		recovered = false;
		const std::string pending = save + ".pending", previous = save + ".previous";
		if (Exists(save) && Check(save)) { Remove(pending); Remove(save + ".previous.pending"); return true; }
		std::string source;
		if (Exists(previous) && Check(previous) && RecoveryCheck(previous)) source = previous;
		else if (!Exists(save) && !Exists(previous) && Exists(pending) && Check(pending) && RecoveryCheck(pending)) source = pending;
		else return !Exists(save) && !Exists(previous) && !Exists(pending) && !Exists(save + ".previous.pending");
		if (source == pending)
		{
			// A complete candidate can still be only in the host cache after an
			// interrupted pre-flush save. Flush its bytes before promotion.
			if (!FlushPath(pending) || !Replace(pending, save) || !FlushPath(save)) return false;
		}
		else
		{
			if (!CopyDurable(previous, pending) || !Replace(pending, save) || !FlushPath(save)) return false;
		}
		Remove(save + ".previous.pending");
		recovered = true;
		return true;
	}
	FILE* Begin()
	{
		if (Fault(save, "open")) return NULL;
		FILE* out = Open(save + ".pending", "wb");
		if (out) Stage(save, "open");
		return out;
	}
	bool Finish(FILE* out, bool written)
	{
		if (written) Stage(save, "write");
		bool ok = written && !Fault(save, "flush") && Flush(out);
		if (ok) Stage(save, "flush");
		if (fclose(out)) ok = false;
		if (Fault(save, "close")) ok = false;
		if (!ok) return false;
		Stage(save, "close");
		if (!Check(save + ".pending")) return false;
		if (Exists(save))
		{
			if (!Check(save) || !CopyDurable(save, save + ".previous.pending", save) || !Replace(save + ".previous.pending", save + ".previous") || !FlushPath(save + ".previous")) return false;
			Stage(save, "backup");
		}
		if (Fault(save, "replace") || !Replace(save + ".pending", save)) return false;
		Stage(save, "published");
		if (Fault(save, "publish_flush") || !FlushPath(save))
		{
			// Namespace publication already happened. Restore the exact preceding
			// committed bytes if possible; otherwise recovery still has .previous.
			if (Exists(save + ".previous"))
			{
				if (CopyDurable(save + ".previous", save + ".pending")) { Replace(save + ".pending", save); FlushPath(save); }
			}
			else Replace(save, save + ".pending"); // retain failed first generation for recovery
			return false;
		}
		Stage(save, "replace");
		return true;
	}
private:
	bool Check(const std::string& path) { return validator ? validator(path) : Validate(path); }
	bool RecoveryCheck(const std::string& path) { return !recovery_validator || recovery_validator(path, recovery_context); }
};
} // namespace DBPSave
#endif
