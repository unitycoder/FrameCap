#include "../src/webm_writer.h"
#include <iostream>
#include <limits>
uint64_t little(const uint8_t* p, int n) {
    uint64_t v = 0; for (int i = n-1; i >= 0; --i) v = (v << 8) | p[i]; return v;
}
int main(int argc, char** argv) {
    if (argc != 3) return 2;
    try {
        std::ifstream in(argv[1], std::ios::binary); in.exceptions(std::ios::badbit);
        uint8_t header[32]; in.read(reinterpret_cast<char*>(header), 32);
        if (!in || std::memcmp(header, "DKIF", 4)) throw std::runtime_error("Invalid IVF");
        unsigned rate = unsigned(little(header+16, 4)), scale = unsigned(little(header+20, 4));
        if (rate == 0 || scale == 0) throw std::runtime_error("Invalid IVF timebase");
        webm::Writer out(argv[2], unsigned(little(header+12, 2)), unsigned(little(header+14, 2)), rate/scale);
        uint64_t last = 0;
        while (true) {
            uint8_t frame[12]; in.read(reinterpret_cast<char*>(frame), 12); if (in.eof()) break;
            if (!in) throw std::runtime_error("IVF read failed");
            constexpr uint64_t MaxFrameSize = 256ULL * 1024 * 1024;
            uint64_t length = little(frame, 4);
            if (length == 0 || length > MaxFrameSize || length > uint64_t(std::numeric_limits<std::streamsize>::max()))
                throw std::runtime_error("Invalid IVF frame size");
            webm::Bytes data(size_t(length)); in.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size()));
            if (!in || data.empty()) throw std::runtime_error("Truncated IVF");
            uint64_t timestamp = little(frame+4, 8);
            uint64_t timeScale = uint64_t(scale) * 1000;
            if (timestamp > std::numeric_limits<uint64_t>::max() / timeScale)
                throw std::runtime_error("IVF timestamp is too large");
            last = timestamp * timeScale / rate;
            out.frame(data.data(), data.size(), last, (data[0] & 1) == 0);
        }
        uint64_t frameDuration = uint64_t(scale) * 1000 / rate;
        if (last > std::numeric_limits<uint64_t>::max() - frameDuration)
            throw std::runtime_error("IVF duration is too large");
        out.finish(last + frameDuration);
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
