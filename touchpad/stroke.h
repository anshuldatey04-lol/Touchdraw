#ifndef __STROKE_H__
#define __STROKE_H__
#include <Windows.h>
#include "point2d.h"

// A single pen stroke: raw PAD-unit points plus its render attributes.
// (Points stay in pad/logical units; mapping to client pixels happens at
// draw time so resize/re-map never loses data.)
struct Stroke
{
  Point2DList Points;
  COLORREF Color;
  int Width;
};

typedef struct Stroke Stroke;

struct StrokeList
{
  Stroke* Entries;
  unsigned int Size;
  unsigned int Capacity;
};

typedef struct StrokeList StrokeList;

int mCreateNewStroke(Point2D point, COLORREF color, int width, StrokeList* strokes);
int mAppendToLastStroke(Point2D point, StrokeList* strokes);
void mFreeStroke(Stroke* stroke);
int mCopyStroke(const Stroke* src, Stroke* dst);
void mFreeStrokeList(StrokeList* list);
int mCopyStrokeList(const StrokeList* src, StrokeList* dst);
int mRemoveStrokeAt(StrokeList* list, unsigned int index);
// Hit-test in PAD units: 1 if any segment of the stroke passes within
// radiusPad of (padX, padY).
int mHitTestStroke(const Stroke* stroke, ULONG padX, ULONG padY, double radiusPad);
#endif  // __STROKE_H__
