#include "physics/hfs_radial.h"

#include <algorithm>
#include <cstdio>

#include "esp_log.h"
#include "util/storage_mount.h"

// data/hfs_tables.bin's u(r) data is fixed at float32 (see tools/hfs_table_gen.py) regardless
// of how orb_real_t is typedef'd; the fread() calls below read `sizeof(orb_real_t)`-sized
// chunks straight into orb_real_t buffers, which is only correct because this project's ESP32
// build always has orb_real_t == float (see orbitals.h). Fail to compile rather than silently
// misread the file if that ever changes (e.g. a future ORBITAL_USE_DOUBLE build of this TU).
static_assert(sizeof(orb_real_t) == sizeof(float), "data/hfs_tables.bin stores u(r) as float32");

namespace
{
    constexpr auto kTag = "hfs";
    constexpr auto kMountPoint = "/storage"; // for log messages only, see util/storage_mount.h
    constexpr auto kPath = "/storage/hfs_tables.bin";

    struct HfsElementEntry
    {
        uint16_t offset;
        uint8_t count;
    };
    struct HfsSubshellEntry
    {
        uint8_t n, ell;
    };

    bool sAttempted = false; // hfsInit() has run once (whether or not it succeeded)
    bool sReady = false;     // header/index loaded, u-data rows readable via hfsFindU()
    orb_real_t sR[kHfsGridSize];
    HfsElementEntry sElement[kHfsElementCount];
    HfsSubshellEntry sSubshell[kHfsSubshellCount];
    long sUDataOffset = 0;
    // Kept open for the process lifetime once hfsInit() succeeds, instead of a fresh
    // fopen()/fclose() per hfsFindU() call: measured on real hardware, re-opening SPIFFS'
    // single hfs_tables.bin file once per subshell (7 opens for Fe) inflated
    // buildAtomPointCloud()'s own build_ms from ~30-60ms (hydrogenic) to ~800-980ms -- SPIFFS
    // open/close is the expensive part (object-index lookup), not the seek/read that follows
    // it. A held-open FILE* only needs fseek()+fread() per call.
    FILE *sDataFile = nullptr;
} // namespace

void hfsInit()
{
    if (sAttempted)
        return;
    sAttempted = true;

    if (!ensureStorageMounted())
    {
        ESP_LOGW(kTag, "%s mount failed -- atom viewer will use the hydrogenic fallback model", kMountPoint);
        return;
    }

    FILE *f = fopen(kPath, "rb");
    if (f == nullptr)
    {
        ESP_LOGW(kTag, "%s not found -- run `pio run -t uploadfs` to deploy it; using the hydrogenic "
                       "fallback model until then",
                 kPath);
        return;
    }

    uint16_t header[3];
    bool ok = fread(header, sizeof(uint16_t), 3, f) == 3;
    ok = ok && header[0] == kHfsGridSize && header[1] == kHfsElementCount && header[2] == kHfsSubshellCount;
    ok = ok && fread(sR, sizeof(orb_real_t), kHfsGridSize, f) == size_t(kHfsGridSize);
    for (int i = 0; ok && i < kHfsElementCount; i++)
    {
        uint16_t offset = 0;
        uint8_t count = 0;
        ok = fread(&offset, sizeof(offset), 1, f) == 1 && fread(&count, sizeof(count), 1, f) == 1;
        sElement[i] = {offset, count};
    }
    for (int i = 0; ok && i < kHfsSubshellCount; i++)
    {
        uint8_t n = 0, ell = 0;
        ok = fread(&n, sizeof(n), 1, f) == 1 && fread(&ell, sizeof(ell), 1, f) == 1;
        sSubshell[i] = {n, ell};
    }
    if (ok)
        sUDataOffset = ftell(f);

    if (!ok)
    {
        fclose(f);
        ESP_LOGE(kTag, "%s is malformed or stale (regenerate with tools/hfs_table_gen.py) -- using the "
                       "hydrogenic fallback model",
                 kPath);
        return;
    }
    sDataFile = f; // kept open, see this variable's own comment above
    sReady = true;
    ESP_LOGI(kTag, "hfs tables ready: %d elements, %d subshells, %d pts/subshell", kHfsElementCount,
             kHfsSubshellCount, kHfsGridSize);
}

const orb_real_t *hfsFindU(int z, int n, int ell)
{
    hfsInit();
    if (!sReady || z < 1 || z > kHfsElementCount)
        return nullptr;

    const HfsElementEntry &e = sElement[z - 1];
    const HfsSubshellEntry *begin = sSubshell + e.offset;
    const HfsSubshellEntry *end = begin + e.count;
    const HfsSubshellEntry *found = std::find_if(begin, end, [n, ell](const HfsSubshellEntry &s)
                                                  { return s.n == n && s.ell == ell; });
    if (found == end)
        return nullptr;
    int rowIndex = e.offset + int(found - begin);

    // Not reentrant -- see this function's docstring in hfs_radial.h for the validity window.
    static orb_real_t sRowScratch[kHfsGridSize];
    fseek(sDataFile, sUDataOffset + long(rowIndex) * long(kHfsGridSize) * long(sizeof(orb_real_t)), SEEK_SET);
    size_t got = fread(sRowScratch, sizeof(orb_real_t), kHfsGridSize, sDataFile);
    if (got != size_t(kHfsGridSize))
        return nullptr;
    return sRowScratch;
}

orb_real_t hfsRLookup(orb_real_t r, const orb_real_t *u)
{
    if (r <= orb_real_t(1e-9))
        return orb_real_t(0);
    return interpOnGrid(r, sR, u, kHfsGridSize) / r;
}

const RadialTable &buildHfsRadialSamplerIsotropic(const orb_real_t *u)
{
    static RadialTable rt;
    static orb_real_t weight[kHfsGridSize];
    for (int i = 0; i < kHfsGridSize; i++)
        weight[i] = u[i] * u[i];
    rt.maxR = sR[kHfsGridSize - 1];
    buildInverseCdfFromGrid(weight, sR, kHfsGridSize, rt.invRTable, kOrbitalTableSize);
    return rt;
}

const RadialTable &buildHfsRadialSamplerOriented(const orb_real_t *u)
{
    static RadialTable rt;
    static orb_real_t weight[kOrbitalTableSize];
    orb_real_t maxR = sR[kHfsGridSize - 1];
    rt.maxR = maxR;
    orb_real_t deltaR = maxR / orb_real_t(kOrbitalTableSize - 1);
    for (int i = 0; i < kOrbitalTableSize; i++)
    {
        orb_real_t r = orb_real_t(i) * deltaR;
        orb_real_t R = hfsRLookup(r, u);
        weight[i] = (r * R) * (r * R);
    }
    buildInverseCdf(weight, kOrbitalTableSize, maxR, rt.invRTable);
    return rt;
}
