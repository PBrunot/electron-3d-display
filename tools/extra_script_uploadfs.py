# extra_script_uploadfs.py
#
# Chains `pio run -t uploadfs` onto the normal `pio run -t upload` so a plain
# firmware flash always also deploys the data/ directory's contents (e.g.
# hfs_tables.bin, orbital_samplers.bin) to the "storage" SPIFFS partition
# (partitions_16M.csv) -- without this, a separate manual step (`pio run -t
# uploadfs`) is easy to forget after regenerating one of those files
# (tools/hfs_table_gen.py, tools/orbital_table_gen.py), silently leaving the
# device running against stale on-demand tables (src/hfs_radial.cpp,
# src/orbital_library.cpp read them from that partition, see each file's
# header comment).
#
# The "storage" partition also holds on-device screenshots (screenshot.cpp);
# uploadfs REFORMATS the whole partition from the `data/` directory's
# contents, so every flash now also wipes any screenshots captured since the
# last one. Accepted tradeoff -- screenshots are a debug/pull-and-discard
# feature (see pc/pull_screenshots.py), not data meant to survive a reflash.
Import("env")


def after_upload(source, target, env):
    env.Execute('"$PYTHONEXE" -m platformio run -t uploadfs -e ' + env["PIOENV"])


env.AddPostAction("upload", after_upload)
