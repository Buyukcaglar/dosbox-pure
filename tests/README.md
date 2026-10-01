# Standalone filesystem regressions

`patch_utility_variant_test.cpp` uses synthetic stored ZIP archives with the real
memory-backed archive and patch-drive classes. It checks that a utility with no
files keeps the preceding game variant's files across one, two and three patch
layers, preserves utility metadata and overlay precedence, and respects utilities
with their own files and a return to the default root.

Build the standalone `ReleaseGLCORE|x64` configuration first. Run from PowerShell,
passing the matching build's object directory and ZillaLib library (adjust the
Visual Studio output-directory suffix for the installed toolset):

```powershell
./tests/Test-PatchUtilityVariant.ps1 `
  -RuntimeObjectsDirectory ../dosbox-pure-unleashed/Release-vs2026x64 `
  -ZillaLibPath ../ZillaLib/Release-vs2026x64/ZillaLib.lib
```

The runner links a separate console executable using those objects. CPU execution,
audio, video, and package extraction never start. `-PatchObject` can substitute a
separately compiled patch-drive object to verify that an intentionally restored
upstream loop defect fails the regression.

The VHD codec tests in `vhd_differencing_test.cpp` use the parent project's
`tools/Test-DifferencingVhd.ps1` runner.
