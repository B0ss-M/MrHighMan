// Omni Sampler 2.4.9: a project's instruments survive a reopen when their files are not there at that moment (the
// user's report: an ISO image instrument was gone from a project after reopening it). Through mpc_engine(), on a
// disk image (ISO 9660) reached through a symlink that is taken away and put back:
//   - a project restored while its image is missing: the slot says MISSING and the project still saves it;
//   - the image comes back: the slot loads by itself;
//   - the drive is mounted under another name (/media/<a> -> /media/<b>): found there;
//   - an extracted .omni deleted: the project loads its source (the image) instead;
//   - clearing a missing slot forgets it.
//   test_restore <image> <preset path inside it> <expected name>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <unistd.h>
#include <sys/stat.h>
#include <vector>
extern "C" {
#include "engine.h"
}

static int fails = 0;
static void check(bool ok, const std::string &what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) fails++;
}
static const mpc_engine_t *e;
static std::string get(void *inst, const char *key) {
    char buf[8192];
    int n = e->get_param(inst, key, buf, sizeof buf);
    return n > 0 ? std::string(buf) : std::string();
}
static bool wait_for(void *inst, const std::function<bool()> &ok, int ms) {
    std::vector<int16_t> buf(256);
    for (int t = 0; t < ms; t += 5) {
        if (ok()) return true;
        e->render(inst, buf.data(), 128);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return ok();
}
static std::string line_of(const std::string &st, const std::string &key) {
    size_t a = st.find("\n" + key + "=");
    if (a == std::string::npos) return "";
    a += key.size() + 2;
    return st.substr(a, st.find('\n', a) - a);
}

int main(int argc, char **argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: test_restore <image> <preset path inside it> <expected name>\n"); return 2; }
    const std::string image = argv[1], inner = argv[2], want = argv[3];
    e = mpc_engine();
    const std::string T = "/tmp/omni-restore-test";
    (void)!std::system(("rm -rf " + T + " /media/omni-rt-a /media/omni-rt-b").c_str());
    mkdir(T.c_str(), 0755);
    const std::string drive = T + "/drive", link = drive + "/disk.iso";
    mkdir(drive.c_str(), 0755);
    check(symlink(image.c_str(), link.c_str()) == 0, "the test drive holds the image");
    const std::string path = link + "/" + inner;
    auto named = [&](void *i) { return get(i, "slot1_name") == want; };

    // 1. a project with the image's preset in slot A
    void *a = e->create((T + "/data").c_str());
    e->set_param(a, "state", ("omni-sampler 2\nslot1_path=" + path + "\nslot1_preset=0\n").c_str());
    { const bool w_ = wait_for(a, [&] { return named(a); }, 30000); check(w_, "the preset inside the image loads: " + get(a, "slot1_name")); }
    const std::string saved = get(a, "state");
    e->destroy(a);
    check(line_of(saved, "slot1_path") == path, "the project saves its path");

    // 2. reopened while the image is not there (a drive not mounted yet)
    unlink(link.c_str());
    void *b = e->create((T + "/data").c_str());
    e->set_param(b, "state", saved.c_str());
    { const bool w_ = wait_for(b, [&] { return get(b, "slot1_name").rfind("MISSING: ", 0) == 0; }, 10000); check(w_, "image missing: the slot says MISSING (" + get(b, "slot1_name") + ")"); }
    check(line_of(get(b, "state"), "slot1_path") == path, "image missing: the project still saves the instrument (the bug)");
    // 3. the image comes back: the slot loads by itself
    check(symlink(image.c_str(), link.c_str()) == 0, "the image is back");
    { const bool w_ = wait_for(b, [&] { return named(b); }, 15000); check(w_, "the slot loads by itself once its file is there: " + get(b, "slot1_name")); }
    e->destroy(b);

    // 4. the drive under another name: /media/omni-rt-a -> /media/omni-rt-b
    if (mkdir("/media", 0755) == 0 || errno == EEXIST) {
        mkdir("/media/omni-rt-a", 0755);
        const std::string mlink = "/media/omni-rt-a/disk.iso", mpath = mlink + "/" + inner;
        if (symlink(image.c_str(), mlink.c_str()) == 0) {
            const std::string st = "omni-sampler 2\nslot1_path=" + mpath + "\nslot1_preset=0\n";
            check(rename("/media/omni-rt-a", "/media/omni-rt-b") == 0, "the drive is renamed");
            void *c = e->create((T + "/data").c_str());
            e->set_param(c, "state", st.c_str());
            { const bool w_ = wait_for(c, [&] { return named(c); }, 30000); check(w_, "a drive mounted under another name: found there (" + get(c, "slot1_name") + ")"); }
            check(line_of(get(c, "state"), "slot1_path").rfind("/media/omni-rt-b/", 0) == 0, "and the project now points there");
            e->destroy(c);
        } else check(false, "could not make /media/omni-rt-a/disk.iso");
        (void)!std::system("rm -rf /media/omni-rt-a /media/omni-rt-b");
    }

    // 5. extracted to a Plugin Library, then the .omni deleted: the source image instead
    const std::string lib = T + "/library";
    mkdir(lib.c_str(), 0755);
    setenv("OMNI_SAMPLER_LIBRARY", lib.c_str(), 1);
    void *d = e->create((T + "/data2").c_str());
    e->set_param(d, "state", ("omni-sampler 2\nslot1_path=" + path + "\nslot1_preset=0\n").c_str());
    { const bool w_ = wait_for(d, [&] { return line_of(get(d, "state"), "slot1_path").find(".omni") != std::string::npos; }, 60000); check(w_, "with a library the preset is extracted (the project points at the .omni)"); }
    const std::string st5 = get(d, "state");
    check(line_of(st5, "slot1_src") == path, "the project also saves the .omni's source: " + line_of(st5, "slot1_src"));
    e->destroy(d);
    (void)!std::system(("rm -rf '" + lib + "/Extracted'").c_str());
    void *f = e->create((T + "/data2").c_str());
    e->set_param(f, "state", st5.c_str());
    { const bool w_ = wait_for(f, [&] { return named(f); }, 60000); check(w_, "the .omni deleted: loaded from its source image (" + get(f, "slot1_name") + ")"); }
    e->destroy(f);
    unsetenv("OMNI_SAMPLER_LIBRARY");

    // 6. clearing a missing slot forgets it
    unlink(link.c_str());
    void *g = e->create((T + "/data").c_str());
    e->set_param(g, "state", saved.c_str());
    wait_for(g, [&] { return get(g, "slot1_name").rfind("MISSING: ", 0) == 0; }, 10000);
    e->set_param(g, "target_slot", "0");
    e->set_param(g, "slot_clear", "1"); e->set_param(g, "slot_clear", "0");
    { const bool w_ = wait_for(g, [&] { return line_of(get(g, "state"), "slot1_path").empty(); }, 5000); check(w_, "SLOT CLEAR on a missing slot: forgotten"); }
    e->destroy(g);

    (void)!std::system(("rm -rf " + T).c_str());
    std::printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}
