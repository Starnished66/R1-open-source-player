# Repository toolchain cache

`mipsel-linux-musl-cross.tar.xz` is the compressed MIPS cross-toolchain used
by GitHub Actions. Keeping the archive in the repository makes scheduled
builds independent of the availability of the upstream download server.

The archive contains GCC 11.2.1 targeting `mipsel-linux-musl`. Its SHA-256 is:

```text
02e6ed140f2df1a937bb3e5bd94dda96de3c9576ce363da34f989abf0aedfc94
```

The checksum is also pinned by `scripts/install_mips_toolchain.sh`; update both
values whenever the archived toolchain is intentionally replaced.
