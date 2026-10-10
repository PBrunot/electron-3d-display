/* Neutral ground-state configurations H–Ca. Box order is conventional;
 * the three p orbitals are degenerate, not three different energy levels. */
(function (root) {
    const subshells = [
        {label: '1s', n: 1, orbitals: 1},
        {label: '2s', n: 2, orbitals: 1},
        {label: '2p', n: 2, orbitals: 3},
        {label: '3s', n: 3, orbitals: 1},
        {label: '3p', n: 3, orbitals: 3},
        {label: '4s', n: 4, orbitals: 1},
    ];
    const elements = 'H Hydrogen,He Helium,Li Lithium,Be Beryllium,B Boron,C Carbon,N Nitrogen,O Oxygen,F Fluorine,Ne Neon,Na Sodium,Mg Magnesium,Al Aluminium,Si Silicon,P Phosphorus,S Sulfur,Cl Chlorine,Ar Argon,K Potassium,Ca Calcium'
        .split(',').map(entry => entry.split(' '));

    function configuration(count) {
        if (!Number.isInteger(count) || count < 0 || count > 20) {
            throw new RangeError('Electron count must be an integer from 0 to 20');
        }
        let remaining = count;
        return subshells.map(subshell => {
            const occupancy = Math.min(remaining, 2 * subshell.orbitals);
            remaining -= occupancy;
            const boxes = Array.from({length: subshell.orbitals}, (_, index) =>
                (index < occupancy ? '↑' : '') +
                (index < occupancy - subshell.orbitals ? '↓' : ''));
            return {...subshell, occupancy, boxes};
        });
    }

    const api = {subshells, elements, configuration};
    if (typeof module !== 'undefined' && module.exports) module.exports = api;
    else root.ShellFilling = api;
})(globalThis);
