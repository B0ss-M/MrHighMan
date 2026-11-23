// The Classic library spans three files with a single byte-transform state.
// Map only PCM pages whose decoded probes agree with the matching Extreme page.
struct ClassicRom {
    static constexpr uint64_t page = 0x1540000;
    static constexpr uint64_t part = 100ull << 20;
    std::array<BlobPtr, 3> parts{};
    std::array<int, 10> pages{};

    void read(uint64_t offset, uint8_t *dst, size_t count) const {
        while (count) {
            const auto index = size_t(offset / part);
            const auto within = offset % part;
            if (index >= parts.size() || !parts[index] || within >= parts[index]->size())
                throw ParseError("TRITON Classic PCM part unavailable");
            const size_t n = size_t(std::min<uint64_t>(count, parts[index]->size() - within));
            parts[index]->read(within, dst, n);
            triton_extreme::decode_span(offset, dst, n);
            dst += n;
            offset += n;
            count -= n;
        }
    }
    int64_t locate(uint64_t byte, uint64_t count) const {
        const size_t index = size_t(byte / page);
        if (!count || index >= pages.size() || pages[index] < 0) return -1;
        const size_t last = size_t((byte + count - 1) / page);
        for (size_t k = index + 1; k <= last; ++k)
            if (k >= pages.size() || pages[k] != pages[index] + int(k-index)) return -1;
        return int64_t(pages[index] * page + byte % page);
    }
};

#include "triton_page_probes.inc"

std::shared_ptr<ClassicRom> find_classic_rom(Volume &vol, const std::string &dir) {
    auto rom = std::make_shared<ClassicRom>();
    rom->pages.fill(-1);
    for (size_t i = 0; i < 3; ++i) {
        std::string path;
        if (!find_ci(vol, path_join(dir, "TritonPcm"+std::to_string(i+1)+".bin"), path)) return {};
        rom->parts[i] = vol.open(path);
        if (rom->parts[i]->size() != (i < 2 ? ClassicRom::part : 35389440ull)) return {};
    }
    for (int i = 0; i < 11; ++i) {
        uint64_t hash = 14695981039346656037ull;
        for (uint64_t pos : {0x20000ull, 0xaa0000ull, 0x1500000ull}) {
            std::array<uint8_t,512> buf;
            rom->read(i * ClassicRom::page + pos, buf.data(), buf.size());
            for (auto byte : buf) hash = (hash ^ byte) * 1099511628211ull;
        }
        for (int k = 0; k < 10; ++k)
            if (hash == triton_page_probes[k]) rom->pages[k] = i;
    }
    return rom;
}
