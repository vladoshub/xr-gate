# Third-party notices for the XR Gate OpenVR driver

The source code in this directory is part of XR Gate and is licensed under the
MIT License in `LICENSE`, unless an individual file states otherwise.

The driver is built against Valve's OpenVR SDK:

- Upstream: https://github.com/ValveSoftware/openvr
- Default pinned ref used by `scripts/build_driver.sh`:
  `0924064316de3effbcd1acf1e309182a2deb1c05`
- License: BSD-3-Clause
- Baseline license text: `LICENSES/OpenVR-BSD-3-Clause.txt`

The OpenVR SDK source and headers are not embedded in this source archive. The
build script downloads or uses a separate SDK checkout, and the installed
binary driver package copies the exact license file from that checkout into its
own `licenses/` directory.
