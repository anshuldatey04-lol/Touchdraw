#ifndef __FILEIO_H__
#define __FILEIO_H__
#include <Windows.h>
#include <tchar.h>
#include "stroke.h"

// Pad->client mapping callback (implemented in main.c as mMapPadToClient).
typedef int (*PadMapFn)(ULONG padX, ULONG padY, int* outX, int* outY);

// Native format: magic "TDRAW1", versioned, stores strokes with color/width
// so a saved file reopens fully editable.
int File_WriteNative(const TCHAR* path, const StrokeList* strokes);
int File_ReadNative(const TCHAR* path, StrokeList* strokes);

// Renders the canvas (bg color + mapped strokes) to a 24-bit BMP file.
int File_ExportBMP(const TCHAR* path, int canvasW, int canvasH, COLORREF bg, const StrokeList* strokes, PadMapFn mapFn);

// File dialogs. Return 1 when the user picked a path (written to pathBuf),
// 0 on cancel, -1 on dialog error.
int File_SaveDialog(HWND owner, TCHAR* pathBuf, int pathBufCch);
int File_OpenDialog(HWND owner, TCHAR* pathBuf, int pathBufCch);
int File_ExportDialog(HWND owner, TCHAR* pathBuf, int pathBufCch);
#endif  // __FILEIO_H__
