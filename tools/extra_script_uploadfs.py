# extra_script_uploadfs.py
#
# Chains `pio run -t uploadfs` onto the normal `pio run -t upload` so a plain
# firmware flash always also deploys data/hfs_tables.bin to the "storage"
# SPIFFS partition (partitions_16M.csv) -- previously a separate manual step
# (`pio run -t uploadfs`), easy to forget after regenerating
# data/hfs_tables.bin (tools/hfs_table_gen.py) and silently leaving the
# device running against stale radial tables (src/hfs_radial.cpp reads them
# on demand from that partition, see that file's header comment).
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
