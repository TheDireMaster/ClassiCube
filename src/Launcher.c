#include "Launcher.h"
#ifndef CC_BUILD_WEB
#include "String.h"
#include "LScreens.h"
#include "LWidgets.h"
#include "LWeb.h"
#include "Resources.h"
#include "Drawer2D.h"
#include "Game.h"
#include "Deflate.h"
#include "Stream.h"
#include "Utils.h"
#include "Input.h"
#include "Window.h"
#include "Event.h"
#include "Http.h"
#include "ExtMath.h"
#include "Funcs.h"
#include "Logger.h"
#include "Options.h"
#include "LBackend.h"
#include "PackedCol.h"

/* The area/region of the window that needs to be redrawn and presented to the screen. */
/* If width is 0, means no area needs to be redrawn. */
static Rect2D dirty_rect;

static struct LScreen* activeScreen;
struct Bitmap Launcher_Framebuffer;
struct FontDesc Launcher_TitleFont, Launcher_TextFont, Launcher_HintFont;

static cc_bool pendingRedraw;
static struct FontDesc logoFont;
static int titleX, titleY;

cc_bool Launcher_ShouldExit, Launcher_ShouldUpdate;
static char hashBuffer[STRING_SIZE], userBuffer[STRING_SIZE];
cc_string Launcher_AutoHash = String_FromArray(hashBuffer);
cc_string Launcher_Username = String_FromArray(userBuffer);

static cc_bool useBitmappedFont, hasBitmappedFont;

static void CloseActiveScreen(void) {
	Window_CloseKeyboard();
	if (activeScreen) activeScreen->Free(activeScreen);
}

void Launcher_SetScreen(struct LScreen* screen) {
	int i;
	CloseActiveScreen();
	activeScreen = screen;
	if (!screen->numWidgets) screen->Init(screen);

	screen->Show(screen);
	screen->Layout(screen);
	/* for hovering over active button etc */
	for (i = 0; i < Pointers_Count; i++) {
		screen->MouseMove(screen, i);
	}

	Launcher_Redraw();
}

void Launcher_DisplayHttpError(cc_result res, int status, const char* action, cc_string* dst) {
	if (res) {
		/* Non HTTP error - this is not good */
		Logger_Warn(res, action, Http_DescribeError);
		String_Format2(dst, "&cError %i when %c", &res, action);
	} else if (status != 200) {
		String_Format2(dst, "&c%i error when %c", &status, action);
	} else {
		String_Format1(dst, "&cEmpty response when %c", action);
	}
}

static CC_NOINLINE void InitFramebuffer(void) {
	Launcher_Framebuffer.width  = max(WindowInfo.Width,  1);
	Launcher_Framebuffer.height = max(WindowInfo.Height, 1);
	Window_AllocFramebuffer(&Launcher_Framebuffer);
}


/*########################################################################################################################*
*--------------------------------------------------------Starter/Updater--------------------------------------------------*
*#########################################################################################################################*/
static cc_uint64 lastJoin;
cc_bool Launcher_StartGame(const cc_string* user, const cc_string* mppass, const cc_string* ip, const cc_string* port, const cc_string* server) {
	cc_string args; char argsBuffer[512];
	TimeMS now;
	cc_result res;
	
	now = Stopwatch_Measure();
	if (Stopwatch_ElapsedMS(lastJoin, now) < 1000) return false;
	lastJoin = now;

	/* Save resume info */
	if (server->length) {
		Options_Set(ROPT_SERVER, server);
		Options_Set(ROPT_USER,   user);
		Options_Set(ROPT_IP,     ip);
		Options_Set(ROPT_PORT,   port);
		Options_SetSecure(ROPT_MPPASS, mppass);
	}
	/* Save options BEFORE starting new game process */
	/* Otherwise can get 'file already in use' errors on startup */
	Options_SaveIfChanged();

	String_InitArray(args, argsBuffer);
	String_AppendString(&args, user);
	if (mppass->length) String_Format3(&args, " %s %s %s", mppass, ip, port);

	res = Process_StartGame(&args);
	if (res) { Logger_SysWarn(res, "starting game"); return false; }

#ifdef CC_BUILD_MOBILE
	Launcher_ShouldExit = true;
#else
	Launcher_ShouldExit = Options_GetBool(OPT_AUTO_CLOSE_LAUNCHER, false);
#endif
	return true;
}

CC_NOINLINE static void StartFromInfo(struct ServerInfo* info) {
	cc_string port; char portBuffer[STRING_INT_CHARS];
	String_InitArray(port, portBuffer);

	String_AppendInt(&port, info->port);
	Launcher_StartGame(&Launcher_Username, &info->mppass, &info->ip, &port, &info->name);
}

cc_bool Launcher_ConnectToServer(const cc_string* hash) {
	struct ServerInfo* info;
	cc_string logMsg;
	int i;
	if (!hash->length) return false;

	for (i = 0; i < FetchServersTask.numServers; i++) {
		info = &FetchServersTask.servers[i];
		if (!String_Equals(hash, &info->hash)) continue;

		StartFromInfo(info);
		return true;
	}

	/* Fallback to private server handling */
	/* TODO: Rewrite to be async */
	FetchServerTask_Run(hash);

	while (!FetchServerTask.Base.completed) { 
		LWebTask_Tick(&FetchServerTask.Base);
		Thread_Sleep(10); 
	}

	if (FetchServerTask.server.hash.length) {
		StartFromInfo(&FetchServerTask.server);
		return true;
	} else if (FetchServerTask.Base.success) {
		Window_ShowDialog("Failed to connect", "No server has that hash");
	} else {
		logMsg = String_Init(NULL, 0, 0);
		LWebTask_DisplayError(&FetchServerTask.Base, "fetching server info", &logMsg);
	}
	return false;
}


/*########################################################################################################################*
*---------------------------------------------------------Event handler---------------------------------------------------*
*#########################################################################################################################*/
static void ReqeustRedraw(void* obj) {
	/* We may get multiple Redraw events in short timespan */
	/* So we just request a redraw at next launcher tick */
	pendingRedraw  = true;
	Launcher_MarkAllDirty();
}

static void OnResize(void* obj) {
	Window_FreeFramebuffer(&Launcher_Framebuffer);
	InitFramebuffer();

	if (activeScreen) activeScreen->Layout(activeScreen);
	Launcher_Redraw();
}

static cc_bool IsShutdown(int key) {
	if (key == KEY_F4 && Key_IsAltPressed()) return true;

	/* On macOS, Cmd+Q should also end the process */
#ifdef CC_BUILD_DARWIN
	return key == 'Q' && Key_IsWinPressed();
#else
	return false;
#endif
}

static void OnInputDown(void* obj, int key, cc_bool was) {
	if (IsShutdown(key)) Launcher_ShouldExit = true;
	activeScreen->KeyDown(activeScreen, key, was);
}

static void OnKeyPress(void* obj, int c) {
	activeScreen->KeyPress(activeScreen, c);
}

static void OnTextChanged(void* obj, const cc_string* str) {
	activeScreen->TextChanged(activeScreen, str);
}

static void OnMouseWheel(void* obj, float delta) {
	activeScreen->MouseWheel(activeScreen, delta);
}

static void OnPointerDown(void* obj, int idx) {
	activeScreen->MouseDown(activeScreen, idx);
}

static void OnPointerUp(void* obj, int idx) {
	activeScreen->MouseUp(activeScreen, idx);
}

static void OnPointerMove(void* obj, int idx) {
	if (!activeScreen) return;
	activeScreen->MouseMove(activeScreen, idx);
}


/*########################################################################################################################*
*-----------------------------------------------------------Main body-----------------------------------------------------*
*#########################################################################################################################*/
static void Launcher_Display(void) {
	if (pendingRedraw) {
		Launcher_Redraw();
		pendingRedraw = false;
	}

	Window_DrawFramebuffer(dirty_rect);
	dirty_rect.X = 0; dirty_rect.Width   = 0;
	dirty_rect.Y = 0; dirty_rect.Height  = 0;
}

static void Launcher_Init(void) {
	Event_Register_(&WindowEvents.Resized,      NULL, OnResize);
	Event_Register_(&WindowEvents.StateChanged, NULL, OnResize);
	Event_Register_(&WindowEvents.Redraw,       NULL, ReqeustRedraw);

	Event_Register_(&InputEvents.Down,        NULL, OnInputDown);
	Event_Register_(&InputEvents.Press,       NULL, OnKeyPress);
	Event_Register_(&InputEvents.Wheel,       NULL, OnMouseWheel);
	Event_Register_(&InputEvents.TextChanged, NULL, OnTextChanged);

	Event_Register_(&PointerEvents.Down,  NULL, OnPointerDown);
	Event_Register_(&PointerEvents.Up,    NULL, OnPointerUp);
	Event_Register_(&PointerEvents.Moved, NULL, OnPointerMove);

	Drawer2D_MakeFont(&Launcher_TitleFont, 16, FONT_FLAGS_BOLD);
	Drawer2D_MakeFont(&Launcher_TextFont,  14, FONT_FLAGS_NONE);
	Drawer2D_MakeFont(&Launcher_HintFont,  12, FONT_FLAGS_NONE);
	titleX = Display_ScaleX(4); titleY = Display_ScaleY(4);

	Drawer2D.Colors['g'] = BitmapCol_Make(125, 125, 125, 255);
	Utils_EnsureDirectory("texpacks");
	Utils_EnsureDirectory("audio");
}

static void Launcher_Free(void) {
	Event_UnregisterAll();
	Flags_Free();
	Font_Free(&logoFont);
	Font_Free(&Launcher_TitleFont);
	Font_Free(&Launcher_TextFont);
	Font_Free(&Launcher_HintFont);
	hasBitmappedFont = false;

	CloseActiveScreen();
	activeScreen = NULL;
	Window_FreeFramebuffer(&Launcher_Framebuffer);
}

void Launcher_Run(void) {
	static const cc_string title = String_FromConst(GAME_APP_TITLE);
	Window_Create2D(640, 400);
#ifdef CC_BUILD_MOBILE
	Window_LockLandscapeOrientation(Options_GetBool(OPT_LANDSCAPE_MODE, false));
#endif
	Window_SetTitle(&title);
	Window_Show();
	LWidget_CalcOffsets();
	LBackend_CalcOffsets();

#ifdef CC_BUILD_WIN
	/* clean leftover exe from updating */
	if (Options_GetBool("update-dirty", false) && Updater_Clean()) {
		Options_Set("update-dirty", NULL);
	}
#endif

	Drawer2D_Component.Init();
	Drawer2D.BitmappedText    = false;
	Drawer2D.BlackTextShadows = true;
	InitFramebuffer();

	Options_Get(LOPT_USERNAME, &Launcher_Username, "");
	LWebTasks_Init();
	Session_Load();
	Launcher_LoadTheme();
	Launcher_Init();
	Launcher_TryLoadTexturePack();

	Http_Component.Init();
	Resources_CheckExistence();
	CheckUpdateTask_Run();

	if (Resources_Count) {
		CheckResourcesScreen_SetActive();
	} else {
		MainScreen_SetActive();
	}

	for (;;) {
		Window_ProcessEvents();
		if (!WindowInfo.Exists || Launcher_ShouldExit) break;

		activeScreen->Tick(activeScreen);
		if (dirty_rect.Width) Launcher_Display();
		Thread_Sleep(10);
	}

	Options_SaveIfChanged();
	Launcher_Free();

#ifdef CC_BUILD_MOBILE
	/* infinite loop on mobile */
	Launcher_ShouldExit = false;
	/* Reset components */
	Platform_LogConst("undoing components");
	Drawer2D_Component.Free();
	Http_Component.Free();
#else
	if (Launcher_ShouldUpdate) {
		const char* action;
		cc_result res = Updater_Start(&action);
		if (res) Logger_SysWarn(res, action);
	}

	if (WindowInfo.Exists) Window_Close();
#endif
}


/*########################################################################################################################*
*---------------------------------------------------------Colours/Skin----------------------------------------------------*
*#########################################################################################################################*/
struct LauncherTheme Launcher_Theme;
const struct LauncherTheme Launcher_ModernTheme = {
	false,
	BitmapCol_Make(153, 127, 172, 255), /* background */
	BitmapCol_Make( 97,  81, 110, 255), /* button border */
	BitmapCol_Make(189, 168, 206, 255), /* active button */
	BitmapCol_Make(141, 114, 165, 255), /* button foreground */
	BitmapCol_Make(162, 131, 186, 255), /* button highlight */
};
const struct LauncherTheme Launcher_ClassicTheme = {
	true,
	BitmapCol_Make( 41,  41,  41, 255), /* background */
	BitmapCol_Make(  0,   0,   0, 255), /* button border */
	BitmapCol_Make(126, 136, 191, 255), /* active button */
	BitmapCol_Make(111, 111, 111, 255), /* button foreground */
	BitmapCol_Make(168, 168, 168, 255), /* button highlight */
};

CC_NOINLINE static void Launcher_GetCol(const char* key, BitmapCol* col) {
	cc_uint8 rgb[3];
	cc_string value;
	if (!Options_UNSAFE_Get(key, &value))    return;
	if (!PackedCol_TryParseHex(&value, rgb)) return;

	*col = BitmapCol_Make(rgb[0], rgb[1], rgb[2], 255);
}

void Launcher_LoadTheme(void) {
	if (Options_GetBool(OPT_CLASSIC_MODE, false)) {
		Launcher_Theme = Launcher_ClassicTheme;
		return;
	}
	Launcher_Theme = Launcher_ModernTheme;
	Launcher_Theme.ClassicBackground = Options_GetBool("nostalgia-classicbg", false);

	Launcher_GetCol("launcher-back-col",                   &Launcher_Theme.BackgroundColor);
	Launcher_GetCol("launcher-btn-border-col",             &Launcher_Theme.ButtonBorderColor);
	Launcher_GetCol("launcher-btn-fore-active-col",        &Launcher_Theme.ButtonForeActiveColor);
	Launcher_GetCol("launcher-btn-fore-inactive-col",      &Launcher_Theme.ButtonForeColor);
	Launcher_GetCol("launcher-btn-highlight-inactive-col", &Launcher_Theme.ButtonHighlightColor);
}

CC_NOINLINE static void Launcher_SetCol(const char* key, BitmapCol col) {
	cc_string value; char valueBuffer[8];
	/* Component order might be different to BitmapCol */
	PackedCol tmp = PackedCol_Make(BitmapCol_R(col), BitmapCol_G(col), BitmapCol_B(col), 0);
	
	String_InitArray(value, valueBuffer);
	PackedCol_ToHex(&value, tmp);
	Options_Set(key, &value);
}

void Launcher_SaveTheme(void) {
	Launcher_SetCol("launcher-back-col",                   Launcher_Theme.BackgroundColor);
	Launcher_SetCol("launcher-btn-border-col",             Launcher_Theme.ButtonBorderColor);
	Launcher_SetCol("launcher-btn-fore-active-col",        Launcher_Theme.ButtonForeActiveColor);
	Launcher_SetCol("launcher-btn-fore-inactive-col",      Launcher_Theme.ButtonForeColor);
	Launcher_SetCol("launcher-btn-highlight-inactive-col", Launcher_Theme.ButtonHighlightColor);
	Options_SetBool("nostalgia-classicbg",                 Launcher_Theme.ClassicBackground);
}


/*########################################################################################################################*
*----------------------------------------------------------Background-----------------------------------------------------*
*#########################################################################################################################*/
static cc_bool Launcher_SelectZipEntry(const cc_string* path) {
	return
		String_CaselessEqualsConst(path, "default.png") ||
		String_CaselessEqualsConst(path, "terrain.png");
}

static cc_result Launcher_ProcessZipEntry(const cc_string* path, struct Stream* data, struct ZipState* s) {
	struct Bitmap bmp;
	cc_result res;

	if (String_CaselessEqualsConst(path, "default.png")) {
		if (hasBitmappedFont) return 0;
		hasBitmappedFont = false;
		res = Png_Decode(&bmp, data);

		if (res) {
			Logger_SysWarn(res, "decoding default.png"); return res;
		} else if (Drawer2D_SetFontBitmap(&bmp)) {
			useBitmappedFont = !Options_GetBool(OPT_USE_CHAT_FONT, false);
			hasBitmappedFont = true;
		} else {
			Mem_Free(bmp.scan0);
		}
	} else if (String_CaselessEqualsConst(path, "terrain.png")) {
		if (LBackend_HasTextures()) return 0;
		res = Png_Decode(&bmp, data);

		if (res) {
			Logger_SysWarn(res, "decoding terrain.png"); return res;
		} else {
			LBackend_LoadTextures(&bmp);
		}
	}
	return 0;
}

static void ExtractTexturePack(const cc_string* path) {
	struct ZipState state;
	struct Stream stream;
	cc_result res;

	res = Stream_OpenFile(&stream, path);
	if (res == ReturnCode_FileNotFound) return;
	if (res) { Logger_SysWarn(res, "opening texture pack"); return; }

	Zip_Init(&state, &stream);
	state.SelectEntry  = Launcher_SelectZipEntry;
	state.ProcessEntry = Launcher_ProcessZipEntry;
	res = Zip_Extract(&state);

	if (res) { Logger_SysWarn(res, "extracting texture pack"); }
	stream.Close(&stream);
}

void Launcher_TryLoadTexturePack(void) {
	static const cc_string defZip = String_FromConst("texpacks/default.zip");
	cc_string path; char pathBuffer[FILENAME_SIZE];
	cc_string texPack;

	if (Options_UNSAFE_Get(OPT_DEFAULT_TEX_PACK, &texPack)) {
		String_InitArray(path, pathBuffer);
		String_Format1(&path, "texpacks/%s", &texPack);
		ExtractTexturePack(&path);
	}

	/* user selected texture pack is missing some required .png files */
	if (!hasBitmappedFont || !LBackend_HasTextures()) ExtractTexturePack(&defZip);
	Launcher_UpdateLogoFont();
}

void Launcher_UpdateLogoFont(void) {
	Font_Free(&logoFont);
	Drawer2D.BitmappedText = (useBitmappedFont || Launcher_Theme.ClassicBackground) && hasBitmappedFont;
	Drawer2D_MakeFont(&logoFont, 32, FONT_FLAGS_NONE);
	Drawer2D.BitmappedText = false;
}

void Launcher_ResetArea(int x, int y, int width, int height) {
	LBackend_ResetArea(x, y, width, height);
	Launcher_MarkDirty(x, y, width, height);
}

void Launcher_ResetPixels(void) {
	cc_string title_fore, title_back;
	struct DrawTextArgs args;
	int x;

	if (activeScreen && !activeScreen->title_fore) {
		Launcher_ResetArea(0, 0, WindowInfo.Width, WindowInfo.Height);
		return;
	}
	title_fore = String_FromReadonly(activeScreen->title_fore);
	title_back = String_FromReadonly(activeScreen->title_back);

	LBackend_ResetPixels();
	DrawTextArgs_Make(&args, &title_fore, &logoFont, false);
	x = WindowInfo.Width / 2 - Drawer2D_TextWidth(&args) / 2;

	args.text = title_back;
	Drawer2D_DrawText(&Launcher_Framebuffer, &args, x + titleX, titleY);
	args.text = title_fore;
	Drawer2D_DrawText(&Launcher_Framebuffer, &args, x, 0);
	Launcher_MarkAllDirty();
}

void Launcher_Redraw(void) {
	Launcher_ResetPixels();
	activeScreen->Draw(activeScreen);
	Launcher_MarkAllDirty();
}

void Launcher_MarkDirty(int x, int y, int width, int height) {
	int x1, y1, x2, y2;
	if (!Drawer2D_Clamp(&Launcher_Framebuffer, &x, &y, &width, &height)) return;

	/* union with existing dirty area */
	if (dirty_rect.Width) {
		x1 = min(x, dirty_rect.X);
		y1 = min(y, dirty_rect.Y);

		x2 = max(x +  width, dirty_rect.X + dirty_rect.Width);
		y2 = max(y + height, dirty_rect.Y + dirty_rect.Height);

		x = x1; width  = x2 - x1;
		y = y1; height = y2 - y1;
	}

	dirty_rect.X = x; dirty_rect.Width  = width;
	dirty_rect.Y = y; dirty_rect.Height = height;
}

void Launcher_MarkAllDirty(void) {
	dirty_rect.X = 0; dirty_rect.Width  = Launcher_Framebuffer.width;
	dirty_rect.Y = 0; dirty_rect.Height = Launcher_Framebuffer.height;
}
#endif
