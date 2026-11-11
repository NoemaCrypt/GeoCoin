#!/usr/bin/env python3
"""Load the packaged C SDK as an external client, without the build tree."""

import argparse
import ctypes
import os
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", type=Path)
    args = parser.parse_args()
    bundle = args.bundle.resolve()
    directory = bundle / ("bin" if os.name == "nt" else "lib")
    dll_directory = os.add_dll_directory(str(directory)) if os.name == "nt" else None
    try:
        sdk = ctypes.CDLL(str(directory / ("geo.dll" if os.name == "nt" else "libgeo.so")))
        sdk.geo_api_version.restype = ctypes.c_uint32
        if sdk.geo_api_version() != 1:
            raise RuntimeError("Unexpected C SDK API version")
        sdk.geo_tx_new.argtypes = [ctypes.POINTER(ctypes.c_void_p)]
        sdk.geo_tx_new.restype = ctypes.c_int
        sdk.geo_tx_free.argtypes = [ctypes.c_void_p]
        sdk.geo_tx_free.restype = None
        transaction = ctypes.c_void_p()
        if sdk.geo_tx_new(ctypes.byref(transaction)) != 0 or not transaction.value:
            raise RuntimeError("Packaged C SDK cannot create a transaction")
        sdk.geo_tx_free(transaction)
        print("Packaged C SDK: API v1 and transaction allocation passed")
    finally:
        if dll_directory is not None:
            dll_directory.close()


if __name__ == "__main__":
    main()
