// Italian element names, Z=1..118 -- hand-transcribed (standard Italian IUPAC names, e.g.
// as used by Treccani/CNR), UNLIKE slater_data.h's kElementSymbols (auto-generated from
// the Python source of truth): there is no Python-side equivalent to port this from, so
// this file IS the source of truth for these names. Used by atom_view.cpp's element-switch
// ticker (ticker.h) -- ASCII-only (Jersey10's baked glyph set has no accented letters), so
// a name that would normally carry an accent in Italian is spelled without one here (none
// of these 118 happen to need one anyway).
#pragma once

constexpr const char *kElementNamesIt[118] = {
    "Idrogeno",       // 1 H
    "Elio",           // 2 He
    "Litio",          // 3 Li
    "Berillio",       // 4 Be
    "Boro",           // 5 B
    "Carbonio",       // 6 C
    "Azoto",          // 7 N
    "Ossigeno",       // 8 O
    "Fluoro",         // 9 F
    "Neon",           // 10 Ne
    "Sodio",          // 11 Na
    "Magnesio",       // 12 Mg
    "Alluminio",      // 13 Al
    "Silicio",        // 14 Si
    "Fosforo",        // 15 P
    "Zolfo",          // 16 S
    "Cloro",          // 17 Cl
    "Argon",          // 18 Ar
    "Potassio",       // 19 K
    "Calcio",         // 20 Ca
    "Scandio",        // 21 Sc
    "Titanio",        // 22 Ti
    "Vanadio",        // 23 V
    "Cromo",          // 24 Cr
    "Manganese",      // 25 Mn
    "Ferro",          // 26 Fe
    "Cobalto",        // 27 Co
    "Nichel",         // 28 Ni
    "Rame",           // 29 Cu
    "Zinco",          // 30 Zn
    "Gallio",         // 31 Ga
    "Germanio",       // 32 Ge
    "Arsenico",       // 33 As
    "Selenio",        // 34 Se
    "Bromo",          // 35 Br
    "Kripton",        // 36 Kr
    "Rubidio",        // 37 Rb
    "Stronzio",       // 38 Sr
    "Ittrio",         // 39 Y
    "Zirconio",       // 40 Zr
    "Niobio",         // 41 Nb
    "Molibdeno",      // 42 Mo
    "Tecnezio",       // 43 Tc
    "Rutenio",        // 44 Ru
    "Rodio",          // 45 Rh
    "Palladio",       // 46 Pd
    "Argento",        // 47 Ag
    "Cadmio",         // 48 Cd
    "Indio",          // 49 In
    "Stagno",         // 50 Sn
    "Antimonio",      // 51 Sb
    "Tellurio",       // 52 Te
    "Iodio",          // 53 I
    "Xeno",           // 54 Xe
    "Cesio",          // 55 Cs
    "Bario",          // 56 Ba
    "Lantanio",       // 57 La
    "Cerio",          // 58 Ce
    "Praseodimio",    // 59 Pr
    "Neodimio",       // 60 Nd
    "Promezio",       // 61 Pm
    "Samario",        // 62 Sm
    "Europio",        // 63 Eu
    "Gadolinio",      // 64 Gd
    "Terbio",         // 65 Tb
    "Disprosio",      // 66 Dy
    "Olmio",          // 67 Ho
    "Erbio",          // 68 Er
    "Tulio",          // 69 Tm
    "Itterbio",       // 70 Yb
    "Lutezio",        // 71 Lu
    "Afnio",          // 72 Hf
    "Tantalio",       // 73 Ta
    "Tungsteno",      // 74 W
    "Renio",          // 75 Re
    "Osmio",          // 76 Os
    "Iridio",         // 77 Ir
    "Platino",        // 78 Pt
    "Oro",            // 79 Au
    "Mercurio",       // 80 Hg
    "Tallio",         // 81 Tl
    "Piombo",         // 82 Pb
    "Bismuto",        // 83 Bi
    "Polonio",        // 84 Po
    "Astato",         // 85 At
    "Radon",          // 86 Rn
    "Francio",        // 87 Fr
    "Radio",          // 88 Ra
    "Attinio",        // 89 Ac
    "Torio",          // 90 Th
    "Protoattinio",   // 91 Pa
    "Uranio",         // 92 U
    "Nettunio",       // 93 Np
    "Plutonio",       // 94 Pu
    "Americio",       // 95 Am
    "Curio",          // 96 Cm
    "Berkelio",       // 97 Bk
    "Californio",     // 98 Cf
    "Einsteinio",     // 99 Es
    "Fermio",         // 100 Fm
    "Mendelevio",     // 101 Md
    "Nobelio",        // 102 No
    "Laurenzio",      // 103 Lr
    "Rutherfordio",   // 104 Rf
    "Dubnio",         // 105 Db
    "Seaborgio",      // 106 Sg
    "Bohrio",         // 107 Bh
    "Hassio",         // 108 Hs
    "Meitnerio",      // 109 Mt
    "Darmstadtio",    // 110 Ds
    "Roentgenio",     // 111 Rg
    "Copernicio",     // 112 Cn
    "Nihonio",        // 113 Nh
    "Flerovio",       // 114 Fl
    "Moscovio",       // 115 Mc
    "Livermorio",     // 116 Lv
    "Tennesso",       // 117 Ts
    "Oganesson",      // 118 Og
};

/** kElementNamesIt[z - 1] -- z must be 1..118, matches slater.h's elementSymbol() convention. */
inline const char *elementNameIt(int z)
{
    return kElementNamesIt[z - 1];
}
