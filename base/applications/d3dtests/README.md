# DirectX test applications

166 standalone console programs that exercise the DirectX stack imported from
Wine. Each one is a separate executable so a crash or hang in one says nothing
about the others, and so you can run just the area you are working on.

They are installed to `reactos/system32`, named `d3dtest_<area>_<feature>.exe`.

There are two kinds. 104 of them poke at the API and check what comes back. The
other 62 are named `<area>vis_<scene>` and actually draw something -- textured
cubes, a lit model, fog, blending, render-to-texture, plasma -- into a visible
window. Those still verify their own output, so the whole set runs unattended.

## Running them

Run one:

    d3dtest_ddraw_blt

Run everything, with a summary at the end:

    rund3dtests

Run one area:

    rund3dtests d3d9

Watch a visual test instead of just letting it verify itself -- `-hold` keeps
the window up when the animation ends, until you press a key:

    d3dtest_d3d9vis_cubetex -hold

## Reading the output

Every program prints one line per check and a summary, then exits 0 if nothing
failed:

    === d3d9_texture ===
    PASS: created a 64x64 mipmapped A8R8G8B8 texture
    PASS: texture has 7 level(s), expected 7 for 64x64
    SKIP: no occlusion query support (0x8876086a)
    info: back buffer format 21, usage 0x00000001
    --- d3d9_texture: 13 checks, 0 failed, 1 skipped ---

`SKIP` matters. These tests run on machines with no 3D driver at all, where
most of the D3D8/9/10/11 surface simply cannot be reached. A skip means the
test could not exercise something and said so, rather than pretending it
passed. A run that is all skips is not a pass -- it means the adapter never
came up, and `ddraw_caps` / `d3d9_enum` will tell you why.

`FAIL` is always a real disagreement with what the API is documented to do.

`info:` lines record behaviour that is real but not worth asserting, usually
because the retail runtime does not enforce what the documentation says.

## Validated against Windows

These are ordinary PE console programs, so they also run on Windows against
Microsoft's DirectX. That is worth doing before trusting a ReactOS result: if a
test fails on Windows, the test is wrong, not the implementation.

Reference run (Windows 11, NVIDIA GeForce GTX 1070, 2026-07-27):

    165 of 165 run, 0 failed, 1104 checks, 0 skipped

`ddraw_flip` is excluded from that figure -- it takes exclusive fullscreen and
changes the display mode, so it is not something to run unattended.

Keep that baseline in mind when reading a ReactOS run. Zero skips is what a
working 3D stack looks like; a column of SKIP lines means the adapter never came
up.

That reference run has already earned its keep. It caught a buffer over-read in
`d3d11_mipmap` (a NULL box makes `UpdateSubresource` read the whole
subresource, not one row), and a `D3DFMT_UNKNOWN` back buffer format that d3d9
accepts but d3d8 rejects -- which had been silently skipping every d3d8 device
test. It also showed that six assertions were simply wrong, because the retail
runtime returns `S_OK` where the documentation says it should fail: drawing
outside `BeginScene` (D3D7), `Clear` with a zero rect count and a non-NULL
pointer (D3D9), an unterminated `#if` in `D3DPreprocess`, `SetClipPlane` past
the advertised maximum, a viewport larger than the render target, and
`SetDisplayMode` at NORMAL cooperative level. Those calls are still made -- a
crash would still be caught -- but the result is recorded rather than asserted.

The visual set turned up its own crop of test bugs, all of them worth knowing
before writing another one:

- A 10.0 feature level only has to support `DXGI_FORMAT_B8G8R8A8_UNORM` for the
  swap chain, **not** as a shader resource -- that arrived with 10.1. Uploading
  a BGRA texture fails outright on real hardware. `d10_make_texture` retries as
  `R8G8B8A8_UNORM` and swaps the channels by hand.
- A fog test that cleared the frame to the fog colour could not tell "drew
  nothing" from "fogged out completely". Never clear to a colour the subject
  can also take.
- The readback helpers sample 256 pixels on a *fixed* pattern. That is not a
  random sample: when the subject is thin (wireframe edges) or small (a cube 20
  units out covers about half a percent of the frame), the pattern misses it on
  every single run, not just occasionally. Those scenes read a contiguous run of
  pixels instead -- a centre *column* rather than a row, because the camera sits
  above the origin looking down at it, so anything translated away in +z climbs
  toward the horizon and leaves the middle row.
- Checking the on-screen frame cannot tell a working render-to-texture from one
  that handed back a uniformly cleared surface, since the cube is covered either
  way. The `*vis_rtt` tests read the offscreen target back directly.

Both toolchains are covered: the whole set builds and passes under RosBE GCC and
under MSVC. That is worth repeating after any change to the CRT maths, since
every one of these does perspective and rotation arithmetic.

## What is covered

| Area | Tests | Notable checks |
|---|---|---|
| DirectDraw | 25 | driver and mode enumeration, caps, surfaces, Lock/Unlock pixel round-trip, colour fill and blits with readback, stretched and mirrored blits, palettes, clippers, fullscreen flip chains, source colour keying, GDI interop through GetDC/ReleaseDC, cooperative level transitions, attached surfaces and mip chains, z-buffer formats, COM refcounting and interface identity |
| Direct3D 7 | 19 | device enumeration, device and viewport, DrawPrimitive, texture formats and stage binding, render state and transform round-trips, depth state, lights and materials, vertex buffers, viewport validation, texture stage independence, user clip planes |
| Direct3D 8 | 22 | adapter and mode enumeration, caps, device and back buffer, Clear/Present, vertex and index buffers, textures, state blocks, automatic depth-stencil, render target switching, cube and volume textures |
| Direct3D 9 | 41 | all of the above plus render-to-texture with readback, shader objects and constants, vertex declarations, queries, device Reset with pool survival, additional swap chains, depth-stencil and stencil state, alpha blending verified by readback, scissor clipping verified by readback, cube and volume textures, lights and materials, plain surfaces and GetDC, viewport-bounded clears, multisample type checking, texture stage and sampler state, stream frequency and instancing, gamma ramps, user clip planes |
| DXGI | 7 | factory and adapter enumeration, adapter memory and LUID, output and display mode lists, swap chain creation and Present, buffer resizing including the outstanding-reference rule, format support across nine formats, window association |
| Direct3D 10 | 17 | device and format support, buffers, 2D textures and views, state objects, blobs, HLSL shaders, a full draw with pixel readback, queries, and a 10.1 device with feature-level walk and `CreateBlendState1` |
| Direct3D 11 | 28 | device and feature levels, buffers with Map/Unmap, textures with staging readback, state objects, HLSL shaders, input layout signature validation, a full triangle draw checked pixel by pixel, queries, constant buffers feeding a shader, index buffers, alpha blending, compute shaders writing a UAV read back element by element, UpdateSubresource and CopySubresourceRegion at an offset, deferred contexts and command lists, mip chain generation, depth testing where the near quad must win |
| d3dcompiler | 7 | HLSL compilation and error blobs, reflection over a constant buffer, disassembly and blob parts, the preprocessor with caller macros, `#include` through a callback, eight shader model targets plus optimisation and debug flags, input and output signature blobs |

Many tests verify results rather than only checking return codes. `ddraw_lock`,
`ddraw_blt` and `ddraw_stretch` read pixels back; `ddraw_gdi` paints with GDI
and reads the result through a DirectDraw lock; `d3d9_blend`, `d3d9_scissor` and
`d3d9_viewport` check the rendered pixels; `d3d11_draw`, `d3d11_depth`,
`d3d11_blend` and `d3d11_constantbuffer` render and inspect the framebuffer;
`d3d11_compute` reads back every element a compute shader wrote; and
`d3d11_deferred` proves a command list does nothing until it is executed.

Negative cases are checked too, because silently accepting an invalid call is
its own kind of bug: `Clear(ZBUFFER)` with no depth buffer, mapping a
`DEFAULT`-pool D3D11 buffer, an input layout missing a semantic the shader
consumes, a render target view over a texture not bound as one, a dynamic
buffer with no CPU access, `ResizeBuffers` with an outstanding back buffer
reference, an invalid shader profile, and junk passed to the shader creation
entry points.

## The visual set

The 62 `<area>vis_<scene>` programs render for 45 frames and then read the
framebuffer back, asserting that something other than the clear colour ended up
there and that the frame holds more than one colour. They are the ones that
catch a stack which returns `S_OK` from everything and draws nothing.

| Area | Scenes | What they draw |
|---|---|---|
| DirectDraw | 9 | a plasma field, a checkerboard, a moving sprite, a stretched blit, GDI drawing through `GetDC`, a tunnel effect, a fire effect, source colour keying, bouncing sprites |
| Direct3D 7 | 8 | a rotating cube, a textured cube, the octahedron model, gouraud shading, linear fog, alpha blending, a directional light with a material, two texture stages |
| Direct3D 8 | 11 | cube, textured cube, model, alpha, vertex and index buffers, fog, wireframe fill, a directional light, two texture stages, render-to-texture, stencil masking |
| Direct3D 9 | 15 | cube, textured cube, model, a shader-driven scene, render-to-texture, depth, mipmaps, alpha, buffers, multitexturing, stencil, point sprites, fog, lighting, a cube map with reflection coordinates |
| Direct3D 10 | 8 | cube, textured cube, model, rings, a gradient texture, alpha blending, render-to-texture, an animated plasma pixel shader |
| Direct3D 11 | 11 | cube, textured cube, model, rings, gradient, alpha blending, render-to-texture, instanced drawing, depth testing, a compute shader writing an image through a UAV, an animated plasma pixel shader |

The geometry, matrix maths and procedural textures are shared across all six
APIs by `d3dvis.h`, so the same cube and the same checkerboard go down the
DirectDraw path and the D3D11 path. When one API's picture differs from the
others', the difference is in the stack, not the test.

## Adding a test

Drop a `.c` file in this directory, include `d3dtest.h`, and add one line to
`CMakeLists.txt`:

    add_d3dtest(d3dtest_<name> <source>.c <import libs...>)

The harness gives you `test_begin`, `ok_`, `skip_`, `info_`, `test_end`, a
window helper and a message pump. Add the test name to the `TESTS` list in
`rund3dtests.cmd` so the driver picks it up.

For a drawing test, include `d3dvis.h` instead and name it `<area>vis_<scene>`.
That adds `vis_create_window`, the `vis_frame` animation loop, `-hold` handling
through `vis_parse_args`, the shared cube and model geometry, the procedural
texture generators, row-vector matrix maths, and `vis_check_rendered`. Copy the
private scene helpers (`d9_open`, `d9_sample` and friends) from the nearest
sibling test -- each file carries its own copy on purpose, so that changing one
scene cannot break another.

Two things to know when writing one: the harness defines `COBJMACROS`, so COM
interfaces are called as `IFoo_Method(obj, ...)` -- except those declared with
`DECLARE_INTERFACE_` (such as `ID3D11ShaderReflection`), which have no such
macros and must go through `->lpVtbl->`. And any static helper a test might not
use needs `D3DTEST_UNUSED`, which for a pointer-returning function has to sit
before the return type or GCC ignores it.
