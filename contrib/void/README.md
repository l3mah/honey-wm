# Void Linux packaging

`template` is an [xbps-src](https://github.com/void-linux/void-packages) package
template for w3ld. Void ships versioned wlroots packages (`wlroots0.20-devel`,
`wlroots0.19-devel`, …), so w3ld builds against a **pinned** wlroots with no
bundling.

## Build & install locally

```sh
git clone --depth 1 https://github.com/void-linux/void-packages
cd void-packages
./xbps-src binary-bootstrap                 # once
mkdir -p srcpkgs/w3ld
cp /path/to/w3ld-wm/contrib/void/template srcpkgs/w3ld/template
./xbps-src pkg w3ld
sudo xbps-install --repository=hostdir/binpkgs w3ld
```

## Version scheme

The major.minor tracks the wlroots ABI w3ld is pinned to; the patch is w3ld's
own release counter:

| w3ld version | wlroots       |
|--------------|---------------|
| 0.20.0       | 0.20 (0.20.x) |
| 0.20.1, .2…  | 0.20          |
| 0.21.0       | 0.21          |

So the version alone tells you which wlroots it needs. On each release: tag
`vX.Y.Z`, bump `version` here, and refresh `checksum` (sha256 of the release
tarball). Retargeting a new wlroots = bump major/minor and swap the
`wlroots0.XX-devel` makedepend.
