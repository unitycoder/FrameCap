#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

// Small seekable, video-only WebM muxer. Each packet gets a finite cluster.
namespace webm {
using Bytes = std::vector<uint8_t>;
inline void id(Bytes& b, uint32_t v) {
    int n = v > 0xffffff ? 4 : v > 0xffff ? 3 : v > 0xff ? 2 : 1;
    for (int i = n - 1; i >= 0; --i) b.push_back(uint8_t(v >> (i * 8)));
}
inline void size(Bytes& b, uint64_t v, int n = 0) {
    if (!n) { n = 1; while (n < 8 && v >= ((uint64_t(1) << (7 * n)) - 1)) ++n; }
    if (v >= ((uint64_t(1) << (7 * n)) - 1)) throw std::runtime_error("WebM element too large");
    v |= uint64_t(1) << (7 * n);
    for (int i = n - 1; i >= 0; --i) b.push_back(uint8_t(v >> (i * 8)));
}
inline Bytes element(uint32_t tag, const Bytes& data) {
    Bytes b; id(b, tag); size(b, data.size()); b.insert(b.end(), data.begin(), data.end()); return b;
}
inline void add(Bytes& dst, const Bytes& src) { dst.insert(dst.end(), src.begin(), src.end()); }
inline Bytes number(uint32_t tag, uint64_t value) {
    Bytes b; int n = 1; while (n < 8 && (value >> (n * 8))) ++n;
    for (int i = n - 1; i >= 0; --i) b.push_back(uint8_t(value >> (i * 8)));
    return element(tag, b);
}
inline Bytes string(uint32_t tag, const std::string& s) { return element(tag, Bytes(s.begin(), s.end())); }
inline Bytes floating(uint32_t tag, double value) {
    uint64_t bits; std::memcpy(&bits, &value, 8); Bytes b;
    for (int i = 7; i >= 0; --i) b.push_back(uint8_t(bits >> (i * 8)));
    return element(tag, b);
}
class Writer {
    std::fstream file;
    uint64_t segmentStart = 0, segmentSizeAt = 0, seekAt = 0, durationAt = 0;
    uint64_t infoPos = 0, tracksPos = 0, lastTime = 0, frames = 0;
    struct Cue { uint64_t time, position; };
    std::vector<Cue> cues;
    void write(const Bytes& b) { file.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size())); }
    uint64_t pos() { return uint64_t(file.tellp()); }
    static Bytes seek(uint32_t tag, uint64_t position) {
        Bytes tagBytes; id(tagBytes, tag); Bytes b = element(0x53ab, tagBytes);
        add(b, number(0x53ac, position)); return element(0x4dbb, b);
    }
public:
    Writer(const std::filesystem::path& path, unsigned width, unsigned height, unsigned fps) {
        file.exceptions(std::ios::badbit | std::ios::failbit);
        file.open(path, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
        Bytes ebml;
        for (auto e : {number(0x4286, 1), number(0x42f7, 1), number(0x42f2, 4), number(0x42f3, 8),
                       string(0x4282, "webm"), number(0x4287, 4), number(0x4285, 2)}) add(ebml, e);
        write(element(0x1a45dfa3, ebml)); Bytes tag; id(tag, 0x18538067); write(tag);
        segmentSizeAt = pos(); Bytes placeholder; size(placeholder, 0, 8); write(placeholder);
        segmentStart = pos(); seekAt = pos(); write(Bytes(256, 0));
        infoPos = pos() - segmentStart;
        Bytes info = number(0x2ad7b1, 1000000);
        add(info, string(0x4d80, "FrameCap")); add(info, string(0x5741, "FrameCap"));
        add(info, floating(0x4489, 0));
        Bytes infoElement = element(0x1549a966, info);
        durationAt = pos() + infoElement.size() - 8; write(infoElement);
        tracksPos = pos() - segmentStart;
        Bytes track = number(0xd7, 1); add(track, number(0x73c5, 1)); add(track, number(0x83, 1));
        add(track, number(0x9c, 0)); add(track, string(0x86, "V_VP8"));
        add(track, number(0x23e383, 1000000000ULL / fps));
        Bytes video = number(0xb0, width); add(video, number(0xba, height));
        // Explicit BT.601 limited-range metadata matches the BGRA conversion.
        Bytes colour = number(0x55b1, 6); add(colour, number(0x55b9, 1));
        add(colour, number(0x55ba, 6)); add(colour, number(0x55bb, 6));
        add(video, element(0x55b0, colour)); add(track, element(0xe0, video));
        write(element(0x1654ae6b, element(0xae, track)));
    }
    void frame(const uint8_t* data, size_t length, uint64_t milliseconds, bool key) {
        if (frames && milliseconds <= lastTime) throw std::runtime_error("Non-increasing WebM timestamp");
        uint64_t clusterPos = pos() - segmentStart;
        Bytes block{0x81, 0, 0, uint8_t(key ? 0x80 : 0)};
        block.insert(block.end(), data, data + length);
        Bytes cluster = number(0xe7, milliseconds); add(cluster, element(0xa3, block));
        write(element(0x1f43b675, cluster));
        if (key) cues.push_back({milliseconds, clusterPos});
        lastTime = milliseconds; ++frames;
    }
    void finish(uint64_t durationMs) {
        if (!frames) throw std::runtime_error("No frames were recorded");
        uint64_t cuesPos = pos() - segmentStart; Bytes body;
        for (const auto& c : cues) {
            Bytes p = number(0xf7, 1); add(p, number(0xf1, c.position));
            Bytes cue = number(0xb3, c.time); add(cue, element(0xb7, p)); add(body, element(0xbb, cue));
        }
        write(element(0x1c53bb6b, body)); uint64_t end = pos();
        file.seekp(std::streamoff(durationAt)); Bytes duration = floating(0x4489, double(std::max(durationMs, lastTime + 1)));
        file.write(reinterpret_cast<const char*>(duration.data() + 3), 8);
        Bytes seeks = seek(0x1549a966, infoPos); add(seeks, seek(0x1654ae6b, tracksPos)); add(seeks, seek(0x1c53bb6b, cuesPos));
        Bytes head = element(0x114d9b74, seeks);
        if (head.size() > 252) throw std::runtime_error("WebM seek header overflow");
        size_t remaining = 256 - head.size(); head.push_back(0xec); size(head, remaining - 3, 2);
        head.resize(256, 0); file.seekp(std::streamoff(seekAt)); write(head);
        Bytes length; size(length, end - segmentStart, 8); file.seekp(std::streamoff(segmentSizeAt)); write(length);
        file.flush(); file.close();
    }
};
}
