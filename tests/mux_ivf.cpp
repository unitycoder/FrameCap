#include "../src/webm_writer.h"
#include <iostream>
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
        webm::Writer out(argv[2], unsigned(little(header+12, 2)), unsigned(little(header+14, 2)), rate/scale);
        uint64_t last = 0;
        while (true) {
            uint8_t frame[12]; in.read(reinterpret_cast<char*>(frame), 12); if (in.eof()) break;
            if (!in) throw std::runtime_error("IVF read failed");
            webm::Bytes data(size_t(little(frame, 4))); in.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size()));
            if (!in || data.empty()) throw std::runtime_error("Truncated IVF");
            last = little(frame+4, 8) * 1000 * scale/rate;
            out.frame(data.data(), data.size(), last, (data[0] & 1) == 0);
        }
        out.finish(last + 1000*scale/rate);
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
