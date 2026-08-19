/**
 * @file orbital_library.cpp
 * @brief On-demand loader for kOrbitalLibrary's OrbitalSampler tables -- sibling of
 *        hfs_radial.cpp's design (see that file's header comment for the full rationale),
 *        applied here to orbital_library.h's fixed 36-preset hydrogen-orbital library
 *        instead of the HFS/atomSFE per-element radial tables.
 *
 * The data lives in data/orbital_samplers.bin, flashed to the "storage" SPIFFS partition
 * (partitions_16M.csv) via `pio run -t uploadfs` (chained onto every `pio run -t upload`,
 * see tools/extra_script_uploadfs.py), and read here on demand -- once per preset switch
 * (orbital_presets.cpp's buildOrbitalPointCloud() and orbital_view.cpp's
 * OrbitalPresetState::load() each call findOrbitalSampler() once per switch), never per
 * frame or per sampled point -- rather than a ~423KB table compiled into firmware for data
 * that's only ever needed a preset at a time.
 *
 * Unlike hfs_radial.cpp, there's no separate element/subshell index table to load at init:
 * kOrbitalLibrary itself (36 small descriptor entries, ~720 bytes) stays compiled into
 * firmware, so a preset's index IS its record's position in the file -- orbitalInit() only
 * needs to open the file and check its 2-value header.
 *
 * Regenerate data/orbital_samplers.bin with tools/orbital_table_gen.py whenever
 * orbital_library.h's kOrbitalLibrary changes -- see that tool's header comment for the
 * binary layout and build/run instructions.
 */
#include "orbital_library.h"

#include <cstdint>
#include <cstdio>

#include "esp_attr.h" // EXT_RAM_BSS_ATTR
#include "esp_log.h"
#include "esp_spiffs.h"

namespace
{
    constexpr auto kTag = "orbital";
    constexpr auto kMountPoint = "/storage";
    // Same partition hfs_radial.cpp/screenshot.cpp mount -- shared, not a separate one, so
    // there is only one filesystem to keep track of.
    constexpr auto kPartitionLabel = "storage";
    constexpr auto kPath = "/storage/orbital_samplers.bin";

    bool sAttempted = false; // orbitalInit() has run once (whether or not it succeeded)
    bool sReady = false;     // header checked out, records readable via findOrbitalSampler()
    long sDataOffset = 0;
    // Kept open for the process lifetime once orbitalInit() succeeds, instead of a fresh
    // fopen()/fclose() per findOrbitalSampler() call -- see hfs_radial.cpp's own comment on
    // this same convention for the measured cost of NOT doing this (SPIFFS open/close is the
    // expensive part, not the seek/read that follows it).
    FILE *sDataFile = nullptr;

    // Not reentrant -- overwritten on every findOrbitalSampler() call, valid until the next
    // one (see that function's docstring in orbital_library.h). Fine here: only one preset
    // is ever active at a time, and every caller consumes the pointer well before the next
    // preset switch could invalidate it.
    //
    // EXT_RAM_BSS_ATTR (PSRAM, not internal RAM): this struct alone is ~12KB (three
    // 1001-float tables), and Display's own present buffers need MALLOC_CAP_DMA allocations
    // that can only come from internal RAM's tight 320KB budget -- leaving a struct this
    // size in the default internal .bss starves that allocation and aborts at boot (see
    // orbital_view.cpp's/atom_view.cpp's identical-purpose attribute on their own preset
    // state for the same constraint). CPU-only access here (file reads, no DMA), so PSRAM's
    // slightly higher latency is a non-issue.
    EXT_RAM_BSS_ATTR OrbitalSampler sSampler{};

    // n, ell, m (int32 each) + maxR (float32) + three kOrbitalTableSize-float32 tables --
    // MUST match tools/orbital_table_gen.py's emitted record layout exactly. int32_t/float
    // here (not `int`/orb_real_t) because the on-disk format is fixed at float32 regardless
    // of how orb_real_t is typedef'd, whereas this project's ESP32 build always uses
    // orb_real_t=float anyway (see orbitals.h), so the two coincide in practice.
    constexpr long kRecordSize =
        long(sizeof(int32_t)) * 3 + long(sizeof(float)) * (1 + 3 * kOrbitalTableSize);

    /// Registers the "storage" SPIFFS partition at /storage if not already mounted -- same
    /// idempotent, never-format convention as hfs_radial.cpp's ensureMounted() (see that
    /// function's comment for why format_if_mount_failed stays false here too).
    bool ensureMounted()
    {
        esp_vfs_spiffs_conf_t conf = {};
        conf.base_path = kMountPoint;
        conf.partition_label = kPartitionLabel;
        conf.max_files = 8;
        conf.format_if_mount_failed = false;

        esp_err_t err = esp_vfs_spiffs_register(&conf);
        return err == ESP_OK || err == ESP_ERR_INVALID_STATE;
    }

    void orbitalInit()
    {
        if (sAttempted)
            return;
        sAttempted = true;

        if (!ensureMounted())
        {
            ESP_LOGE(kTag, "%s mount failed -- orbital presets will render as a single point "
                           "at the origin until this is fixed",
                     kMountPoint);
            return;
        }

        FILE *f = fopen(kPath, "rb");
        if (f == nullptr)
        {
            ESP_LOGE(kTag, "%s not found -- run `pio run -t uploadfs` to deploy it (see "
                           "tools/orbital_table_gen.py); orbital presets will render as a "
                           "single point at the origin until then",
                     kPath);
            return;
        }

        uint16_t header[2];
        bool ok = fread(header, sizeof(uint16_t), 2, f) == 2;
        ok = ok && header[0] == uint16_t(kOrbitalLibraryCount) && header[1] == uint16_t(kOrbitalTableSize);
        if (ok)
            sDataOffset = ftell(f);

        if (!ok)
        {
            fclose(f);
            ESP_LOGE(kTag, "%s is malformed or stale (regenerate with "
                           "tools/orbital_table_gen.py)",
                     kPath);
            return;
        }
        sDataFile = f; // kept open, see this variable's own comment above
        sReady = true;
        ESP_LOGI(kTag, "orbital sampler table ready: %d presets, %d pts/table", kOrbitalLibraryCount,
                 kOrbitalTableSize);
    }
} // namespace

const OrbitalSampler *findOrbitalSampler(int n, int ell, int m)
{
    orbitalInit();

    int index = -1;
    for (int i = 0; i < kOrbitalLibraryCount; i++)
    {
        if (kOrbitalLibrary[i].n == n && kOrbitalLibrary[i].ell == ell && kOrbitalLibrary[i].m == m)
        {
            index = i;
            break;
        }
    }
    if (index < 0)
        return nullptr; // (n,ell,m) not in kOrbitalLibrary at all -- caller bug, not a data problem

    if (sReady)
    {
        fseek(sDataFile, sDataOffset + long(index) * kRecordSize, SEEK_SET);
        bool ok = fread(&sSampler.n, sizeof(int32_t), 1, sDataFile) == 1;
        ok = ok && fread(&sSampler.ell, sizeof(int32_t), 1, sDataFile) == 1;
        ok = ok && fread(&sSampler.m, sizeof(int32_t), 1, sDataFile) == 1;
        ok = ok && fread(&sSampler.maxR, sizeof(orb_real_t), 1, sDataFile) == 1;
        ok = ok && fread(sSampler.invRTable, sizeof(orb_real_t), kOrbitalTableSize, sDataFile) ==
                       size_t(kOrbitalTableSize);
        ok = ok && fread(sSampler.invThetaTable, sizeof(orb_real_t), kOrbitalTableSize, sDataFile) ==
                       size_t(kOrbitalTableSize);
        ok = ok && fread(sSampler.invPhiTable, sizeof(orb_real_t), kOrbitalTableSize, sDataFile) ==
                       size_t(kOrbitalTableSize);
        if (ok)
            return &sSampler;
        ESP_LOGE(kTag, "read failed for preset %d (n=%d l=%d m=%d)", index, n, ell, m);
    }

    // No table available (not deployed yet / read failed) -- degenerate fallback: every
    // table zeroed, which getValueFromLookupTable()/sampleOrbitalPoint() turn into a single
    // point at the origin rather than a crash, matching this project's "debug/optional
    // feature degrades gracefully" convention (screenshot::init(), hfsFindU()) as closely as
    // possible here -- unlike atom_cloud.h's hydrogenic fallback for the HFS table, there's
    // no alternate MODEL to fall back to for hydrogen orbitals; this is the base model.
    sSampler = OrbitalSampler{};
    sSampler.n = n;
    sSampler.ell = ell;
    sSampler.m = m;
    return &sSampler;
}
