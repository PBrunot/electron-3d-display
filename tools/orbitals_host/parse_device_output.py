#!/usr/bin/env python3
"""Split the flat directory of CSVs pulled back from the device's /out_dev/
(via `mpremote fs cp -r`) into the usual wave/points directory split, so
compare.py can use them exactly like out/js, out/c_f64, out/mpy, etc.

Usage:
    python3 parse_device_output.py <pulled_dir> <wave_out_dir> <points_out_dir>
"""
import os
import shutil
import sys

pulled_dir = sys.argv[1]
wave_dir = sys.argv[2]
points_dir = sys.argv[3]
os.makedirs(wave_dir, exist_ok=True)
os.makedirs(points_dir, exist_ok=True)

n_wave = 0
n_points = 0
for name in os.listdir(pulled_dir):
    if not name.endswith('.csv'):
        continue
    src = os.path.join(pulled_dir, name)
    if name.endswith('_points.csv'):
        shutil.copyfile(src, os.path.join(points_dir, name))
        n_points += 1
    else:
        shutil.copyfile(src, os.path.join(wave_dir, name))
        n_wave += 1

print('parse_device_output: split {} wave files into {}, {} points files into {}'.format(
    n_wave, wave_dir, n_points, points_dir))
