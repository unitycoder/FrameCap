#include "../src/encoder.h"
#include <iostream>
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    try {
        constexpr int width = 322, height = 242, fps = 20;
        encoding::Encoder encoder(width, height, fps); webm::Writer out(argv[1], width, height, fps);
        std::vector<uint8_t> bgra(width*height*4);
        for (int n = 0; n < 60; ++n) {
            for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
                size_t p = size_t(y*width+x)*4;
                bgra[p] = uint8_t(x+n); bgra[p+1] = uint8_t(y); bgra[p+2] = uint8_t(n*4); bgra[p+3] = 255;
            }
            encoding::convert(bgra.data(), width, height, encoder.image);
            // Include a half-second capture stall; wall-clock timestamps must survive.
            encoder.encode(out, &encoder.image, n*50 + (n >= 30 ? 500 : 0), 50);
        }
        encoder.encode(out, nullptr, 0, 0); out.finish(3500);
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
