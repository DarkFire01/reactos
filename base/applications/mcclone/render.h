/* GL state setup and per-frame rendering (sky, world, water, HUD crosshair). */
#ifndef RENDER_H
#define RENDER_H

#include <windows.h>

void        RenderInit(HDC hdc);          /* build atlas+mesh, set GL state */
void        RenderResize(int w, int h);
void        RenderFrame(void);            /* draws a frame and swaps buffers */
const char *RenderRendererName(void);     /* GL_RENDERER (filled at init)    */

#endif /* RENDER_H */
