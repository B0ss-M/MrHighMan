// Omni Sampler 2.4.10: ZIP mount, filename, decompression, safety, and corruption checks.
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <random>
#include <string>
#include <vector>
#include "../src/formats/format.hpp"
#include "../src/fs/fs.hpp"

using namespace omni;

static int fails = 0;
static void check(bool ok, const std::string &what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) fails++;
}

static std::string fixture(const std::string &dir, const std::string &name) {
    return dir + "/" + name;
}

static bool has_entry(const std::vector<DirEntry> &entries, const std::string &name, bool directory) {
    return std::any_of(entries.begin(), entries.end(), [&](const DirEntry &entry) {
        return entry.name == name && entry.dir == directory;
    });
}

static bool throws_on_mount(Vfs &vfs, const std::string &path) {
    try { (void)vfs.list(path); } catch (const ParseError &) { return true; }
    return false;
}

static bool throws_on_open(Vfs &vfs, const std::string &path, const std::string &inner) {
    try { (void)vfs.open(path + "/" + inner); } catch (const ParseError &) { return true; }
    return false;
}

static void check_wav(Vfs &vfs, const std::string &dir, const std::string &zip, const std::string &inner,
                      const std::vector<uint8_t> &expected) {
    const std::string path = fixture(dir, zip);
    const std::vector<DirEntry> root = vfs.list(path);
    check(has_entry(root, "a.wav", false), zip + " lists root WAV");
    check(has_entry(root, "empty", true), zip + " lists empty folder");
    check(has_entry(root, "sub", true), zip + " lists subfolder");
    check(has_entry(vfs.list(path + "/sub"), "b.wav", false), zip + " lists nested WAV");
    check(vfs.is_container(path + "/empty"), zip + " mounts empty folder");
    check(vfs.open(path + "/" + inner)->all() == expected, zip + " WAV bytes round-trip");
}

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: test_zip <zip-fixture-directory>\n");
        return 2;
    }
    register_all();
    const std::string dir = argv[1];
    Vfs vfs;
    const std::vector<uint8_t> expected = open_host_file(fixture(dir, "expected.wav"))->all();

    check(vfs.list(fixture(dir, "empty.zip")).empty(), "empty ZIP mounts");
    check_wav(vfs, dir, "stored.zip", "a.wav", expected);
    check_wav(vfs, dir, "deflate.zip", "a.wav", expected);
    check_wav(vfs, dir, "zip64.zip", "a.wav", expected);
    const std::vector<DirEntry> utf8 = vfs.list(fixture(dir, "utf8.zip"));
    check(has_entry(utf8, "Ünïcode drum.wav", false), "UTF-8 name retained");
    check(vfs.open(fixture(dir, "utf8.zip") + "/Ünïcode drum.wav")->all() == expected, "UTF-8 entry opens");
    const std::vector<DirEntry> cp437 = vfs.list(fixture(dir, "cp437.zip"));
    check(has_entry(cp437, "Café.wav", false), "CP437 name converted to UTF-8");
    check(vfs.open(fixture(dir, "cp437.zip") + "/Café.wav")->all() == expected, "CP437 entry opens");

    const std::vector<DirEntry> traversal = vfs.list(fixture(dir, "traversal.zip"));
    check(has_entry(traversal, "safe.wav", false) && traversal.size() == 1, "unsafe absolute and parent paths are omitted");
    check(throws_on_mount(vfs, fixture(dir, "truncated.zip")), "truncated archive rejected");
    check(throws_on_open(vfs, fixture(dir, "badcrc.zip"), "a.wav"), "bad CRC rejected");
    check(throws_on_mount(vfs, fixture(dir, "bomb.zip")), "zip bomb ratio rejected before inflate");
    check(throws_on_mount(vfs, fixture(dir, "many.zip")), "entry-count limit enforced");
    check(throws_on_open(vfs, fixture(dir, "encrypted.zip"), "a.wav"), "encrypted entry reports unsupported");
    check(throws_on_mount(vfs, fixture(dir, "garbage.zip")), "garbage archive rejected");

    const char *bad[] = {"truncated.zip", "badcrc.zip", "traversal.zip", "bomb.zip", "many.zip", "encrypted.zip", "garbage.zip"};
    std::mt19937 rng(0x5A17);
    for (const char *name : bad) {
        const std::vector<uint8_t> original = open_host_file(fixture(dir, name))->all();
        bool safe = true;
        for (int i = 0; i < 1000 && safe; i++) {
            std::vector<uint8_t> mutated = original;
            std::uniform_int_distribution<size_t> byte(0, mutated.size() - 1);
            mutated[byte(rng)] ^= uint8_t(1u << (rng() & 7));
            BlobPtr blob = make_mem_blob(std::move(mutated));
            const ImageType *type = probe_image("fuzz.zip", *blob);
            if (!type) continue;
            try { (void)type->mount(blob); } catch (const ParseError &) {}
        }
        check(safe, std::string(name) + " survives 1000 single-byte corruptions");
    }
    std::printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}
