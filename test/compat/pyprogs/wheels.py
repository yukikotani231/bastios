"""Uses the binary wheels python.txt installs (numpy, cryptography, lxml,
pillow, PyYAML, greenlet, psutil, orjson, cffi): one line of output per
module."""
import os, sys

import numpy as np
a = np.arange(12.).reshape(3, 4)
print("numpy", np.__version__, (a @ a.T).sum(), round(float(np.linalg.det(np.eye(3) * 2)), 6), abs(np.fft.fft(np.arange(8))).sum().round(3), np.sort(np.array([3, 1, 2])).tolist())

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives.asymmetric import ec, rsa
c = Cipher(algorithms.AES(b"k" * 16), modes.CBC(b"i" * 16)).encryptor()
print("cryptography", (c.update(b"a" * 32) + c.finalize()).hex()[:16], ec.generate_private_key(ec.SECP256R1()).key_size, rsa.generate_private_key(65537, 2048).key_size)

from lxml import etree
print("lxml", etree.fromstring("<a><b>x</b></a>").find("b").text)

from PIL import Image
im = Image.new("RGB", (40, 30), "red")
im.save("/tmp/a.png")
print("pillow", Image.open("/tmp/a.png").size, Image.open("/tmp/a.png").getpixel((1, 1)))

import yaml
print("pyyaml", yaml.load("a: [1, 2]", Loader=yaml.CSafeLoader))

import greenlet
g = greenlet.greenlet(lambda: 5)
print("greenlet", g.switch())

import orjson
print("orjson", orjson.dumps({"a": [1, 2]}))

import cffi
ff = cffi.FFI()
ff.cdef("int abs(int);")
print("cffi", ff.dlopen(None).abs(-3))

import psutil
print("psutil", psutil.virtual_memory().total > 0, os.getpid() in psutil.pids(), psutil.Process().name().startswith("python"), psutil.Process().num_threads() >= 1)
