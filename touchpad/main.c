#include <Windows.h>
#include <windowsx.h>

#include <hidusage.h>
#include <hidpi.h>
#pragma comment(lib, "hid.lib")

#include <Commdlg.h>
#pragma comment(lib, "comdlg32.lib")

#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <tchar.h>

#include "termcolor.h"
#include "utils.h"
#include "touchpad.h"
#include "touchevents.h"
#include "point2d.h"
#include "stroke.h"
#include "history.h"
#include "fileio.h"

#define LOG_EVERY_INPUT_MESSAGES
#undef LOG_EVERY_INPUT_MESSAGES

static TCHAR szWindowClass[] = _T("TouchDraw");
static TCHAR szTitle[]       = _T("TouchDraw - F3 draw | ESC normal | E pen/eraser | 1-5 colors | [ ] brush | Ctrl+Z/Y undo/redo | Ctrl+S save | Ctrl+O open | C clear");

// https://docs.microsoft.com/en-us/windows/win32/inputdev/virtual-key-codes
#define VK_C_KEY 0x43;
#define VK_Q_KEY 0x51;
#define VK_S_KEY 0x53;
#define VK_E_KEY 0x45

// Drawing tools.
#define TOOL_PEN 0
#define TOOL_ERASER 1

// In-window toolbar strip height (client pixels).
#define TOOLBAR_H 64

// Brush width limits.
#define BRUSH_MIN 1
#define BRUSH_MAX 64
#define BRUSH_DEFAULT 8

// Toolbar buttons (order matches Toolbar_Layout).
#define TB_PEN 0
#define TB_ERASER 1
#define TB_C0 2
#define TB_C1 3
#define TB_C2 4
#define TB_C3 5
#define TB_C4 6
#define TB_CUSTOM 7
#define TB_SMALLER 8
#define TB_BIGGER 9
#define TB_UNDO 10
#define TB_REDO 11
#define TB_CLEAR 12
#define TB_SAVE 13
#define TB_OPEN 14
#define TB_BMP 15
#define TB_COUNT 16

struct ApplicationState
{
  HID_DEVICE_INFO_LIST device_info_list;
  TOUCH_DATA_LIST previous_touches;
  StrokeList strokes;
  ULONG tracking_touch_id;
  // flag to toggle drawing state
  int is_drawing;

  // TODO add option to change the key bindings
  // TODO add option to add multiple key bindings (array of key bindings for each actions)

  int turn_off_drawing_key_code;
  int turn_on_drawing_key_code;
  int export_writing_data_key_code;
  int quit_application_key_code;
  int clear_drawing_canvas_key_code;
  // Current tool / brush state.
  int tool;
  COLORREF curColor;
  int curWidth;
  // Absolute pad -> canvas mapping (letterboxed, see mComputePadRanges /
  // mUpdateCanvasMap). Raw HidP_GetUsageValue coordinates are normalized
  // against the HID logical range, then scaled to fit the canvas area
  // (client rect minus the toolbar strip), preserving aspect ratio.
  int mapValid;
  LONG padLogMinX;
  LONG padLogMaxX;
  LONG padLogMinY;
  LONG padLogMaxY;
  int canvasW;
  int canvasH;
  double mapScale;
  double mapOffX;
  double mapOffY;
  // Undo/redo stacks (snapshots, HIST_MAX_DEPTH deep).
  History hist;
  // Current file for Save (Ctrl+S / Save button reuse it; empty = prompt).
  TCHAR curFile[MAX_PATH];
  int curFileSet;
  // Eraser-gesture bookkeeping: push exactly one undo snapshot per gesture,
  // and only if the gesture actually removed something.
  int eraseActive;
  int erasePushed;
};

typedef struct ApplicationState ApplicationState;

static ApplicationState* g_app_state = NULL;

// Draw-mode cursor confinement. The old build swallowed ALL mouse input with
// a WH_MOUSE_LL hook, which would make the in-window toolbar unclickable.
// Instead we confine the (hidden) cursor to our window with ClipCursor: the
// touchpad's relative cursor motion can no longer escape and click other
// apps, while toolbar clicks (touchpad tap-to-click, buttons, external
// mouse) keep working. Raw absolute touch data is unaffected (RIDEV_INPUTSINK).
void mEnterDrawMode(HWND hwnd)
{
  if (g_app_state->is_drawing != 0)
  {
    return;
  }
  g_app_state->is_drawing = 1;
  RECT winRect;
  GetWindowRect(hwnd, &winRect);
  ClipCursor(&winRect);
  ShowCursor(FALSE);
  printf(FG_GREEN);
  printf("Draw mode ON (cursor confined to window, hidden). ESC to exit.\n");
  printf(RESET_COLOR);
  InvalidateRect(hwnd, NULL, FALSE);
}

void mExitDrawMode(HWND hwnd)
{
  if (g_app_state->is_drawing == 0)
  {
    return;
  }
  g_app_state->is_drawing = 0;
  g_app_state->tracking_touch_id = (ULONG)-1;
  g_app_state->eraseActive = 0;
  ClipCursor(NULL);
  ShowCursor(TRUE);
  printf(FG_GREEN);
  printf("Draw mode OFF (cursor restored).\n");
  printf(RESET_COLOR);
  InvalidateRect(hwnd, NULL, FALSE);
}

void mParseConnectedInputDevices()
{
  printf(FG_BLUE);
  printf("Parsing all HID devices...\n");
  printf(RESET_COLOR);

  // find number of connected devices

  UINT numDevices;
  RAWINPUTDEVICELIST* rawInputDeviceList = NULL;

  mGetRawInputDeviceList(&numDevices, &rawInputDeviceList);

  printf("Number of raw input devices: %d\n", numDevices);
  for (UINT deviceIndex = 0; deviceIndex < numDevices; deviceIndex++)
  {
    printf(BG_GREEN);
    printf("===== Device #%d =====\n", deviceIndex);
    printf(RESET_COLOR);
    RAWINPUTDEVICELIST rawInputDevice = rawInputDeviceList[deviceIndex];
    if (rawInputDevice.dwType != RIM_TYPEHID)
    {
      // skip keyboards and mouses
      continue;
    }

    // get preparsed data for HidP
    UINT cbDataSize                    = 0;
    PHIDP_PREPARSED_DATA preparsedData = NULL;

    mGetRawInputDevicePreparsedData(rawInputDevice.hDevice, &preparsedData, &cbDataSize);

    NTSTATUS hidpReturnCode;

    // find HID capabilities
    HIDP_CAPS caps;
    hidpReturnCode = HidP_GetCaps(preparsedData, &caps);
    if (hidpReturnCode != HIDP_STATUS_SUCCESS)
    {
      print_HidP_errors(hidpReturnCode, __FILE__, __LINE__);
      exit(-1);
    }

    printf("NumberInputValueCaps: %d\n", caps.NumberInputValueCaps);
    printf("NumberInputButtonCaps: %d\n", caps.NumberInputButtonCaps);

    int isButtonCapsEmpty = (caps.NumberInputButtonCaps == 0);

    if (!isButtonCapsEmpty)
    {
      UINT deviceNameLength;
      TCHAR* deviceName = NULL;
      unsigned int cbDeviceName;

      mGetRawInputDeviceName(rawInputDevice.hDevice, &deviceName, &deviceNameLength, &cbDeviceName);

      printf("Device name: ");
      wprintf(deviceName);
      printf("\n");

      printf(FG_GREEN);
      printf("Finding device in global list...\n");
      printf(RESET_COLOR);
      unsigned int foundHidIdx;
      int returnCode = FindInputDeviceInList(&(g_app_state->device_info_list), deviceName, cbDeviceName, preparsedData, cbDataSize, &foundHidIdx);
      if (returnCode != 0)
      {
        printf("FindInputDeviceInList failed at %s:%d\n", __FILE__, __LINE__);
        exit(-1);
      }

      printf(FG_GREEN);
      printf("foundHIDIdx: %d\n", foundHidIdx);
      printf(RESET_COLOR);

      printf(FG_BRIGHT_BLUE);
      printf("found device name: ");
      wprintf(g_app_state->device_info_list.Entries[foundHidIdx].Name);
      printf("\n");
      printf(RESET_COLOR);

      printf(FG_BRIGHT_BLUE);
      printf("current device name: ");
      wprintf(deviceName);
      printf("\n");
      printf(RESET_COLOR);

      if (caps.NumberInputValueCaps != 0)
      {
        const USHORT numValueCaps = caps.NumberInputValueCaps;
        USHORT _numValueCaps      = numValueCaps;

        PHIDP_VALUE_CAPS valueCaps = (PHIDP_VALUE_CAPS)mMalloc(sizeof(HIDP_VALUE_CAPS) * numValueCaps, __FILE__, __LINE__);

        hidpReturnCode = HidP_GetValueCaps(HidP_Input, valueCaps, &_numValueCaps, preparsedData);
        if (hidpReturnCode != HIDP_STATUS_SUCCESS)
        {
          print_HidP_errors(hidpReturnCode, __FILE__, __LINE__);
        }

        // x check if numValueCaps value has been changed
        printf("NumberInputValueCaps: %d (old) vs %d (new)\n", numValueCaps, _numValueCaps);

        for (USHORT valueCapIndex = 0; valueCapIndex < numValueCaps; valueCapIndex++)
        {
          HIDP_VALUE_CAPS cap = valueCaps[valueCapIndex];

          if (cap.IsRange || !cap.IsAbsolute)
          {
            continue;
          }

          unsigned int foundLinkColIdx;
          int returnCode = FindLinkCollectionInList(&(g_app_state->device_info_list.Entries[foundHidIdx].LinkColInfoList), cap.LinkCollection, &foundLinkColIdx);
          if (returnCode != 0)
          {
            printf("FindLinkCollectionInList failed at %s:%d\n", __FILE__, __LINE__);
            exit(-1);
          }

          printf(FG_GREEN);
          printf("[ValueCaps] foundLinkCollectionIndex: %d\n", foundLinkColIdx);
          printf(RESET_COLOR);

          if (cap.UsagePage == HID_USAGE_PAGE_GENERIC)
          {
            printf("=====================================================\n");
            printf("LinkCollection: %d\n", cap.LinkCollection);

            if (cap.NotRange.Usage == HID_USAGE_GENERIC_X)
            {
              g_app_state->device_info_list.Entries[foundHidIdx].LinkColInfoList.Entries[foundLinkColIdx].HasX               = 1;
              g_app_state->device_info_list.Entries[foundHidIdx].LinkColInfoList.Entries[foundLinkColIdx].PhysicalRect.left  = cap.PhysicalMin;
              g_app_state->device_info_list.Entries[foundHidIdx].LinkColInfoList.Entries[foundLinkColIdx].PhysicalRect.right = cap.PhysicalMax;
              // NOTE: per-event HidP_GetUsageValue coordinates are in LOGICAL
              // units, so the pad->screen mapping must normalize against
              // LogicalMin/Max, not the physical extent above (physical units
              // such as hundredths of a millimeter).
              g_app_state->device_info_list.Entries[foundHidIdx].LinkColInfoList.Entries[foundLinkColIdx].LogicalXMin = cap.LogicalMin;
              g_app_state->device_info_list.Entries[foundHidIdx].LinkColInfoList.Entries[foundLinkColIdx].LogicalXMax = cap.LogicalMax;
              printf("  Left: %d\n", cap.PhysicalMin);
              printf("  Right: %d\n", cap.PhysicalMax);
              printf("  LogicalXMin: %d\n", cap.LogicalMin);
              printf("  LogicalXMax: %d\n", cap.LogicalMax);
            }
            else if (cap.NotRange.Usage == HID_USAGE_GENERIC_Y)
            {
              g_app_state->device_info_list.Entries[foundHidIdx].LinkColInfoList.Entries[foundLinkColIdx].HasY                = 1;
              g_app_state->device_info_list.Entries[foundHidIdx].LinkColInfoList.Entries[foundLinkColIdx].PhysicalRect.top    = cap.PhysicalMin;
              g_app_state->device_info_list.Entries[foundHidIdx].LinkColInfoList.Entries[foundLinkColIdx].PhysicalRect.bottom = cap.PhysicalMax;
              g_app_state->device_info_list.Entries[foundHidIdx].LinkColInfoList.Entries[foundLinkColIdx].LogicalYMin = cap.LogicalMin;
              g_app_state->device_info_list.Entries[foundHidIdx].LinkColInfoList.Entries[foundLinkColIdx].LogicalYMax = cap.LogicalMax;
              printf("  Top: %d\n", cap.PhysicalMin);
              printf("  Bottom: %d\n", cap.PhysicalMax);
              printf("  LogicalYMin: %d\n", cap.LogicalMin);
              printf("  LogicalYMax: %d\n", cap.LogicalMax);
            }
          }
          else if (cap.UsagePage == HID_USAGE_PAGE_DIGITIZER)
          {
            if (cap.NotRange.Usage == HID_USAGE_DIGITIZER_CONTACT_ID)
            {
              g_app_state->device_info_list.Entries[foundHidIdx].LinkColInfoList.Entries[foundLinkColIdx].HasContactID = 1;
            }
            else if (cap.NotRange.Usage == HID_USAGE_DIGITIZER_CONTACT_COUNT)
            {
              g_app_state->device_info_list.Entries[foundHidIdx].ContactCountLinkCollection = cap.LinkCollection;
            }
          }
        }

        free(valueCaps);
      }

      if (caps.NumberInputButtonCaps != 0)
      {
        const USHORT numButtonCaps = caps.NumberInputButtonCaps;
        USHORT _numButtonCaps      = numButtonCaps;

        PHIDP_BUTTON_CAPS buttonCaps = (PHIDP_BUTTON_CAPS)mMalloc(sizeof(HIDP_BUTTON_CAPS) * numButtonCaps, __FILE__, __LINE__);

        hidpReturnCode = HidP_GetButtonCaps(HidP_Input, buttonCaps, &_numButtonCaps, preparsedData);
        if (hidpReturnCode != HIDP_STATUS_SUCCESS)
        {
          print_HidP_errors(hidpReturnCode, __FILE__, __LINE__);
          exit(-1);
        }

        for (USHORT buttonCapIndex = 0; buttonCapIndex < numButtonCaps; buttonCapIndex++)
        {
          HIDP_BUTTON_CAPS buttonCap = buttonCaps[buttonCapIndex];

          if (buttonCap.IsRange)
          {
            continue;
          }

          printf(FG_BLUE);
          printf("[ButtonCaps] Index: %d, UsagePage: %d, Usage: %d, DIGITIZER: %d, IsRange: %d\n", buttonCapIndex, buttonCap.UsagePage, buttonCap.NotRange.Usage, buttonCap.UsagePage, buttonCap.IsRange);
          printf(RESET_COLOR);

          if (buttonCap.UsagePage == HID_USAGE_PAGE_DIGITIZER)
          {
            if (buttonCap.NotRange.Usage == HID_USAGE_DIGITIZER_TIP_SWITCH)
            {
              unsigned int foundLinkColIdx;
              int returnCode = FindLinkCollectionInList(&(g_app_state->device_info_list.Entries[foundHidIdx].LinkColInfoList), buttonCap.LinkCollection, &foundLinkColIdx);

              printf(FG_GREEN);
              printf("[ButtonCaps] foundLinkCollectionIndex: %d\n", foundLinkColIdx);
              printf(RESET_COLOR);

              g_app_state->device_info_list.Entries[foundHidIdx].LinkColInfoList.Entries[foundLinkColIdx].HasTipSwitch = 1;
            }
          }
        }

        free(buttonCaps);
      }

      free(deviceName);
    }

    free(preparsedData);
  }

  free(rawInputDeviceList);
}

void mRegisterRawInput(HWND hwnd)
{
  // register Windows Precision Touchpad top-level HID collection
  RAWINPUTDEVICE rid;
  clock_t ts = clock();

  rid.usUsagePage = HID_USAGE_PAGE_DIGITIZER;
  rid.usUsage     = HID_USAGE_DIGITIZER_TOUCH_PAD;
  rid.dwFlags     = RIDEV_INPUTSINK;
  rid.hwndTarget  = hwnd;

  if (RegisterRawInputDevices(&rid, 1, sizeof(RAWINPUTDEVICE)))
  {
    printf(FG_GREEN);
    printf("[%d] Successfully register touchpad!\n", ts);
    printf(RESET_COLOR);
  }
  else
  {
    printf(FG_RED);
    printf("[%d] Failed to register touchpad at %s:%d\n", ts, __FILE__, __LINE__);
    printf(RESET_COLOR);
    mGetLastError();
    exit(-1);
  }
}

// Reads the absolute pad coordinate frame from the HID logical ranges found
// during device enumeration. HID parsing itself (touchpad.c / utils.c / the
// WM_INPUT block) is intentionally untouched: this only consumes the
// already-parsed link-collection table.
void mUpdateCanvasMap();
void mInitAppState();
void mComputePadRanges()
{
  g_app_state->mapValid = 0;

  // Pick the first link collection that carries both X and Y.
  HID_TOUCH_LINK_COL_INFO* padRange = NULL;
  HID_DEVICE_INFO* padDevice        = NULL;
  HID_DEVICE_INFO* inputDevices     = g_app_state->device_info_list.Entries;
  unsigned int numInputDevices      = g_app_state->device_info_list.Size;
  if ((inputDevices != NULL) && (numInputDevices != 0))
  {
    for (unsigned int deviceIdx = 0; (deviceIdx < numInputDevices) && (padRange == NULL); deviceIdx++)
    {
      HID_LINK_COL_INFO_LIST* cols = &inputDevices[deviceIdx].LinkColInfoList;
      if ((cols->Entries == NULL) || (cols->Size == 0))
      {
        continue;
      }
      for (unsigned int linkColIdx = 0; linkColIdx < cols->Size; linkColIdx++)
      {
        if (cols->Entries[linkColIdx].HasX && cols->Entries[linkColIdx].HasY)
        {
          padRange  = &cols->Entries[linkColIdx];
          padDevice = &inputDevices[deviceIdx];
          break;
        }
      }
    }
  }

  if (padRange == NULL)
  {
    printf(FG_RED);
    printf("No touchpad X/Y link collection found; ink mapping disabled (identity fallback).\n");
    printf(RESET_COLOR);
    return;
  }

  g_app_state->padLogMinX = padRange->LogicalXMin;
  g_app_state->padLogMaxX = padRange->LogicalXMax;
  g_app_state->padLogMinY = padRange->LogicalYMin;
  g_app_state->padLogMaxY = padRange->LogicalYMax;

  LONG padW = padRange->LogicalXMax - padRange->LogicalXMin;
  LONG padH = padRange->LogicalYMax - padRange->LogicalYMin;
  if ((padW <= 0) || (padH <= 0))
  {
    printf(FG_RED);
    printf("Degenerate HID logical range (%d x %d); ink mapping disabled.\n", padW, padH);
    printf(RESET_COLOR);
    return;
  }

  {
    LONG physW = padRange->PhysicalRect.right - padRange->PhysicalRect.left;
    LONG physH = padRange->PhysicalRect.bottom - padRange->PhysicalRect.top;
    wprintf(L"Touchpad '%s'\n", (padDevice != NULL) ? padDevice->Name : L"<unknown>");
    printf("  HID logical range: X [%d, %d]  Y [%d, %d]\n", g_app_state->padLogMinX, g_app_state->padLogMaxX, g_app_state->padLogMinY, g_app_state->padLogMaxY);
    printf("  HID physical extent: %d x %d (aspect %.3f)\n", physW, physH, (physH != 0) ? ((double)physW / (double)physH) : 0.0);
  }

  mUpdateCanvasMap();
}

// Recomputes the letterboxed pad->canvas mapping from the current canvas
// size (client area below the toolbar). Called on startup, WM_CREATE and
// WM_SIZE, so resize never loses strokes (points are stored in pad units).
void mUpdateCanvasMap()
{
  LONG padW = g_app_state->padLogMaxX - g_app_state->padLogMinX;
  LONG padH = g_app_state->padLogMaxY - g_app_state->padLogMinY;
  if ((padW <= 0) || (padH <= 0) || (g_app_state->canvasW <= 0) || (g_app_state->canvasH <= 0))
  {
    g_app_state->mapValid = 0;
    return;
  }

  // Letterbox: uniform scale fitting the pad range inside the canvas,
  // centered, preserving aspect ratio (no stretch).
  double scaleX = (double)g_app_state->canvasW / (double)padW;
  double scaleY = (double)g_app_state->canvasH / (double)padH;
  g_app_state->mapScale = (scaleX < scaleY) ? scaleX : scaleY;
  double mappedW        = (double)padW * g_app_state->mapScale;
  double mappedH        = (double)padH * g_app_state->mapScale;
  g_app_state->mapOffX  = ((double)g_app_state->canvasW - mappedW) / 2.0;
  // Canvas starts below the toolbar strip.
  g_app_state->mapOffY  = (double)TOOLBAR_H + ((double)g_app_state->canvasH - mappedH) / 2.0;
  g_app_state->mapValid = 1;

  printf("  Canvas %d x %d -> scale %.4f, offset (%.1f, %.1f)%s\n", g_app_state->canvasW, g_app_state->canvasH, g_app_state->mapScale, g_app_state->mapOffX, g_app_state->mapOffY, (scaleX != scaleY) ? " (letterboxed)" : " (exact fit)");
}

// Syncs the cached canvas size with the window's client area.
void mSyncCanvasSize(HWND hwnd)
{
  RECT rc;
  GetClientRect(hwnd, &rc);
  int w = rc.right - rc.left;
  int h = rc.bottom - rc.top - TOOLBAR_H;
  if (h < 0)
  {
    h = 0;
  }
  if ((w != g_app_state->canvasW) || (h != g_app_state->canvasH))
  {
    g_app_state->canvasW = w;
    g_app_state->canvasH = h;
    mUpdateCanvasMap();
  }
}

// Maps raw HID-logical pad coordinates to canvas client pixels.
// Out-of-range contacts are clamped to the mapped area. Returns 0 on success.
int mMapPadToClient(ULONG padX, ULONG padY, int* outX, int* outY)
{
  if ((outX == NULL) || (outY == NULL))
  {
    return -1;
  }
  if (!g_app_state->mapValid)
  {
    // Identity fallback (pre-fix behavior): raw units as pixels.
    *outX = (int)padX;
    *outY = (int)padY;
    return 0;
  }
  double nx = (double)((LONG)padX - g_app_state->padLogMinX);
  double ny = (double)((LONG)padY - g_app_state->padLogMinY);
  double padW = (double)(g_app_state->padLogMaxX - g_app_state->padLogMinX);
  double padH = (double)(g_app_state->padLogMaxY - g_app_state->padLogMinY);
  if (nx < 0.0)
  {
    nx = 0.0;
  }
  else if (nx > padW)
  {
    nx = padW;
  }
  if (ny < 0.0)
  {
    ny = 0.0;
  }
  else if (ny > padH)
  {
    ny = padH;
  }
  *outX = (int)(nx * g_app_state->mapScale + g_app_state->mapOffX + 0.5);
  *outY = (int)(ny * g_app_state->mapScale + g_app_state->mapOffY + 0.5);
  return 0;
}

// Instant ink helpers (used by the WM_INPUT path; full repaints go through
// WM_PAINT). All coordinates mapped pad->canvas via mMapPadToClient.
static void mDrawDotAt(HWND hwnd, ULONG padX, ULONG padY, COLORREF color, int width)
{
  int dotX = 0;
  int dotY = 0;
  mMapPadToClient(padX, padY, &dotX, &dotY);
  int r = width / 2;
  if (r < 1)
  {
    r = 1;
  }
  HDC hdc         = GetDC(hwnd);
  HPEN strokePen  = CreatePen(PS_SOLID, width, color);
  HGDIOBJ oldPen  = SelectObject(hdc, strokePen);
  HBRUSH dotBrush = CreateSolidBrush(color);
  HGDIOBJ oldBrush = SelectObject(hdc, dotBrush);
  Ellipse(hdc, dotX - r, dotY - r, dotX + r, dotY + r);
  SelectObject(hdc, oldBrush);
  SelectObject(hdc, oldPen);
  DeleteObject(dotBrush);
  DeleteObject(strokePen);
  ReleaseDC(hwnd, hdc);
}

static void mDrawLastSegment(HWND hwnd)
{
  if ((g_app_state->strokes.Entries == NULL) || (g_app_state->strokes.Size == 0))
  {
    return;
  }
  Stroke* stroke = &g_app_state->strokes.Entries[g_app_state->strokes.Size - 1];
  if (stroke->Points.Size < 2)
  {
    return;
  }
  int x0 = 0;
  int y0 = 0;
  int x1 = 0;
  int y1 = 0;
  mMapPadToClient(stroke->Points.Entries[stroke->Points.Size - 2].X, stroke->Points.Entries[stroke->Points.Size - 2].Y, &x0, &y0);
  mMapPadToClient(stroke->Points.Entries[stroke->Points.Size - 1].X, stroke->Points.Entries[stroke->Points.Size - 1].Y, &x1, &y1);
  HDC hdc        = GetDC(hwnd);
  HPEN strokePen = CreatePen(PS_SOLID, stroke->Width, stroke->Color);
  HGDIOBJ oldPen = SelectObject(hdc, strokePen);
  MoveToEx(hdc, x0, y0, (LPPOINT)NULL);
  LineTo(hdc, x1, y1);
  SelectObject(hdc, oldPen);
  DeleteObject(strokePen);
  ReleaseDC(hwnd, hdc);
}

// Eraser: removes every stroke passing within the eraser radius of the pad
// point. Pushes one undo snapshot per gesture (on first actual removal).
// Returns the number of strokes removed.
static int mEraseAt(HWND hwnd, ULONG padX, ULONG padY)
{
  int removed = 0;
  // Eraser follows the brush size so the user has one size control.
  int radiusClient = g_app_state->curWidth * 2;
  if (radiusClient < 12)
  {
    radiusClient = 12;
  }
  double radiusPad = (g_app_state->mapValid && (g_app_state->mapScale > 0.0)) ? ((double)radiusClient / g_app_state->mapScale) : (double)radiusClient;
  for (unsigned int i = g_app_state->strokes.Size; i > 0; i--)
  {
    if (mHitTestStroke(&g_app_state->strokes.Entries[i - 1], padX, padY, radiusPad))
    {
      if (!g_app_state->erasePushed)
      {
        Hist_PushUndo(&g_app_state->hist, &g_app_state->strokes);
        Hist_ClearRedo(&g_app_state->hist);
        g_app_state->erasePushed = 1;
      }
      mRemoveStrokeAt(&g_app_state->strokes, i - 1);
      removed++;
    }
  }
  if (removed > 0)
  {
    InvalidateRect(hwnd, NULL, FALSE);
  }
  return removed;
}

void mHandleCreateMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
  mRegisterRawInput(hwnd);
  mSyncCanvasSize(hwnd);
}

void mHandleInputMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
  clock_t ts = clock();

  // following guide: https://docs.microsoft.com/en-us/windows/win32/inputdev/using-raw-input#performing-a-standard-read-of-raw-input

  // Get the size of RAWINPUT by calling GetRawInputData() with pData = NULL

  if (g_app_state->is_drawing != 0)
  {
    UINT rawInputSize;
    PRAWINPUT rawInputData = NULL;

    mGetRawInputData((HRAWINPUT)lParam, &rawInputSize, (LPVOID*)(&rawInputData));

    // Parse the RAWINPUT data.
    if (rawInputData->header.dwType == RIM_TYPEHID)
    {
      // TODO what does `dwCount` represent?
      DWORD count   = rawInputData->data.hid.dwCount;
      BYTE* rawData = rawInputData->data.hid.bRawData;

      if (count != 0)
      {
        UINT deviceNameLength;
        TCHAR* deviceName = NULL;
        unsigned int cbDeviceName;

        mGetRawInputDeviceName(rawInputData->header.hDevice, &deviceName, &deviceNameLength, &cbDeviceName);

        unsigned int foundHidIdx = (unsigned int)-1;

        int isTouchpadsNull              = (g_app_state->device_info_list.Entries == NULL);
        int isTouchpadsRecordedSizeEmpty = (g_app_state->device_info_list.Size == 0);
        if (isTouchpadsNull || isTouchpadsRecordedSizeEmpty)
        {
          // TODO parse new connected device data
        }
        else
        {
          for (unsigned int touchpadIndex = 0; touchpadIndex < g_app_state->device_info_list.Size; touchpadIndex++)
          {
            int compareNameResult = _tcscmp(deviceName, g_app_state->device_info_list.Entries[touchpadIndex].Name);
            if (compareNameResult == 0)
            {
              foundHidIdx = touchpadIndex;
              break;
            }
          }
        }

        if (foundHidIdx == (unsigned int)-1)
        {
          // TODO parse new connected device data
        }
        else
        {
          int isLinkColArrayNull  = (g_app_state->device_info_list.Entries[foundHidIdx].LinkColInfoList.Entries == NULL);
          int isLinkColArrayEmpty = (g_app_state->device_info_list.Entries[foundHidIdx].LinkColInfoList.Size == 0);
          int isPreparsedDataNull = (g_app_state->device_info_list.Entries[foundHidIdx].PreparedData == NULL);

          if (isLinkColArrayNull || isLinkColArrayEmpty)
          {
            printf(FG_RED);
            printf("Cannot find any LinkCollection(s). Try parse the PREPARED_DATA may help. TODO\n");
            printf(RESET_COLOR);
          }
          else if (isPreparsedDataNull)
          {
            printf(FG_RED);
            printf("Cannot find PreparsedData at %s:%d\n", __FILE__, __LINE__);
            printf(RESET_COLOR);
          }
          else
          {
#ifdef LOG_EVERY_INPUT_MESSAGES
            printf("[%d]", ts);
#endif
            NTSTATUS hidpReturnCode;
            ULONG usageValue;

            PHIDP_PREPARSED_DATA preparsedHIDData = g_app_state->device_info_list.Entries[foundHidIdx].PreparedData;

            if (g_app_state->device_info_list.Entries[foundHidIdx].ContactCountLinkCollection == (USHORT)-1)
            {
              printf(FG_RED);
              printf("Cannot find contact count Link Collection!\n");
              printf(RESET_COLOR);
            }
            else
            {
              hidpReturnCode = HidP_GetUsageValue(HidP_Input, HID_USAGE_PAGE_DIGITIZER, g_app_state->device_info_list.Entries[foundHidIdx].ContactCountLinkCollection, HID_USAGE_DIGITIZER_CONTACT_COUNT, &usageValue, preparsedHIDData, (PCHAR)rawInputData->data.hid.bRawData, rawInputData->data.hid.dwSizeHid);

              if (hidpReturnCode != HIDP_STATUS_SUCCESS)
              {
                printf(FG_RED);
                printf("Failed to read number of contacts!\n");
                printf(RESET_COLOR);
                print_HidP_errors(hidpReturnCode, __FILE__, __LINE__);
                exit(-1);
              }

              ULONG numContacts = usageValue;

#ifdef LOG_EVERY_INPUT_MESSAGES
              printf(FG_BRIGHT_BLUE);
              printf("numContacts: %d\n", numContacts);
              printf(RESET_COLOR);
#endif

              if (numContacts > g_app_state->device_info_list.Entries[foundHidIdx].LinkColInfoList.Size)
              {
                // TODO how should we deal with this edge case
                printf(FG_RED);
                printf("number of contacts is greater than Link Collection Array size at %s:%d\n", __FILE__, __LINE__);
                printf(RESET_COLOR);
                exit(-1);
              }
              else
              {
                for (unsigned int linkColIdx = 0; linkColIdx < numContacts; linkColIdx++)
                {
                  HID_TOUCH_LINK_COL_INFO collectionInfo = g_app_state->device_info_list.Entries[foundHidIdx].LinkColInfoList.Entries[linkColIdx];

                  if (collectionInfo.HasX && collectionInfo.HasY && collectionInfo.HasContactID && collectionInfo.HasTipSwitch)
                  {
                    hidpReturnCode = HidP_GetUsageValue(HidP_Input, 0x01, collectionInfo.LinkColID, 0x30, &usageValue, preparsedHIDData, (PCHAR)rawInputData->data.hid.bRawData, rawInputData->data.hid.dwSizeHid);

                    if (hidpReturnCode != HIDP_STATUS_SUCCESS)
                    {
                      printf(FG_RED);
                      printf("Failed to read x position!\n");
                      printf(RESET_COLOR);
                      print_HidP_errors(hidpReturnCode, __FILE__, __LINE__);
                      exit(-1);
                    }

                    ULONG xPos = usageValue;

                    hidpReturnCode = HidP_GetUsageValue(HidP_Input, 0x01, collectionInfo.LinkColID, 0x31, &usageValue, preparsedHIDData, (PCHAR)rawInputData->data.hid.bRawData, rawInputData->data.hid.dwSizeHid);
                    if (hidpReturnCode != HIDP_STATUS_SUCCESS)
                    {
                      printf(FG_RED);
                      printf("Failed to read y position!\n");
                      printf(RESET_COLOR);
                      print_HidP_errors(hidpReturnCode, __FILE__, __LINE__);
                      exit(-1);
                    }

                    ULONG yPos = usageValue;

                    hidpReturnCode = HidP_GetUsageValue(HidP_Input, HID_USAGE_PAGE_DIGITIZER, collectionInfo.LinkColID, HID_USAGE_DIGITIZER_CONTACT_ID, &usageValue, preparsedHIDData, (PCHAR)rawInputData->data.hid.bRawData, rawInputData->data.hid.dwSizeHid);
                    if (hidpReturnCode != HIDP_STATUS_SUCCESS)
                    {
                      printf(FG_RED);
                      printf("Failed to read touch ID!\n");
                      printf(RESET_COLOR);
                      print_HidP_errors(hidpReturnCode, __FILE__, __LINE__);
                      exit(-1);
                    }

                    ULONG touchId = usageValue;

                    const ULONG maxNumButtons = HidP_MaxUsageListLength(HidP_Input, HID_USAGE_PAGE_DIGITIZER, preparsedHIDData);

                    ULONG _maxNumButtons = maxNumButtons;

                    USAGE* buttonUsageArray = (USAGE*)mMalloc(sizeof(USAGE) * maxNumButtons, __FILE__, __LINE__);

                    hidpReturnCode = HidP_GetUsages(HidP_Input, HID_USAGE_PAGE_DIGITIZER, collectionInfo.LinkColID, buttonUsageArray, &_maxNumButtons, preparsedHIDData, (PCHAR)rawInputData->data.hid.bRawData, rawInputData->data.hid.dwSizeHid);

                    if (hidpReturnCode != HIDP_STATUS_SUCCESS)
                    {
                      printf(FG_RED);
                      printf("HidP_GetUsages failed!\n");
                      printf(RESET_COLOR);
                      print_HidP_errors(hidpReturnCode, __FILE__, __LINE__);
                      exit(-1);
                    }

                    int isContactOnSurface = 0;

                    for (ULONG usageIdx = 0; usageIdx < maxNumButtons; usageIdx++)
                    {
                      if (buttonUsageArray[usageIdx] == HID_USAGE_DIGITIZER_TIP_SWITCH)
                      {
                        isContactOnSurface = 1;
                        break;
                      }
                    }

                    free(buttonUsageArray);

                    TOUCH_DATA curTouch;
                    curTouch.TouchID   = touchId;
                    curTouch.X         = xPos;
                    curTouch.Y         = yPos;
                    curTouch.OnSurface = isContactOnSurface;

                    unsigned int touchType;
                    int cStyleFunctionReturnCode = mInterpretRawTouchInput(&g_app_state->previous_touches, curTouch, &touchType);
                    if (cStyleFunctionReturnCode != 0)
                    {
                      printf(FG_RED);
                      printf("mInterpretRawTouchInput failed at %s:%d\n", __FILE__, __LINE__);
                      printf(RESET_COLOR);
                      exit(-1);
                    }

                    Point2D touchPos = (Point2D){.X = curTouch.X, .Y = curTouch.Y};

                    if (g_app_state->tracking_touch_id == (ULONG)-1)
                    {
                      if (touchType == EVENT_TYPE_TOUCH_DOWN)
                      {
                        g_app_state->tracking_touch_id = curTouch.TouchID;
                        if (g_app_state->tool == TOOL_ERASER)
                        {
                          g_app_state->eraseActive = 1;
                          g_app_state->erasePushed = 0;
                          mEraseAt(hwnd, curTouch.X, curTouch.Y);
                        }
                        else
                        {
                          // Snapshot BEFORE the stroke so undo removes it.
                          Hist_PushUndo(&g_app_state->hist, &g_app_state->strokes);
                          Hist_ClearRedo(&g_app_state->hist);
                          // TODO check return value for indication of errors
                          mCreateNewStroke(touchPos, g_app_state->curColor, g_app_state->curWidth, &g_app_state->strokes);
                          // Draw a dot immediately so taps (down+up with no
                          // move) still leave visible ink at the mapped spot.
                          mDrawDotAt(hwnd, curTouch.X, curTouch.Y, g_app_state->curColor, g_app_state->curWidth);
                        }
                      }
                      else
                      {
                        // wait for touch down event to register new stroke
                      }
                    }
                    else if (curTouch.TouchID == g_app_state->tracking_touch_id)
                    {
                      if (touchType == EVENT_TYPE_TOUCH_MOVE)
                      {
                        // we skip EVENT_TYPE_TOUCH_MOVE_UNCHANGED here
                        if (g_app_state->tool == TOOL_ERASER)
                        {
                          mEraseAt(hwnd, curTouch.X, curTouch.Y);
                        }
                        else
                        {
                          // TODO append touch position to the last stroke
                          // TODO check return value for indication of errors
                          if (mAppendToLastStroke(touchPos, &g_app_state->strokes) == 0)
                          {
                            mDrawLastSegment(hwnd);
                          }
                        }
                      }
                      else if (touchType == EVENT_TYPE_TOUCH_UP)
                      {
                        // I sure that the touch position is the same with the last touch position
                        g_app_state->tracking_touch_id = (ULONG)-1;
                        g_app_state->eraseActive       = 0;
                      }
                    }

#ifdef LOG_EVERY_INPUT_MESSAGES
                    const char* touchTypeStr;

                    if (touchType == EVENT_TYPE_TOUCH_UP)
                    {
                      touchTypeStr = "touch up";
                    }
                    else if (touchType == EVENT_TYPE_TOUCH_MOVE)
                    {
                      touchTypeStr = "touch move";
                    }
                    else if (touchType == EVENT_TYPE_TOUCH_DOWN)
                    {
                      touchTypeStr = "touch down";
                    }
                    else if (touchType == EVENT_TYPE_TOUCH_MOVE_UNCHANGED)
                    {
                      touchTypeStr = "touch move unchanged";
                    }
                    else
                    {
                      printf(FG_RED);
                      printf("unknown event type: %d\n", touchType);
                      printf(RESET_COLOR);
                      exit(-1);
                    }

                    printf(FG_GREEN);
                    printf("LinkColId: %d, touchID: %d, tipSwitch: %d, position: (%d, %d), eventType: %s\n", collectionInfo.LinkColID, touchId, isContactOnSurface, xPos, yPos, touchTypeStr);
                    printf(RESET_COLOR);
#endif
                  }
                }
              }
            }
          }

          free(deviceName);
        }
      }

      free(rawInputData);
    }
  }
}

void mHandleResizeMessage(_In_ HWND hwnd, _In_ UINT uMsg, _In_ WPARAM wParam, _In_ LPARAM lParam)
{
  mSyncCanvasSize(hwnd);
  InvalidateRect(hwnd, NULL, FALSE);
}

// ---- In-window toolbar (dark theme, owner-drawn, no extra dependencies) ----

static const COLORREF kPalette[5] = {
  RGB(255, 255, 255),
  RGB(231, 76, 60),
  RGB(46, 204, 113),
  RGB(52, 152, 219),
  RGB(241, 196, 15)
};

static const TCHAR* kPaletteNames[5] = {_T("White"), _T("Red"), _T("Green"), _T("Blue"), _T("Yellow")};

#define TB_FACE RGB(48, 48, 54)
#define TB_BAR RGB(28, 28, 31)
#define TB_ACCENT RGB(0, 120, 215)
#define TB_TEXT RGB(240, 240, 240)
#define TB_DIM RGB(130, 130, 135)
#define CANVAS_BG RGB(15, 15, 18)

static void Toolbar_Layout(int clientW, RECT rects[TB_COUNT], RECT* sizeLabel)
{
  int x = 8;
  int y = 8;
  int h = TOOLBAR_H - 16;
  for (int i = 0; i < TB_COUNT; i++)
  {
    rects[i].left = 0;
    rects[i].top = 0;
    rects[i].right = 0;
    rects[i].bottom = 0;
  }
  rects[TB_PEN].left = x; rects[TB_PEN].top = y; rects[TB_PEN].right = x + 64; rects[TB_PEN].bottom = y + h; x += 64 + 6;
  rects[TB_ERASER].left = x; rects[TB_ERASER].top = y; rects[TB_ERASER].right = x + 64; rects[TB_ERASER].bottom = y + h; x += 64 + 12;
  for (int i = 0; i < 5; i++)
  {
    int id = TB_C0 + i;
    rects[id].left = x; rects[id].top = y; rects[id].right = x + 40; rects[id].bottom = y + h; x += 40 + 6;
  }
  rects[TB_CUSTOM].left = x; rects[TB_CUSTOM].top = y; rects[TB_CUSTOM].right = x + 76; rects[TB_CUSTOM].bottom = y + h; x += 76 + 12;
  rects[TB_SMALLER].left = x; rects[TB_SMALLER].top = y; rects[TB_SMALLER].right = x + 40; rects[TB_SMALLER].bottom = y + h; x += 40 + 6;
  if (sizeLabel != NULL)
  {
    sizeLabel->left = x; sizeLabel->top = y; sizeLabel->right = x + 56; sizeLabel->bottom = y + h; x += 56 + 6;
  }
  rects[TB_BIGGER].left = x; rects[TB_BIGGER].top = y; rects[TB_BIGGER].right = x + 40; rects[TB_BIGGER].bottom = y + h; x += 40 + 12;
  rects[TB_UNDO].left = x; rects[TB_UNDO].top = y; rects[TB_UNDO].right = x + 64; rects[TB_UNDO].bottom = y + h; x += 64 + 6;
  rects[TB_REDO].left = x; rects[TB_REDO].top = y; rects[TB_REDO].right = x + 64; rects[TB_REDO].bottom = y + h; x += 64 + 6;
  rects[TB_CLEAR].left = x; rects[TB_CLEAR].top = y; rects[TB_CLEAR].right = x + 64; rects[TB_CLEAR].bottom = y + h; x += 64 + 12;
  rects[TB_SAVE].left = x; rects[TB_SAVE].top = y; rects[TB_SAVE].right = x + 64; rects[TB_SAVE].bottom = y + h; x += 64 + 6;
  rects[TB_OPEN].left = x; rects[TB_OPEN].top = y; rects[TB_OPEN].right = x + 64; rects[TB_OPEN].bottom = y + h; x += 64 + 6;
  rects[TB_BMP].left = x; rects[TB_BMP].top = y; rects[TB_BMP].right = x + 64; rects[TB_BMP].bottom = y + h;
  (void)clientW;
}

static int Toolbar_HitTest(int x, int y, int clientW)
{
  if (y >= TOOLBAR_H)
  {
    return -1;
  }
  RECT rects[TB_COUNT];
  RECT sizeLabel;
  Toolbar_Layout(clientW, rects, &sizeLabel);
  for (int i = 0; i < TB_COUNT; i++)
  {
    if ((x >= rects[i].left) && (x < rects[i].right) && (y >= rects[i].top) && (y < rects[i].bottom))
    {
      return i;
    }
  }
  return -1;
}

static void Toolbar_PaintButton(HDC hdc, const RECT* rc, const TCHAR* label, int active, int enabled)
{
  HBRUSH face = CreateSolidBrush(TB_FACE);
  FillRect(hdc, rc, face);
  DeleteObject(face);
  HPEN border = CreatePen(PS_SOLID, 2, active ? TB_ACCENT : TB_FACE);
  HGDIOBJ oldPen = SelectObject(hdc, border);
  HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
  Rectangle(hdc, rc->left, rc->top, rc->right, rc->bottom);
  SelectObject(hdc, oldBrush);
  SelectObject(hdc, oldPen);
  DeleteObject(border);
  SetBkMode(hdc, TRANSPARENT);
  SetTextColor(hdc, enabled ? TB_TEXT : TB_DIM);
  DrawText(hdc, label, -1, (LPRECT)rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void Toolbar_Paint(HDC hdc, int clientW)
{
  RECT rects[TB_COUNT];
  RECT sizeLabel;
  Toolbar_Layout(clientW, rects, &sizeLabel);

  HBRUSH bar = CreateSolidBrush(TB_BAR);
  RECT barRc = {0, 0, clientW, TOOLBAR_H};
  FillRect(hdc, &barRc, bar);
  DeleteObject(bar);
  HPEN sep = CreatePen(PS_SOLID, 1, RGB(60, 60, 66));
  HGDIOBJ oldPen = SelectObject(hdc, sep);
  MoveToEx(hdc, 0, TOOLBAR_H - 1, (LPPOINT)NULL);
  LineTo(hdc, clientW, TOOLBAR_H - 1);
  SelectObject(hdc, oldPen);
  DeleteObject(sep);

  Toolbar_PaintButton(hdc, &rects[TB_PEN], _T("Pen"), g_app_state->tool == TOOL_PEN, 1);
  Toolbar_PaintButton(hdc, &rects[TB_ERASER], _T("Eraser"), g_app_state->tool == TOOL_ERASER, 1);

  for (int i = 0; i < 5; i++)
  {
    int id = TB_C0 + i;
    int active = (g_app_state->curColor == kPalette[i]) && (g_app_state->tool == TOOL_PEN);
    HBRUSH face = CreateSolidBrush(TB_FACE);
    FillRect(hdc, &rects[id], face);
    DeleteObject(face);
    RECT sw = rects[id];
    sw.left += 6; sw.top += 6; sw.right -= 6; sw.bottom -= 6;
    HBRUSH fill = CreateSolidBrush(kPalette[i]);
    FillRect(hdc, &sw, fill);
    DeleteObject(fill);
    HPEN border = CreatePen(PS_SOLID, 2, active ? TB_ACCENT : RGB(90, 90, 96));
    HGDIOBJ op = SelectObject(hdc, border);
    HGDIOBJ ob = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, rects[id].left, rects[id].top, rects[id].right, rects[id].bottom);
    SelectObject(hdc, ob);
    SelectObject(hdc, op);
    DeleteObject(border);
  }

  {
    int isCustom = 1;
    for (int i = 0; i < 5; i++)
    {
      if (g_app_state->curColor == kPalette[i])
      {
        isCustom = 0;
        break;
      }
    }
    Toolbar_PaintButton(hdc, &rects[TB_CUSTOM], _T("Custom"), isCustom, 1);
    if (isCustom)
    {
      RECT sw = rects[TB_CUSTOM];
      sw.left += 6; sw.right = sw.left + 18; sw.top += 6; sw.bottom -= 6;
      HBRUSH fill = CreateSolidBrush(g_app_state->curColor);
      FillRect(hdc, &sw, fill);
      DeleteObject(fill);
    }
  }

  Toolbar_PaintButton(hdc, &rects[TB_SMALLER], _T("A-"), 0, g_app_state->curWidth > BRUSH_MIN);
  {
    TCHAR buf[16];
    _stprintf_s(buf, 16, _T("%dpx"), g_app_state->curWidth);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, TB_TEXT);
    DrawText(hdc, buf, -1, &sizeLabel, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  }
  Toolbar_PaintButton(hdc, &rects[TB_BIGGER], _T("A+"), 0, g_app_state->curWidth < BRUSH_MAX);
  Toolbar_PaintButton(hdc, &rects[TB_UNDO], _T("Undo"), 0, Hist_CanUndo(&g_app_state->hist));
  Toolbar_PaintButton(hdc, &rects[TB_REDO], _T("Redo"), 0, Hist_CanRedo(&g_app_state->hist));
  Toolbar_PaintButton(hdc, &rects[TB_CLEAR], _T("Clear"), 0, g_app_state->strokes.Size > 0);
  Toolbar_PaintButton(hdc, &rects[TB_SAVE], _T("Save"), 0, 1);
  Toolbar_PaintButton(hdc, &rects[TB_OPEN], _T("Open"), 0, 1);
  Toolbar_PaintButton(hdc, &rects[TB_BMP], _T("BMP"), 0, g_app_state->strokes.Size > 0);

  // Mode indicator at the right edge.
  {
    TCHAR mode[64];
    if (g_app_state->is_drawing)
    {
      _stprintf_s(mode, 64, _T("DRAWING - ESC to exit"));
      SetTextColor(hdc, RGB(255, 120, 120));
    }
    else
    {
      _stprintf_s(mode, 64, _T("F3 to draw"));
      SetTextColor(hdc, TB_DIM);
    }
    SetBkMode(hdc, TRANSPARENT);
    RECT rc = {rects[TB_BMP].right + 12, 8, clientW - 8, TOOLBAR_H - 8};
    if (rc.left < rc.right)
    {
      DrawText(hdc, mode, -1, &rc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
  }
}

static void mPickCustomColor(HWND hwnd)
{
  static DWORD customColors[16] = {
    RGB(255, 255, 255), RGB(231, 76, 60), RGB(46, 204, 113), RGB(52, 152, 219),
    RGB(241, 196, 15), RGB(155, 89, 182), RGB(26, 188, 156), RGB(230, 126, 34),
    0, 0, 0, 0, 0, 0, 0, 0
  };
  CHOOSECOLOR cc;
  cc.lStructSize  = sizeof(cc);
  cc.hwndOwner    = hwnd;
  cc.hInstance    = (HWND)NULL;
  cc.rgbResult    = g_app_state->curColor;
  cc.lpCustColors = customColors;
  cc.Flags        = CC_FULLOPEN | CC_RGBINIT;
  cc.lCustData    = 0;
  cc.lpfnHook     = NULL;
  cc.lpTemplateName = NULL;
  if (ChooseColor(&cc))
  {
    g_app_state->curColor = cc.rgbResult;
    if (g_app_state->tool != TOOL_PEN)
    {
      g_app_state->tool = TOOL_PEN;
    }
    printf("Custom color: #%02X%02X%02X\n", GetRValue(cc.rgbResult), GetGValue(cc.rgbResult), GetBValue(cc.rgbResult));
    if ((GetRValue(cc.rgbResult) < 24) && (GetGValue(cc.rgbResult) < 24) && (GetBValue(cc.rgbResult) < 24))
    {
      printf(FG_BRIGHT_YELLOW);
      printf("Note: very dark colors are near-invisible on the dark canvas.\n");
      printf(RESET_COLOR);
    }
  }
}

// Canvas-relative mapping for BMP export (mMapPadToClient includes the
// toolbar-strip offset; the BMP covers the canvas area only).
static int mMapPadToCanvas(ULONG padX, ULONG padY, int* outX, int* outY)
{
  int r = mMapPadToClient(padX, padY, outX, outY);
  if ((r == 0) && (outY != NULL))
  {
    *outY -= TOOLBAR_H;
  }
  return r;
}

static void mUpdateTitle(HWND hwnd)
{
  TCHAR buf[256];
  if (g_app_state->curFileSet)
  {
    const TCHAR* name = _tcsrchr(g_app_state->curFile, _T('\\'));
    name              = (name != NULL) ? (name + 1) : g_app_state->curFile;
    _stprintf_s(buf, 256, _T("TouchDraw - %s"), name);
  }
  else
  {
    _stprintf_s(buf, 256, _T("TouchDraw"));
  }
  SetWindowText(hwnd, buf);
}

static void mDoSave(HWND hwnd)
{
  if (!g_app_state->curFileSet)
  {
    TCHAR path[MAX_PATH] = _T("");
    int dlg              = File_SaveDialog(hwnd, path, MAX_PATH);
    if (dlg <= 0)
    {
      if (dlg < 0)
      {
        printf(FG_RED);
        printf("Save dialog failed.\n");
        printf(RESET_COLOR);
      }
      return;
    }
    _tcsncpy_s(g_app_state->curFile, MAX_PATH, path, _TRUNCATE);
    g_app_state->curFileSet = 1;
  }
  if (File_WriteNative(g_app_state->curFile, &g_app_state->strokes) == 0)
  {
    _tprintf(_T("Saved: %s\n"), g_app_state->curFile);
    mUpdateTitle(hwnd);
  }
  else
  {
    printf(FG_RED);
    printf("Save failed.\n");
    printf(RESET_COLOR);
  }
}

static void mDoOpen(HWND hwnd)
{
  TCHAR path[MAX_PATH] = _T("");
  StrokeList loaded    = {0};
  int dlg              = File_OpenDialog(hwnd, path, MAX_PATH);
  if (dlg <= 0)
  {
    if (dlg < 0)
    {
      printf(FG_RED);
      printf("Open dialog failed.\n");
      printf(RESET_COLOR);
    }
    return;
  }
  if (File_ReadNative(path, &loaded) != 0)
  {
    printf(FG_RED);
    _tprintf(_T("Could not open (not a TouchDraw file or corrupt): %s\n"), path);
    printf(RESET_COLOR);
    return;
  }
  // Undoable open: snapshot first, then swap.
  g_app_state->tracking_touch_id = (ULONG)-1;
  g_app_state->eraseActive       = 0;
  Hist_PushUndo(&g_app_state->hist, &g_app_state->strokes);
  Hist_ClearRedo(&g_app_state->hist);
  mFreeStrokeList(&g_app_state->strokes);
  g_app_state->strokes = loaded;
  _tcsncpy_s(g_app_state->curFile, MAX_PATH, path, _TRUNCATE);
  g_app_state->curFileSet = 1;
  _tprintf(_T("Opened: %s\n"), path);
  mUpdateTitle(hwnd);
  InvalidateRect(hwnd, NULL, FALSE);
}

static void mDoExportBMP(HWND hwnd)
{
  TCHAR path[MAX_PATH] = _T("");
  int dlg              = File_ExportDialog(hwnd, path, MAX_PATH);
  if (dlg <= 0)
  {
    if (dlg < 0)
    {
      printf(FG_RED);
      printf("Export dialog failed.\n");
      printf(RESET_COLOR);
    }
    return;
  }
  if (File_ExportBMP(path, g_app_state->canvasW, g_app_state->canvasH, CANVAS_BG, &g_app_state->strokes, mMapPadToCanvas) == 0)
  {
    _tprintf(_T("Exported BMP: %s\n"), path);
  }
  else
  {
    printf(FG_RED);
    printf("BMP export failed.\n");
    printf(RESET_COLOR);
  }
}

static void mClearCanvas(HWND hwnd)
{
  // Undoable clear: snapshot first. Also commits any in-progress stroke.
  g_app_state->tracking_touch_id = (ULONG)-1;
  g_app_state->eraseActive       = 0;
  Hist_PushUndo(&g_app_state->hist, &g_app_state->strokes);
  Hist_ClearRedo(&g_app_state->hist);
  mFreeStrokeList(&g_app_state->strokes);
  printf("Canvas cleared.\n");
  InvalidateRect(hwnd, NULL, FALSE);
}

static void mDoUndo(HWND hwnd)
{
  g_app_state->tracking_touch_id = (ULONG)-1;
  g_app_state->eraseActive       = 0;
  if (Hist_Undo(&g_app_state->hist, &g_app_state->strokes) == 0)
  {
    printf("Undo.\n");
    InvalidateRect(hwnd, NULL, FALSE);
  }
}

static void mDoRedo(HWND hwnd)
{
  g_app_state->tracking_touch_id = (ULONG)-1;
  g_app_state->eraseActive       = 0;
  if (Hist_Redo(&g_app_state->hist, &g_app_state->strokes) == 0)
  {
    printf("Redo.\n");
    InvalidateRect(hwnd, NULL, FALSE);
  }
}

static void Toolbar_DoAction(HWND hwnd, int id)
{
  switch (id)
  {
    case TB_PEN:
      g_app_state->tool = TOOL_PEN;
      printf("Tool: pen.\n");
      break;
    case TB_ERASER:
      g_app_state->tool = TOOL_ERASER;
      printf("Tool: eraser.\n");
      break;
    case TB_C0:
    case TB_C1:
    case TB_C2:
    case TB_C3:
    case TB_C4:
      g_app_state->curColor = kPalette[id - TB_C0];
      g_app_state->tool     = TOOL_PEN;
      printf("Color: %ls.\n", kPaletteNames[id - TB_C0]);
      break;
    case TB_CUSTOM:
      mPickCustomColor(hwnd);
      break;
    case TB_SMALLER:
      if (g_app_state->curWidth > BRUSH_MIN)
      {
        g_app_state->curWidth -= 2;
        if (g_app_state->curWidth < BRUSH_MIN)
        {
          g_app_state->curWidth = BRUSH_MIN;
        }
        printf("Brush: %dpx.\n", g_app_state->curWidth);
      }
      break;
    case TB_BIGGER:
      if (g_app_state->curWidth < BRUSH_MAX)
      {
        g_app_state->curWidth += 2;
        if (g_app_state->curWidth > BRUSH_MAX)
        {
          g_app_state->curWidth = BRUSH_MAX;
        }
        printf("Brush: %dpx.\n", g_app_state->curWidth);
      }
      break;
    case TB_UNDO:
      mDoUndo(hwnd);
      break;
    case TB_REDO:
      mDoRedo(hwnd);
      break;
    case TB_CLEAR:
      mClearCanvas(hwnd);
      break;
    case TB_SAVE:
      mDoSave(hwnd);
      break;
    case TB_OPEN:
      mDoOpen(hwnd);
      break;
    case TB_BMP:
      mDoExportBMP(hwnd);
      break;
    default:
      return;
  }
  InvalidateRect(hwnd, NULL, FALSE);
}

void mHandlePaintMessage(_In_ HWND hwnd, _In_ UINT uMsg, _In_ WPARAM wParam, _In_ LPARAM lParam)
{
  HDC hdc;
  PAINTSTRUCT ps;
  RECT rc;

  GetClientRect(hwnd, &rc);

  if (rc.bottom == 0)
  {
    return;
  }

  hdc = BeginPaint(hwnd, &ps);

  // Dark theme: toolbar strip on top, near-black canvas below.
  Toolbar_Paint(hdc, rc.right - rc.left);

  RECT canvasRc = rc;
  canvasRc.top += TOOLBAR_H;
  HBRUSH bgBrush = CreateSolidBrush(CANVAS_BG);
  FillRect(hdc, &canvasRc, bgBrush);
  DeleteObject(bgBrush);

  if ((g_app_state->strokes.Entries != NULL) && (g_app_state->strokes.Size != 0))
  {
    for (unsigned int strokeIdx = 0; strokeIdx < g_app_state->strokes.Size; strokeIdx++)
    {
      Stroke* stroke = &g_app_state->strokes.Entries[strokeIdx];
      if (stroke->Points.Size < 1)
      {
        continue;
      }

      int sx = 0;
      int sy = 0;
      mMapPadToClient(stroke->Points.Entries[0].X, stroke->Points.Entries[0].Y, &sx, &sy);

      HPEN strokePen   = CreatePen(PS_SOLID, stroke->Width, stroke->Color);
      HGDIOBJ oldPen   = SelectObject(hdc, strokePen);
      HBRUSH dotBrush  = CreateSolidBrush(stroke->Color);
      HGDIOBJ oldBrush = SelectObject(hdc, dotBrush);

      if (stroke->Points.Size < 2)
      {
        // Single-point stroke (tap): render as a dot.
        int r = stroke->Width / 2;
        if (r < 1)
        {
          r = 1;
        }
        Ellipse(hdc, sx - r, sy - r, sx + r, sy + r);
      }
      else
      {
        MoveToEx(hdc, sx, sy, (LPPOINT)NULL);
        for (unsigned int pointIdx = 1; pointIdx < stroke->Points.Size; pointIdx++)
        {
          int px = 0;
          int py = 0;
          mMapPadToClient(stroke->Points.Entries[pointIdx].X, stroke->Points.Entries[pointIdx].Y, &px, &py);
          LineTo(hdc, px, py);
        }
      }

      SelectObject(hdc, oldBrush);
      SelectObject(hdc, oldPen);
      DeleteObject(dotBrush);
      DeleteObject(strokePen);
    }
  }

  EndPaint(hwnd, &ps);
}

void mHandleKeyDownMessage(_In_ HWND hwnd, _In_ UINT uMsg, _In_ WPARAM wParam, _In_ LPARAM lParam)
{
  // Ctrl+Z undo, Ctrl+Y / Ctrl+Shift+Z redo. (Key-up path below handles the
  // single-key shortcuts; no HID code is involved in either.)
  int ctrlDown = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
  if (!ctrlDown)
  {
    return;
  }
  int shiftDown = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
  int vk        = (int)wParam;
  if ((vk == 'Z') && !shiftDown)
  {
    mDoUndo(hwnd);
  }
  else if ((vk == 'Y') || ((vk == 'Z') && shiftDown))
  {
    mDoRedo(hwnd);
  }
  else if (vk == 'S')
  {
    mDoSave(hwnd);
  }
  else if (vk == 'O')
  {
    mDoOpen(hwnd);
  }
}

void mHandleKeyUpMessage(_In_ HWND hwnd, _In_ UINT uMsg, _In_ WPARAM wParam, _In_ LPARAM lParam)
{
  clock_t ts = clock();

  printf(FG_GREEN);
  printf("[%d] WM_KEYUP: 0x%x\n", ts, wParam);
  printf(RESET_COLOR);

  int virtual_key_code = (int)wParam;
  if (virtual_key_code == g_app_state->turn_off_drawing_key_code)
  {
    mExitDrawMode(hwnd);
  }
  else if (virtual_key_code == g_app_state->turn_on_drawing_key_code)
  {
    if (g_app_state->is_drawing)
    {
      mExitDrawMode(hwnd);
    }
    else
    {
      mEnterDrawMode(hwnd);
    }
  }
  else if (virtual_key_code == g_app_state->clear_drawing_canvas_key_code)
  {
    mClearCanvas(hwnd);
  }
  else if (virtual_key_code == g_app_state->quit_application_key_code)
  {
    PostQuitMessage(0);
  }
  else if (virtual_key_code == VK_E_KEY)
  {
    g_app_state->tool = (g_app_state->tool == TOOL_PEN) ? TOOL_ERASER : TOOL_PEN;
    printf("Tool: %s.\n", (g_app_state->tool == TOOL_PEN) ? "pen" : "eraser");
    InvalidateRect(hwnd, NULL, FALSE);
  }
  else if ((virtual_key_code >= '1') && (virtual_key_code <= '5'))
  {
    g_app_state->curColor = kPalette[virtual_key_code - '1'];
    g_app_state->tool     = TOOL_PEN;
    printf("Color: %ls.\n", kPaletteNames[virtual_key_code - '1']);
    InvalidateRect(hwnd, NULL, FALSE);
  }
  else if (virtual_key_code == VK_OEM_4)  // '[' smaller brush
  {
    if (g_app_state->curWidth > BRUSH_MIN)
    {
      g_app_state->curWidth -= 2;
      if (g_app_state->curWidth < BRUSH_MIN)
      {
        g_app_state->curWidth = BRUSH_MIN;
      }
      printf("Brush: %dpx.\n", g_app_state->curWidth);
      InvalidateRect(hwnd, NULL, FALSE);
    }
  }
  else if (virtual_key_code == VK_OEM_6)  // ']' bigger brush
  {
    if (g_app_state->curWidth < BRUSH_MAX)
    {
      g_app_state->curWidth += 2;
      if (g_app_state->curWidth > BRUSH_MAX)
      {
        g_app_state->curWidth = BRUSH_MAX;
      }
      printf("Brush: %dpx.\n", g_app_state->curWidth);
      InvalidateRect(hwnd, NULL, FALSE);
    }
  }
}

void mHandleMouseDownMessage(_In_ HWND hwnd, _In_ UINT uMsg, _In_ WPARAM wParam, _In_ LPARAM lParam)
{
  int x = GET_X_LPARAM(lParam);
  int y = GET_Y_LPARAM(lParam);
  RECT rc;
  GetClientRect(hwnd, &rc);
  int id = Toolbar_HitTest(x, y, rc.right - rc.left);
  if (id >= 0)
  {
    Toolbar_DoAction(hwnd, id);
  }
  // Clicks on the canvas do nothing: ink comes only from absolute touch
  // position (WM_INPUT), never from the relative cursor.
}

LRESULT CALLBACK WndProc(_In_ HWND hwnd, _In_ UINT uMsg, _In_ WPARAM wParam, _In_ LPARAM lParam)
{
  clock_t ts = clock();

  switch (uMsg)
  {
    case WM_CREATE:
    {
      mHandleCreateMessage(hwnd, uMsg, wParam, lParam);
      break;
    }
    case WM_INPUT:
    {
      mHandleInputMessage(hwnd, uMsg, wParam, lParam);
      break;
    }
    case WM_PAINT:
    {
      mHandlePaintMessage(hwnd, uMsg, wParam, lParam);
      break;
    }
    case WM_SIZE:
    {
      mHandleResizeMessage(hwnd, uMsg, wParam, lParam);
      break;
    }
    case WM_KEYDOWN:
    {
      mHandleKeyDownMessage(hwnd, uMsg, wParam, lParam);
      break;
    }
    case WM_LBUTTONDOWN:
    {
      mHandleMouseDownMessage(hwnd, uMsg, wParam, lParam);
      break;
    }
    case WM_KEYUP:
    {
      mHandleKeyUpMessage(hwnd, uMsg, wParam, lParam);
      break;
    }
    case WM_SYSKEYUP:
    {
      printf(FG_GREEN);
      printf("[%d] WM_SYSKEYUP: 0x%x\n", ts, wParam);
      printf(RESET_COLOR);
      break;
    }
    case WM_DESTROY:
    {
      PostQuitMessage(0);
      break;
    }
    default:
    {
      return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
  }

  return 0;
}

int CALLBACK wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine, _In_ int nCmdShow)
{
  if (g_app_state == NULL)
  {
    mInitAppState();
  }
  mParseConnectedInputDevices();

  // Read the absolute pad coordinate frame (HID logical ranges). The
  // pad->canvas scale is finished once the window exists (WM_CREATE/WM_SIZE
  // call mSyncCanvasSize -> mUpdateCanvasMap).
  mComputePadRanges();

  WNDCLASSEX wcex;

  wcex.cbSize        = sizeof(WNDCLASSEX);
  wcex.style         = CS_HREDRAW | CS_VREDRAW;
  wcex.lpfnWndProc   = WndProc;
  wcex.cbClsExtra    = 0;
  wcex.cbWndExtra    = 0;
  wcex.hInstance     = hInstance;
  wcex.hIcon         = LoadIcon(hInstance, IDI_APPLICATION);
  wcex.hCursor       = LoadCursor(NULL, IDC_ARROW);
  wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
  wcex.lpszMenuName  = NULL;
  wcex.lpszClassName = szWindowClass;
  wcex.hIconSm       = LoadIcon(wcex.hInstance, IDI_APPLICATION);

  if (!RegisterClassEx(&wcex))
  {
    printf("RegisterClassEx failed at %s:%d\n", __FILE__, __LINE__);
    mGetLastError();
    return -1;
  }

  // TouchDraw is a regular app window (dark theme, painted in WM_PAINT):
  // toolbar strip on top, canvas below. Raw touch input still arrives via
  // RIDEV_INPUTSINK regardless of focus.
  HWND hwnd = CreateWindowEx(0, szWindowClass, szTitle, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1200, 800, NULL, NULL, hInstance, NULL);

  if (!hwnd)
  {
    printf("CreateWindow failed at %s:%d\n", __FILE__, __LINE__);
    mGetLastError();
    return -1;
  }

  ShowWindow(hwnd, nCmdShow);
  UpdateWindow(hwnd);

  MSG msg;
  while (GetMessage(&msg, NULL, 0, 0))
  {
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }

  // Cursor confinement is released here in case the app quits mid-draw-mode.
  ClipCursor(NULL);
  ShowCursor(TRUE);

  return (int)msg.wParam;
}

// One-time application state init. Called from main() (console builds) or
// from wWinMain() when the CRT routes straight to wWinMain (GUI subsystem).
// The NULL guard makes it safe under both paths.
void mInitAppState()
{
  if (g_app_state != NULL)
  {
    return;
  }
  g_app_state = (ApplicationState*)mMalloc(sizeof(ApplicationState), __FILE__, __LINE__);

  g_app_state->device_info_list  = (HID_DEVICE_INFO_LIST){.Entries = NULL, .Size = 0};
  g_app_state->previous_touches  = (TOUCH_DATA_LIST){.Entries = NULL, .Size = 0};
  g_app_state->strokes           = (StrokeList){.Entries = NULL, .Size = 0, .Capacity = 0};
  g_app_state->tracking_touch_id = -1;
  g_app_state->is_drawing        = 0;

  g_app_state->turn_off_drawing_key_code     = VK_ESCAPE;
  g_app_state->turn_on_drawing_key_code      = VK_F3;
  g_app_state->quit_application_key_code     = VK_Q_KEY;
  g_app_state->export_writing_data_key_code  = VK_S_KEY;
  g_app_state->clear_drawing_canvas_key_code = VK_C_KEY;

  g_app_state->tool      = TOOL_PEN;
  g_app_state->curColor  = RGB(255, 255, 255);
  g_app_state->curWidth  = BRUSH_DEFAULT;
  g_app_state->eraseActive = 0;
  g_app_state->erasePushed = 0;
  Hist_Init(&g_app_state->hist);
  g_app_state->curFile[0]  = _T('\0');
  g_app_state->curFileSet  = 0;

  g_app_state->mapValid  = 0;
  g_app_state->padLogMinX = 0;
  g_app_state->padLogMaxX = 0;
  g_app_state->padLogMinY = 0;
  g_app_state->padLogMaxY = 0;
  g_app_state->canvasW   = 0;
  g_app_state->canvasH   = 0;
  g_app_state->mapScale  = 1.0;
  g_app_state->mapOffX   = 0.0;
  g_app_state->mapOffY   = 0.0;

  // Per-monitor DPI awareness BEFORE any monitor/window measurement, so the
  // monitor rect is in physical pixels matching our drawing coordinates.
  // (Win10 1703+. Touches no HID code.)
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}

int main()
{
  mInitAppState();

  return wWinMain(GetModuleHandle(NULL), NULL, GetCommandLine(), SW_SHOWNORMAL);
};
