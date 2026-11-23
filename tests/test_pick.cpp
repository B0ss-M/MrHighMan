// Omni Sampler 2.4.13: pick presets to check, per reader. Walks folders (and the disk images / ZIPs / packages inside
// them, as the browser opens them), asks the reader registry which reader takes each file (the browser and loader use
// the same registry), and prints up to N presets per reader, spread over different files where there are several.
//   test_pick <N> <root> [root ...]
// stdout, one line per pick:  PICK <reader>\t<file>::<preset position>::<preset count>\t<preset name>
//         one per reader:     READER <reader>\t<files seen>
//         per extension no reader took (instrument-like files only): NOREADER <ext>\t<count>\t<one example>
//         a file whose listing failed: LISTERR <reader>\t<file>\t<error>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "../src/formats/format.hpp"

using namespace omni;

struct Cand { std::string path; std::vector<std::string> names; };
static const size_t MAX_CANDS = 40;           // files listed per reader (enough to spread the picks)
static const size_t MAX_ENTRIES_PER_DIR = 5000;
static std::map<std::string, std::vector<Cand>> cands;
static std::map<std::string, size_t> seen;     // files each reader took
static std::map<std::string, std::pair<size_t, std::string>> noreader;
static std::set<std::string> skip_ext = {"txt", "pdf", "jpg", "jpeg", "png", "gif", "html", "htm", "url", "nfo", "md", "js",
    "css", "svg", "doc", "rtf", "xls", "xlsx", "ini", "exe", "dll", "db", "ds_store", "log", "json", "xml", "bat", "sys", "cab",
    "rar", "7z", "hqx", "mid", "pid", "diz", "sfv", "jsp", "webp", "ocx", "jar", "properties", "readme", "cpp", "c", "sh", "pl",
    "vst3", "index", "iss", "inx", "mp3", "jacheck", "exz", "db"};

static std::string lower_ext(const std::string &name) {
    size_t d = name.rfind('.');
    if (d == std::string::npos || d == 0) return "";
    std::string e = name.substr(d + 1);
    for (auto &c : e) c = char(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

static void walk(Vfs &vfs, const std::string &dir, int depth) {
    if (depth > 12) return;
    std::vector<DirEntry> entries;
    try { entries = vfs.list(dir); } catch (const std::exception &) { return; }
    std::sort(entries.begin(), entries.end(), [](const DirEntry &a, const DirEntry &b) { return a.name < b.name; });
    size_t n = 0;
    for (auto &e : entries) {
        if (e.name.empty() || e.name[0] == '.') continue;
        if (++n > MAX_ENTRIES_PER_DIR) break;
        const std::string full = dir + "/" + e.name;
        if (e.dir) { walk(vfs, full, depth + 1); continue; }
        const FormatReader *r = nullptr;
        try {
            Location loc = vfs.resolve(full);
            r = find_reader(*loc.volume, loc.inner);
            if (r) {
                seen[r->name]++;
                auto &v = cands[r->name];
                if (v.size() < MAX_CANDS) {
                    try {
                        std::vector<PresetInfo> ps = r->list(loc.volume, loc.inner);
                        Cand c{full, {}};
                        for (auto &p : ps) c.names.push_back(p.name);
                        if (!c.names.empty()) v.push_back(c);
                        else std::printf("LISTERR %s\t%s\t(no presets)\n", r->name, full.c_str());
                    } catch (const std::exception &ex) {
                        std::printf("LISTERR %s\t%s\t%s\n", r->name, full.c_str(), ex.what());
                    }
                }
            }
        } catch (const std::exception &) {}
        // a container the browser opens as a folder (disk image, ZIP, .vitalbank, .omnisphere): look inside
        bool container = false;
        try { container = vfs.is_container(full); } catch (const std::exception &) {}
        if (container && !r) { walk(vfs, full, depth + 1); continue; }
        if (!r && !container) {
            std::string ext = lower_ext(e.name);
            if (!ext.empty() && !skip_ext.count(ext)) {
                auto &nr = noreader[ext];
                if (nr.first++ == 0) nr.second = full;
            }
        }
    }
    std::fflush(stdout);
}

int main(int argc, char **argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: test_pick <N> <root> [root ...]\n"); return 2; }
    const size_t N = size_t(std::max(1, std::atoi(argv[1])));
    register_all();
    Vfs vfs;
    for (int a = 2; a < argc; a++) walk(vfs, argv[a], 0);
    for (auto &f : formats()) if (!seen.count(f.name)) std::printf("READER %s\t0\n", f.name);
    for (auto &kv : cands) {
        std::printf("READER %s\t%zu\n", kv.first.c_str(), seen[kv.first]);
        const auto &v = kv.second;
        std::vector<std::pair<size_t, size_t>> picks;   // (candidate, preset position)
        if (v.size() >= N) {
            for (size_t i = 0; i < N; i++) {
                size_t c = i * v.size() / N;
                picks.push_back({c, (i * 7) % v[c].names.size()});
            }
        } else {
            // fewer files than picks: the presets of each file in turn, spread inside each
            for (size_t round = 0; picks.size() < N && round < N; round++)
                for (size_t c = 0; c < v.size() && picks.size() < N; c++) {
                    size_t cnt = v[c].names.size();
                    if (round >= cnt) continue;
                    picks.push_back({c, (round * (cnt / N + 1) + round) % cnt});
                }
            std::sort(picks.begin(), picks.end());
            picks.erase(std::unique(picks.begin(), picks.end()), picks.end());
        }
        for (auto &p : picks)
            std::printf("PICK %s\t%s::%zu::%zu\t%s\n", kv.first.c_str(), v[p.first].path.c_str(), p.second, v[p.first].names.size(),
                        v[p.first].names[p.second].c_str());
    }
    for (auto &kv : noreader) std::printf("NOREADER %s\t%zu\t%s\n", kv.first.c_str(), kv.second.first, kv.second.second.c_str());
    return 0;
}
