# Mesa Probe Shaders

These GLSL ES 1.00 shaders are intentionally small. They are not the final BMX
CRT shader. Their purpose is to drive the Linux Mesa GLES compiler through the
same kinds of inputs the Pi5 backend needs later:

- one source texture
- output-size dependent fragment math
- a scanline-style parameter block

Use `tools/pi5/mesa-v3d-probe/run_probe.sh` on Raspberry Pi OS or another Linux
system with a real Mesa V3D driver. The probe records renderer identity, shader
compile/link logs, a tiny rendered image, and whether Mesa exposes an OpenGL ES
program binary for the linked shader.
