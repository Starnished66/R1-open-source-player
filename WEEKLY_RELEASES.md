# Weekly Beta Release Setup

The `Weekly staging beta` GitHub Actions workflow builds the player and
bootloader every Sunday at 00:47 Costa Rica time, repacks the approved Staging
Image, and publishes a dated prerelease containing:

- `r1.upt`
- `plugins_examples.zip`
- `SHA256SUMS`

Release notes use the current [WHATS_NEW.md](WHATS_NEW.md) contents and link to
that file at the exact source commit used for the build.

## One-time repository setup

1. In GitHub, create a **draft** release with the tag `staging-image-base`.
2. Upload the approved Staging Image as an asset named exactly `r1.upt`.
3. Add a repository Actions secret named `STAGING_IMAGE_SHA256` containing the
   Staging Image's lowercase SHA-256. The workflow rejects a missing, malformed,
   or mismatched value rather than publishing an incomplete firmware. Neither
   this checksum nor the image itself is committed to the public repository.
   Keeping the release as a draft prevents the Staging Image from appearing on
   the public Releases page.
4. Under **Settings > Actions > General > Workflow permissions**, allow
   **Read and write permissions** so `GITHUB_TOKEN` can create prereleases.
5. Push `.github/workflows/weekly-beta.yml` to the default branch. Use its
   **Run workflow** button once to verify the setup before the first Sunday.

## Ongoing maintenance

- Update `WHATS_NEW.md` whenever a user-visible change is made. Its contents
  automatically become the next weekly beta's release notes.
- Normal C or Lua changes need no base-image update; the workflow replaces the
  player and bootloader and packages the current plugin examples each run.
- If the Staging Image gains firmware-side changes outside those binaries (web
  resources, fonts, boot assets, wrapper scripts, and so on), replace the
  `staging-image-base` draft asset and update the `STAGING_IMAGE_SHA256`
  repository secret at the same time.
- A rerun on the same Sunday updates that day's existing prerelease and
  replaces its assets instead of creating a duplicate release.
