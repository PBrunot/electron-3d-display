// Periodic-table (period, group) layout for all 118 elements, Z=1..118 -- lets AtomView's
// Up/Down/Left/Right walk the table like a real periodic table instead of stepping Z by +-1
// (see runAtomView()'s tilt-confirm block). Header-only constexpr data, same pattern as
// element_names_it.h.
//
// Layout convention: row 1-7 are the real periods. Lanthanides (Ce..Lu, Z58-71) and
// actinides (Th..Lr, Z90-103) are pulled out into two extra rows (8 and 9) below the main
// table -- the standard "short form" textbook layout -- rather than inlined at their real
// group in periods 6/7 (the alternative "32-column long form"). col is the IUPAC group
// (1-18) for the main table; the two pulled-out rows reuse col 3-16 (14 slots) purely to
// order their own members left-to-right, not real group numbers.
//
// Consequence: group 3's column (Sc/Y/La/Ac) is the ONLY column that touches the f-block
// rows, so vertical navigation needs one splice: La (row6,col3) -> Ce..Lu (row8) -> Ac
// (row7,col3) -> Th..Lr (row9) -- see kGroup3Column below. Every other column's vertical
// sequence is just "scan rows 1-7 for a matching col", no special-casing.
#pragma once

#include <cstdint>

struct ElementGridPos
{
    int8_t row; // 1-7 = real periods, 8 = lanthanide row, 9 = actinide row
    int8_t col; // 1-18 IUPAC group (main table); 3-16 ordering-only for rows 8/9
};

// Indexed by Z-1.
inline constexpr ElementGridPos kElementGrid[118] = {
    {1, 1},  {1, 18},                                                                   // H,He
    {2, 1},  {2, 2},  {2, 13}, {2, 14}, {2, 15}, {2, 16}, {2, 17}, {2, 18},              // Li..Ne
    {3, 1},  {3, 2},  {3, 13}, {3, 14}, {3, 15}, {3, 16}, {3, 17}, {3, 18},              // Na..Ar
    {4, 1},  {4, 2},  {4, 3},  {4, 4},  {4, 5},  {4, 6},  {4, 7},  {4, 8},  {4, 9},
    {4, 10}, {4, 11}, {4, 12}, {4, 13}, {4, 14}, {4, 15}, {4, 16}, {4, 17}, {4, 18},     // K..Kr
    {5, 1},  {5, 2},  {5, 3},  {5, 4},  {5, 5},  {5, 6},  {5, 7},  {5, 8},  {5, 9},
    {5, 10}, {5, 11}, {5, 12}, {5, 13}, {5, 14}, {5, 15}, {5, 16}, {5, 17}, {5, 18},     // Rb..Xe
    {6, 1},  {6, 2},  {6, 3},                                                            // Cs,Ba,La
    {8, 3},  {8, 4},  {8, 5},  {8, 6},  {8, 7},  {8, 8},  {8, 9},
    {8, 10}, {8, 11}, {8, 12}, {8, 13}, {8, 14}, {8, 15}, {8, 16},                       // Ce..Lu
    {6, 4},  {6, 5},  {6, 6},  {6, 7},  {6, 8},  {6, 9},  {6, 10}, {6, 11}, {6, 12},
    {6, 13}, {6, 14}, {6, 15}, {6, 16}, {6, 17}, {6, 18},                               // Hf..Rn
    {7, 1},  {7, 2},  {7, 3},                                                            // Fr,Ra,Ac
    {9, 3},  {9, 4},  {9, 5},  {9, 6},  {9, 7},  {9, 8},  {9, 9},
    {9, 10}, {9, 11}, {9, 12}, {9, 13}, {9, 14}, {9, 15}, {9, 16},                       // Th..Lr
    {7, 4},  {7, 5},  {7, 6},  {7, 7},  {7, 8},  {7, 9},  {7, 10}, {7, 11}, {7, 12},
    {7, 13}, {7, 14}, {7, 15}, {7, 16}, {7, 17}, {7, 18},                               // Rf..Og
};

// Group 3's vertical sequence, with the lanthanide/actinide detour spliced in between La
// and Ac -- see this file's header comment. Z values, top to bottom.
inline constexpr int kGroup3Column[] = {
    21, 39, 57,                                                     // Sc, Y, La
    58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71,          // Ce..Lu
    89,                                                              // Ac
    90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100, 101, 102, 103,      // Th..Lr
};
inline constexpr int kGroup3ColumnCount = sizeof(kGroup3Column) / sizeof(kGroup3Column[0]);

namespace periodic_grid_detail
{
// Compile-time transcription check: every Z in 1..118 must appear in kElementGrid exactly
// once (catches a duplicated/missing (row,col) entry), and kGroup3Column must be exactly
// the 32 elements whose kElementGrid entry has col==3 (rows 1-7) or row>=8 (any col) --
// group 3's main-table members plus the whole f-block. A transcription mistake here fails
// the build instead of silently producing a wrong neighbor on-device.
constexpr bool checkGridCoverage()
{
    // Every Z 1..118 appears exactly once as an index (kElementGrid is indexed by Z-1 by
    // construction) -- what actually needs checking is that no two entries share a
    // (row, col), which would mean two elements landed on the same table cell.
    for (int a = 0; a < 118; a++)
        for (int b = a + 1; b < 118; b++)
            if (kElementGrid[a].row == kElementGrid[b].row && kElementGrid[a].col == kElementGrid[b].col)
                return false;
    return true;
}

constexpr bool checkGroup3Membership()
{
    if (kGroup3ColumnCount != 32)
        return false;
    for (int i = 0; i < kGroup3ColumnCount; i++)
    {
        int z = kGroup3Column[i];
        if (z < 1 || z > 118)
            return false;
        ElementGridPos pos = kElementGrid[z - 1];
        bool isGroup3Main = (pos.col == 3 && pos.row >= 1 && pos.row <= 7);
        bool isFBlock = pos.row >= 8;
        if (!isGroup3Main && !isFBlock)
            return false;
    }
    // And conversely, every f-block / group-3-main element must be IN kGroup3Column.
    for (int z = 1; z <= 118; z++)
    {
        ElementGridPos pos = kElementGrid[z - 1];
        bool shouldBeMember = (pos.col == 3 && pos.row >= 1 && pos.row <= 7) || pos.row >= 8;
        if (!shouldBeMember)
            continue;
        bool found = false;
        for (int i = 0; i < kGroup3ColumnCount; i++)
            if (kGroup3Column[i] == z)
                found = true;
        if (!found)
            return false;
    }
    return true;
}
} // namespace periodic_grid_detail

static_assert(sizeof(kElementGrid) / sizeof(kElementGrid[0]) == 118, "kElementGrid must cover Z=1..118");
static_assert(periodic_grid_detail::checkGridCoverage(), "kElementGrid has two elements sharing one grid cell");
static_assert(periodic_grid_detail::checkGroup3Membership(),
              "kGroup3Column must be exactly group 3's main-table members plus the whole f-block");

/** True if `z` is one of kGroup3Column's 32 members (Sc/Y/La/Ac or any lanthanide/actinide)
 * -- these route vertical movement through the splice instead of a plain column scan. */
constexpr bool isGroup3Member(int z)
{
    for (int i = 0; i < kGroup3ColumnCount; i++)
        if (kGroup3Column[i] == z)
            return true;
    return false;
}

namespace periodic_grid_detail
{
// Every main-table element (row 1-7) whose col != 3, in Z order for that col -- Z order and
// row order coincide within a single column of this table (higher Z means higher period),
// so this is already row-sorted top to bottom. Group 3's own column is never built this way
// -- see isGroup3Member()/kGroup3Column -- so col is never 3 here.
inline int buildColumnMembers(int col, int *members)
{
    int count = 0;
    for (int z = 1; z <= 118; z++)
    {
        ElementGridPos pos = kElementGrid[z - 1];
        if (pos.row >= 1 && pos.row <= 7 && pos.col == col)
            members[count++] = z;
    }
    return count;
}
} // namespace periodic_grid_detail

// Display Z range cap (2026-08): the SPARC-atomSFE radial tables cover Z=1..92 and the
// whole project is deliberately limited to that range -- AtomView navigation never steps
// past kMaxDisplayZ. The Z=93..118 data in kElementGrid/kGroup3Column stays (physics and
// validation code still read it).
inline constexpr int kMaxDisplayZ = 92;

/**
 * Step `currentZ` by one position in the periodic table's "read down a column, then jump to
 * the top of the next column" order (delta = +1 "forward"/"down", -1 "backward"/"up"),
 * wrapping col 18 -> col 1 (and back). Backs AtomView's Up/Down tilt (see runAtomView()) --
 * a single axis walks the WHOLE table in this one order, not an independent vertical/
 * horizontal 2D grid (Left/Right are used for menu-return/dissection instead, see
 * atom_view.cpp).
 *
 * Group 3 (Sc/Y/La/Ac) occupies col 3's slot in this order, but its own members span
 * kGroup3Column's splice (Sc, Y, La, Ce..Lu, Ac, Th..Lr -- see that array's comment); reaching
 * either end of the splice moves to col 2's or col 4's column exactly as if group 3 were a
 * normal, if unusually tall, column. Every other column scans kElementGrid directly
 * (buildColumnMembers() above). Each step is the exact inverse of the opposite-delta step
 * from the resulting element, so alternating Up/Down always returns to where you started.
 */
namespace periodic_grid_detail
{

// Full 118-element snake step -- the un-capped walk behind the capped
// periodicTableSnakeStep() below (see its docstring for the walk's order).
inline int periodicTableSnakeStepFull(int currentZ, int delta)
{
    if (isGroup3Member(currentZ))
    {
        int idx = 0;
        for (int i = 0; i < kGroup3ColumnCount; i++)
            if (kGroup3Column[i] == currentZ)
                idx = i;
        int newIdx = idx + delta;
        if (newIdx >= 0 && newIdx < kGroup3ColumnCount)
            return kGroup3Column[newIdx];
        // Exiting the splice at either end -- move into col 4 (forward) or col 2 (backward).
        int members[18];
        int count = periodic_grid_detail::buildColumnMembers(delta > 0 ? 4 : 2, members);
        return delta > 0 ? members[0] : members[count - 1];
    }

    int col = kElementGrid[currentZ - 1].col;
    int members[18];
    int count = periodic_grid_detail::buildColumnMembers(col, members);
    int idx = 0;
    for (int i = 0; i < count; i++)
        if (members[i] == currentZ)
            idx = i;
    int newIdx = idx + delta;
    if (newIdx >= 0 && newIdx < count)
        return members[newIdx];

    // Exiting this column at either end -- move to the adjacent column, wrapping 18<->1.
    // col is never 3 here (group 3 members are handled above), so a plain +-1 step can only
    // ever land exactly on 3 by arriving from col 2 (forward) or col 4 (backward): both
    // correctly mean "enter group 3's splice", handled as a special case below.
    int nextCol = col + delta;
    if (nextCol < 1)
        nextCol = 18;
    else if (nextCol > 18)
        nextCol = 1;
    if (nextCol == 3)
        return delta > 0 ? kGroup3Column[0] : kGroup3Column[kGroup3ColumnCount - 1];
    int nextCount = periodic_grid_detail::buildColumnMembers(nextCol, members);
    return delta > 0 ? members[0] : members[nextCount - 1];
}
} // namespace periodic_grid_detail

/**
 * Capped snake step: the full-table walk with every Z > kMaxDisplayZ skipped, so the cycle
 * runs over exactly the Z=1..92 elements (forward from the last in-range element wraps to
 * H, backward from H wraps to the last in-range one -- the same structure the full table
 * has at 118). Every in-range element keeps exactly one successor/predecessor, so
 * alternating Up/Down still returns to where you started.
 */
inline int periodicTableSnakeStep(int currentZ, int delta)
{
    int z = currentZ;
    for (int guard = 0; guard < 118; guard++)
    {
        z = periodic_grid_detail::periodicTableSnakeStepFull(z, delta);
        if (z <= kMaxDisplayZ)
            return z;
    }
    return currentZ; // unreachable: the full cycle always reaches an in-range element
}
