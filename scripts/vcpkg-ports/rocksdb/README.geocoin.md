# GeoCoin RocksDB Overlay

Copied unchanged from vcpkg baseline
`3855d4d6b52353ee2e2f73044327e59d01a18c84`, except for enabling
`WITH_WINDOWS_UTF8_FILENAMES` and normalizing internal slash separators before
the UTF-8 Windows wide-file API boundary. Extended-length Windows paths do not
normalize forward slashes themselves. RocksDB otherwise uses ANSI paths on Windows,
while GeoCoin passes UTF-8 paths from Qt. Source version, hashes, patches,
compression options and static CRT/linkage policy remain pinned upstream.
