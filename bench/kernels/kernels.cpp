// M9/M10 part zero: kernels.jade's kernels in C++, the same work the same way, for comparison.
//   build.bat, then kernels.exe [name]
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using Clock = std::chrono::steady_clock;
static long long ms(Clock::time_point from) { return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - from).count(); }
static int64_t lcg(int64_t s) { return (s * 1103515245 + 12345) & 0x7fffffff; }

static void lineEnds() {
    const int64_t n = 100000000;
    std::vector<uint8_t> data(n);
    int64_t s = 1, i = 0;
    while (i < n) { s = lcg(s); i += 20 + s % 80; if (i < n) data[i] = 10; }
    auto t = Clock::now();
    int64_t lines = 0;
    std::vector<int64_t> checkpoints;
    for (int64_t k = 0; k < n; k++)
        if (data[k] == 10) { lines++; if (lines % 4096 == 0) checkpoints.push_back(k + 1); }
    printf("lineEnds %lld ms (lines %lld, checkpoints %zu)\n", ms(t), (long long)lines, checkpoints.size());
    // And with memchr, as a native search would.
    t = Clock::now();
    lines = 0;
    const uint8_t* p = data.data(); const uint8_t* end = p + n;
    while ((p = (const uint8_t*)memchr(p, 10, end - p)) != nullptr) { lines++; p++; }
    printf("lineEnds (memchr) %lld ms (lines %lld)\n", ms(t), (long long)lines);
}

static void idct() {
    double c[64];
    for (int u = 0; u < 8; u++)
        for (int x = 0; x < 8; x++) c[u * 8 + x] = (u == 0 ? std::sqrt(0.125) : 0.5) * std::cos((2 * x + 1) * u * 3.141592653589793 / 16);
    double block[64], tmp[64], out[64];
    for (int i = 0; i < 64; i++) block[i] = (double)(i * 7 % 23 - 11);
    auto t = Clock::now();
    double check = 0;
    for (int b = 0; b < 200000; b++) {
        block[b % 64] = (double)(b % 31 - 15);
        for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) { double sum = 0; for (int u = 0; u < 8; u++) sum += c[u * 8 + x] * block[y * 8 + u]; tmp[y * 8 + x] = sum; }
        for (int x = 0; x < 8; x++) for (int y = 0; y < 8; y++) { double sum = 0; for (int v = 0; v < 8; v++) sum += c[v * 8 + y] * tmp[v * 8 + x]; out[y * 8 + x] = sum; }
        check += out[b % 64];
    }
    printf("idct %lld ms (check %lld)\n", ms(t), (long long)(check * 1000));
}

static void huffman() {
    const int n = 5000000;
    std::vector<uint8_t> data(n);
    int64_t s = 7;
    for (int i = 0; i < n; i++) { s = lcg(s); data[i] = (uint8_t)(s >> 8 & 255); }
    int symbol[512], length[512];
    for (int i = 0; i < 512; i++) { symbol[i] = i % 200; length[i] = 1 + i % 9; }
    auto t = Clock::now();
    int64_t bits = 0, sum = 0, decoded = 0; int count = 0, pos = 0;
    while (pos < n || count >= 9) {
        while (count < 9 && pos < n) { bits = (bits << 8) | data[pos]; pos++; count += 8; }
        if (count < 9) break;
        int peek = (int)((bits >> (count - 9)) & 511);
        count -= length[peek];
        bits &= ((int64_t)1 << count) - 1;
        sum += symbol[peek]; decoded++;
    }
    printf("huffman %lld ms (decoded %lld, sum %lld)\n", ms(t), (long long)decoded, (long long)sum);
}

static void gradient() {
    const int w = 3840, h = 2160;
    std::vector<uint8_t> px((size_t)w * h * 4);
    auto t = Clock::now();
    double dx = 0.6 / w, dy = 0.4 / h;
    for (int y = 0; y < h; y++) {
        size_t at = (size_t)y * w * 4;
        for (int x = 0; x < w; x++) {
            double tt = x * dx + y * dy;
            px[at] = (uint8_t)(int)(40 + tt * 200); px[at + 1] = (uint8_t)(int)(90 + tt * 120); px[at + 2] = (uint8_t)(int)(200 - tt * 150); px[at + 3] = 255;
            at += 4;
        }
    }
    printf("gradient %lld ms (pixel %d)\n", ms(t), px[(size_t)w * 4 * 100 + 400]);
}

static void blur() {
    const int w = 3840, h = 2160, r = 5;
    std::vector<uint8_t> src((size_t)w * h * 4), mid(src.size()), dst(src.size());
    for (size_t i = 0; i < src.size(); i++) src[i] = (uint8_t)(i * 31 % 251);
    auto t = Clock::now();
    int span = 2 * r + 1;
    for (int y = 0; y < h; y++) for (int ch = 0; ch < 4; ch++) {
        int sum = 0;
        for (int k = -r; k <= r; k++) sum += src[((size_t)y * w + std::max(0, std::min(w - 1, k))) * 4 + ch];
        for (int x = 0; x < w; x++) {
            mid[((size_t)y * w + x) * 4 + ch] = (uint8_t)(sum / span);
            int out = std::max(0, x - r), in = std::min(w - 1, x + r + 1);
            sum += src[((size_t)y * w + in) * 4 + ch] - src[((size_t)y * w + out) * 4 + ch];
        }
    }
    for (int x = 0; x < w; x++) for (int ch = 0; ch < 4; ch++) {
        int sum = 0;
        for (int k = -r; k <= r; k++) sum += mid[((size_t)std::max(0, std::min(h - 1, k)) * w + x) * 4 + ch];
        for (int y = 0; y < h; y++) {
            dst[((size_t)y * w + x) * 4 + ch] = (uint8_t)(sum / span);
            int out = std::max(0, y - r), in = std::min(h - 1, y + r + 1);
            sum += mid[((size_t)in * w + x) * 4 + ch] - mid[((size_t)out * w + x) * 4 + ch];
        }
    }
    printf("blur %lld ms (pixel %d)\n", ms(t), dst[(size_t)w * 4 * 100 + 400]);
}

static void shrink() {
    const int w = 4000, h = 3000;
    std::vector<uint8_t> src((size_t)w * h * 4);
    for (size_t i = 0; i < src.size(); i++) src[i] = (uint8_t)(i * 17 % 253);
    int ow = w / 4, oh = h / 4;
    std::vector<uint8_t> dst((size_t)ow * oh * 4);
    auto t = Clock::now();
    for (int oy = 0; oy < oh; oy++) for (int ox = 0; ox < ow; ox++) for (int ch = 0; ch < 4; ch++) {
        int sum = 0;
        for (int sy = 0; sy < 4; sy++) { size_t row = ((size_t)(oy * 4 + sy) * w + ox * 4) * 4 + ch; sum += src[row] + src[row + 4] + src[row + 8] + src[row + 12]; }
        dst[((size_t)oy * ow + ox) * 4 + ch] = (uint8_t)(sum / 16);
    }
    printf("shrink %lld ms (pixel %d)\n", ms(t), dst[(size_t)ow * 4 * 10 + 40]);
}

static void flood() {
    const int w = 2000, h = 2000;
    std::vector<uint8_t> px((size_t)w * h * 4);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        bool wall = (x % 97 == 0 && y % 5 != 0) || (y % 89 == 0 && x % 7 != 0);
        int v = wall ? 0 : 200 + (x + y) % 20;
        size_t at = ((size_t)y * w + x) * 4;
        px[at] = px[at + 1] = px[at + 2] = (uint8_t)v; px[at + 3] = 255;
    }
    auto t = Clock::now();
    std::vector<uint8_t> seen((size_t)w * h);
    std::vector<int> queue;
    int start = (h / 2) * w + w / 2, target = px[(size_t)start * 4];
    queue.push_back(start); seen[start] = 1;
    size_t head = 0; long long filled = 0;
    while (head < queue.size()) {
        int p = queue[head++]; filled++;
        int x = p % w, y = p / w;
        for (int d = 0; d < 4; d++) {
            int nx = d == 0 ? x - 1 : d == 1 ? x + 1 : x, ny = d == 2 ? y - 1 : d == 3 ? y + 1 : y;
            if (nx >= 0 && nx < w && ny >= 0 && ny < h) {
                int q = ny * w + nx;
                if (!seen[q] && std::abs(px[(size_t)q * 4] - target) <= 30) { seen[q] = 1; queue.push_back(q); }
            }
        }
    }
    printf("flood %lld ms (filled %lld)\n", ms(t), filled);
}

static void radial() {
    const int w = 3840, h = 2160;
    std::vector<uint8_t> px((size_t)w * h * 4);
    auto t = Clock::now();
    const double cx = w / 2.0, cy = h / 2.0, radius = 1500.0;
    for (int y = 0; y < h; y++) {
        size_t at = (size_t)y * w * 4;
        const double dy = y - cy;
        for (int x = 0; x < w; x++) {
            const double dx = x - cx;
            const double tt = std::min(std::sqrt(dx * dx + dy * dy) / radius, 1.0);
            px[at] = (uint8_t)(int)(30 + tt * 200); px[at + 1] = (uint8_t)(int)(200 - tt * 150); px[at + 2] = (uint8_t)(int)(90 + tt * 60); px[at + 3] = 255;
            at += 4;
        }
    }
    printf("radial %lld ms (pixel %d)\n", ms(t), px[(size_t)w * 4 * 100 + 400]);
}

static void gaussian() {
    const int w = 1920, h = 1080, r = 6;
    const double sigma = 2.0;
    double weights[13], total = 0;
    for (int k = -r; k <= r; k++) { weights[k + r] = std::exp(-(double)(k * k) / (2 * sigma * sigma)); total += weights[k + r]; }
    for (double& v : weights) v /= total;
    std::vector<uint8_t> src((size_t)w * h * 4), dst(src.size());
    for (size_t i = 0; i < src.size(); i++) src[i] = (uint8_t)(i * 31 % 251);
    std::vector<double> mid(src.size());
    auto t = Clock::now();
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) for (int ch = 0; ch < 4; ch++) {
        double sum = 0;
        for (int k = -r; k <= r; k++) sum += src[((size_t)y * w + std::max(0, std::min(w - 1, x + k))) * 4 + ch] * weights[k + r];
        mid[((size_t)y * w + x) * 4 + ch] = sum;
    }
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) for (int ch = 0; ch < 4; ch++) {
        double sum = 0;
        for (int k = -r; k <= r; k++) sum += mid[((size_t)std::max(0, std::min(h - 1, y + k)) * w + x) * 4 + ch] * weights[k + r];
        dst[((size_t)y * w + x) * 4 + ch] = (uint8_t)(int)(sum + 0.5);
    }
    printf("gaussian %lld ms (pixel %d)\n", ms(t), dst[(size_t)w * 4 * 100 + 400]);
}

int main(int argc, char** argv) {
    const std::string which = argc > 1 ? argv[1] : "all";
    struct Kernel { const char* name; void (*run)(); };
    const Kernel all[] = {{"lineEnds", lineEnds}, {"idct", idct}, {"huffman", huffman}, {"gradient", gradient}, {"blur", blur},
                          {"shrink", shrink}, {"flood", flood}, {"radial", radial}, {"gaussian", gaussian}};
    for (const Kernel& k : all)
        if (which == "all" || which == k.name) k.run();
}
