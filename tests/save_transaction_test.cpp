/* Production save transaction regression tests. GPL-2.0-or-later. */
#include "../src/dos/save_transaction.h"
#include <cassert>
#include <iostream>

static unsigned checks = 0;
static void Check(bool value) { ++checks; if (!value) { std::cerr << "Failed check " << checks << '\n'; abort(); } }
static void Put16(std::vector<unsigned char>& bytes, size_t offset, uint16_t value)
{
	bytes[offset] = (unsigned char)value; bytes[offset + 1] = (unsigned char)(value >> 8);
}
static void Put32(std::vector<unsigned char>& bytes, size_t offset, uint32_t value)
{
	for (size_t i = 0; i < 4; ++i) bytes[offset + i] = (unsigned char)(value >> (i * 8));
}
static std::vector<unsigned char> Generation(unsigned value)
{
	const std::string name = "SAVE.DAT";
	std::vector<unsigned char> data(131079, (unsigned char)value);
	const size_t central = 30 + name.size() + data.size(), end = central + 46 + name.size();
	std::vector<unsigned char> bytes(end + 22, 0);
	Put32(bytes, 0, 0x04034b50); Put32(bytes, 14, DBPSave::CRC(&data[0], data.size()));
	Put32(bytes, 18, (uint32_t)data.size()); Put32(bytes, 22, (uint32_t)data.size()); Put16(bytes, 26, (uint16_t)name.size());
	memcpy(&bytes[30], name.data(), name.size()); memcpy(&bytes[30 + name.size()], &data[0], data.size());
	Put32(bytes, central, 0x02014b50); memcpy(&bytes[central + 6], &bytes[4], 26); memcpy(&bytes[central + 46], name.data(), name.size());
	Put32(bytes, end, 0x06054b50); Put16(bytes, end + 8, 1); Put16(bytes, end + 10, 1);
	Put32(bytes, end + 12, (uint32_t)(46 + name.size())); Put32(bytes, end + 16, (uint32_t)central);
	return bytes;
}
static bool Seed(const std::string& path, const std::vector<unsigned char>& bytes)
{
	FILE* file = DBPSave::Open(path, "wb");
	if (!file) return false;
	bool ok = fwrite(&bytes[0], 1, bytes.size(), file) == bytes.size() && DBPSave::Flush(file);
	return fclose(file) == 0 && ok;
}
static std::vector<unsigned char> Read(const std::string& path)
{
	FILE* file = DBPSave::Open(path, "rb");
	if (!file) return std::vector<unsigned char>();
	std::vector<unsigned char> result((size_t)DBPSave::Size(file));
	DBPSave::Seek(file, 0); Check(fread(&result[0], 1, result.size(), file) == result.size()); fclose(file);
	return result;
}
static void Env(const char* key, const std::string& value)
{
#if defined(_WIN32)
	Check(_putenv_s(key, value.c_str()) == 0);
#else
	Check(setenv(key, value.c_str(), 1) == 0);
#endif
}
static bool Publish(DBPSave::Transaction& transaction, const std::string& path, const std::vector<unsigned char>& bytes)
{
	FILE* file = transaction.Begin();
	return file && transaction.Finish(file, DBPSave::Write(file, &bytes[0], bytes.size(), path));
}
static bool RejectRecovery(const std::string&, void*) { return false; }

int main(int argc, char** argv)
{
	if (argc != 2) { std::cerr << "Pass a newly created isolated output directory.\n"; return 2; }
	std::string root = argv[1], save = root + "/transaction.pure.zip";
	Env("DBP_TEST_SAVE_ROOT", root);
	Env("DBP_TEST_SAVE_FAULT", ""); Env("DBP_TEST_SAVE_PAUSE", "");
	const auto first = Generation(1), second = Generation(2);
	Check(!DBPSave::Exists(save));
	{
		DBPSave::Transaction writer, rival, different;
		Check(writer.Acquire(save)); Check(!rival.Acquire(save)); Check(different.Acquire(root + "/separate.pure.zip"));
		bool recovered = true; Check(writer.Recover(recovered) && !recovered);
		Check(Publish(writer, save, first)); Check(DBPSave::Validate(save)); Check(Read(save) == first);
		for (const char* fault : {"open", "short_write", "flush", "close", "backup_flush", "replace", "publish_flush"})
		{
			Env("DBP_TEST_SAVE_FAULT", fault);
			Check(!Publish(writer, save, second)); Check(DBPSave::Validate(save)); Check(Read(save) == first);
			Env("DBP_TEST_SAVE_FAULT", "");
			Check(writer.Recover(recovered)); Check(Read(save) == first);
		}
		Env("DBP_TEST_SAVE_FAULT", "flush"); Env("DBP_TEST_SAVE_FAULT_ONCE", "1");
		Check(!Publish(writer, save, second)); Check(Publish(writer, save, second));
		Env("DBP_TEST_SAVE_FAULT", ""); Env("DBP_TEST_SAVE_FAULT_ONCE", "");
		Check(Read(save) == second); Check(Read(save + ".previous") == first);
	}
	{
		DBPSave::Transaction writer; Check(writer.Acquire(save)); // orderly release
		auto corrupt = second; corrupt[50] ^= 1;
		Check(Seed(save, corrupt)); Check(!DBPSave::Validate(save));
		bool recovered = false; Check(writer.Recover(recovered) && recovered); Check(Read(save) == first);
		Check(Seed(save + ".pending", second)); Check(writer.Recover(recovered) && !recovered); Check(!DBPSave::Exists(save + ".pending"));
		for (size_t length : {size_t(1), size_t(30), first.size() - 1})
		{
			std::vector<unsigned char> truncated(first.begin(), first.begin() + length);
			Check(Seed(root + "/invalid.zip", truncated)); Check(!DBPSave::Validate(root + "/invalid.zip"));
		}
	}
	const std::string initial = root + "/first-save.pure.zip";
	{
		Check(Seed(initial + ".pending", first)); DBPSave::Transaction writer;
		Check(writer.Acquire(initial)); bool recovered = false; Check(writer.Recover(recovered) && recovered); Check(Read(initial) == first);
	}
	const std::string unrelated = root + "/unrelated.pure.zip";
	{
		Check(Seed(unrelated + ".pending", first)); DBPSave::Transaction writer;
		Check(writer.Acquire(unrelated, NULL, RejectRecovery)); bool recovered = false;
		Check(!writer.Recover(recovered)); Check(!DBPSave::Exists(unrelated)); Check(Read(unrelated + ".pending") == first);
	}
	const std::string failed_initial = root + "/first-flush-failure.pure.zip";
	{
		DBPSave::Transaction writer; Check(writer.Acquire(failed_initial));
		Env("DBP_TEST_SAVE_FAULT", "publish_flush");
		Check(!Publish(writer, failed_initial, first));
		Check(!DBPSave::Exists(failed_initial)); Check(Read(failed_initial + ".pending") == first);
		Env("DBP_TEST_SAVE_FAULT", "");
		bool recovered = false; Check(writer.Recover(recovered) && recovered); Check(Read(failed_initial) == first);
	}
	std::cout << checks << " save transaction checks passed\n";
	return 0;
}
