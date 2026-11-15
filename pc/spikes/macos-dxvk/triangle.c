#include <SDL3/SDL.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <exception>
#include <util_error.h>
#include <d3d9.h>

#ifndef D3D_SDK_VERSION
#define D3D_SDK_VERSION 32
#endif

typedef IDirect3D9 * (WINAPI *Direct3DCreate9Fn)(UINT);
typedef struct Vertex { float x, y, z; DWORD color; } Vertex;

/* vs_3_0: copy v0 position to o0 and v1 color to o1. */
static const DWORD color_vs[] = {
  0xfffe0300,
  0x0200001f, 0x80000000, 0x900f0000, /* dcl_position v0 */
  0x0200001f, 0x8000000a, 0x900f0001, /* dcl_color0 v1 */
  0x0200001f, 0x80000000, 0x600f0000, /* dcl_position o0 */
  0x0200001f, 0x8000000a, 0x600f0001, /* dcl_color0 o1 */
  0x02000001, 0x600f0000, 0x90e40000, /* mov o0, v0 */
  0x02000001, 0x600f0001, 0x90e40001, /* mov o1, v1 */
  0x0000ffff
};

static const Vertex tri[] = {
  {  0.0f,  0.65f, 0.5f, 0xffff3030 },
  {  0.65f,-0.55f, 0.5f, 0xff30ff30 },
  { -0.65f,-0.55f, 0.5f, 0xff3030ff },
};

int main(void) {
  int result = 1;
  void *library = NULL;
  IDirect3D9 *d3d = NULL;
  IDirect3DDevice9 *dev = NULL;
  IDirect3DVertexShader9 *shader = NULL;
  SDL_Window *window = NULL;
  Direct3DCreate9Fn create9 = NULL;
  HRESULT hr = D3D_OK;
  if (!SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
  window = SDL_CreateWindow("DXVK Native D3D9 triangle", 800, 600, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
  if (!window) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); goto done; }
  const char *dylib_path = getenv("DXVK_D3D9_DYLIB");
  if (!dylib_path) { fprintf(stderr, "Set DXVK_D3D9_DYLIB to libdxvk_d3d9.0.dylib\n"); goto done; }
  library = dlopen(dylib_path, RTLD_NOW | RTLD_LOCAL);
  if (!library) { fprintf(stderr, "dlopen: %s\n", dlerror()); goto done; }
  create9 = (Direct3DCreate9Fn)dlsym(library, "Direct3DCreate9");
  if (!create9) { fprintf(stderr, "dlsym Direct3DCreate9: %s\n", dlerror()); goto done; }
  try { d3d = create9(D3D_SDK_VERSION); } catch (const dxvk::DxvkError& e) { fprintf(stderr, "Direct3DCreate9: %s\n", e.message().c_str()); goto done; }
  if (!d3d) { fprintf(stderr, "Direct3DCreate9 failed\n"); goto done; }

  D3DPRESENT_PARAMETERS pp;
  memset(&pp, 0, sizeof(pp));
  pp.BackBufferWidth = 800;
  pp.BackBufferHeight = 600;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.BackBufferCount = 1;
  pp.MultiSampleType = D3DMULTISAMPLE_NONE;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.hDeviceWindow = (HWND)window;
  pp.Windowed = TRUE;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  try { hr = d3d->CreateDevice( D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, (HWND)window,
      D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &dev); } catch (const dxvk::DxvkError& e) { fprintf(stderr, "CreateDevice: %s\n", e.message().c_str()); goto done; }
  if (FAILED(hr)) { fprintf(stderr, "CreateDevice: 0x%08x\n", (unsigned)hr); goto done; }
  hr = dev->CreateVertexShader( color_vs, &shader);
  if (FAILED(hr)) { fprintf(stderr, "CreateVertexShader: 0x%08x\n", (unsigned)hr); goto done; }

  for (int frame = 0; frame < 120; frame++) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) if (event.type == SDL_EVENT_QUIT) goto rendered;
    dev->Clear( 0, NULL, D3DCLEAR_TARGET, 0xff101820, 1.0f, 0);
    if (FAILED(dev->BeginScene())) goto done;
    dev->SetVertexShader( NULL);
    dev->SetFVF( D3DFVF_XYZ | D3DFVF_DIFFUSE);
    dev->DrawPrimitiveUP( D3DPT_TRIANGLELIST, 1, tri, sizeof(Vertex));
    dev->SetFVF( 0);
    dev->SetVertexShader( shader);
    dev->DrawPrimitiveUP( D3DPT_TRIANGLELIST, 1, tri, sizeof(Vertex));
    dev->SetVertexShader( NULL);
    dev->EndScene();
    hr = dev->Present( NULL, NULL, NULL, NULL);
    if (FAILED(hr)) { fprintf(stderr, "Present: 0x%08x\n", (unsigned)hr); goto done; }
    SDL_Delay(16);
  }
rendered:
  result = 0;
done:
  if (shader) shader->Release();
  if (dev) dev->Release();
  if (d3d) d3d->Release();
  if (library) dlclose(library);
  if (window) SDL_DestroyWindow(window);
  SDL_Quit();
  return result;
}
