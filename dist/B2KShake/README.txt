B2K Shake - OFX plugins for VEGAS Pro
=====================================

Contains three effects:
  * B2K Shake               (Video FX)    camera shake: X/Y/Z(zoom)/Tilt, motion blur, edge wrap, RGB split
  * B2K Dissolve Shake      (Video FX)    animate "Dissolve Amount" 0 -> 1: the clip shakes up and
                                          dissolves to transparent (1 -> 0 to shake in)
  * B2K Dissolve Shake      (Transition)  both clips shake, the shake peaks in the middle while they dissolve

Requirements
------------
  * Windows 10/11, 64-bit
  * VEGAS Pro with OFX support (made and tested with VEGAS Pro 2027)
  * GPU rendering uses OpenCL (NVIDIA/AMD/Intel drivers). Without it, it renders on the CPU.

Install
-------
  1. Close VEGAS.
  2. Extract the whole ZIP to a folder (do not run anything from inside the ZIP).
  3. Double-click Install.bat and allow the administrator prompt.
     (If Windows SmartScreen shows "Windows protected your PC": More info -> Run anyway.)
  4. Start VEGAS. You will find the effects under:
       Video FX     > OFX > B2K
       Transitions  > OFX > B2K

  Manual install instead: copy the folder "B2KShake.ofx.bundle" to
       C:\Program Files\Common Files\OFX\Plugins\

Uninstall
---------
  Close VEGAS, double-click Uninstall.bat (or delete
  C:\Program Files\Common Files\OFX\Plugins\B2KShake.ofx.bundle).

Using B2K Dissolve Shake
------------------------
  Main controls: Dissolve Amount (Video FX only), Dissolve Speed, Style (Normal/Smooth/Jerky),
  Amplitude, Frequency, Motion Blur, Mo Blur Length, Seed, Wrap X/Y.
  More options in the collapsed groups (X/Y/Z/Tilt Shake, Channels, Crop Input, More).

  Curve editor in the preview (group "Curve Editor" > Show In Preview):
    yellow curve = shake strength over the dissolve, blue line = dissolve, white line = current position
    * red dot at the top:          move the peak (left/right, higher/lower)
    * red dot below the peak:      round or sharpen the peak
    * red dots at the bottom:      how softly the shake starts / ends
    * blue dots:                   where and how fast the dissolve happens
    * drag the yellow or blue curve itself up/down: exponential / logarithmic bend
  Turn "Show In Preview" off to hide the curves.

Notes
-----
  * The plugin writes a small diagnostics file to %TEMP%\B2KShake_host.txt (host/GPU info).
  * Projects that used an older test build called "ShakeFX" need the effect re-applied.
