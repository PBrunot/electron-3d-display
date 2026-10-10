// Run with: node web/shell_filling_model.test.js
const assert = require('node:assert/strict');
const {configuration} = require('./shell_filling_model.js');

for (let count = 0; count <= 20; ++count) {
    const state = configuration(count);
    assert.equal(state.reduce((sum, s) => sum + s.occupancy, 0), count);
    for (const subshell of state) {
        assert.equal(subshell.boxes.join('').length, subshell.occupancy);
        assert(subshell.boxes.every(box => ['', '↑', '↑↓'].includes(box)));
        if (subshell.boxes.some(box => box === '↑↓')) {
            assert(subshell.boxes.every(box => box.length >= 1), 'Hund: no pairing with empty orbitals');
        }
    }
    if (count > 0) {
        const before = configuration(count - 1);
        assert.equal(state.filter((s, i) => s.occupancy !== before[i].occupancy).length, 1);
    }
}
assert.deepEqual(configuration(7)[2].boxes, ['↑', '↑', '↑']);
assert.deepEqual(configuration(8)[2].boxes, ['↑↓', '↑', '↑']);
assert.deepEqual(configuration(10)[2].boxes, ['↑↓', '↑↓', '↑↓']);
assert.deepEqual(configuration(20).map(s => s.occupancy), [2, 2, 6, 2, 6, 2]);
for (const invalid of [-1, 21, 1.5, NaN, '8']) {
    assert.throws(() => configuration(invalid), RangeError);
}
console.log('Shell filling: conservation, Hund, Pauli, H–Ca order and bounds passed.');
