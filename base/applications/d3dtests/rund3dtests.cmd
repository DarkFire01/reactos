@echo off
REM PROJECT:     ReactOS DirectX test applications
REM LICENSE:     MIT (https://spdx.org/licenses/MIT)
REM PURPOSE:     Run the whole d3dtest_* set and summarise the results
REM
REM Usage:  rund3dtests            run everything
REM         rund3dtests ddraw      run only the tests whose name contains "ddraw"
REM
REM Each test is a separate process, so one crashing says nothing about the
REM rest -- the run continues and the crash is reported as a failure.

setlocal enabledelayedexpansion

set FILTER=%1
set PASSED=0
set FAILED=0
set MISSING=0
set FAILLIST=

set TESTS=^
  d3d10_1_device d3d10_blob d3d10_buffer d3d10_device d3d10_draw^
  d3d10_query d3d10_shader d3d10_state d3d10_texture d3d10vis_alpha^
  d3d10vis_cube d3d10vis_cubetex d3d10vis_gradient d3d10vis_model^
  d3d10vis_plasma d3d10vis_rings d3d10vis_rtt d3d11_blend d3d11_buffer^
  d3d11_compute d3d11_constantbuffer d3d11_copy d3d11_deferred d3d11_depth^
  d3d11_device d3d11_draw d3d11_featurelevel d3d11_indexbuffer^
  d3d11_inputlayout d3d11_mipmap d3d11_query d3d11_shader d3d11_state^
  d3d11_texture d3d11vis_alpha d3d11vis_compute d3d11vis_cube^
  d3d11vis_cubetex d3d11vis_depth d3d11vis_gradient d3d11vis_instanced^
  d3d11vis_model d3d11vis_plasma d3d11vis_rings d3d11vis_rtt^
  d3d7_clipplane d3d7_device d3d7_draw d3d7_enum d3d7_light^
  d3d7_renderstate d3d7_texture d3d7_texturestage d3d7_vertexbuffer^
  d3d7_viewport d3d7_zbuffer d3d7vis_alpha d3d7vis_cube d3d7vis_cubetex^
  d3d7vis_fog d3d7vis_gouraud d3d7vis_lit d3d7vis_model d3d7vis_multitex^
  d3d8_caps d3d8_clear d3d8_cubetexture d3d8_depthstencil d3d8_device^
  d3d8_enum d3d8_indexbuffer d3d8_rendertarget d3d8_stateblock^
  d3d8_texture d3d8_vertexbuffer d3d8vis_alpha d3d8vis_buffers^
  d3d8vis_cube d3d8vis_cubetex d3d8vis_fog d3d8vis_lit d3d8vis_model^
  d3d8vis_multitex d3d8vis_rtt d3d8vis_stencil d3d8vis_wireframe^
  d3d9_blend d3d9_caps d3d9_clear d3d9_clipplane d3d9_cubetexture^
  d3d9_depthstencil d3d9_device d3d9_enum d3d9_gamma d3d9_indexbuffer^
  d3d9_light d3d9_multisample d3d9_query d3d9_rendertarget d3d9_reset^
  d3d9_scissor d3d9_shader d3d9_stateblock d3d9_streamfreq d3d9_surface^
  d3d9_swapchain d3d9_texture d3d9_texturestage d3d9_vertexbuffer^
  d3d9_vertexdecl d3d9_viewport d3d9vis_alpha d3d9vis_buffers d3d9vis_cube^
  d3d9vis_cubemap d3d9vis_cubetex d3d9vis_depth d3d9vis_fog d3d9vis_lit^
  d3d9vis_mipmap d3d9vis_model d3d9vis_multitex d3d9vis_pointsprite^
  d3d9vis_rtt d3d9vis_shader d3d9vis_stencil d3dcompiler_compile^
  d3dcompiler_disasm d3dcompiler_include d3dcompiler_preprocess^
  d3dcompiler_reflect d3dcompiler_signature d3dcompiler_targets^
  ddraw_attach ddraw_blt ddraw_caps ddraw_clipper ddraw_colorkey^
  ddraw_cooplevel ddraw_enum ddraw_flip ddraw_gdi ddraw_lock ddraw_modes^
  ddraw_palette ddraw_refcount ddraw_stretch ddraw_surface ddraw_zbuffer^
  ddrawvis_bounce ddrawvis_checker ddrawvis_colorkey ddrawvis_fire^
  ddrawvis_gdi ddrawvis_plasma ddrawvis_sprite ddrawvis_stretch^
  ddrawvis_tunnel dxgi_adapter dxgi_factory dxgi_format dxgi_output^
  dxgi_resize dxgi_swapchain dxgi_windowassoc

for %%T in (%TESTS%) do (
    set RUN=1
    if not "%FILTER%"=="" (
        echo %%T | find "%FILTER%" >nul || set RUN=0
    )
    if "!RUN!"=="1" (
        if exist "%SystemRoot%\system32\d3dtest_%%T.exe" (
            d3dtest_%%T.exe
            if errorlevel 1 (
                set /a FAILED+=1
                set FAILLIST=!FAILLIST! %%T
            ) else (
                set /a PASSED+=1
            )
        ) else (
            echo MISSING: d3dtest_%%T.exe is not installed
            set /a MISSING+=1
        )
        echo.
    )
)

echo ============================================================
echo  d3dtests summary: %PASSED% passed, %FAILED% failed, %MISSING% missing
if not "%FAILLIST%"=="" echo  failing: %FAILLIST%
echo ============================================================

endlocal
