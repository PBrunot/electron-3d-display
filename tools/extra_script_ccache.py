# extra_script_ccache.py
import os

Import("env")


def before_build(env):
    # Enable ccache for ESP-IDF builds
    os.environ["CCACHE_ENABLE"] = "1"
    os.environ["CCACHE_DIR"] = os.path.expanduser("~/.ccache")
    os.environ["IDF_CCACHE_ENABLE"] = "1"


env.AddPreAction("build", before_build)
