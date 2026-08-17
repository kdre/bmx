BMX CRT PRESETS
===============

CRT presets are read-only in BMX. BMX never creates or modifies files in this
directory.

To create a preset:

1. Copy Default.crt to a new file in this directory.
2. Keep the .crt extension.
3. Edit the integer values with a text editor.
4. Keep every setting in the file. The loader rejects incomplete presets.

The file name without .crt is shown in the CRT Shader menu. For example,
Commodore 1702.crt is displayed as Commodore 1702.

Values outside their menu range are accepted but clamped. For example, a value
of 200 for a 0..100 setting becomes 100. Invalid numbers, duplicate settings,
missing settings, and unsupported format versions cause the entire preset to
be rejected; the currently active CRT settings remain unchanged.

The global Enable CRT Shader switch and Scaling Interpolation are intentionally
not part of a preset.
