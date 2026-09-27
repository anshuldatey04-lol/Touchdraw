#include <stdio.h>

#include "fileio.h"
#include "termcolor.h"
#include "utils.h"

#define TDRAW_MAGIC 0x315741524454u  // "TDRAW1" as bytes T,D,R,A,W,1
#define TDRAW_VERSION 1u

static int WriteAll(HANDLE hf, const void* buf, DWORD len)
{
  DWORD written = 0;
  if (!WriteFile(hf, buf, len, &written, NULL) || (written != len))
  {
    return -1;
  }
  return 0;
}

static int ReadAll(HANDLE hf, void* buf, DWORD len)
{
  DWORD got = 0;
  if (!ReadFile(hf, buf, len, &got, NULL) || (got != len))
  {
    return -1;
  }
  return 0;
}

int File_WriteNative(const TCHAR* path, const StrokeList* strokes)
{
  HANDLE hf = CreateFile(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  unsigned int i;
  unsigned int j;
  if (hf == INVALID_HANDLE_VALUE)
  {
    return -1;
  }
  {
    DWORD magic   = (DWORD)TDRAW_MAGIC;
    DWORD version = (DWORD)TDRAW_VERSION;
    DWORD count   = (strokes != NULL) ? strokes->Size : 0;
    int ok = (WriteAll(hf, &magic, 4) == 0) && (WriteAll(hf, &version, 4) == 0) && (WriteAll(hf, &count, 4) == 0);
    if (ok && (strokes != NULL))
    {
      for (i = 0; (i < strokes->Size) && ok; i++)
      {
        const Stroke* s = &strokes->Entries[i];
        DWORD color = (DWORD)s->Color;
        LONG width  = (LONG)s->Width;
        DWORD npts  = s->Points.Size;
        ok = (WriteAll(hf, &color, 4) == 0) && (WriteAll(hf, &width, 4) == 0) && (WriteAll(hf, &npts, 4) == 0);
        for (j = 0; (j < npts) && ok; j++)
        {
          DWORD x = (DWORD)s->Points.Entries[j].X;
          DWORD y = (DWORD)s->Points.Entries[j].Y;
          ok = (WriteAll(hf, &x, 4) == 0) && (WriteAll(hf, &y, 4) == 0);
        }
      }
    }
    CloseHandle(hf);
    return ok ? 0 : -1;
  }
}

int File_ReadNative(const TCHAR* path, StrokeList* strokes)
{
  HANDLE hf = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  DWORD magic   = 0;
  DWORD version = 0;
  DWORD count   = 0;
  DWORD i;
  DWORD j;
  StrokeList loaded = {0};
  if ((hf == INVALID_HANDLE_VALUE) || (strokes == NULL))
  {
    if (hf != INVALID_HANDLE_VALUE)
    {
      CloseHandle(hf);
    }
    return -1;
  }
  if ((ReadAll(hf, &magic, 4) != 0) || (ReadAll(hf, &version, 4) != 0) || (ReadAll(hf, &count, 4) != 0))
  {
    CloseHandle(hf);
    return -1;
  }
  if ((magic != (DWORD)TDRAW_MAGIC) || (version != (DWORD)TDRAW_VERSION) || (count > 100000))
  {
    CloseHandle(hf);
    return -1;
  }
  for (i = 0; i < count; i++)
  {
    DWORD color = 0;
    LONG width  = 0;
    DWORD npts  = 0;
    Stroke s;
    s.Points.Entries  = NULL;
    s.Points.Size     = 0;
    s.Points.Capacity = 0;
    s.Color           = RGB(255, 255, 255);
    s.Width           = 8;
    if ((ReadAll(hf, &color, 4) != 0) || (ReadAll(hf, &width, 4) != 0) || (ReadAll(hf, &npts, 4) != 0) || (npts > 1000000))
    {
      mFreeStroke(&s);
      mFreeStrokeList(&loaded);
      CloseHandle(hf);
      return -1;
    }
    s.Color = (COLORREF)color;
    s.Width = (int)width;
    if ((s.Width < 1) || (s.Width > 256))
    {
      s.Width = 8;
    }
    if (npts > 0)
    {
      s.Points.Entries = (Point2D*)mMalloc(sizeof(Point2D) * npts, __FILE__, __LINE__);
      for (j = 0; j < npts; j++)
      {
        DWORD x = 0;
        DWORD y = 0;
        if ((ReadAll(hf, &x, 4) != 0) || (ReadAll(hf, &y, 4) != 0))
        {
          mFreeStroke(&s);
          mFreeStrokeList(&loaded);
          CloseHandle(hf);
          return -1;
        }
        s.Points.Entries[j].X = (ULONG)x;
        s.Points.Entries[j].Y = (ULONG)y;
      }
      s.Points.Size     = npts;
      s.Points.Capacity = npts;
    }
    // Append to loaded list (capacity-doubled).
    if (loaded.Size >= loaded.Capacity)
    {
      unsigned int newCap = (loaded.Capacity == 0) ? 4 : (loaded.Capacity * 2);
      Stroke* grown       = (Stroke*)mMalloc(sizeof(Stroke) * newCap, __FILE__, __LINE__);
      for (j = 0; j < loaded.Size; j++)
      {
        grown[j] = loaded.Entries[j];
      }
      free(loaded.Entries);
      loaded.Entries  = grown;
      loaded.Capacity = newCap;
    }
    loaded.Entries[loaded.Size] = s;
    loaded.Size++;
  }
  CloseHandle(hf);
  mFreeStrokeList(strokes);
  *strokes = loaded;
  return 0;
}

int File_ExportBMP(const TCHAR* path, int canvasW, int canvasH, COLORREF bg, const StrokeList* strokes, PadMapFn mapFn)
{
  BITMAPINFO bmi;
  HDC hdcScreen;
  HDC hdcMem;
  HBITMAP hBmp;
  HBITMAP hOld;
  HGDIOBJ hOldBrush;
  int stride;
  BITMAPFILEHEADER bfh;
  HANDLE hf;
  DWORD written   = 0;
  DWORD imageSize = 0;
  unsigned int si;
  unsigned int pi;
  BYTE* bits = NULL;

  if ((canvasW <= 0) || (canvasH <= 0) || (mapFn == NULL))
  {
    return -1;
  }

  ZeroMemory(&bmi, sizeof(bmi));
  bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth       = canvasW;
  bmi.bmiHeader.biHeight      = -canvasH;  // top-down DIB, no row flip needed
  bmi.bmiHeader.biPlanes      = 1;
  bmi.bmiHeader.biBitCount    = 24;
  bmi.bmiHeader.biCompression = BI_RGB;

  stride    = ((canvasW * 3 + 3) / 4) * 4;
  imageSize = (DWORD)(stride * canvasH);

  hdcScreen = GetDC(NULL);
  hdcMem    = CreateCompatibleDC(hdcScreen);
  hBmp      = CreateDIBSection(hdcScreen, &bmi, DIB_RGB_COLORS, (void**)&bits, NULL, 0);
  ReleaseDC(NULL, hdcScreen);
  if ((hdcMem == NULL) || (hBmp == NULL) || (bits == NULL))
  {
    if (hdcMem != NULL)
    {
      DeleteDC(hdcMem);
    }
    if (hBmp != NULL)
    {
      DeleteObject(hBmp);
    }
    return -1;
  }
  hOld = (HBITMAP)SelectObject(hdcMem, hBmp);

  // Background.
  {
    HBRUSH bgBrush = CreateSolidBrush(bg);
    RECT rc        = {0, 0, canvasW, canvasH};
    FillRect(hdcMem, &rc, bgBrush);
    DeleteObject(bgBrush);
  }

  // Strokes (same mapping as the live canvas; letterbox bars stay bg color).
  if (strokes != NULL)
  {
    for (si = 0; si < strokes->Size; si++)
    {
      const Stroke* s = &strokes->Entries[si];
      HPEN pen;
      HGDIOBJ oldPen;
      HBRUSH dot;
      if (s->Points.Size == 0)
      {
        continue;
      }
      pen    = CreatePen(PS_SOLID, (s->Width < 1) ? 1 : s->Width, s->Color);
      oldPen = SelectObject(hdcMem, pen);
      dot    = CreateSolidBrush(s->Color);
      hOldBrush = SelectObject(hdcMem, dot);
      if (s->Points.Size == 1)
      {
        int dx = 0;
        int dy = 0;
        int r  = s->Width / 2;
        if (r < 1)
        {
          r = 1;
        }
        mapFn(s->Points.Entries[0].X, s->Points.Entries[0].Y, &dx, &dy);
        Ellipse(hdcMem, dx - r, dy - r, dx + r, dy + r);
      }
      else
      {
        int mx = 0;
        int my = 0;
        mapFn(s->Points.Entries[0].X, s->Points.Entries[0].Y, &mx, &my);
        MoveToEx(hdcMem, mx, my, NULL);
        for (pi = 1; pi < s->Points.Size; pi++)
        {
          int px = 0;
          int py = 0;
          mapFn(s->Points.Entries[pi].X, s->Points.Entries[pi].Y, &px, &py);
          LineTo(hdcMem, px, py);
        }
      }
      SelectObject(hdcMem, hOldBrush);
      SelectObject(hdcMem, oldPen);
      DeleteObject(dot);
      DeleteObject(pen);
    }
  }
  SelectObject(hdcMem, hOld);

  // NOTE: mapFn must be canvas-relative (no toolbar-strip offset); the BMP
  // covers the canvas area only. main.c passes a wrapper for this.
  bfh.bfType      = 0x4D42;  // 'BM'
  bfh.bfSize      = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + imageSize;
  bfh.bfReserved1 = 0;
  bfh.bfReserved2 = 0;
  bmi.bmiHeader.biHeight = canvasH;  // bottom-up for file storage
  bfh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);

  hf = CreateFile(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (hf == INVALID_HANDLE_VALUE)
  {
    DeleteDC(hdcMem);
    DeleteObject(hBmp);
    return -1;
  }
  {
    // Convert top-down DIB rows to bottom-up for the file.
    BYTE* flipped = (BYTE*)mMalloc(imageSize, __FILE__, __LINE__);
    int y;
    int ok = 1;
    for (y = 0; y < canvasH; y++)
    {
      memcpy(flipped + (size_t)y * stride, bits + (size_t)(canvasH - 1 - y) * stride, stride);
    }
    ok = WriteFile(hf, &bfh, sizeof(bfh), &written, NULL) && (written == sizeof(bfh));
    if (ok)
    {
      ok = WriteFile(hf, &bmi.bmiHeader, sizeof(BITMAPINFOHEADER), &written, NULL) && (written == sizeof(BITMAPINFOHEADER));
    }
    if (ok)
    {
      ok = WriteFile(hf, flipped, imageSize, &written, NULL) && (written == imageSize);
    }
    free(flipped);
    CloseHandle(hf);
    DeleteDC(hdcMem);
    DeleteObject(hBmp);
    return ok ? 0 : -1;
  }
}

static int File_Dialog(HWND owner, TCHAR* pathBuf, int pathBufCch, const TCHAR* filter, const TCHAR* defExt, DWORD flags)
{
  OPENFILENAME ofn;
  if ((pathBuf == NULL) || (pathBufCch <= 0))
  {
    return -1;
  }
  ZeroMemory(&ofn, sizeof(ofn));
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner   = owner;
  ofn.lpstrFile   = pathBuf;
  ofn.nMaxFile    = (DWORD)pathBufCch;
  ofn.lpstrFilter = filter;
  ofn.lpstrDefExt = defExt;
  ofn.Flags       = flags;
  if (flags & OFN_OVERWRITEPROMPT)
  {
    if (!GetSaveFileName(&ofn))
    {
      return (CommDlgExtendedError() == 0) ? 0 : -1;
    }
    return 1;
  }
  if (!GetOpenFileName(&ofn))
  {
    return (CommDlgExtendedError() == 0) ? 0 : -1;
  }
  return 1;
}

int File_SaveDialog(HWND owner, TCHAR* pathBuf, int pathBufCch)
{
  return File_Dialog(owner, pathBuf, pathBufCch, TEXT("TouchDraw drawings (*.tdraw)\0*.tdraw\0All files (*.*)\0*.*\0\0"), TEXT("tdraw"), OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST);
}

int File_OpenDialog(HWND owner, TCHAR* pathBuf, int pathBufCch)
{
  return File_Dialog(owner, pathBuf, pathBufCch, TEXT("TouchDraw drawings (*.tdraw)\0*.tdraw\0All files (*.*)\0*.*\0\0"), TEXT("tdraw"), OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST);
}

int File_ExportDialog(HWND owner, TCHAR* pathBuf, int pathBufCch)
{
  return File_Dialog(owner, pathBuf, pathBufCch, TEXT("Bitmap (*.bmp)\0*.bmp\0All files (*.*)\0*.*\0\0"), TEXT("bmp"), OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST);
}
