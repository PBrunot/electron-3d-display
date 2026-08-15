// Generate C++-reference point-cloud CSV artifacts for the hydrogen-orbital
// rejection sampler, using src/pointcloud.h/.cpp -- the same source
// PlatformIO will later compile for the ESP32.
//
// Usage: gen_points_c <test_cases.csv> <out_dir>
//
// Build in double precision (tight cross-check vs the JS/MicroPython
// references):
//   g++ -std=c++17 -O2 -DORBITAL_USE_DOUBLE gen_points_c.cpp ../../src/orbitals.cpp ../../src/pointcloud.cpp -I ../../src -o out/gen_points_c_f64
// Build in float precision (matches the eventual ESP32 build):
//   g++ -std=c++17 -O2 gen_points_c.cpp ../../src/orbitals.cpp ../../src/pointcloud.cpp -I ../../src -o out/gen_points_c_f32
#include "pointcloud.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace {

// MUST match the constants in gen_points_js.js and gen_points_mpy.py exactly.
constexpr uint32_t kSeed = 12345;
constexpr int kPointsPerCase = 100;

struct TestCase {
    int n, l, m;
};

std::vector<TestCase> parseTestCases(const std::string& csvPath) {
    std::ifstream in(csvPath);
    if (!in) {
        std::fprintf(stderr, "Cannot open %s\n", csvPath.c_str());
        std::exit(1);
    }
    std::vector<TestCase> cases;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream ss(line);
        std::string tok;
        int vals[3];
        for (int i = 0; i < 3; i++) {
            if (!std::getline(ss, tok, ','))
                goto skip;
            vals[i] = std::atoi(tok.c_str());
        }
        cases.push_back({vals[0], vals[1], vals[2]});
    skip:;
    }
    return cases;
}

std::string prefix(int n, int l, int m) {
    return std::to_string(n) + "_" + std::to_string(l) + "_" + std::to_string(m);
}

std::string fmt(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    return buf;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "Usage: %s <test_cases.csv> <out_dir>\n", argv[0]);
        return 1;
    }
    std::string testCasesPath = argv[1];
    std::string outDir = argv[2];
    mkdir(outDir.c_str(), 0755);

    auto cases = parseTestCases(testCasesPath);
    for (auto& tc : cases) {
        int n = tc.n, l = tc.l, m = tc.m;
        std::string p = prefix(n, l, m);

        OrbitalSampler sampler;
        initOrbitalSampler(&sampler, n, l, m);

        XorShift32 rng(kSeed);
        std::ofstream out(outDir + "/" + p + "_points.csv");
        out << "index,x,y,z\n";
        for (int i = 0; i < kPointsPerCase; i++) {
            OrbitalPoint pt = sampleOrbitalPoint(&sampler, &rng);
            out << i << "," << fmt(pt.x) << "," << fmt(pt.y) << "," << fmt(pt.z) << "\n";
        }

        std::printf("C++: wrote %d points for n=%d l=%d m=%d\n", kPointsPerCase, n, l, m);
    }
    return 0;
}
