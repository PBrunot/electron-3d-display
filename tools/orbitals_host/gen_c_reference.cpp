// Generate C++-reference CSV artifacts for the hydrogen-orbital math
// cross-check, using src/orbitals.h/.cpp -- the same source PlatformIO will
// later compile for the ESP32.
//
// Usage: gen_c_reference <test_cases.csv> <out_dir>
//
// Build in double precision (tight cross-check vs the JS reference):
//   g++ -std=c++17 -O2 -DORBITAL_USE_DOUBLE gen_c_reference.cpp ../../src/orbitals.cpp -I ../../src -o out/gen_c_f64
// Build in float precision (matches the eventual ESP32 build; used to
// quantify precision loss, not as the correctness gate):
//   g++ -std=c++17 -O2 gen_c_reference.cpp ../../src/orbitals.cpp -I ../../src -o out/gen_c_f32
#include "orbitals.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace {

constexpr int kTableSize = kOrbitalTableSize;

// Sample grid for psi_samples.csv -- MUST match the constants in
// gen_js_reference.js exactly (r as a fraction of maxR, theta/phi in radians).
const std::vector<double> kRFracs = {0.0, 0.02, 0.05, 0.1, 0.2, 0.35, 0.55, 0.8};
const std::vector<double> kThetaValues = {0, 1, 2, 3, 4, 5, 6}; // multiplied by pi/6 below
const std::vector<double> kPhiFracs = {0, 0.25, 0.5, 1.0, 1.5}; // multiplied by pi below

constexpr double kPiD = 3.14159265358979323846;

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

// Print with enough digits to round-trip a double, regardless of whether the
// underlying computation ran in float or double (implicit float->double
// promotion at call sites covers orb_real_t == float).
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

        orb_real_t legendreCoeff[kOrbitalEllMax + 1] = {};
        legendreCoeffs(l, m, legendreCoeff);

        orb_real_t laguerreCoeff[kOrbitalNMax] = {};
        laguerreCoeffs(n, l, laguerreCoeff);

        // coeffs.csv
        {
            std::ofstream out(outDir + "/" + p + "_coeffs.csv");
            out << "kind,index,value\n";
            for (int i = 0; i <= l; i++)
                out << "legendre," << i << "," << fmt(legendreCoeff[i]) << "\n";
            int degree = n - l > 0 ? n - l : 1;
            for (int i = 0; i < degree; i++)
                out << "laguerre," << i << "," << fmt(laguerreCoeff[i]) << "\n";
        }

        // legendre_table.csv
        {
            orb_real_t table[kTableSize];
            buildLegendreTable(l, m, table, kTableSize);
            std::ofstream out(outDir + "/" + p + "_legendre_table.csv");
            out << "i,theta,value\n";
            for (int i = 0; i < kTableSize; i++) {
                double theta = kPiD * i / (kTableSize - 1);
                out << i << "," << fmt(theta) << "," << fmt(table[i]) << "\n";
            }
        }

        // radial_table.csv
        orb_real_t radialTable[kTableSize];
        orb_real_t maxR = 0;
        buildRadialTable(n, l, radialTable, kTableSize, &maxR);
        {
            std::ofstream out(outDir + "/" + p + "_radial_table.csv");
            out << "# maxR=" << fmt(maxR) << "\n";
            out << "i,r,value\n";
            double deltaR = double(maxR) / (kTableSize - 1);
            for (int i = 0; i < kTableSize; i++) {
                out << i << "," << fmt(i * deltaR) << "," << fmt(radialTable[i]) << "\n";
            }
        }

        // psi_samples.csv
        {
            std::ofstream out(outDir + "/" + p + "_psi_samples.csv");
            out << "r_frac,theta,phi,psi\n";
            for (double rf : kRFracs) {
                orb_real_t r = orb_real_t(rf * double(maxR));
                for (double thetaMul : kThetaValues) {
                    orb_real_t theta = orb_real_t(kPiD * thetaMul / 6.0);
                    for (double phiFrac : kPhiFracs) {
                        orb_real_t phi = orb_real_t(kPiD * phiFrac);
                        orb_real_t psi = psiReal(r, theta, phi, n, l, m, laguerreCoeff, legendreCoeff);
                        out << fmt(rf) << "," << fmt(theta) << "," << fmt(phi) << "," << fmt(psi) << "\n";
                    }
                }
            }
        }

        std::printf("C++: wrote artifacts for n=%d l=%d m=%d\n", n, l, m);
    }
    return 0;
}
