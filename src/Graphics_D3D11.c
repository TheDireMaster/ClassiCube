#include "Core.h"
#ifdef CC_BUILD_D3D11
#include "_GraphicsBase.h"
#include "Errors.h"
#include "Logger.h"
#include "Window.h"
#include "_D3D11Shaders.h"

/* Avoid pointless includes */
#define WIN32_LEAN_AND_MEAN
#define NOSERVICE
#define NOMCX
#define NOIME
#define COBJMACROS
#include <d3d11.h>
#pragma comment(lib, "d3d11.lib")
static const GUID guid_ID3D11Texture2D = { 0x6f15aaf2, 0xd208, 0x4e89, { 0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c } };

// some generally useful debugging links
//   https://docs.microsoft.com/en-us/visualstudio/debugger/graphics/visual-studio-graphics-diagnostics
//   https://stackoverflow.com/questions/50591937/how-do-i-launch-the-hlsl-debugger-in-visual-studio-2017
//   https://docs.microsoft.com/en-us/visualstudio/debugger/graphics/graphics-object-table?view=vs-2019
//   https://github.com/gfx-rs/wgpu/issues/1171
// Some generally useful background links
//   https://gist.github.com/d7samurai/261c69490cce0620d0bfc93003cd1052

static int gfx_format = -1, depthBits;
static UINT gfx_stride;
static ID3D11Device* device;
static ID3D11DeviceContext* context;
static IDXGIDevice1* dxgi_device;
static IDXGIAdapter* dxgi_adapter;
static IDXGIFactory1* dxgi_factory;
static IDXGISwapChain* swapchain;
struct ShaderDesc { const void* data; int len; };

static void IA_Init(void);
static void IA_UpdateLayout(void);
static void VS_Init(void);
static void VS_UpdateShader(void);
static void RS_Init(void);
static void PS_Init(void);
static void PS_UpdateShader(void);
static void OM_Init(void);
static void OM_Free(void);

void Gfx_Create(void) {
	// https://docs.microsoft.com/en-us/windows/uwp/gaming/simple-port-from-direct3d-9-to-11-1-part-1--initializing-direct3d
	DWORD createFlags = 0;
	D3D_FEATURE_LEVEL fl;
	HRESULT hr;
#ifdef _DEBUG
	createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

	DXGI_SWAP_CHAIN_DESC desc = { 0 };
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

	// https://docs.microsoft.com/en-us/windows/win32/direct3d11/overviews-direct3d-11-graphics-pipeline
	IA_Init();
	VS_Init();
	RS_Init();
	PS_Init();
	OM_Init();

	// TODO need a better solution
	// https://docs.microsoft.com/en-us/windows/win32/direct3d11/overviews-direct3d-11-resources-limits
	// https://docs.microsoft.com/en-us/windows/win32/api/d3d11/ns-d3d11-d3d11_texture2d_desc
	Gfx.MaxTexWidth  = 8192;
	Gfx.MaxTexHeight = 8192;
}

cc_bool Gfx_TryRestoreContext(void) {
	return true;
}

void Gfx_Free(void) {
	ID3D11DeviceContext_ClearState(context);
	OM_Free();
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
	ID3D11Texture2D* tex = NULL;
	ID3D11ShaderResourceView* view = NULL;
	HRESULT hr;

	D3D11_TEXTURE2D_DESC desc = { 0 };
	desc.Width     = bmp->width;
	desc.Height    = bmp->height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format    = DXGI_FORMAT_B8G8R8A8_UNORM;
	desc.Usage     = D3D11_USAGE_IMMUTABLE;
	desc.SampleDesc.Count = 1;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

	D3D11_SUBRESOURCE_DATA data;
	data.pSysMem          = bmp->scan0;
	data.SysMemPitch      = bmp->width * 4;
	data.SysMemSlicePitch = 0;

	hr = ID3D11Device_CreateTexture2D(device, &desc, &data, &tex);
	if (hr) Logger_Abort2(hr, "Failed to create texture");

	hr = ID3D11Device_CreateShaderResourceView(device, tex, NULL, &view);
	if (hr) Logger_Abort2(hr, "Failed to create view");
	return view;
}

void Gfx_UpdateTexture(GfxResourceID texId, int x, int y, struct Bitmap* part, int rowWidth, cc_bool mipmaps) {
}

void Gfx_UpdateTexturePart(GfxResourceID texId, int x, int y, struct Bitmap* part, cc_bool mipmaps) {
}

void Gfx_DeleteTexture(GfxResourceID* texId) {
	ID3D11ShaderResourceView* view = (ID3D11ShaderResourceView*)(*texId);
	ID3D11Resource* res = NULL;

	if (view) {
		ID3D11ShaderResourceView_GetResource(view, &res);
		ID3D11Resource_Release(res);
		ID3D11ShaderResourceView_Release(view);
	}
	*texId = NULL;
}

void Gfx_SetTexturing(cc_bool enabled) {
}

void Gfx_EnableMipmaps(void) {
}

void Gfx_DisableMipmaps(void) {
}


/*########################################################################################################################*
*-----------------------------------------------------State management----------------------------------------------------*
*#########################################################################################################################*/


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

void Gfx_SetAlphaArgBlend(cc_bool enabled) {
}


/*########################################################################################################################*
*-------------------------------------------------------Index buffers-----------------------------------------------------*
*#########################################################################################################################*/
GfxResourceID Gfx_CreateIb(void* indices, int indicesCount) {
	ID3D11Buffer* buffer = NULL;
	
	D3D11_BUFFER_DESC desc = { 0 };
	desc.Usage     = D3D11_USAGE_DEFAULT;
	desc.ByteWidth = sizeof(cc_uint16) * indicesCount;
	desc.BindFlags = D3D11_BIND_INDEX_BUFFER;

	D3D11_SUBRESOURCE_DATA data;
	data.pSysMem          = indices;
	data.SysMemPitch      = 0;
	data.SysMemSlicePitch = 0;

	HRESULT hr = ID3D11Device_CreateBuffer(device, &desc, &data, &buffer);
	if (hr) Logger_Abort2(hr, "Failed to create index buffer");
	return buffer;
}

void Gfx_DeleteIb(GfxResourceID* ib) {
	ID3D11Buffer* buffer = (ID3D11Buffer*)(*ib);
	if (buffer) ID3D11Buffer_Release(buffer);
	*ib = NULL;
}


/*########################################################################################################################*
*------------------------------------------------------Vertex buffers-----------------------------------------------------*
*#########################################################################################################################*/
static ID3D11Buffer* CreateVertexBuffer(VertexFormat fmt, int count, cc_bool dynamic) {
	ID3D11Buffer* buffer = NULL;

	D3D11_BUFFER_DESC desc = { 0 };
	desc.Usage          = dynamic ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_DEFAULT;
	desc.CPUAccessFlags = dynamic ? D3D11_CPU_ACCESS_WRITE : 0;
	desc.ByteWidth = count * strideSizes[fmt];
	desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	/* TODO set data initially */

	HRESULT hr = ID3D11Device_CreateBuffer(device, &desc, NULL, &buffer);
	if (hr) Logger_Abort2(hr, "Failed to create index buffer");
	return buffer;
}

GfxResourceID Gfx_CreateVb(VertexFormat fmt, int count) {
	/* TODO immutable? */
	return CreateVertexBuffer(fmt, count, false);
}

void Gfx_DeleteVb(GfxResourceID* vb) { 
	ID3D11Buffer* buffer = (ID3D11Buffer*)(*vb);
	if (buffer) ID3D11Buffer_Release(buffer);
	*vb = NULL;
}

static void* tmp;
void* Gfx_LockVb(GfxResourceID vb, VertexFormat fmt, int count) {
	tmp = Mem_TryAlloc(count, strideSizes[fmt]);
	return tmp;
}

void Gfx_UnlockVb(GfxResourceID vb) {
	ID3D11Buffer* buffer = (ID3D11Buffer*)vb;
	ID3D11DeviceContext_UpdateSubresource(context, buffer, 0, NULL, tmp, 0, 0);
	Mem_Free(tmp);
	tmp = NULL;
}

void Gfx_SetVertexFormat(VertexFormat fmt) {
	if (fmt == gfx_format) return;
	gfx_format = fmt;
	gfx_stride = strideSizes[fmt];

	IA_UpdateLayout();
	VS_UpdateShader();
	PS_UpdateShader();
}

void Gfx_DrawVb_Lines(int verticesCount) {
	ID3D11DeviceContext_IASetPrimitiveTopology(context, D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
	ID3D11DeviceContext_Draw(context, verticesCount, 0);
	ID3D11DeviceContext_IASetPrimitiveTopology(context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void Gfx_DrawVb_IndexedTris(int verticesCount) {
	ID3D11DeviceContext_DrawIndexed(context, ICOUNT(verticesCount), 0, 0);
}

void Gfx_DrawVb_IndexedTris_Range(int verticesCount, int startVertex) {
	ID3D11DeviceContext_DrawIndexed(context, ICOUNT(verticesCount), 0, startVertex);
}

void Gfx_DrawIndexedTris_T2fC4b(int verticesCount, int startVertex) {
	ID3D11DeviceContext_DrawIndexed(context, ICOUNT(verticesCount), 0, startVertex);
}


/*########################################################################################################################*
*--------------------------------------------------Dynamic vertex buffers-------------------------------------------------*
*#########################################################################################################################*/
GfxResourceID Gfx_CreateDynamicVb(VertexFormat fmt, int maxVertices) {
	// TODO pass true instead
	return CreateVertexBuffer(fmt, maxVertices, true);
}

static D3D11_MAPPED_SUBRESOURCE mapDesc;
void* Gfx_LockDynamicVb(GfxResourceID vb, VertexFormat fmt, int count) {
	ID3D11Buffer* buffer = (ID3D11Buffer*)vb;
	mapDesc.pData = NULL;

	HRESULT hr = ID3D11DeviceContext_Map(context, buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapDesc);
	if (hr) Logger_Abort2(hr, "Failed to lock dynamic VB");
	return mapDesc.pData;
}

void Gfx_UnlockDynamicVb(GfxResourceID vb) {
	ID3D11Buffer* buffer = (ID3D11Buffer*)vb;
	ID3D11DeviceContext_Unmap(context, buffer, 0);
}

void Gfx_SetDynamicVbData(GfxResourceID vb, void* vertices, int vCount) {
	void* data = Gfx_LockDynamicVb(vb, gfx_format, vCount);
	Mem_Copy(data, vertices, vCount * gfx_stride);
	Gfx_UnlockDynamicVb(vb);
	Gfx_BindVb(vb);
}


/*########################################################################################################################*
*---------------------------------------------------------Matrices--------------------------------------------------------*
*#########################################################################################################################*/
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
}

//#####################z###################################################################################################
//-------------------------------------------------------Input Assembler--------------------------------------------------
//########################################################################################################################
// https://docs.microsoft.com/en-us/windows/win32/direct3d11/d3d10-graphics-programming-guide-input-assembler-stage
static ID3D11InputLayout* input_textured;

void Gfx_BindIb(GfxResourceID ib) {
	ID3D11Buffer* buffer = (ID3D11Buffer*)ib;
	ID3D11DeviceContext_IASetIndexBuffer(context, buffer, DXGI_FORMAT_R16_UINT, 0);
}

void Gfx_BindVb(GfxResourceID vb) {
	ID3D11Buffer* buffer   = (ID3D11Buffer*)vb;
	static UINT32 offset[] = { 0 };
	ID3D11DeviceContext_IASetVertexBuffers(context, 0, 1, &buffer, &gfx_stride, offset);
}

static void IA_CreateLayouts(void) {
	ID3D11InputLayout* input = NULL;
	// https://docs.microsoft.com/en-us/windows/win32/direct3d11/d3d10-graphics-programming-guide-input-assembler-stage-getting-started
	// https://docs.microsoft.com/en-us/windows/win32/direct3d10/d3d10-graphics-programming-guide-resources-legacy-formats
	// https://stackoverflow.com/questions/23398711/d3d11-input-element-desc-element-types-ordering-packing
	// D3D11_APPEND_ALIGNED_ELEMENT
	static D3D11_INPUT_ELEMENT_DESC T_layout[] =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "COLOR"   , 0, DXGI_FORMAT_R8G8B8A8_UNORM,  0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
	};
	HRESULT hr = ID3D11Device_CreateInputLayout(device, T_layout, Array_Elems(T_layout), 
												vs_shader_textured, sizeof(vs_shader_textured), &input);
	input_textured = input;
}

static void IA_UpdateLayout(void) {
	ID3D11DeviceContext_IASetInputLayout(context, input_textured);
}

static void IA_Init(void) {
	IA_CreateLayouts();
	ID3D11DeviceContext_IASetPrimitiveTopology(context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}


//########################################################################################################################
//--------------------------------------------------------Vertex shader---------------------------------------------------
//########################################################################################################################
// https://docs.microsoft.com/en-us/windows/win32/direct3d11/vertex-shader-stage
static ID3D11VertexShader* vs_shaders[3];
static ID3D11Buffer* vs_cBuffer;

static _declspec(align(64)) struct VSConstants {
	struct Matrix mvp;
	float texX, texY;
} vs_constants;
static const struct ShaderDesc vs_descs[3] = {
	{ vs_shader_colored,         sizeof(vs_shader_colored) },
	{ vs_shader_textured,        sizeof(vs_shader_textured) },
	{ vs_shader_textured_offset, sizeof(vs_shader_textured_offset) },
};

static void VS_CreateShaders(void) {
	for (int i = 0; i < Array_Elems(vs_shaders); i++) {
		HRESULT hr = ID3D11Device_CreateVertexShader(device, vs_descs[i].data, vs_descs[i].len, NULL, &vs_shaders[i]);
		if (hr) Logger_Abort2(hr, "Failed to compile vertex shader");
	}
}

static void VS_CreateConstants(void) {
	// https://developer.nvidia.com/content/constant-buffers-without-constant-pain-0
	// https://docs.microsoft.com/en-us/windows/win32/direct3d11/overviews-direct3d-11-resources-buffers-constant-how-to
	// https://gamedev.stackexchange.com/questions/18026/directx11-how-do-i-manage-and-update-multiple-shader-constant-buffers
	D3D11_BUFFER_DESC desc = { 0 };
	desc.ByteWidth      = sizeof(vs_constants);
	//desc.Usage          = D3D11_USAGE_DYNAMIC;
	//desc.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
	//desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	desc.Usage     = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

	D3D11_SUBRESOURCE_DATA data;
	data.pSysMem          = &vs_constants;
	data.SysMemPitch      = 0;
	data.SysMemSlicePitch = 0;

	HRESULT hr = ID3D11Device_CreateBuffer(device, &desc, &data, &vs_cBuffer);
	ID3D11DeviceContext_VSSetConstantBuffers(context, 0, 1, &vs_cBuffer);
}

static int VS_CalcShaderIndex(void) {
	if (gfx_format == VERTEX_FORMAT_COLOURED) return 0;

	cc_bool has_offset = vs_constants.texX != 0 || vs_constants.texY != 0;
	return has_offset ? 2 : 1;
}

static void VS_UpdateShader(void) {
	int idx = VS_CalcShaderIndex();
	ID3D11DeviceContext_VSSetShader(context, vs_shaders[idx], NULL, 0);
}

static void VS_UpdateConstants(void) {
	ID3D11DeviceContext_UpdateSubresource(context, vs_cBuffer, 0, NULL, &vs_constants, 0, 0);
}

static void VS_Init(void) {
	VS_CreateShaders();
	VS_CreateConstants();
	VS_UpdateShader();
}

static struct Matrix _view, _proj;
void Gfx_LoadMatrix(MatrixType type, const struct Matrix* matrix) {
	if (type == MATRIX_VIEW)       _view = *matrix;
	if (type == MATRIX_PROJECTION) _proj = *matrix;

	Matrix_Mul(&vs_constants.mvp, &_view, &_proj);
	VS_UpdateConstants();
}

void Gfx_LoadIdentityMatrix(MatrixType type) {
	Gfx_LoadMatrix(type, &Matrix_Identity);
}

void Gfx_EnableTextureOffset(float x, float y) {
	vs_constants.texX = x;
	vs_constants.texY = y;
	VS_UpdateShader();
	VS_UpdateConstants();
}

void Gfx_DisableTextureOffset(void) {
	vs_constants.texX = 0;
	vs_constants.texY = 0;
	VS_UpdateShader();
}


//########################################################################################################################
//---------------------------------------------------------Rasteriser-----------------------------------------------------
//########################################################################################################################
// https://docs.microsoft.com/en-us/windows/win32/direct3d11/d3d10-graphics-programming-guide-rasterizer-stage
static ID3D11RasterizerState* rs_state;

static void RS_CreateRasterState(void) {
	// https://docs.microsoft.com/en-us/windows/win32/api/d3d11/ns-d3d11-d3d11_rasterizer_desc
	D3D11_RASTERIZER_DESC desc = { 0 };
	desc.CullMode              = D3D11_CULL_NONE;
	desc.FillMode              = D3D11_FILL_SOLID;
	desc.FrontCounterClockwise = true;
	desc.DepthClipEnable       = true; // otherwise vertices/pixels beyond far plane are still wrongly rendered
	ID3D11Device_CreateRasterizerState(device, &desc, &rs_state);
}

static void RS_UpdateViewport(void) {
	D3D11_VIEWPORT viewport;
	viewport.TopLeftX = 0;
	viewport.TopLeftY = 0;
	viewport.Width    = WindowInfo.Width;
	viewport.Height   = WindowInfo.Height;
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;
	ID3D11DeviceContext_RSSetViewports(context, 1, &viewport);
}

static void RS_UpdateRasterState(void) {
	ID3D11DeviceContext_RSSetState(context, rs_state);
}

static void RS_Init(void) {
	RS_CreateRasterState();
	RS_UpdateViewport();
	RS_UpdateRasterState();
}


//########################################################################################################################
//--------------------------------------------------------Pixel shader----------------------------------------------------
//########################################################################################################################
// https://docs.microsoft.com/en-us/windows/win32/direct3d11/pixel-shader-stage
static ID3D11SamplerState* ps_sampler;
static ID3D11PixelShader* ps_shaders[2];

static const struct ShaderDesc ps_descs[2] = {
	{ ps_shader_colored,  sizeof(ps_shader_colored) },
	{ ps_shader_textured, sizeof(ps_shader_textured) },
};

static void PS_CreateShaders(void) {
	for (int i = 0; i < Array_Elems(ps_shaders); i++) {
		HRESULT hr = ID3D11Device_CreatePixelShader(device, ps_descs[i].data, ps_descs[i].len, NULL, &ps_shaders[i]);
		if (hr) Logger_Abort2(hr, "Failed to compile pixel shader");
	}
}

static void PS_UpdateShader(void) {
	int idx = gfx_format == VERTEX_FORMAT_COLOURED ? 0 : 1;
	ID3D11DeviceContext_PSSetShader(context, ps_shaders[idx], NULL, 0);
}

static void PS_CreateSamplers(void) {
	// https://docs.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11device-createsamplerstate
	// https://gamedev.stackexchange.com/questions/18026/directx11-how-do-i-manage-and-update-multiple-shader-constant-buffers
	// https://docs.microsoft.com/en-us/windows/win32/direct3d11/overviews-direct3d-11-resources-buffers-constant-how-to
	D3D11_SAMPLER_DESC desc = { 0 };

	desc.Filter   = D3D11_FILTER_MIN_MAG_MIP_POINT;
	desc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
	desc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
	desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
	desc.MaxAnisotropy  = 1;
	desc.MaxLOD         = D3D11_FLOAT32_MAX;
	desc.ComparisonFunc = D3D11_COMPARISON_NEVER;

	HRESULT hr = ID3D11Device_CreateSamplerState(device, &desc, &ps_sampler);
}

static void PS_UpdateSampler(void) {
	ID3D11DeviceContext_PSSetSamplers(context, 0, 1, &ps_sampler);
}

static void PS_Init(void) {
	PS_CreateShaders();
	PS_CreateSamplers();
	PS_UpdateSampler();
	PS_UpdateShader();
}

void Gfx_BindTexture(GfxResourceID texId) {
	ID3D11ShaderResourceView* view = (ID3D11ShaderResourceView*)texId;
	ID3D11DeviceContext_PSSetShaderResources(context, 0, 1, &view);
}


//########################################################################################################################
//-------------------------------------------------------Output merger----------------------------------------------------
//########################################################################################################################
// https://docs.microsoft.com/en-us/windows/win32/direct3d11/d3d10-graphics-programming-guide-output-merger-stage
static ID3D11RenderTargetView* backbuffer;
static ID3D11Texture2D* depthbuffer;
static ID3D11DepthStencilView* depthbufferView;
static ID3D11BlendState* om_blendStates[4];
static ID3D11DepthStencilState* om_depthStates[4];
static float gfx_clearColor[4];
static cc_bool gfx_alphaBlending, gfx_colorEnabled = true;
static cc_bool gfx_depthTest, gfx_depthWrite;

static void OM_Clear(void) {
	ID3D11DeviceContext_ClearRenderTargetView(context, backbuffer, gfx_clearColor);
	ID3D11DeviceContext_ClearDepthStencilView(context, depthbufferView, D3D11_CLEAR_DEPTH, 1.0f, 0);
}

static void OM_InitTargets(void) {
	// https://docs.microsoft.com/en-us/windows/win32/direct3d11/d3d10-graphics-programming-guide-depth-stencil
	D3D11_TEXTURE2D_DESC desc;
	ID3D11Texture2D* pBackBuffer;
	HRESULT hr;

	hr = IDXGISwapChain_GetBuffer(swapchain, 0, &guid_ID3D11Texture2D, (void**)&pBackBuffer);
	if (hr) Logger_Abort2(hr, "Failed to get swapchain backbuffer");

	hr = ID3D11Device_CreateRenderTargetView(device, pBackBuffer, NULL, &backbuffer);
	if (hr) Logger_Abort2(hr, "Failed to create render target");

	ID3D11Texture2D_GetDesc(pBackBuffer, &desc);
    desc.Format    = DXGI_FORMAT_D24_UNORM_S8_UINT;
    desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

    hr = ID3D11Device_CreateTexture2D(device, &desc, NULL, &depthbuffer);
	if (hr) Logger_Abort2(hr, "Failed to create depthbuffer texture");

	hr = ID3D11Device_CreateDepthStencilView(device, depthbuffer, NULL, &depthbufferView);
	if (hr) Logger_Abort2(hr, "Failed to create depthbuffer view");

	ID3D11DeviceContext_OMSetRenderTargets(context, 1, &backbuffer, depthbufferView);
	ID3D11Texture2D_Release(pBackBuffer);
}

static void OM_CreateDepthStates(void) {
	D3D11_DEPTH_STENCIL_DESC desc = { 0 };
	HRESULT hr;
	desc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;

	for (int i = 0; i < Array_Elems(om_depthStates); i++) {
		desc.DepthEnable    = (i & 1) != 0;
		desc.DepthWriteMask = (i & 2) != 0;

		hr = ID3D11Device_CreateDepthStencilState(device, &desc, &om_depthStates[i]);
		if (hr) Logger_Abort2(hr, "Failed to create depth state");
	}
}

static void OM_UpdateDepthState(void) {
	ID3D11DepthStencilState* depthState = om_depthStates[gfx_depthTest | (gfx_depthWrite << 1)];
	ID3D11DeviceContext_OMSetDepthStencilState(context, depthState, 0);
}

static void OM_CreateBlendStates(void) {
	// https://docs.microsoft.com/en-us/windows/win32/direct3d11/d3d10-graphics-programming-guide-blend-state
	D3D11_BLEND_DESC desc = { 0 };
	HRESULT hr;
	desc.RenderTarget[0].BlendOp        = D3D11_BLEND_OP_ADD;
	desc.RenderTarget[0].BlendOpAlpha   = D3D11_BLEND_OP_ADD;
	desc.RenderTarget[0].SrcBlend       = D3D11_BLEND_SRC_ALPHA;
	desc.RenderTarget[0].SrcBlendAlpha  = D3D11_BLEND_SRC_ALPHA;
	desc.RenderTarget[0].DestBlend      = D3D11_BLEND_INV_SRC_ALPHA;
	desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;

	for (int i = 0; i < Array_Elems(om_blendStates); i++) {
		desc.RenderTarget[0].RenderTargetWriteMask = (i & 1) ? D3D11_COLOR_WRITE_ENABLE_ALL : 0;
		desc.RenderTarget[0].BlendEnable           = (i & 2) != 0;

		hr = ID3D11Device_CreateBlendState(device, &desc, &om_blendStates[i]);
		if (hr) Logger_Abort2(hr, "Failed to create blend state");
	}
}

static void OM_UpdateBlendState(void) {
	ID3D11BlendState* blendState = om_blendStates[gfx_colorEnabled | (gfx_alphaBlending << 1)];
	ID3D11DeviceContext_OMSetBlendState(context, blendState, NULL, 0xffffffff);
}

static void OM_Init(void) {
	OM_InitTargets();
	OM_CreateDepthStates();
	OM_UpdateDepthState();
	OM_CreateBlendStates();
	OM_UpdateBlendState();
}

static void OM_FreeTargets(void) {
	ID3D11DeviceContext_OMSetRenderTargets(context, 0, NULL, NULL);
	ID3D11RenderTargetView_Release(backbuffer);
	ID3D11DepthStencilView_Release(depthbufferView);
	ID3D11Texture2D_Release(depthbuffer);
}

static void OM_Free(void) {
	OM_FreeTargets();
}

void Gfx_ClearCol(PackedCol col) {
	gfx_clearColor[0] = PackedCol_R(col) / 255.0f;
	gfx_clearColor[1] = PackedCol_G(col) / 255.0f;
	gfx_clearColor[2] = PackedCol_B(col) / 255.0f;
	gfx_clearColor[3] = PackedCol_A(col) / 255.0f;
}

void Gfx_SetDepthTest(cc_bool enabled) {
	gfx_depthTest = enabled;
	OM_UpdateDepthState();
}

void Gfx_SetDepthWrite(cc_bool enabled) {
	gfx_depthWrite = enabled;
	OM_UpdateDepthState();
}

void Gfx_SetAlphaBlending(cc_bool enabled) {
	gfx_alphaBlending = enabled;
	OM_UpdateBlendState();
}

void Gfx_SetColWriteMask(cc_bool r, cc_bool g, cc_bool b, cc_bool a) {
	gfx_colorEnabled = r;
	OM_UpdateBlendState();
}


/*########################################################################################################################*
*-----------------------------------------------------------Misc----------------------------------------------------------*
*#########################################################################################################################*/
static BitmapCol* D3D11_GetRow(struct Bitmap* bmp, int y) {
	// You were expecting a BitmapCol*, but it was me, D3D11_MAPPED_SUBRESOURCE*!
	//  This is necessary because the stride of the mapped backbuffer often doesn't equal width of the bitmap
	//    e.g. with backbuffer width of 854, stride is 3456 bytes instead of expected 3416 (854*4)
	//  Therefore have to calculate row address manually instead of using Bitmap_GetRow
	D3D11_MAPPED_SUBRESOURCE* buffer = (D3D11_MAPPED_SUBRESOURCE*)bmp->scan0;

	char* row = (char*)buffer->pData + y * buffer->RowPitch;
	return (BitmapCol*)row;
}

cc_result Gfx_TakeScreenshot(struct Stream* output) {
	ID3D11Texture2D* tmp;
	struct Bitmap bmp;
	HRESULT hr;

	ID3D11Resource* backbuffer_res;
	D3D11_RENDER_TARGET_VIEW_DESC backbuffer_desc;
	D3D11_MAPPED_SUBRESOURCE buffer;
	ID3D11RenderTargetView_GetResource(backbuffer, &backbuffer_res);
	ID3D11RenderTargetView_GetDesc(backbuffer, &backbuffer_desc);

	D3D11_TEXTURE2D_DESC desc = { 0 };
	desc.Width     = WindowInfo.Width;
	desc.Height    = WindowInfo.Height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format    = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage     = D3D11_USAGE_STAGING;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

	hr = ID3D11Device_CreateTexture2D(device, &desc, NULL, &tmp);
	if (hr) goto finished;
	ID3D11DeviceContext_CopyResource(context, tmp, backbuffer_res);

	hr = ID3D11DeviceContext_Map(context, tmp, 0, D3D11_MAP_READ, 0, &buffer);
	if (hr) goto finished;
	{
		Bitmap_Init(bmp, desc.Width, desc.Height, (BitmapCol*)&buffer);
		hr = Png_Encode(&bmp, output, D3D11_GetRow, false);
	}
	ID3D11DeviceContext_Unmap(context, tmp, 0);

finished:
	if (tmp) { ID3D11Texture2D_Release(tmp); }
	return hr;
}

void Gfx_SetFpsLimit(cc_bool vsync, float minFrameMs) {
	gfx_vsync = vsync;
}
void Gfx_BeginFrame(void) { }
void Gfx_Clear(void)      { OM_Clear(); }

void Gfx_EndFrame(void) {
	// https://docs.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-present
	// gfx_vsync happens to match SyncInterval parameter
	HRESULT hr = IDXGISwapChain_Present(swapchain, gfx_vsync, 0);
	if (hr) Logger_Abort2(hr, "Failed to swap buffers");
}

cc_bool Gfx_WarnIfNecessary(void) { return false; }

void Gfx_GetApiInfo(cc_string* info) {
}

void Gfx_OnWindowResize(void) {
	// https://docs.microsoft.com/en-us/windows/win32/direct3ddxgi/d3d10-graphics-programming-guide-dxgi#handling-window-resizing
	OM_FreeTargets();
	HRESULT hr = IDXGISwapChain_ResizeBuffers(swapchain, 0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
	if (hr) Logger_Abort2(hr, "Failed to resize swapchain");

	OM_InitTargets();
	RS_UpdateViewport();
}
#endif