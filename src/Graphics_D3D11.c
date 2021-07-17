#include "Core.h"
#ifdef CC_BUILD_D3D11
#include "_GraphicsBase.h"
#include "Errors.h"
#include "Logger.h"
#include "Window.h"

/* Avoid pointless includes */
#define WIN32_LEAN_AND_MEAN
#define NOSERVICE
#define NOMCX
#define NOIME
#define COBJMACROS
#include <d3d11.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxguid.lib")

static int gfx_stride, gfx_format = -1;
static int depthBits;
static ID3D11Device* device;
static ID3D11DeviceContext* context;
static IDXGIDevice1* dxgi_device;
static IDXGIAdapter* dxgi_adapter;
static IDXGIFactory1* dxgi_factory;
static IDXGISwapChain* swapchain;
static ID3D11RenderTargetView* backbuffer;

void Gfx_Create(void) {
	DXGI_SWAP_CHAIN_DESC desc = { 0 };
	ID3D11Texture2D* pBackBuffer;
	DWORD createFlags = 0;
	D3D_FEATURE_LEVEL fl;
	HRESULT hr;

#ifdef _DEBUG
	createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

	desc.BufferCount = 1;
	desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.BufferDesc.RefreshRate.Numerator   = 60;
	desc.BufferDesc.RefreshRate.Denominator = 1;
	desc.BufferUsage  = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	desc.OutputWindow = WindowInfo.Handle;
	desc.SampleDesc.Count   = 1;
	desc.SampleDesc.Quality = 0;
	desc.Windowed           = TRUE;

	hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
			createFlags, NULL, 0, D3D11_SDK_VERSION,
			&desc, &swapchain, &device, &fl, &context);
	if (hr) Logger_Abort2(hr, "Failed to create D3D11 device");
	
	hr = IDXGISwapChain_GetBuffer(swapchain, 0, &IID_ID3D11Texture2D, (void**)&pBackBuffer);
	if (hr) Logger_Abort2(hr, "Failed to get DXGI buffer");

	hr = ID3D11Device_CreateRenderTargetView(device, pBackBuffer, NULL, &backbuffer);
	if (hr) Logger_Abort2(hr, "Failed to create render target");
	ID3D11Texture2D_Release(pBackBuffer);
	ID3D11DeviceContext_OMSetRenderTargets(context, 1, &backbuffer, NULL);

	D3D11_VIEWPORT viewport = { 0 };
	viewport.TopLeftX = 0;
	viewport.TopLeftY = 0;
	viewport.Width    = WindowInfo.Width;
	viewport.Height   = WindowInfo.Height;
	ID3D11DeviceContext_RSSetViewports(context, 1, &viewport);
}

cc_bool Gfx_TryRestoreContext(void) {
	return true;
}

void Gfx_Free(void) {
	ID3D11RenderTargetView_Release(backbuffer);
	IDXGISwapChain_Release(swapchain);
	ID3D11DeviceContext_Release(context);
	ID3D11Device_Release(device);
}

static void Gfx_FreeState(void) {
	FreeDefaultResources();
}

static void Gfx_RestoreState(void) {
	InitDefaultResources();
	gfx_format = -1;
}


/*########################################################################################################################*
*---------------------------------------------------------Textures--------------------------------------------------------*
*#########################################################################################################################*/
GfxResourceID Gfx_CreateTexture(struct Bitmap* bmp, cc_bool managedPool, cc_bool mipmaps) {
	return 0;
}

void Gfx_UpdateTexture(GfxResourceID texId, int x, int y, struct Bitmap* part, int rowWidth, cc_bool mipmaps) {
}

void Gfx_BindTexture(GfxResourceID texId) {
}

void Gfx_DeleteTexture(GfxResourceID* texId) { }

void Gfx_SetTexturing(cc_bool enabled) {
}

void Gfx_EnableMipmaps(void) {
}

void Gfx_DisableMipmaps(void) {
}


/*########################################################################################################################*
*-----------------------------------------------------State management----------------------------------------------------*
*#########################################################################################################################*/
static float gfx_clearColor[4];

void Gfx_SetFaceCulling(cc_bool enabled) {
}

void Gfx_SetFog(cc_bool enabled) {
}

void Gfx_SetFogCol(PackedCol col) {
}

void Gfx_SetFogDensity(float value) {
}

void Gfx_SetFogEnd(float value) {
}

void Gfx_SetFogMode(FogFunc func) {
}

void Gfx_SetAlphaTest(cc_bool enabled) {
}

void Gfx_SetAlphaBlending(cc_bool enabled) {
}

void Gfx_SetAlphaArgBlend(cc_bool enabled) {
}

void Gfx_ClearCol(PackedCol col) {
	gfx_clearColor[0] = PackedCol_R(col) / 255.0f;
	gfx_clearColor[1] = PackedCol_G(col) / 255.0f;
	gfx_clearColor[2] = PackedCol_B(col) / 255.0f;
	gfx_clearColor[3] = PackedCol_A(col) / 255.0f;
}

void Gfx_SetColWriteMask(cc_bool r, cc_bool g, cc_bool b, cc_bool a) {
}

void Gfx_SetDepthTest(cc_bool enabled) {
}

void Gfx_SetDepthWrite(cc_bool enabled) {
}


/*########################################################################################################################*
*-------------------------------------------------------Index buffers-----------------------------------------------------*
*#########################################################################################################################*/
GfxResourceID Gfx_CreateIb(void* indices, int indicesCount) {
	ID3D11Buffer* buffer = NULL;
	D3D11_BUFFER_DESC desc;
	D3D11_SUBRESOURCE_DATA data;
	HRESULT hr;

	desc.Usage     = D3D11_USAGE_DEFAULT;
	desc.ByteWidth = sizeof(cc_uint16) * indicesCount;
	desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
	desc.CPUAccessFlags = 0;
	desc.MiscFlags = 0;

	data.pSysMem          = indices;
	data.SysMemPitch      = 0;
	data.SysMemSlicePitch = 0;

	hr = ID3D11Device_CreateBuffer(device, &desc, &data, &buffer);
	if (hr) Logger_Abort2(hr, "Failed to create index buffer");
	return buffer;
}

void Gfx_BindIb(GfxResourceID ib) {
	ID3D11Buffer* buffer = (ID3D11Buffer*)ib;
	ID3D11DeviceContext_IASetIndexBuffer(context, buffer, DXGI_FORMAT_R16_UINT, 0);
}

void Gfx_DeleteIb(GfxResourceID* ib) {
	ID3D11Buffer* buffer = (ID3D11Buffer*)*ib;
	ID3D11Buffer_Release(buffer);
	*ib = NULL;
}


/*########################################################################################################################*
*------------------------------------------------------Vertex buffers-----------------------------------------------------*
*#########################################################################################################################*/
GfxResourceID Gfx_CreateVb(VertexFormat fmt, int count) {
	return 0;
}

void Gfx_BindVb(GfxResourceID vb) {
}

void Gfx_DeleteVb(GfxResourceID* vb) { }
static void* tmp;
void* Gfx_LockVb(GfxResourceID vb, VertexFormat fmt, int count) {
	tmp = Mem_TryAlloc(count, strideSizes[fmt]);
	return tmp;
}

void Gfx_UnlockVb(GfxResourceID vb) {
	Mem_Free(tmp);
	tmp = NULL;
}


void Gfx_SetVertexFormat(VertexFormat fmt) {
}

void Gfx_DrawVb_Lines(int verticesCount) {
}

void Gfx_DrawVb_IndexedTris(int verticesCount) {
}

void Gfx_DrawVb_IndexedTris_Range(int verticesCount, int startVertex) {
}

void Gfx_DrawIndexedTris_T2fC4b(int verticesCount, int startVertex) {
}


/*########################################################################################################################*
*--------------------------------------------------Dynamic vertex buffers-------------------------------------------------*
*#########################################################################################################################*/
GfxResourceID Gfx_CreateDynamicVb(VertexFormat fmt, int maxVertices) {
	return 0;
}

void* Gfx_LockDynamicVb(GfxResourceID vb, VertexFormat fmt, int count) {
	return Gfx_LockVb(vb, fmt, count);
}

void Gfx_UnlockDynamicVb(GfxResourceID vb) {
	Gfx_UnlockVb(vb);
}

void Gfx_SetDynamicVbData(GfxResourceID vb, void* vertices, int vCount) {
}


/*########################################################################################################################*
*---------------------------------------------------------Matrices--------------------------------------------------------*
*#########################################################################################################################*/
void Gfx_LoadMatrix(MatrixType type, const struct Matrix* matrix) {
}

void Gfx_LoadIdentityMatrix(MatrixType type) {
}

void Gfx_EnableTextureOffset(float x, float y) {
}

void Gfx_DisableTextureOffset(void) {
}

void Gfx_CalcOrthoMatrix(float width, float height, struct Matrix* matrix) {
	Matrix_Orthographic(matrix, 0.0f, width, 0.0f, height, ORTHO_NEAR, ORTHO_FAR);
	matrix->row3.Z = 1.0f       / (ORTHO_NEAR - ORTHO_FAR);
	matrix->row4.Z = ORTHO_NEAR / (ORTHO_NEAR - ORTHO_FAR);
}

static float CalcZNear(float fov) {
	/* With reversed z depth, near Z plane can be much closer (with sufficient depth buffer precision) */
	/*   This reduces clipping with high FOV without sacrificing depth precision for faraway objects */
	/*   However for low FOV, don't reduce near Z in order to gain a bit more depth precision */
	if (depthBits < 24 || fov <= 70 * MATH_DEG2RAD) return 0.05f;
	if (fov <= 100 * MATH_DEG2RAD) return 0.025f;
	if (fov <= 150 * MATH_DEG2RAD) return 0.0125f;
	return 0.00390625f;
}

void Gfx_CalcPerspectiveMatrix(float fov, float aspect, float zFar, struct Matrix* matrix) {
	Matrix_PerspectiveFieldOfView(matrix, fov, aspect, CalcZNear(fov), zFar);
	/* Adjust the projection matrix to produce reversed Z values */
	matrix->row3.Z = -matrix->row3.Z - 1.0f;
	matrix->row4.Z = -matrix->row4.Z;
}


/*########################################################################################################################*
*-----------------------------------------------------------Misc----------------------------------------------------------*
*#########################################################################################################################*/
cc_result Gfx_TakeScreenshot(struct Stream* output) {
	ID3D11Resource* backbuffer_res;
	D3D11_RENDER_TARGET_VIEW_DESC backbuffer_desc;
	D3D11_MAPPED_SUBRESOURCE buffer;
	D3D11_TEXTURE2D_DESC desc = { 0 };
	ID3D11Texture2D* tmp;
	struct Bitmap bmp;
	cc_result res;

	int i = 100;
	ID3D11RenderTargetView_GetResource(backbuffer, &backbuffer_res);
	ID3D11RenderTargetView_GetDesc(backbuffer, &backbuffer_desc);

	desc.Width     = WindowInfo.Width;
	desc.Height    = WindowInfo.Height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format    = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage     = D3D11_USAGE_STAGING;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

	res = ID3D11Device_CreateTexture2D(device, &desc, NULL, &tmp);
	if (res) goto finished;
	ID3D11DeviceContext_CopyResource(context, tmp, backbuffer_res);

	res = ID3D11DeviceContext_Map(context, tmp, 0, D3D11_MAP_READ, 0, &buffer);
	if (res) goto finished;
	{
		Bitmap_Init(bmp, desc.Width, desc.Height, (BitmapCol*)buffer.pData);
		res = Png_Encode(&bmp, output, NULL, false);
	}
	ID3D11DeviceContext_Unmap(context, tmp, 0);

finished:
	if (tmp) { ID3D11Texture2D_Release(tmp); }
	return res;
}

void Gfx_SetFpsLimit(cc_bool vsync, float minFrameMs) {
}

void Gfx_BeginFrame(void) {
}

void Gfx_Clear(void) {
	ID3D11DeviceContext_ClearRenderTargetView(context, backbuffer, gfx_clearColor);
}

void Gfx_EndFrame(void) {
	HRESULT hr = IDXGISwapChain_Present(swapchain, 0, 0);
	if (hr) Logger_Abort2(hr, "Failed to swap buffers");
}

cc_bool Gfx_WarnIfNecessary(void) { return false; }

void Gfx_GetApiInfo(cc_string* info) {
}

void Gfx_OnWindowResize(void) {
}
#endif
