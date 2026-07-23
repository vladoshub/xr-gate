# License texts

`LICENSE` at the repository root applies to project-owned XR Gate code unless a
file or directory states otherwise. The files in this directory are retained
for third-party source, configuration, SDK, and binary redistribution.

The release packager also copies license bundles and source metadata generated
from the exact upstream checkouts used for a build into
`LICENSES/components/`. In particular:

- Basalt includes its upstream BSD-3-Clause license and vcpkg copyright files.
- Monado/Mercury includes the complete upstream `LICENSES/` directory from each
  pinned Monado checkout.
- ONNX Runtime includes its upstream `LICENSE` and `ThirdPartyNotices.txt`.
- OpenVR packages include the license from the exact SDK checkout.
- Optional Mercury model archives include the upstream BSL-1.0 text and source
  metadata.
- Optional xrizer packages include the upstream GPL license and Corresponding
  Source through the existing xrizer compliance flow.
- Debian/Ubuntu builds copy exact distro package copyright metadata for the
  header and library dependencies detected in the build environment into
  `LICENSES/system-packages/`.

Including a third-party license here does not relicense XR Gate as a whole.
