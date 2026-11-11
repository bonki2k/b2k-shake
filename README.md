# B2K Shake

Free, open-source camera-shake effects for **VEGAS Pro** (OpenFX plugin, Windows 64-bit).

| Effect | Where in VEGAS | What it does |
|---|---|---|
| **B2K Shake** | Video FX › OFX › B2K | Camera shake on X / Y / Z (zoom) / Tilt with random + wave motion, motion blur, edge wrapping, RGB split, crop |
| **B2K Dissolve Shake** | Video FX › OFX › B2K | Animate *Dissolve Amount* 0 → 1: the clip shakes up and dissolves to transparent (1 → 0 to shake in) |
| **B2K Dissolve Shake** | Transitions › OFX › B2K | Both clips shake, the shake peaks in the middle while they dissolve |

* GPU rendering via OpenCL (VEGAS' OFX GPU path), CPU fallback, 8/16/32-bit
* Curve editor drawn in the VEGAS preview: drag the shake envelope and dissolve curve, bend them exponentially
* Single DLL, no runtime installers needed

## Download & install

1. Download `B2KShake_v1.0.zip` from **[Releases](../../releases)**.
2. Close VEGAS, extract the ZIP, run `Install.bat` (asks for administrator rights).
   Manual alternative: copy `B2KShake.ofx.bundle` to `C:\Program Files\Common Files\OFX\Plugins\`.
3. Start VEGAS.

To remove it, run `Uninstall.bat` or delete `C:\Program Files\Common Files\OFX\Plugins\B2KShake.ofx.bundle`.

## Dissolve Shake curve editor

Group *Curve Editor* › *Show In Preview* draws the curves over the preview:

* **yellow** – shake strength over the dissolve, **blue** – dissolve, **white** – current position
* red dot at the top: move the peak · red dot below it: round/sharpen the peak
* red dots at the bottom: soft start / end · blue dots: dissolve position and speed
* drag the yellow or blue curve itself up/down for exponential / logarithmic bends

## Build from source

Requirements: Visual Studio 2022 with *Desktop development with C++*, Git.

```bat
build.bat
```

`build.bat` fetches the OpenFX headers (BSD-3-Clause) on first run and produces
`B2KShake.ofx.bundle\Contents\Win64\B2KShake.ofx`. `install.ps1` copies the bundle into the OFX
plugin folder (run as administrator). `test\testhost.cpp` is a small mock OFX host used to test
rendering (CPU vs. GPU), parameters and the overlay without VEGAS.

## Notes

* The plugin writes a small diagnostics file to `%TEMP%\B2KShake_host.txt` (host and GPU capabilities).
* This is an independent project. It is not affiliated with Boris FX / Sapphire or VEGAS Creative Software.

## License

MIT, see [LICENSE](LICENSE). OpenFX headers © Contributors to the OpenFX Project, BSD-3-Clause.
