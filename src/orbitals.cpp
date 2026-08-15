#include "orbitals.h"

#include <algorithm>
#include <cmath>

void legendreCoeffs(int ell, int m, orb_real_t* coeff) {
    for (int i = 0; i <= ell; i++)
        coeff[i] = orb_real_t(0);

    int absM = m < 0 ? -m : m;
    int ellEll1 = ell * (ell + 1);
    coeff[0] = orb_real_t(1 - 2 * (ell % 2));

    for (int iM = ell; iM > absM; iM--) {
        orb_real_t denominator = std::sqrt(orb_real_t(ellEll1 - iM * (iM - 1)));
        int kStart = (ell - iM) % 2;
        if (kStart == 1)
            coeff[0] = orb_real_t(0);
        for (int k = kStart; k <= (ell - iM); k += 2) {
            if (k > 0)
                coeff[k - 1] += orb_real_t(k) * coeff[k] / denominator;
            coeff[k + 1] = -orb_real_t(k + 2 * iM) * coeff[k] / denominator;
        }
    }

    // Normalization so that maximum value is equal to one, sampled over
    // theta in [0, pi/2) in steps of pi/100 -- matches initLegendreCoeffs()
    // in quantum-physics.js exactly (same step count/bound), so the two
    // implementations converge to the same normalization constant.
    orb_real_t maxValue = orb_real_t(0);
    for (orb_real_t theta = orb_real_t(0); theta < kOrbitalPi / 2; theta += kOrbitalPi / 100) {
        orb_real_t value = computePLM(theta, ell, m, coeff);
        maxValue = std::max(maxValue, std::abs(value));
    }
    for (int iM = 0; iM <= ell; iM++)
        coeff[iM] /= maxValue;
}

orb_real_t computePLM(orb_real_t theta, int ell, int m, const orb_real_t* coeff) {
    orb_real_t u = std::cos(theta);
    int absM = m < 0 ? -m : m;
    orb_real_t sum = orb_real_t(0);
    orb_real_t uPowJ;
    if ((ell - absM) % 2 == 0)
        uPowJ = orb_real_t(1);
    else
        uPowJ = u;
    orb_real_t u2 = u * u;
    for (int j = (ell - absM) % 2; j <= ell - absM; j += 2, uPowJ *= u2)
        sum += coeff[j] * uPowJ;
    return sum * std::pow(std::sin(theta), orb_real_t(absM));
}

void buildLegendreTable(int ell, int m, orb_real_t* table, int n) {
    orb_real_t coeff[kOrbitalEllMax + 1];
    legendreCoeffs(ell, m, coeff);
    for (int i = 0; i < n; i++) {
        orb_real_t theta = kOrbitalPi * orb_real_t(i) / orb_real_t(n - 1);
        table[i] = computePLM(theta, ell, m, coeff);
    }
}

void laguerreCoeffs(int n, int ell, orb_real_t* coeff) {
    for (int k = 0; k < kOrbitalNMax; k++)
        coeff[k] = orb_real_t(0);

    int nClamped = n < kOrbitalNMax ? n : kOrbitalNMax;
    int ellClamped = ell < nClamped - 1 ? ell : nClamped - 1;
    if (ellClamped < 0)
        ellClamped = 0;
    int degree = nClamped - ellClamped;

    coeff[0] = orb_real_t(1);
    for (int k = 0; k + 1 < degree; k++) {
        orb_real_t kk = orb_real_t(k);
        coeff[k + 1] = -orb_real_t(2) * (orb_real_t(1) - (orb_real_t(ellClamped) + kk + orb_real_t(1)) / orb_real_t(nClamped)) /
                       (kk + orb_real_t(1)) / orb_real_t(2 * ellClamped + k + 2) * coeff[k];
    }
}

orb_real_t hydrogenRadialFunction(orb_real_t r, int n, int ell, const orb_real_t* coeff) {
    orb_real_t result = coeff[0];
    orb_real_t p = orb_real_t(1);
    for (int k = 1; k < n - ell; k++) {
        p = p * r;
        result += p * coeff[k];
    }
    result *= std::pow(r, orb_real_t(ell)) * std::exp(-r / orb_real_t(n));
    return result;
}

void buildRadialTable(int n, int ell, orb_real_t* table, int tableSize, orb_real_t* maxROut) {
    orb_real_t coeff[kOrbitalNMax];
    laguerreCoeffs(n, ell, coeff);
    orb_real_t maxR = orb_real_t(6 * n * n);
    orb_real_t deltaR = maxR / orb_real_t(tableSize - 1);
    for (int i = 0; i < tableSize; i++) {
        orb_real_t r = orb_real_t(i) * deltaR;
        table[i] = hydrogenRadialFunction(r, n, ell, coeff);
    }
    if (maxROut)
        *maxROut = maxR;
}

orb_real_t getValueFromLookupTable(orb_real_t x, const orb_real_t* table, int n) {
    orb_real_t iFloat = x * orb_real_t(n - 1);
    iFloat = std::max(orb_real_t(0), iFloat);
    int i = int(std::floor(iFloat));
    orb_real_t eta = iFloat - orb_real_t(i);
    if (i < n - 1)
        return table[i] * (orb_real_t(1) - eta) + table[i + 1] * eta;
    else
        return table[n - 1];
}

orb_real_t psiReal(orb_real_t r, orb_real_t theta, orb_real_t phi, int n, int ell, int m,
                    const orb_real_t* radialCoeff, const orb_real_t* legendreCoeffArr) {
    orb_real_t R = hydrogenRadialFunction(r, n, ell, radialCoeff);
    orb_real_t P = computePLM(theta, ell, m, legendreCoeffArr);
    orb_real_t azimuthal = (m >= 0) ? std::cos(orb_real_t(m) * phi) : std::sin(orb_real_t(-m) * phi);
    return R * P * azimuthal;
}
