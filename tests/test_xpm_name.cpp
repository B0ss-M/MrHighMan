// Omni Sampler 2.4.10: legacy Latin-1 names remain valid UTF-8 XML in the MPC program.
#include <cstdio>
#include <filesystem>
#include <string>
#include <unistd.h>
#include "../src/formats/format.hpp"
#include "../src/formats/omni_native.hpp"

using namespace omni;

static int fails = 0;
static void check(bool ok, const std::string &what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) fails++;
}

int main() {
    register_all();
    Instrument instrument;
    instrument.name = std::string("Caf") + char(0xE9) + " &<>\"'";
    instrument.format = "XPM name test";
    Zone zone;
    zone.name = "tone";
    instrument.zones.push_back(zone);
    auto pcm = std::make_shared<Pcm>();
    pcm->data = {12000, -12000};
    PcmPtr samples = pcm;
    const std::string out = "/tmp/omni-xpm-name-test-" + std::to_string(getpid());
    std::filesystem::remove_all(out);
    const std::string omni = write_omni(instrument, {samples}, out, "xpm-name-test", 0);
    const std::string stem = path_stem(omni);
    const std::string xpm = out + "/" + stem + " Samples/" + stem + ".xpm";
    BlobPtr xml = open_host_file(xpm);
    const std::vector<uint8_t> bytes = xml->all();
    const std::string text(bytes.begin(), bytes.end());
    check(text.find("<ProgramName>Caf\xc3\xa9 &amp;&lt;&gt;&quot;&apos;</ProgramName>") != std::string::npos,
          "Latin-1 converted and XML metacharacters escaped");
    Location location = Vfs().resolve(xpm);
    const FormatReader *reader = find_reader(*location.volume, location.inner);
    check(reader != nullptr, "written XPM is recognized");
    if (reader) {
        Instrument round_trip = reader->load(location.volume, location.inner, 0);
        check(round_trip.name == std::string("Caf\xc3\xa9 &<>\"'"), "XPM program name round-trips as UTF-8");
    }
    std::filesystem::remove_all(out);
    std::printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}
