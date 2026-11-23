// Omni Sampler 2.4.14: the GRID's chord detector (src/engine/chord.hpp) and voicing (src/engine/voicing.hpp), pure.
//   test_chord
#include <chrono>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>
#include "../src/engine/chord.hpp"
#include "../src/engine/voicing.hpp"

using namespace omni;
static int fails = 0, checks = 0;
static void check(bool ok, const std::string &w) { checks++; if (!ok) { std::printf("FAIL %s\n", w.c_str()); fails++; } }
static std::string name(std::initializer_list<int> l) { std::vector<int> v(l); return chord_detect(v.data(), int(v.size())).name; }
static void is(std::initializer_list<int> l, const char *want) {
    const std::string got = name(l);
    check(got == want, std::string("chord ") + want + ": got " + got);
}

int main() {
    // triads, sevenths, sixths, added tones, extensions
    is({60, 64, 67}, "C"); is({60, 63, 67}, "Cm"); is({60, 63, 66}, "Cdim"); is({60, 64, 68}, "Caug");
    is({60, 62, 67}, "Csus2"); is({60, 65, 67}, "Csus4"); is({60, 67}, "C5");
    is({60, 64, 67, 70}, "C7"); is({60, 64, 67, 71}, "Cmaj7"); is({60, 63, 67, 70}, "Cm7"); is({60, 63, 67, 71}, "CmMaj7");
    is({60, 63, 66, 70}, "Cm7b5"); is({60, 63, 66, 69}, "Cdim7"); is({60, 65, 67, 70}, "C7sus4");
    is({60, 64, 67, 69}, "C6"); is({60, 63, 67, 69}, "Cm6"); is({60, 62, 64, 67}, "Cadd9");
    is({60, 64, 67, 70, 74}, "C9"); is({60, 64, 67, 71, 74}, "Cmaj9"); is({60, 64, 67, 69, 74}, "C6/9");
    // the bass decides between relative chords
    is({57, 60, 64, 67}, "Am7");
    // inversions
    is({64, 67, 72}, "C/E"); is({67, 72, 76}, "C/G"); is({58, 60, 64, 67}, "C7/A#");
    { const int v[] = {64, 67, 72}; check(chord_detect(v, 3).inversion == 1, "C/E is the first inversion"); }
    { const int v[] = {67, 72, 76}; check(chord_detect(v, 3).inversion == 2, "C/G is the second inversion"); }
    // octave doubles and wide spacing
    is({48, 60, 64, 67, 72}, "C"); is({36, 55, 64}, "C"); is({60, 60, 64, 64, 67}, "C");
    // a missing fifth
    is({60, 64, 70}, "C7(no5)"); is({60, 64, 71}, "Cmaj7(no5)"); is({60, 64}, "C(no5)"); is({60, 63, 70}, "Cm7(no5)");
    // every template, every root: the same suffix and root (root position)
    for (int t = 0; t < chord_detail::NT; t++)
        for (int r = 0; r < 12; r++) {
            std::vector<int> v;
            for (int i = 0; i < 12; i++) if (chord_detail::TPL[t].iv >> i & 1) v.push_back(48 + r + i);
            const ChordInfo ci = chord_detect(v.data(), int(v.size()));
            check(ci.root == r && ci.type == t, std::string("template ") + chord_detail::TPL[t].suf + " on " + chord_detail::PC[r] + ": got " + ci.name);
        }
    // robustness: nothing, one note, out of range, many notes, a crowded set
    check(std::string(chord_detect(nullptr, 0).name) == "-", "no notes: -");
    is({62}, "D");
    { const int v[] = {-5, 200, 60, 64, 67}; check(std::string(chord_detect(v, 5).name) == "C", "notes outside 0..127 ignored"); }
    { int v[16]; for (int i = 0; i < 16; i++) v[i] = 40 + i * 5; const ChordInfo ci = chord_detect(v, 16);
      check(std::strlen(ci.name) > 0 && std::strlen(ci.name) < sizeof ci.name, "16 notes: a name, terminated"); }
    { const int v[] = {60, 64, 67, 70, 74, 81}; const ChordInfo ci = chord_detect(v, 6);
      check(ci.root == 0 && !ci.exact, std::string("a 13th: C with a ?: ") + ci.name); }
    { const int v[] = {60, 61, 62}; const ChordInfo ci = chord_detect(v, 3); check(ci.type < 0 && std::string(ci.name) == "C C# D", std::string("a cluster lists its notes: ") + ci.name); }
    // cheap: 200000 detections of a four-note chord
    {
        const int v[] = {57, 60, 64, 67};
        auto t0 = std::chrono::steady_clock::now();
        int sum = 0;
        for (int i = 0; i < 200000; i++) sum += chord_detect(v, 4).root;
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("200000 detections: %.1f ms (%d)\n", ms, sum % 7);
        check(ms < 600, "chord detection is cheap");
    }

    // ---- voicing: every result inside the key and scale, the GRID's scale tables
    // the audit's table (C major)
    auto voice = [](std::vector<int> in, int mode, double anchor = -1) {
        const VoiceResult r = voice_apply(in.data(), int(in.size()), VoiceMode(mode), 1, 0, 0, 127, 48, anchor);
        std::vector<int> out;
        if (r.ok) for (int i = 0; i < r.n; i++) out.push_back(r.v[i].pitch);
        return out;
    };
    check(voice({60, 64, 67}, VM_INV_UP) == std::vector<int>({64, 67, 72}), "Invert Up: C E G -> E G C");
    check(voice({64, 67, 72}, VM_INV_UP, 191.0 / 3) == std::vector<int>({55, 60, 64}), "Invert Up again (anchor): -> G C E, recentred");
    check(voice({55, 60, 64}, VM_INV_UP, 191.0 / 3) == std::vector<int>({60, 64, 67}), "and again: back to C E G");
    check(voice({60, 64, 67}, VM_INV_DOWN) == std::vector<int>({55, 60, 64}), "Invert Down: C E G -> G C E");
    check(voice({60, 64, 67, 71}, VM_DROP2) == std::vector<int>({55, 60, 64, 71}), "Drop 2");
    check(voice({60, 64, 67, 71}, VM_DROP3) == std::vector<int>({52, 60, 67, 71}), "Drop 3");
    check(voice({60, 64, 67}, VM_OPEN) == std::vector<int>({60, 67, 76}), "Open: C G E'");
    check(voice({60, 67, 76}, VM_CLOSE) == std::vector<int>({60, 64, 67}), "Close: back to C E G");
    // snap
    check(snap_to_scale(61, 1, 0) == 60 && snap_to_scale(61, 1, 0, 1) == 62 && snap_to_scale(64, 1, 0) == 64, "snap to C major");
    // the in-key property over every scale, root and mode, triads and sevenths from the scale's own degrees
    int voiced = 0;
    for (int sc = 0; sc < 16; sc++)
        for (int root = 0; root < 12; root++)
            for (int m = 0; m < VM_COUNT; m++)
                for (const std::vector<int> &rows : {std::vector<int>{0, 2, 4}, std::vector<int>{0, 2, 4, 6}, std::vector<int>{1, 3, 5}}) {
                    std::vector<int> in;
                    for (int r : rows) in.push_back(Arp::seq_note(60 + root, r, sc, root));
                    const VoiceResult res = voice_apply(in.data(), int(in.size()), VoiceMode(m), sc, root, 0, 127, 48);
                    if (!res.ok) continue;
                    voiced++;
                    bool inkey = true, asc = true, perm = res.n == int(in.size());
                    int seen = 0;
                    for (int i = 0; i < res.n; i++) {
                        inkey = inkey && Arp::in_scale(res.v[i].pitch, sc, root) && res.v[i].pitch >= 0 && res.v[i].pitch <= 127;
                        if (i) asc = asc && res.v[i].pitch >= res.v[i - 1].pitch;
                        seen |= 1 << res.v[i].src;
                    }
                    perm = perm && seen == (1 << res.n) - 1;
                    check(inkey && asc && perm, "voicing stays in key: scale " + std::to_string(sc) + " root " + std::to_string(root) + " mode " + std::to_string(m));
                }
    std::printf("%d voicings checked in key\n", voiced);
    check(voiced > 2000, "most voicings succeed");
    // range and span limits are kept
    { const int in[] = {60, 64, 67}; const VoiceResult r = voice_apply(in, 3, VM_INV_UP, 1, 0, 60, 72, 48);
      bool inside = true; if (r.ok) for (int i = 0; i < r.n; i++) inside = inside && r.v[i].pitch >= 60 && r.v[i].pitch <= 72;
      check(inside && (r.ok || std::strlen(r.why) > 0), "a narrow range: inside it, or refused with a reason"); }
    { const int in[] = {120, 124, 127}; const VoiceResult r = voice_apply(in, 3, VM_INV_UP, 1, 0, 0, 127, 48);
      bool inside = true; if (r.ok) for (int i = 0; i < r.n; i++) inside = inside && r.v[i].pitch <= 127;
      check(inside, "never above 127"); }
    { const int in[] = {48, 52, 55, 59, 62}; const VoiceResult r = voice_apply(in, 5, VM_OPEN, 1, 0, 0, 127, 20);   // opens to 23
      check(!r.ok && std::string(r.why) == "wider than SPAN", "Open wider than SPAN is refused");
      const VoiceResult r2 = voice_apply(in, 5, VM_OPEN, 1, 0, 0, 127, 24);
      check(r2.ok && r2.v[4].pitch - r2.v[0].pitch == 23, "and allowed within it"); }
    // cells: a pitch back to its row (+ NOTE OCT), every scale, root, base and octave
    int cells = 0;
    for (int sc = 0; sc < 16; sc++)
        for (int root = 0; root < 12; root += 5)
            for (int base : {-3, 0, 5})
                for (int oc : {-3, 0, 2})
                    for (int row = 0; row < 16; row++)
                        for (int k = -2; k <= 2; k++) {
                            const int q = Arp::seq_note(60 + root, row, sc, root, base);
                            if (q < 0) continue;
                            const int p = q + 12 * (oc + k);
                            if (p < 0 || p > 127) continue;
                            int r2 = -1, k2 = 9;
                            const bool ok = pitch_to_cell(p, sc, root, base, oc, r2, k2);
                            const bool back = ok && r2 >= 0 && r2 < 16 && Arp::seq_note(60 + root, r2, sc, root, base) + 12 * (oc + k2) == p;
                            check(back, "pitch_to_cell round trip: scale " + std::to_string(sc) + " row " + std::to_string(row) + " oct " + std::to_string(k));
                            cells++;
                        }
    std::printf("%d cells round-tripped\n", cells);
    std::printf("%d checks, %s\n", checks, fails ? "FAILED" : "PASSED");
    return fails ? 1 : 0;
}
