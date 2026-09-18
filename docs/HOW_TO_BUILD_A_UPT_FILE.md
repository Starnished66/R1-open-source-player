# How to build an R1 `.upt` firmware image

This guide explains how to build a complete HiBy R1 firmware update package
from:

- an approved R1 **Staging Image** supplied by the user;
- a newly built player binary; and
- a newly built bootloader binary.

The repository does not contain a redistributable stock firmware image. The
base image must therefore be obtained separately and must be the approved
Staging Image described below.

## What the script produces

[`scripts/repack_upt.sh`](../scripts/repack_upt.sh) creates an ISO 9660 `.upt`
file with the layout expected by the R1 recovery updater. It:

1. extracts the base image;
2. joins and unpacks its `rootfs.squashfs` and `xImage` chunks;
3. verifies that the base image's `usr/bin/hiby_player.sh` invokes
   `/usr/bin/open_hiby_bootloader`;
4. installs the supplied player and bootloader binaries;
5. copies the repository's git-tracked assets and fonts to their device paths,
   then every file under `firmware/overlay/` into the root filesystem;
6. recompresses the root filesystem with SquashFS;
7. splits the kernel and root filesystem into 512 KiB OTA chunks and writes
   the MD5 metadata and hash chains; and
8. creates the final ISO and rejects it if it is larger than 45 MiB
   (47,185,920 bytes).

The script uses a temporary working directory and does not modify the base
image or the source tree. The output path must be different from the base
image path.

## Requirements

Run the commands below from the repository root. A Linux host is assumed.

The repack step requires these commands:

```text
7z
unsquashfs
mksquashfs
genisoimage
md5sum
sha256sum
split
```

On Debian or Ubuntu, the non-standard tools can be installed with:

```sh
sudo apt-get install genisoimage p7zip-full squashfs-tools
```

`md5sum`, `sha256sum`, and `split` are provided by the usual GNU coreutils
package.

Building the binaries also requires the project build dependencies and the
MIPS musl cross-toolchain. See the build section in [`README.md`](../README.md)
for the toolchain setup.

## 1. Obtain a compatible base image

Use a complete R1 Staging Image, for example:

```text
/path/to/base_staging.upt
```

Do not use an arbitrary stock `.upt` file or an older public beta. The repack
script intentionally refuses a base image whose `hiby_player.sh` does not
contain the bootloader handoff. This check prevents creating an image that
contains the new bootloader binary but never starts it.

The base image contains proprietary HiBy files and is not supplied by this
repository. Keep it outside the repository if it is not already ignored.

## 2. Build the R1 binaries

Build the player and the bootloader for the R1. `r1` is the Makefile default,
but specifying it explicitly avoids accidentally packaging binaries for a
different board:

```sh
make target BOARD=r1
make bootloader BOARD=r1
```

The commands must produce these two non-empty files in the repository root:

```text
open_hiby_player_target
open_hiby_bootloader
```

The repack script installs them as:

```text
/usr/bin/open_hiby_player
/usr/bin/open_hiby_bootloader
```

Both files must be R1 MIPS binaries. Do not substitute the R3 Pro II build
outputs or a host executable.

## 3. Check what the repository contributes before packaging

Besides the two binary arguments, the repack script copies repository-managed
files from three places, after the binaries are installed:

| Repository path | Lands at | Holds |
| --- | --- | --- |
| `assets/theme2/` | `/usr/resource/litegui/theme2/` | UI icons |
| `assets/fonts/` | `/usr/resource/fonts/` | fonts |
| `firmware/overlay/` | `/` (root-relative) | non-asset files |

The two `assets/` trees mirror the device layout, so a file's path under
`assets/` is its path on the device minus the prefix above. `firmware/overlay/`
is root-filesystem-relative instead, and holds only what is not an asset --
currently `usr/bin/sync_ntp.sh` and a `udhcpc` NTP hook.

**Only files tracked in git are copied from the `assets/` trees.** The script
lists them with `git ls-files`, so "tracked in git" and "shipped on the device"
mean the same thing. That matters because `assets/theme2/` is where a
contributor also populates their own stock-firmware dump for local work (see
`.gitignore`): those files are ignored, so they are never packaged, and stock
assets come from the base image instead.

To see exactly what the repository will contribute:

```sh
git ls-files assets/theme2 assets/fonts
find firmware/overlay -type f -o -type l | sort
```

An asset that is present locally but untracked will silently not ship. That is
a real failure mode, not a hypothetical: it is how the Subsonic download icon
went missing from a release while appearing to be present in the source tree.

The copies happen after the binaries are installed, so do not place
`usr/bin/open_hiby_player` or `usr/bin/open_hiby_bootloader` under
`firmware/overlay/` unless overriding the command-line binaries is deliberate.

## 4. Create the `.upt` file

Invoke the script with exactly four arguments, in this order:

```sh
scripts/repack_upt.sh \
  /path/to/base_staging.upt \
  open_hiby_player_target \
  open_hiby_bootloader \
  output/r1-custom.upt
```

The output directory is created automatically. On success, the script prints
the output size and its SHA-256 checksum. A successful run also means that:

- the base image contained both `rootfs.squashfs` and `xImage` chunks;
- the base image passed the bootloader-handoff check;
- the root filesystem was recompressed successfully;
- the OTA chunk and MD5 metadata files were generated; and
- the final image passed the 45 MiB size limit.

If the command reports that the image is too large, do not flash it. Reduce
the contents of the root filesystem or overlay, then rebuild the image.

## 5. Inspect the generated package

The result should be a valid ISO archive containing `ota_config.in`,
`ota_v0/ota_update.in`, `ota_v0/ota_v0.ok`, the `xImage` chunks, the
`rootfs.squashfs` chunks, and one `ota_md5_*` file for each image.

Run these checks after building:

```sh
file output/r1-custom.upt
7z t output/r1-custom.upt
7z l output/r1-custom.upt | sed -n '1,40p'
```

`file` should identify the output as an ISO 9660 filesystem, and `7z t`
should report that the archive has no errors. `7z l` should show the
`ota_v0/` directory and the files listed above.

For a more detailed inspection, extract the package to a temporary directory
and review the metadata:

```sh
inspect_dir=$(mktemp -d)
7z x -y output/r1-custom.upt -o"$inspect_dir" >/dev/null
cat "$inspect_dir/ota_v0/ota_update.in"
cat "$inspect_dir/ota_config.in"
rm -rf "$inspect_dir"
```

`ota_update.in` should identify both `xImage` and `rootfs.squashfs`, including
each image's byte size and initial MD5. `ota_config.in` should contain:

```text
current_version=0
```

## Important limitations

This procedure creates the same package format used by the repository's R1
release workflow, but a successful build is not a substitute for hardware
testing. Before distributing or installing an image, verify it on an R1 and
keep a known-good recovery image available.

Changing only the player does not require a full `.upt` rebuild: the standalone
`open_hiby_player` update can be used when appropriate. Changes to the kernel,
root filesystem, bootloader, fonts, icons, scripts, or other packaged content
require a complete repack.
