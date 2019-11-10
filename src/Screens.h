#ifndef CC_SCREENS_H
#define CC_SCREENS_H
#include "String.h"
/* Contains all 2D non-menu screen implementations.
   Copyright 2014-2019 ClassiCube | Licensed under BSD-3
*/
struct Screen;
struct Widget;

/* These always return false */
int Screen_FInput(void* s, int key);
int Screen_FKeyPress(void* s, char keyChar);
int Screen_FText(void* s, const String* str);
int Screen_FMouseScroll(void* s, float delta);
int Screen_FPointer(void* s, int id, int x, int y);

/* These always return true */
int Screen_TInput(void* s, int key);
int Screen_TKeyPress(void* s, char keyChar);
int Screen_TText(void* s, const String* str);
int Screen_TMouseScroll(void* s, float delta);
int Screen_TPointer(void* s, int id, int x, int y);

void InventoryScreen_Show(void);
void HUDScreen_Show(void);
void LoadingScreen_Show(const String* title, const String* message);
void GeneratingScreen_Show(void);
void ChatScreen_Show(void);
void DisconnectScreen_Show(const String* title, const String* message);
#ifdef CC_BUILD_TOUCH
void TouchScreen_Show(void);
#endif

/* Raw pointer to loading screen. DO NOT USE THIS. Use LoadingScreen_MakeInstance() */
extern struct Screen* LoadingScreen_UNSAFE_RawPointer;
/* Opens chat input for the HUD with the given initial text. */
void ChatScreen_OpenInput(const String* text);
/* Appends text to the chat input in the HUD. */
void ChatScreen_AppendInput(const String* text);
void ChatScreen_SetChatlines(int lines);
struct Widget* ChatScreen_GetHotbar(void);
#endif
