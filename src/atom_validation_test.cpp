#include "atom_validation_test.h"

#include "atom_cloud.h"
#include "esp_log.h"

static const char *kAtomValidationTestTag = "orbital_test";

// MUST match tools/orbitals_host/gen_atom_reference.py's ATOM_TEST_CASES/SEED/POINTS_PER_CASE
// exactly, or the two sides aren't comparing the same thing.
static constexpr int kValidationZs[] = {1, 2, 6, 10, 24, 26, 46, 58}; // H, He, C, Ne, Cr, Fe, Pd, Ce
static constexpr uint32_t kValidationSeed = 12345;
static constexpr int kValidationPoints = 50;

void runAtomValidationTest()
{
    static AtomPoint points[kValidationPoints];

    for (int zi = 0; zi < int(sizeof(kValidationZs) / sizeof(kValidationZs[0])); zi++)
    {
        int z = kValidationZs[zi];
        const char *symbol = elementSymbol(z);
        AtomSubshellRange ranges[kMaxConfigSubshells];
        int rangeCount = 0;
        ElectronConfig config = buildAtomPointCloud(z, points, kValidationPoints, kValidationSeed, ranges, &rangeCount);

        for (int i = 0; i < config.count; i++)
        {
            ESP_LOGI(kAtomValidationTestTag, "ATOMTEST,CONFIG,%s,%d,%d,%d", symbol, config.subshells[i].n,
                     config.subshells[i].ell, config.subshells[i].occ);
        }
        for (int i = 0; i < config.count; i++)
        {
            int n = config.subshells[i].n, ell = config.subshells[i].ell;
            orb_real_t zEff = zEffRadial(z, config, n, ell);
            ESP_LOGI(kAtomValidationTestTag, "ATOMTEST,ZEFF,%s,%d,%d,%.17g", symbol, n, ell, double(zEff));
        }
        for (int i = 0; i < kValidationPoints; i++)
        {
            ESP_LOGI(kAtomValidationTestTag, "ATOMTEST,POINT,%s,%d,%.17g,%.17g,%.17g", symbol, i,
                     double(points[i].x), double(points[i].y), double(points[i].z));
        }
    }
    ESP_LOGI(kAtomValidationTestTag, "ATOMTEST,DONE");
}
