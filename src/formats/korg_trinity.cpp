// Omni Sampler: Korg TRINITY program banks (.PCG, as KORG Collection TRINITY keeps them), for the programs whose
// oscillators play the TFD sample disks (the TRINITY's flash ROM option), whose multisamples are a KSC script beside
// them: <TRINITY>/Presets/PCGData/TFD-1S.PCG -> <TRINITY>/Resource/PCM/KSC/TFD-1S/Disk1/ALL_INST.KSC.
// The browser shows a .PCG as a folder (TrinityView): one folder per TRINITY category, a small .ttp entry per program.
// What the format turned out to be (measured on the KORG Collection banks, checked against their CSV lists):
//   - "KORG", product 0x3B; at 0x20 a section table: (u16 id, u16 banks, u32 size) big-endian, ended by id 0xFFFF;
//     sections follow one another from 0x50. Section 0 = programs: per bank a u16 bank number (0-3 = A-D) and 128
//     records of 433 bytes.
//   - A program: name (16 bytes), byte 16 low nibble = category, byte 17 low 2 bits = oscillator mode (0 single,
//     1 double, 2 drums), bytes 31-34 = OSC1 / OSC2 multisample, big-endian u16: bit 12 = a flash (TFD disk)
//     multisample, its index in the KSC (names checked on all four disks); bit 15 = a stereo pair (index = left,
//     index + 1 = right); otherwise a TRINITY ROM multisample, which is not available.
//   - The other program parameters (envelopes, filter, tuning, velocity split) are not decoded: the sampler's own apply.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include "../core/parse.hpp"
#include "format.hpp"

namespace omni {
namespace {

constexpr size_t PROG_SIZE = 433;
const char *const CATEGORIES[16] = {"Keyboard", "Organ", "Mallet", "Bell", "Strings", "Vocal-Airy", "Brass", "Woodwind-Reeds",
                                    "Guitar", "Bass", "Plucked", "Synth", "Synth Lead", "Motion Synth", "Sound Effect", "Drums-Perc"};
const char TTP_HEAD[] = "TRINITY PROGRAM ";   // a .ttp entry: "TRINITY PROGRAM <bank> <program>\n"

uint16_t be16(const uint8_t *p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t be32(const uint8_t *p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

bool is_trinity_pcg(const uint8_t *h, size_t n) { return n >= 0x50 && !std::memcmp(h, "KORG", 4) && h[4] == 0x3B; }

struct Prog {
    int bank = 0, index = 0;   // bank 0-3 = A-D
    std::string name;
    int category = 0, mode = 0;
    uint16_t osc[2] = {0, 0};
    int oscs() const { return mode == 0 ? 1 : 2; }
    bool flash(int o) const { return osc[o] & 0x1000; }
    bool any_flash() const { return mode != 2 && (flash(0) || (oscs() > 1 && flash(1))); }
};

// the programs of a TRINITY .PCG
std::vector<Prog> read_programs(Volume &vol, const std::string &path) {
    std::vector<uint8_t> d = vol.open(path)->all(size_t(16) << 20);
    if (!is_trinity_pcg(d.data(), d.size())) throw ParseError("not a Korg TRINITY .PCG");
    std::vector<Prog> out;
    size_t pos = 0x50;
    for (size_t t = 0x20; t + 8 <= 0x50; t += 8) {
        uint16_t id = be16(&d[t]), banks = be16(&d[t + 2]);
        uint32_t size = be32(&d[t + 4]);
        if (id == 0xFFFF) break;
        if (id == 0) {
            for (int b = 0; b < banks; b++) {
                size_t bp = pos + size_t(b) * (2 + 128 * PROG_SIZE);
                if (bp + 2 + 128 * PROG_SIZE > d.size()) break;
                int bank = be16(&d[bp]);
                for (int i = 0; i < 128; i++) {
                    const uint8_t *p = &d[bp + 2 + size_t(i) * PROG_SIZE];
                    Prog pr;
                    pr.bank = bank;
                    pr.index = i;
                    pr.name = trim(std::string(reinterpret_cast<const char *>(p), 16));
                    pr.category = p[16] & 0x0F;
                    pr.mode = p[17] & 3;
                    pr.osc[0] = be16(p + 31);
                    pr.osc[1] = be16(p + 33);
                    out.push_back(pr);
                }
            }
        }
        pos += size;
    }
    return out;
}

// The TFD disk's KSC for a .PCG (same name): <root>/Resource/PCM/KSC/<name>/Disk1/ALL_INST.KSC, or a KSC beside it
std::string find_ksc(Volume &vol, const std::string &pcg) {
    std::string stem = path_stem(pcg), found;
    std::string root = path_dir(path_dir(path_dir(pcg)));   // <root>/Presets/PCGData/x.PCG
    for (const std::string &p : {path_join(root, "Resource/PCM/KSC/" + stem + "/Disk1/ALL_INST.KSC"),
                                 path_join(path_dir(pcg), stem + "/Disk1/ALL_INST.KSC"), path_join(path_dir(pcg), stem + ".KSC")})
        if (find_ci(vol, p, found) && !vol.is_dir(found)) return found;
    return "";
}

std::string entry_name(const Prog &p) {
    char num[8];
    std::snprintf(num, sizeof num, "%c%03d ", "ABCD"[p.bank & 3], p.index);
    std::string n = num + p.name;
    for (char &c : n) if (c == '/' || c == '\\') c = '-';
    return n + ".ttp";
}

// a .PCG seen as category folders of the programs it can play; other paths read through the folder two levels up
// (<TRINITY>, from <TRINITY>/Presets/PCGData/x.PCG), so a program finds its disk's KSC as from the PCG itself
class TrinityView : public Volume {
public:
    TrinityView(VolumePtr parent, const std::string &pcg)
        : parent_(std::move(parent)), dir_(path_dir(path_dir(path_dir(pcg)))), pcg_(dir_.empty() ? pcg : pcg.substr(dir_.size() + 1)) {
        if (find_ksc(*parent_, pcg).empty())
            throw ParseError("no TFD sample disk (KSC) for " + path_name(pcg) + ": its ROM programs are not supported");
        dirs_[""];
        for (const Prog &p : read_programs(*parent_, pcg)) {
            if (!p.any_flash() || p.name.empty()) continue;
            std::string cat = CATEGORIES[p.category & 15];
            auto &dir = dirs_[cat];
            if (dir.empty()) dirs_[""].push_back(DirEntry{cat, true, 0});
            std::string file = entry_name(p);
            dir.push_back(DirEntry{file, false, 32});
            files_[path_join(cat, file)] = std::to_string(p.bank) + " " + std::to_string(p.index);
        }
        if (files_.empty()) throw ParseError("no programs in " + path_name(pcg) + " use the TFD disk samples (ROM programs are not supported)");
    }
    std::string kind() const override { return "Korg TRINITY categories"; }
    bool view() const override { return true; }
    std::vector<DirEntry> list(const std::string &dir) override {
        auto it = dirs_.find(dir);
        return it != dirs_.end() ? it->second : parent_->list(path_join(dir_, dir));
    }
    BlobPtr open(const std::string &path) override {
        auto it = files_.find(path);
        if (it == files_.end()) return parent_->open(path_join(dir_, path));
        std::string s = TTP_HEAD + it->second + "\n" + pcg_ + "\n";
        return make_mem_blob(std::vector<uint8_t>(s.begin(), s.end()));
    }
    bool is_dir(const std::string &path) override { return dirs_.count(path) || (!files_.count(path) && parent_->is_dir(path_join(dir_, path))); }
    bool exists(const std::string &path) override { return dirs_.count(path) || files_.count(path) || parent_->exists(path_join(dir_, path)); }

private:
    VolumePtr parent_;
    std::string dir_, pcg_;
    std::map<std::string, std::vector<DirEntry>> dirs_;
    std::map<std::string, std::string> files_;   // "<category>/<entry>.ttp" -> "<bank> <program>"
};

bool probe_view(const std::string &lower_name, const uint8_t *h, size_t n, uint64_t) {
    return lower_name.size() > 4 && !lower_name.compare(lower_name.size() - 4, 4, ".pcg") && is_trinity_pcg(h, n);
}
VolumePtr mount_view_blob(BlobPtr) { throw ParseError("a TRINITY .PCG opens from its folder"); }
VolumePtr mount_view(VolumePtr parent, const std::string &inner) { return std::make_shared<TrinityView>(parent, inner); }

// One program: each oscillator that plays a flash multisample, loaded from the KSC (a stereo pair panned hard L / R)
Instrument load_program(VolumePtr vol, const std::string &pcg, int bank, int index) {
    const Prog *pr = nullptr;
    std::vector<Prog> progs = read_programs(*vol, pcg);
    for (auto &p : progs) if (p.bank == bank && p.index == index) pr = &p;
    if (!pr) throw ParseError("no such TRINITY program");
    if (pr->mode == 2) throw ParseError("TRINITY drum programs use the ROM drum samples: not supported");
    std::string ksc = find_ksc(*vol, pcg);
    if (ksc.empty()) throw ParseError("the TFD sample disk (KSC) for " + path_name(pcg) + " is missing");
    const FormatReader *r = find_reader(*vol, ksc);
    if (!r) throw ParseError("cannot read " + ksc);
    // multisample n is the script's n-th KMP; the KSC reader takes the script line
    std::vector<int> kmp_line;
    {
        std::vector<uint8_t> d = vol->open(ksc)->head(1 << 20);
        std::string cur;
        int line = 0;
        auto take = [&] {
            cur = trim(cur);
            if (!cur.empty() && cur[0] != '#') { if (path_ext(cur) == "kmp") kmp_line.push_back(line); line++; }
            cur.clear();
        };
        for (uint8_t c : d) { if (c == '\r' || c == '\n') take(); else cur += char(c); }
        take();
    }
    int count = int(kmp_line.size());
    Instrument inst;
    inst.name = pr->name;
    inst.format = "Korg TRINITY program";
    inst.library = "TRINITY";
    inst.category = CATEGORIES[pr->category & 15];
    int rom = 0;
    for (int o = 0; o < pr->oscs(); o++) {
        uint16_t v = pr->osc[o];
        if (!(v & 0x1000)) { rom++; continue; }
        int ms = v & 0x0FFF;
        bool stereo = v & 0x8000;
        for (int side = 0; side < (stereo ? 2 : 1); side++) {
            if (ms + side >= count) { inst.warnings.push_back("OSC" + std::to_string(o + 1) + " multisample not on the disk"); continue; }
            Instrument m = r->load(vol, ksc, kmp_line[size_t(ms + side)]);
            for (auto &z : m.zones) {
                if (stereo) z.pan = side ? 1.0 : -1.0;
                inst.zones.push_back(std::move(z));
            }
            for (auto &w : m.warnings) inst.warnings.push_back(w);
        }
    }
    if (rom) inst.warnings.push_back(std::to_string(rom) + " oscillator uses the TRINITY ROM: not played");
    finish_instrument(inst);
    if (inst.zones.empty()) throw ParseError("this TRINITY program has no samples on its disk");
    return inst;
}

bool probe_ttp(const std::string &, const uint8_t *h, size_t n, uint64_t) { return n >= sizeof TTP_HEAD - 1 && !std::memcmp(h, TTP_HEAD, sizeof TTP_HEAD - 1); }

std::vector<PresetInfo> list_ttp(VolumePtr, const std::string &path) { return {PresetInfo{path_stem(path), 0}}; }

// a program entry of the category view (which reads the PCG's folder through)
Instrument load_ttp(VolumePtr vol, const std::string &path, int) {
    std::vector<uint8_t> d = vol->open(path)->head(512);
    std::string s(d.begin(), d.end());
    int bank = 0, index = 0;
    if (std::sscanf(s.c_str() + std::strlen(TTP_HEAD), "%d %d", &bank, &index) != 2) throw ParseError("bad TRINITY program entry");
    size_t nl = s.find('\n');
    std::string pcg = nl == std::string::npos ? "" : trim(s.substr(nl + 1));
    if (pcg.empty()) throw ParseError("bad TRINITY program entry");
    return load_program(vol, pcg, bank, index);
}

}  // namespace

void register_korg_trinity() {
    register_image_type({"TRINITY by Category", probe_view, mount_view_blob, mount_view, "pcg"});
    register_reader({"Korg TRINITY program", "ttp", probe_ttp, list_ttp, load_ttp});
}

}  // namespace omni
