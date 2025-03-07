#include "Core.h"
#ifdef CC_BUILD_D3D9
#include "_GraphicsBase.h"
#include "Errors.h"
#include "Logger.h"
#include "Window.h"

/* Avoid pointless includes */
#define WIN32_LEAN_AND_MEAN
#define NOSERVICE
#define NOMCX
#define NOIME
/*#define D3D_DISABLE_9EX causes compile errors*/
#include <d3d9.h>
#include <d3d9caps.h>
#include <d3d9types.h>

/* https://docs.microsoft.com/en-us/windows/win32/dxtecharts/resource-management-best-practices */
/* https://docs.microsoft.com/en-us/windows/win32/dxtecharts/the-direct3d-transformation-pipeline */

/* https://docs.microsoft.com/en-us/windows/win32/direct3d9/d3dfvf-texcoordsizen */
static DWORD d3d9_formatMappings[2] = { D3DFVF_XYZ | D3DFVF_DIFFUSE, D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1 };
/* Current format and size of vertices */
static int gfx_stride, gfx_format = -1;

static cc_bool using_d3d9Ex;
static IDirect3D9* d3d;
static IDirect3DDevice9* device;
static DWORD createFlags;
static D3DFORMAT viewFormat, depthFormat;
static int cachedWidth, cachedHeight;
static int depthBits;
static float totalMem;

static void D3D9_RestoreRenderStates(void);
static void D3D9_FreeResource(GfxResourceID* resource) {
	cc_uintptr addr;
	ULONG refCount;
	IUnknown* unk;
	
	unk = (IUnknown*)(*resource);
	if (!unk) return;
	*resource = 0;

#ifdef __cplusplus
	refCount = unk->Release();
#else
	refCount = unk->lpVtbl->Release(unk);
#endif

	if (refCount <= 0) return;
	addr = (cc_uintptr)unk;
	Platform_Log2("D3D9 resource has %i outstanding references! ID 0x%x", &refCount, &addr);
}

static IDirect3D9* (WINAPI *_Direct3DCreate9)(UINT SDKVersion);
static HRESULT     (WINAPI *_Direct3DCreate9Ex)(UINT SDKVersion, IDirect3D9** __d3d9);

static void LoadD3D9Library(void) {
	static const struct DynamicLibSym funcs[] = {
		DynamicLib_Sym(Direct3DCreate9), 
		DynamicLib_Sym(Direct3DCreate9Ex)
	};
	static const cc_string path = String_FromConst("d3d9.dll");
	void* lib = DynamicLib_Load2(&path);

	if (!lib) {
		Logger_DynamicLibWarn("loading", &path);
		Logger_Abort("Failed to load d3d9.dll. You may need to install Direct3D9.");
	}
	DynamicLib_GetAll(lib, funcs, Array_Elems(funcs));
}

static void CreateD3D9Instance(void) {
	cc_result res;
	// still to check: managed texture perf, driver reset
	//   consider optimised CreateTexture??
	if (_Direct3DCreate9Ex && Options_GetBool("gfx-direct3d9ex", false)) {
		res = _Direct3DCreate9Ex(D3D_SDK_VERSION, &d3d);
		if (res == D3DERR_NOTAVAILABLE) {
			/* Direct3D9Ex not supported, fallback to normal Direct3D9 */
		} else if (res) {
			Logger_Abort2(res, "Direct3D9Create9Ex failed");
		} else {
			using_d3d9Ex = true;
			/* NOTE: Direct3D9Ex does not support managed textures */
			return;
		}
	}

	d3d = _Direct3DCreate9(D3D_SDK_VERSION);
	/* Normal Direct3D9 supports POOL_MANAGED textures */
	Gfx.ManagedTextures = true;
	if (!d3d) Logger_Abort("Direct3DCreate9 returned NULL");
}

static void FindCompatibleViewFormat(void) {
	static const D3DFORMAT formats[4] = { D3DFMT_X8R8G8B8, D3DFMT_R8G8B8, D3DFMT_R5G6B5, D3DFMT_X1R5G5B5 };
	cc_result res;
	int i;

	for (i = 0; i < Array_Elems(formats); i++) {
		viewFormat = formats[i];
		res = IDirect3D9_CheckDeviceType(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, viewFormat, viewFormat, true);
		if (!res) return;
	}
	Logger_Abort("Failed to create back buffer. Graphics drivers may not be installed.");
}

static void FindCompatibleDepthFormat(void) {
	static const D3DFORMAT formats[6] = { D3DFMT_D32, D3DFMT_D24X8, D3DFMT_D24S8, D3DFMT_D24X4S4, D3DFMT_D16, D3DFMT_D15S1 };
	cc_result res;
	int i;

	for (i = 0; i < Array_Elems(formats); i++) {
		depthFormat = formats[i];
		res = IDirect3D9_CheckDepthStencilMatch(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, viewFormat, viewFormat, depthFormat);
		if (!res) return;
	}
	Logger_Abort("Failed to create depth buffer. Graphics drivers may not be installed.");
}

static void D3D9_FillPresentArgs(D3DPRESENT_PARAMETERS* args) {
	args->AutoDepthStencilFormat = depthFormat;
	args->BackBufferWidth  = Game.Width;
	args->BackBufferHeight = Game.Height;
	args->BackBufferFormat = viewFormat;
	args->BackBufferCount  = 1;
	args->EnableAutoDepthStencil = true;
	args->PresentationInterval   = gfx_vsync ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
	args->SwapEffect = D3DSWAPEFFECT_DISCARD;
	args->Windowed   = true;
}

static const int D3D9_DepthBufferBits(void) {
	switch (depthFormat) {
	case D3DFMT_D32:     return 32;
	case D3DFMT_D24X8:   return 24;
	case D3DFMT_D24S8:   return 24;
	case D3DFMT_D24X4S4: return 24;
	case D3DFMT_D16:     return 16;
	case D3DFMT_D15S1:   return 15;
	}
	return 0;
}

static void D3D9_UpdateCachedDimensions(void) {
	cachedWidth  = Game.Width;
	cachedHeight = Game.Height;
}

static cc_bool deviceCreated;
static void TryCreateDevice(void) {
	cc_result res;
	D3DCAPS9 caps;
	HWND winHandle = (HWND)WindowInfo.Handle;
	D3DPRESENT_PARAMETERS args = { 0 };
	D3D9_FillPresentArgs(&args);

	/* Try to create a device with as much hardware usage as possible. */
	createFlags = D3DCREATE_HARDWARE_VERTEXPROCESSING;
	res = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, winHandle, createFlags, &args, &device);
	/* Another running fullscreen application might prevent creating device */
	if (res == D3DERR_DEVICELOST) { Gfx.LostContext = true; return; }

	/* Fallback with using CPU for some parts of rendering */
	if (res) {
		createFlags = D3DCREATE_MIXED_VERTEXPROCESSING;
		res = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, winHandle, createFlags, &args, &device);
	}
	if (res) {
		createFlags = D3DCREATE_SOFTWARE_VERTEXPROCESSING;
		res = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, winHandle, createFlags, &args, &device);
	}

	/* Not enough memory? Try again later in a bit */
	if (res == D3DERR_OUTOFVIDEOMEMORY) { Gfx.LostContext = true; return; }

	if (res) Logger_Abort2(res, "Creating Direct3D9 device");
	res = IDirect3DDevice9_GetDeviceCaps(device, &caps);
	if (res) Logger_Abort2(res, "Getting Direct3D9 capabilities");

	D3D9_UpdateCachedDimensions();
	deviceCreated    = true;
	Gfx.MaxTexWidth  = caps.MaxTextureWidth;
	Gfx.MaxTexHeight = caps.MaxTextureHeight;
	totalMem = IDirect3DDevice9_GetAvailableTextureMem(device) / (1024.0f * 1024.0f);
}

void Gfx_Create(void) {
	LoadD3D9Library();
	CreateD3D9Instance();
	FindCompatibleViewFormat();
	FindCompatibleDepthFormat();
	depthBits = D3D9_DepthBufferBits();

	customMipmapsLevels = true;
	Gfx.Created         = true;
	TryCreateDevice();
}

cc_bool Gfx_TryRestoreContext(void) {
	D3DPRESENT_PARAMETERS args = { 0 };
	cc_result res;
	/* Rarely can't even create device to begin with */
	if (!deviceCreated) {
		TryCreateDevice();
		return deviceCreated;
	}

	res = IDirect3DDevice9_TestCooperativeLevel(device);
	if (res && res != D3DERR_DEVICENOTRESET) return false;

	D3D9_FillPresentArgs(&args);
	res = IDirect3DDevice9_Reset(device, &args);
	if (res == D3DERR_DEVICELOST) return false;

	if (res) Logger_Abort2(res, "Error recreating D3D9 context");
	D3D9_UpdateCachedDimensions();
	return true;
}

void Gfx_Free(void) {
	Gfx_FreeState();
	D3D9_FreeResource(&device);
	D3D9_FreeResource(&d3d);
}

static void Gfx_FreeState(void) { 
	FreeDefaultResources();
	cachedWidth  = 0;
	cachedHeight = 0;
}

static void Gfx_RestoreState(void) {
	Gfx_SetFaceCulling(false);
	InitDefaultResources();
	gfx_format = -1;

	IDirect3DDevice9_SetRenderState(device, D3DRS_COLORVERTEX,       false);
	IDirect3DDevice9_SetRenderState(device, D3DRS_LIGHTING,          false);
	IDirect3DDevice9_SetRenderState(device, D3DRS_SPECULARENABLE,    false);
	IDirect3DDevice9_SetRenderState(device, D3DRS_LOCALVIEWER,       false);
	IDirect3DDevice9_SetRenderState(device, D3DRS_DEBUGMONITORTOKEN, false);

	/* States relevant to the game */
	IDirect3DDevice9_SetRenderState(device, D3DRS_ALPHAFUNC, D3DCMP_GREATER);
	IDirect3DDevice9_SetRenderState(device, D3DRS_ALPHAREF,  127);
	IDirect3DDevice9_SetRenderState(device, D3DRS_SRCBLEND,  D3DBLEND_SRCALPHA);
	IDirect3DDevice9_SetRenderState(device, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
	IDirect3DDevice9_SetRenderState(device, D3DRS_ZFUNC,     D3DCMP_GREATEREQUAL);
	D3D9_RestoreRenderStates();
}

static cc_bool D3D9_CheckResult(cc_result res, const char* func) {
	if (!res) return true;

	if (res == D3DERR_OUTOFVIDEOMEMORY || res == E_OUTOFMEMORY) {
		Event_RaiseVoid(&GfxEvents.LowVRAMDetected);
	} else {
		Logger_Abort2(res, func);
	}
	return false;
}


/*########################################################################################################################*
*---------------------------------------------------------Textures--------------------------------------------------------*
*#########################################################################################################################*/
static void D3D9_SetTextureData(IDirect3DTexture9* texture, struct Bitmap* bmp, int lvl) {
	D3DLOCKED_RECT rect;
	cc_result res = IDirect3DTexture9_LockRect(texture, lvl, &rect, NULL, 0);
	if (res) Logger_Abort2(res, "D3D9_LockTextureData");

	cc_uint32 size = Bitmap_DataSize(bmp->width, bmp->height);
	Mem_Copy(rect.pBits, bmp->scan0, size);

	res = IDirect3DTexture9_UnlockRect(texture, lvl);
	if (res) Logger_Abort2(res, "D3D9_UnlockTextureData");
}

static void D3D9_SetTexturePartData(IDirect3DTexture9* texture, int x, int y, const struct Bitmap* bmp, int rowWidth, int lvl) {
	D3DLOCKED_RECT rect;
	cc_result res;
	RECT part;

	part.left = x; part.right  = x + bmp->width;
	part.top  = y; part.bottom = y + bmp->height;

	res = IDirect3DTexture9_LockRect(texture, lvl, &rect, &part, 0);
	if (res) Logger_Abort2(res, "D3D9_LockTexturePartData");

	CopyTextureData(rect.pBits, rect.Pitch, bmp, rowWidth << 2);
	res = IDirect3DTexture9_UnlockRect(texture, lvl);
	if (res) Logger_Abort2(res, "D3D9_UnlockTexturePartData");
}

static void D3D9_DoMipmaps(IDirect3DTexture9* texture, int x, int y, struct Bitmap* bmp, int rowWidth, cc_bool partial) {
	BitmapCol* prev = bmp->scan0;
	BitmapCol* cur;
	struct Bitmap mipmap;

	int lvls = CalcMipmapsLevels(bmp->width, bmp->height);
	int lvl, width = bmp->width, height = bmp->height;

	for (lvl = 1; lvl <= lvls; lvl++) {
		x /= 2; y /= 2;
		if (width > 1)  width /= 2;
		if (height > 1) height /= 2;

		cur = (BitmapCol*)Mem_Alloc(width * height, 4, "mipmaps");
		GenMipmaps(width, height, cur, prev, rowWidth);

		Bitmap_Init(mipmap, width, height, cur);
		if (partial) {
			D3D9_SetTexturePartData(texture, x, y, &mipmap, width, lvl);
		} else {
			D3D9_SetTextureData(texture, &mipmap, lvl);
		}

		if (prev != bmp->scan0) Mem_Free(prev);
		prev     = cur;
		rowWidth = width;
	}
	if (prev != bmp->scan0) Mem_Free(prev);
}

static IDirect3DTexture9* DoCreateTexture(struct Bitmap* bmp, int levels, int usage, int pool, void** data) {
	IDirect3DTexture9* tex;
	cc_result res;
	
	for (;;) {
		res = IDirect3DDevice9_CreateTexture(device, bmp->width, bmp->height, levels,
			usage, D3DFMT_A8R8G8B8, pool, &tex, data);
		if (D3D9_CheckResult(res, "D3D9_CreateTexture failed")) break;
	}
	return tex;
}

GfxResourceID Gfx_CreateTexture(struct Bitmap* bmp, cc_uint8 flags, cc_bool mipmaps) {
	IDirect3DTexture9* tex;
	IDirect3DTexture9* sys;
	DWORD usage = 0;
	cc_result res;

	int mipmapsLevels = CalcMipmapsLevels(bmp->width, bmp->height);
	int levels = 1 + (mipmaps ? mipmapsLevels : 0);

	if (!Math_IsPowOf2(bmp->width) || !Math_IsPowOf2(bmp->height)) {
		Logger_Abort("Textures must have power of two dimensions");
	}
	if (Gfx.LostContext) return 0;

	if ((flags & TEXTURE_FLAG_MANAGED) && !using_d3d9Ex) {
		/* Direct3D9Ex doesn't support managed textures */
		tex = DoCreateTexture(bmp, levels, 0, D3DPOOL_MANAGED, NULL);
		D3D9_SetTextureData(tex, bmp, 0);
		if (mipmaps) D3D9_DoMipmaps(tex, 0, 0, bmp, bmp->width, false);
	} else {
		/* Direct3D9Ex requires this for dynamically updatable textures */
		if ((flags & TEXTURE_FLAG_DYNAMIC) && using_d3d9Ex) usage = D3DUSAGE_DYNAMIC;

		if (using_d3d9Ex && !mipmaps) {
			/* Direct3D9Ex allows avoiding copying data altogether in some circumstances */
			/* https://docs.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3ddevice9-createtexture */
			void** pixels = &bmp->scan0;
			sys = DoCreateTexture(bmp, levels, 0, D3DPOOL_SYSTEMMEM, pixels);
		} else {
			sys = DoCreateTexture(bmp, levels, 0, D3DPOOL_SYSTEMMEM, NULL);
			D3D9_SetTextureData(sys, bmp, 0);
			if (mipmaps) D3D9_DoMipmaps(sys, 0, 0, bmp, bmp->width, false);
		}
		
		tex = DoCreateTexture(bmp, levels, usage, D3DPOOL_DEFAULT, NULL);
		res = IDirect3DDevice9_UpdateTexture(device, (IDirect3DBaseTexture9*)sys, (IDirect3DBaseTexture9*)tex);
		if (res) Logger_Abort2(res, "D3D9_CreateTexture - Update");
		D3D9_FreeResource(&sys);
	}
	return tex;
}

void Gfx_UpdateTexture(GfxResourceID texId, int x, int y, struct Bitmap* part, int rowWidth, cc_bool mipmaps) {
	IDirect3DTexture9* texture = (IDirect3DTexture9*)texId;
	D3D9_SetTexturePartData(texture, x, y, part, rowWidth, 0);
	if (mipmaps) D3D9_DoMipmaps(texture, x, y, part, rowWidth, true);
}

void Gfx_UpdateTexturePart(GfxResourceID texId, int x, int y, struct Bitmap* part, cc_bool mipmaps) {
	Gfx_UpdateTexture(texId, x, y, part, part->width, mipmaps);
}

void Gfx_BindTexture(GfxResourceID texId) {
	cc_result res = IDirect3DDevice9_SetTexture(device, 0, (IDirect3DBaseTexture9*)texId);
	if (res) Logger_Abort2(res, "D3D9_BindTexture");
}

void Gfx_DeleteTexture(GfxResourceID* texId) { D3D9_FreeResource(texId); }

void Gfx_SetTexturing(cc_bool enabled) {
	if (enabled) return;
	cc_result res = IDirect3DDevice9_SetTexture(device, 0, NULL);
	if (res) Logger_Abort2(res, "D3D9_SetTexturing");
}

void Gfx_EnableMipmaps(void) {
	if (!Gfx.Mipmaps) return;
	IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
}

void Gfx_DisableMipmaps(void) {
	if (!Gfx.Mipmaps) return;
	IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
}


/*########################################################################################################################*
*-----------------------------------------------------State management----------------------------------------------------*
*#########################################################################################################################*/
static D3DFOGMODE gfx_fogMode = D3DFOG_NONE;
static cc_bool gfx_alphaTesting, gfx_alphaBlending;
static cc_bool gfx_depthTesting, gfx_depthWriting;
static PackedCol gfx_clearColor, gfx_fogColor;
static float gfx_fogEnd = -1.0f, gfx_fogDensity = -1.0f;

/* NOTE: Although SetRenderState is okay to call on a lost device, it's also possible */
/*   the context is lost because the device was never created to begin with!          */
/* In that case, device will be NULL, so calling SetRenderState will crash the game.  */
/*  (see Gfx_Create, TryCreateDevice, Gfx_TryRestoreContext)                          */

void Gfx_SetFaceCulling(cc_bool enabled) {
	D3DCULL mode = enabled ? D3DCULL_CW : D3DCULL_NONE;
	IDirect3DDevice9_SetRenderState(device, D3DRS_CULLMODE, mode);
}

void Gfx_SetFog(cc_bool enabled) {
	if (gfx_fogEnabled == enabled) return;
	gfx_fogEnabled = enabled;

	if (Gfx.LostContext) return;
	IDirect3DDevice9_SetRenderState(device, D3DRS_FOGENABLE, enabled);
}

void Gfx_SetFogCol(PackedCol col) {
	if (col == gfx_fogColor) return;
	gfx_fogColor = col;

	if (Gfx.LostContext) return;
	IDirect3DDevice9_SetRenderState(device, D3DRS_FOGCOLOR, gfx_fogColor);
}

void Gfx_SetFogDensity(float value) {
	union IntAndFloat raw;
	if (value == gfx_fogDensity) return;
	gfx_fogDensity = value;

	raw.f = value;
	if (Gfx.LostContext) return;
	IDirect3DDevice9_SetRenderState(device, D3DRS_FOGDENSITY, raw.u);
}

void Gfx_SetFogEnd(float value) {
	union IntAndFloat raw;
	if (value == gfx_fogEnd) return;
	gfx_fogEnd = value;

	raw.f = value;
	if (Gfx.LostContext) return;
	IDirect3DDevice9_SetRenderState(device, D3DRS_FOGEND, raw.u);
}

void Gfx_SetFogMode(FogFunc func) {
	static D3DFOGMODE modes[3] = { D3DFOG_LINEAR, D3DFOG_EXP, D3DFOG_EXP2 };
	D3DFOGMODE mode = modes[func];
	if (mode == gfx_fogMode) return;

	gfx_fogMode = mode;
	if (Gfx.LostContext) return;
	IDirect3DDevice9_SetRenderState(device, D3DRS_FOGTABLEMODE, mode);
}

void Gfx_SetAlphaTest(cc_bool enabled) {
	if (gfx_alphaTesting == enabled) return;
	gfx_alphaTesting = enabled;

	if (Gfx.LostContext) return;
	IDirect3DDevice9_SetRenderState(device, D3DRS_ALPHATESTENABLE, enabled);
}

void Gfx_SetAlphaBlending(cc_bool enabled) {
	if (gfx_alphaBlending == enabled) return;
	gfx_alphaBlending = enabled;

	if (Gfx.LostContext) return;
	IDirect3DDevice9_SetRenderState(device, D3DRS_ALPHABLENDENABLE, enabled);
}

void Gfx_SetAlphaArgBlend(cc_bool enabled) {
	D3DTEXTUREOP op = enabled ? D3DTOP_MODULATE : D3DTOP_SELECTARG1;
	if (Gfx.LostContext) return;
	IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_ALPHAOP, op);
}

void Gfx_ClearCol(PackedCol col) { gfx_clearColor = col; }
void Gfx_SetColWriteMask(cc_bool r, cc_bool g, cc_bool b, cc_bool a) {
	DWORD channels = (r ? 1u : 0u) | (g ? 2u : 0u) | (b ? 4u : 0u) | (a ? 8u : 0u);
	if (Gfx.LostContext) return;
	IDirect3DDevice9_SetRenderState(device, D3DRS_COLORWRITEENABLE, channels);
}

void Gfx_SetDepthTest(cc_bool enabled) {
	gfx_depthTesting = enabled;
	if (Gfx.LostContext) return;
	IDirect3DDevice9_SetRenderState(device, D3DRS_ZENABLE, enabled);
}

void Gfx_SetDepthWrite(cc_bool enabled) {
	gfx_depthWriting = enabled;
	if (Gfx.LostContext) return;
	IDirect3DDevice9_SetRenderState(device, D3DRS_ZWRITEENABLE, enabled);
}

static void D3D9_RestoreRenderStates(void) {
	union IntAndFloat raw;
	IDirect3DDevice9_SetRenderState(device, D3DRS_ALPHATESTENABLE,   gfx_alphaTesting);
	IDirect3DDevice9_SetRenderState(device, D3DRS_ALPHABLENDENABLE, gfx_alphaBlending);

	IDirect3DDevice9_SetRenderState(device, D3DRS_FOGENABLE, gfx_fogEnabled);
	IDirect3DDevice9_SetRenderState(device, D3DRS_FOGCOLOR,  gfx_fogColor);
	raw.f = gfx_fogDensity;
	IDirect3DDevice9_SetRenderState(device, D3DRS_FOGDENSITY, raw.u);
	raw.f = gfx_fogEnd;
	IDirect3DDevice9_SetRenderState(device, D3DRS_FOGEND, raw.u);
	IDirect3DDevice9_SetRenderState(device, D3DRS_FOGTABLEMODE, gfx_fogMode);

	IDirect3DDevice9_SetRenderState(device, D3DRS_ZENABLE,      gfx_depthTesting);
	IDirect3DDevice9_SetRenderState(device, D3DRS_ZWRITEENABLE, gfx_depthWriting);
}


/*########################################################################################################################*
*-------------------------------------------------------Index buffers-----------------------------------------------------*
*#########################################################################################################################*/
static void D3D9_SetIbData(IDirect3DIndexBuffer9* buffer, void* data, int size) {
	void* dst = NULL;
	cc_result res = IDirect3DIndexBuffer9_Lock(buffer, 0, size, &dst, 0);
	if (res) Logger_Abort2(res, "D3D9_LockIb");

	Mem_Copy(dst, data, size);
	res = IDirect3DIndexBuffer9_Unlock(buffer);
	if (res) Logger_Abort2(res, "D3D9_UnlockIb");
}

GfxResourceID Gfx_CreateIb(void* indices, int indicesCount) {
	int size = indicesCount * 2;
	IDirect3DIndexBuffer9* ibuffer;
	cc_result res = IDirect3DDevice9_CreateIndexBuffer(device, size, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &ibuffer, NULL);
	if (res) Logger_Abort2(res, "D3D9_CreateIb");

	D3D9_SetIbData(ibuffer, indices, size);
	return ibuffer;
}

void Gfx_BindIb(GfxResourceID ib) {
	IDirect3DIndexBuffer9* ibuffer = (IDirect3DIndexBuffer9*)ib;
	cc_result res = IDirect3DDevice9_SetIndices(device, ibuffer);
	if (res) Logger_Abort2(res, "D3D9_BindIb");
}

void Gfx_DeleteIb(GfxResourceID* ib) { D3D9_FreeResource(ib); }


/*########################################################################################################################*
*------------------------------------------------------Vertex buffers-----------------------------------------------------*
*#########################################################################################################################*/
static IDirect3DVertexBuffer9* D3D9_AllocVertexBuffer(VertexFormat fmt, int count, DWORD usage) {
	IDirect3DVertexBuffer9* vbuffer;
	cc_result res;
	int size = count * strideSizes[fmt];

	for (;;) {
		res = IDirect3DDevice9_CreateVertexBuffer(device, size, usage,
					d3d9_formatMappings[fmt], D3DPOOL_DEFAULT, &vbuffer, NULL);
		if (D3D9_CheckResult(res, "D3D9_CreateVb failed")) break;
	}
	return vbuffer;
}

static void D3D9_SetVbData(IDirect3DVertexBuffer9* buffer, void* data, int size, int lockFlags) {
	void* dst = NULL;
	cc_result res = IDirect3DVertexBuffer9_Lock(buffer, 0, size, &dst, lockFlags);
	if (res) Logger_Abort2(res, "D3D9_LockVb");

	Mem_Copy(dst, data, size);
	res = IDirect3DVertexBuffer9_Unlock(buffer);
	if (res) Logger_Abort2(res, "D3D9_UnlockVb");
}

static void* D3D9_LockVb(GfxResourceID vb, VertexFormat fmt, int count, int lockFlags) {
	IDirect3DVertexBuffer9* buffer = (IDirect3DVertexBuffer9*)vb;
	void* dst = NULL;
	int size  = count * strideSizes[fmt];

	cc_result res = IDirect3DVertexBuffer9_Lock(buffer, 0, size, &dst, lockFlags);
	if (res) Logger_Abort2(res, "D3D9_LockVb");
	return dst;
}

GfxResourceID Gfx_CreateVb(VertexFormat fmt, int count) {
	return D3D9_AllocVertexBuffer(fmt, count, D3DUSAGE_WRITEONLY);
}

void Gfx_BindVb(GfxResourceID vb) {
	IDirect3DVertexBuffer9* vbuffer = (IDirect3DVertexBuffer9*)vb;
	cc_result res = IDirect3DDevice9_SetStreamSource(device, 0, vbuffer, 0, gfx_stride);
	if (res) Logger_Abort2(res, "D3D9_BindVb");
}

void Gfx_DeleteVb(GfxResourceID* vb) { D3D9_FreeResource(vb); }
void* Gfx_LockVb(GfxResourceID vb, VertexFormat fmt, int count) {
	return D3D9_LockVb(vb, fmt, count, 0);
}

void Gfx_UnlockVb(GfxResourceID vb) {
	IDirect3DVertexBuffer9* buffer = (IDirect3DVertexBuffer9*)vb;
	cc_result res = IDirect3DVertexBuffer9_Unlock(buffer);
	if (res) Logger_Abort2(res, "Gfx_UnlockVb");
}


void Gfx_SetVertexFormat(VertexFormat fmt) {
	cc_result res;
	if (fmt == gfx_format) return;
	gfx_format = fmt;

	res = IDirect3DDevice9_SetFVF(device, d3d9_formatMappings[fmt]);
	if (res) Logger_Abort2(res, "D3D9_SetVertexFormat");
	gfx_stride = strideSizes[fmt];
}

void Gfx_DrawVb_Lines(int verticesCount) {
	/* NOTE: Skip checking return result for Gfx_DrawXYZ for performance */
	IDirect3DDevice9_DrawPrimitive(device, D3DPT_LINELIST, 0, verticesCount >> 1);
}

void Gfx_DrawVb_IndexedTris(int verticesCount) {
	IDirect3DDevice9_DrawIndexedPrimitive(device, D3DPT_TRIANGLELIST,
		0, 0, verticesCount, 0, verticesCount >> 1);
}

void Gfx_DrawVb_IndexedTris_Range(int verticesCount, int startVertex) {
	IDirect3DDevice9_DrawIndexedPrimitive(device, D3DPT_TRIANGLELIST,
		startVertex, 0, verticesCount, 0, verticesCount >> 1);
}

void Gfx_DrawIndexedTris_T2fC4b(int verticesCount, int startVertex) {
	IDirect3DDevice9_DrawIndexedPrimitive(device, D3DPT_TRIANGLELIST,
		startVertex, 0, verticesCount, 0, verticesCount >> 1);
}


/*########################################################################################################################*
*--------------------------------------------------Dynamic vertex buffers-------------------------------------------------*
*#########################################################################################################################*/
GfxResourceID Gfx_CreateDynamicVb(VertexFormat fmt, int maxVertices) {
	if (Gfx.LostContext) return 0;
	return D3D9_AllocVertexBuffer(fmt, maxVertices, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY);
}

void* Gfx_LockDynamicVb(GfxResourceID vb, VertexFormat fmt, int count) {
	return D3D9_LockVb(vb, fmt, count, D3DLOCK_DISCARD);
}

void Gfx_UnlockDynamicVb(GfxResourceID vb) {
	Gfx_UnlockVb(vb);
	Gfx_BindVb(vb); /* TODO: Inline this? */
}

void Gfx_SetDynamicVbData(GfxResourceID vb, void* vertices, int vCount) {
	int size = vCount * gfx_stride;
	IDirect3DVertexBuffer9* buffer = (IDirect3DVertexBuffer9*)vb;
	D3D9_SetVbData(buffer, vertices, size, D3DLOCK_DISCARD);

	cc_result res = IDirect3DDevice9_SetStreamSource(device, 0, buffer, 0, gfx_stride);
	if (res) Logger_Abort2(res, "D3D9_SetDynamicVbData - Bind");
}


/*########################################################################################################################*
*---------------------------------------------------------Matrices--------------------------------------------------------*
*#########################################################################################################################*/
static D3DTRANSFORMSTATETYPE matrix_modes[2] = { D3DTS_PROJECTION, D3DTS_VIEW };

void Gfx_LoadMatrix(MatrixType type, const struct Matrix* matrix) {
	if (Gfx.LostContext) return;
	IDirect3DDevice9_SetTransform(device, matrix_modes[type], (const D3DMATRIX*)matrix);
}

void Gfx_LoadIdentityMatrix(MatrixType type) {
	if (Gfx.LostContext) return;
	IDirect3DDevice9_SetTransform(device, matrix_modes[type], (const D3DMATRIX*)&Matrix_Identity);
}

static struct Matrix texMatrix = Matrix_IdentityValue;
void Gfx_EnableTextureOffset(float x, float y) {
	texMatrix.row3.X = x; texMatrix.row3.Y = y;
	if (Gfx.LostContext) return;

	IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);
	IDirect3DDevice9_SetTransform(device, D3DTS_TEXTURE0, (const D3DMATRIX*)&texMatrix);
}

void Gfx_DisableTextureOffset(void) {
	if (Gfx.LostContext) return;
	IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
	IDirect3DDevice9_SetTransform(device, D3DTS_TEXTURE0, (const D3DMATRIX*)&Matrix_Identity);
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
	IDirect3DSurface9* backbuffer = NULL;
	IDirect3DSurface9* temp = NULL;
	D3DSURFACE_DESC desc;
	D3DLOCKED_RECT rect;
	struct Bitmap bmp;
	cc_result res;

	res = IDirect3DDevice9_GetBackBuffer(device, 0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer);
	if (res) goto finished;
	res = IDirect3DSurface9_GetDesc(backbuffer, &desc);
	if (res) goto finished;

	res = IDirect3DDevice9_CreateOffscreenPlainSurface(device, desc.Width, desc.Height, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &temp, NULL);
	if (res) goto finished; /* TODO: For DX 8 use IDirect3DDevice8::CreateImageSurface */
	res = IDirect3DDevice9_GetRenderTargetData(device, backbuffer, temp);
	if (res) goto finished;
	
	res = IDirect3DSurface9_LockRect(temp, &rect, NULL, D3DLOCK_READONLY | D3DLOCK_NO_DIRTY_UPDATE);
	if (res) goto finished;
	{
		Bitmap_Init(bmp, desc.Width, desc.Height, (BitmapCol*)rect.pBits);
		res = Png_Encode(&bmp, output, NULL, false);
		if (res) { IDirect3DSurface9_UnlockRect(temp); goto finished; }
	}
	res = IDirect3DSurface9_UnlockRect(temp);
	if (res) goto finished;

finished:
	D3D9_FreeResource(&backbuffer);
	D3D9_FreeResource(&temp);
	return res;
}

static void UpdateSwapchain(const char* reason) {
	D3DPRESENT_PARAMETERS args = { 0 };
	if (using_d3d9Ex) {
		/* Try to use ResetEx first to avoid resetting resources */
		IDirect3DDevice9Ex* dev = (IDirect3DDevice9Ex*)device;
		D3D9_FillPresentArgs(&args);

		if (!IDirect3DDevice9Ex_ResetEx(dev, &args, NULL)) {
			/* Fast path succeeded */
			D3D9_UpdateCachedDimensions(); return;
		}
	}
	Gfx_LoseContext(reason);
}

void Gfx_SetFpsLimit(cc_bool vsync, float minFrameMs) {
	gfx_minFrameMs = minFrameMs;
	if (gfx_vsync == vsync) return;

	gfx_vsync = vsync;
	if (device) UpdateSwapchain(" (toggling VSync)");
}

void Gfx_BeginFrame(void) { 
	IDirect3DDevice9_BeginScene(device);
	frameStart = Stopwatch_Measure();
}

void Gfx_Clear(void) {
	DWORD flags = D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER;
	cc_result res = IDirect3DDevice9_Clear(device, 0, NULL, flags, gfx_clearColor, 0.0f, 0);
	if (res) Logger_Abort2(res, "D3D9_Clear");
}

void Gfx_EndFrame(void) {
	IDirect3DDevice9_EndScene(device);
	cc_result res = IDirect3DDevice9_Present(device, NULL, NULL, NULL, NULL);

	/* Direct3D9Ex returns S_PRESENT_OCCLUDED when e.g. window is minimised */
	if (res && res != S_PRESENT_OCCLUDED) {
		if (res != D3DERR_DEVICELOST) Logger_Abort2(res, "D3D9_EndFrame");
		/* TODO: Make sure this actually works on all graphics cards. */
		Gfx_LoseContext(" (Direct3D9 device lost)");
	}
	if (gfx_minFrameMs) LimitFPS();
}

cc_bool Gfx_WarnIfNecessary(void) { return false; }
static const char* D3D9_StrFlags(void) {
	if (createFlags & D3DCREATE_HARDWARE_VERTEXPROCESSING) return "Hardware";
	if (createFlags & D3DCREATE_MIXED_VERTEXPROCESSING)    return "Mixed";
	if (createFlags & D3DCREATE_SOFTWARE_VERTEXPROCESSING) return "Software";
	return "(none)";
}

void Gfx_GetApiInfo(cc_string* info) {
	D3DADAPTER_IDENTIFIER9 adapter = { 0 };
	int pointerSize = sizeof(void*) * 8;
	float curMem;

	IDirect3D9_GetAdapterIdentifier(d3d, D3DADAPTER_DEFAULT, 0, &adapter);
	curMem = IDirect3DDevice9_GetAvailableTextureMem(device) / (1024.0f * 1024.0f);

	if (using_d3d9Ex) {
		String_Format1(info, "-- Using Direct3D9Ex (%i bit) --\n", &pointerSize);
	} else {
		String_Format1(info, "-- Using Direct3D9 (%i bit) --\n", &pointerSize);
	}

	String_Format1(info, "Adapter: %c\n",         adapter.Description);
	String_Format1(info, "Processing mode: %c\n", D3D9_StrFlags());
	String_Format2(info, "Video memory: %f2 MB total, %f2 free\n", &totalMem, &curMem);
	String_Format2(info, "Max texture size: (%i x %i)\n", &Gfx.MaxTexWidth, &Gfx.MaxTexHeight);
	String_Format1(info, "Depth buffer bits: %i", &depthBits);
}

void Gfx_OnWindowResize(void) {
	if (Game.Width == cachedWidth && Game.Height == cachedHeight) return;
	/* Only resize when necessary */
	UpdateSwapchain(" (resizing window)");
}
#endif
