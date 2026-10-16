#!/usr/bin/env python3
"""Copies Alpine Linux's root file system, the one layer of Docker Hub's
alpine image (a gzip-compressed tar file), out of the cache the compat suite
keeps (test/compat/alpinedisk.py), fetching it from Docker Hub when the
cache lacks it.

    alpineroot.py OUT.tgz IMAGE@sha256:DIGEST CACHE_DIR
"""
import os, shutil, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "test", "compat"))
import alpinedisk

if len(sys.argv) != 4:
    sys.exit(__doc__)
out, image, cache = sys.argv[1:]
os.makedirs(cache, exist_ok=True)
paths = alpinedisk.layers(image, cache)
if len(paths) != 1:
    sys.exit(f"alpineroot: {image} has {len(paths)} layers, and a root file system is one")
shutil.copyfile(paths[0], out)
