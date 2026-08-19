# First ionization energy, in eV. Source: NIST SRD 111 (Kramida, Ralchenko,
# Reader, and NIST ASD Team), the same reference validate_atoms.py's IP_EV
# curated subset draws from -- this table just extends coverage to every
# Z=1..92 element (the SPARC-atomSFE radial tables' cap) instead of only the
# high-confidence subset validate_atoms.py's Koopmans check needs. Values
# for Z in IP_EV match that table exactly; the rest fill the gaps from the
# same NIST ASD ground-state ionization energy listings. Keys: Z -> eV.

IONIZATION_EV = {
    1: 13.598, 2: 24.587, 3: 5.392, 4: 9.323, 5: 8.298, 6: 11.260,
    7: 14.534, 8: 13.618, 9: 17.423, 10: 21.565, 11: 5.139, 12: 7.646,
    13: 5.986, 14: 8.152, 15: 10.487, 16: 10.360, 17: 12.968, 18: 15.760,
    19: 4.341, 20: 6.113, 21: 6.562, 22: 6.828, 23: 6.746, 24: 6.767,
    25: 7.434, 26: 7.902, 27: 7.881, 28: 7.640, 29: 7.726, 30: 9.394,
    31: 5.999, 32: 7.899, 33: 9.789, 34: 9.752, 35: 11.814, 36: 13.999,
    37: 4.177, 38: 5.695, 39: 6.217, 40: 6.634, 41: 6.759, 42: 7.092,
    43: 7.280, 44: 7.361, 45: 7.459, 46: 8.337, 47: 7.576, 48: 8.994,
    49: 5.786, 50: 7.344, 51: 8.608, 52: 9.010, 53: 10.451, 54: 12.130,
    55: 3.894, 56: 5.212, 57: 5.577, 58: 5.539, 59: 5.473, 60: 5.525,
    61: 5.582, 62: 5.644, 63: 5.670, 64: 6.150, 65: 5.864, 66: 5.939,
    67: 6.022, 68: 6.108, 69: 6.184, 70: 6.254, 71: 5.426, 72: 6.825,
    73: 7.550, 74: 7.864, 75: 7.834, 76: 8.438, 77: 8.967, 78: 8.959,
    79: 9.226, 80: 10.438, 81: 6.108, 82: 7.417, 83: 7.286, 84: 8.414,
    85: 9.318, 86: 10.749, 87: 4.073, 88: 5.278, 89: 5.170, 90: 6.307,
    91: 5.890, 92: 6.194,
}
