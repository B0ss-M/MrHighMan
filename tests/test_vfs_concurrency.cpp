#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <thread>
#include <vector>
#include "../src/fs/fs.hpp"

using namespace omni;

static int fails = 0;
static void check(bool ok, const char *what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++fails;
}

static std::atomic<int> mounts{0};
static bool probe(const std::string &name, const uint8_t *, size_t, uint64_t) {
    return name == "t5-vfs.img";
}
static VolumePtr mount(BlobPtr image) {
    mounts.fetch_add(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    auto v = std::make_shared<TreeVolume>("T5");
    auto h = image->head(1);
    v->add_file(h.empty() || h[0] == 0 ? "OLD" : "NEW", make_mem_blob({1}));
    return VolumePtr(v);
}

static void write(const std::string &path, unsigned char b, bool changed = false) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.put(char(b));
    if (changed) f.put('x');
}

int main() {
    register_image_type(ImageType{"T5 synthetic", probe, mount});
    const std::string path = "/tmp/t5-vfs.img";
    write(path, 0);
    Vfs vfs;
    std::vector<std::thread> ts;
    std::vector<std::vector<DirEntry>> lists(2);
    for (int i = 0; i < 2; ++i) ts.emplace_back([&, i] { lists[size_t(i)] = vfs.list(path); });
    for (auto &t : ts) t.join();
    check(mounts.load() == 1, "same image mount is single-flight");
    check(lists[0][0].name == "OLD" && lists[1][0].name == "OLD", "concurrent callers see the one mount");

    write(path, 1, true);
    auto replaced = vfs.list(path);
    check(replaced[0].name == "NEW" && mounts.load() == 2, "host identity/size invalidates a replaced image");

    TreeVolume lazy("lazy");
    std::atomic<int> builds{0};
    lazy.add_lazy("A", 1, [&] {
        builds.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        return make_mem_blob({7});
    }, false);
    BlobPtr a, b;
    std::thread t1([&] { a = lazy.open("A"); });
    std::thread t2([&] { b = lazy.open("A"); });
    t1.join(); t2.join();
    check(builds.load() == 1, "concurrent lazy opens inflate once");
    check(a && b && a->head(1)[0] == 7 && b->head(1)[0] == 7, "both lazy callers receive the built entry");

    std::remove(path.c_str());
    std::printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}
