#include <Windows.h>

#include <stdio.h>

#include "termcolor.h"
#include "utils.h"
#include "stroke.h"

int mCreateNewStroke(Point2D point, COLORREF color, int width, StrokeList* strokes)
{
  if (strokes == NULL)
  {
    printf(FG_RED);
    printf("strokes argument is NULL!\n");
    printf(RESET_COLOR);
    exit(-1);
    return -1;
  }

  if ((strokes->Entries == NULL) || (strokes->Size == 0))
  {
    strokes->Size     = 1;
    strokes->Capacity = 1;
    strokes->Entries  = (Stroke*)mMalloc(sizeof(Stroke), __FILE__, __LINE__);

    strokes->Entries[0].Points.Entries  = NULL;
    strokes->Entries[0].Points.Size     = 0;
    strokes->Entries[0].Points.Capacity = 0;
    strokes->Entries[0].Color           = color;
    strokes->Entries[0].Width           = width;
    return mAppendPoint2DToList(point, &(strokes->Entries[0].Points));
  }

  if (strokes->Size >= strokes->Capacity)
  {
    unsigned int newCapacity = (strokes->Capacity == 0) ? 4 : (strokes->Capacity * 2);
    Stroke* newArray         = (Stroke*)mMalloc(sizeof(Stroke) * newCapacity, __FILE__, __LINE__);

    for (unsigned int strokeIdx = 0; strokeIdx < strokes->Size; strokeIdx++)
    {
      newArray[strokeIdx] = strokes->Entries[strokeIdx];
    }

    free(strokes->Entries);

    strokes->Entries  = newArray;
    strokes->Capacity = newCapacity;
  }

  {
    Stroke* fresh = &strokes->Entries[strokes->Size];
    fresh->Points.Entries  = NULL;
    fresh->Points.Size     = 0;
    fresh->Points.Capacity = 0;
    fresh->Color           = color;
    fresh->Width           = width;
    strokes->Size++;
    return mAppendPoint2DToList(point, &(fresh->Points));
  }

  return 0;
}

int mAppendToLastStroke(Point2D point, StrokeList* strokes)
{
  if ((strokes == NULL) || (strokes->Entries == NULL) || (strokes->Size == 0))
  {
    printf(FG_RED);
    printf("No current stroke to append to!\n");
    printf(RESET_COLOR);
    return -1;
  }
  return mAppendPoint2DToList(point, &(strokes->Entries[strokes->Size - 1].Points));
}

void mFreeStroke(Stroke* stroke)
{
  if (stroke == NULL)
  {
    return;
  }
  if (stroke->Points.Entries != NULL)
  {
    free(stroke->Points.Entries);
    stroke->Points.Entries = NULL;
  }
  stroke->Points.Size     = 0;
  stroke->Points.Capacity = 0;
}

int mCopyStroke(const Stroke* src, Stroke* dst)
{
  if ((src == NULL) || (dst == NULL))
  {
    return -1;
  }
  dst->Color = src->Color;
  dst->Width = src->Width;
  dst->Points.Size     = 0;
  dst->Points.Capacity = 0;
  dst->Points.Entries  = NULL;
  if (src->Points.Size == 0)
  {
    return 0;
  }
  dst->Points.Entries = (Point2D*)mMalloc(sizeof(Point2D) * src->Points.Size, __FILE__, __LINE__);
  for (unsigned int i = 0; i < src->Points.Size; i++)
  {
    dst->Points.Entries[i] = src->Points.Entries[i];
  }
  dst->Points.Size     = src->Points.Size;
  dst->Points.Capacity = src->Points.Size;
  return 0;
}

void mFreeStrokeList(StrokeList* list)
{
  if (list == NULL)
  {
    return;
  }
  if (list->Entries != NULL)
  {
    for (unsigned int i = 0; i < list->Size; i++)
    {
      mFreeStroke(&list->Entries[i]);
    }
    free(list->Entries);
    list->Entries = NULL;
  }
  list->Size     = 0;
  list->Capacity = 0;
}

int mCopyStrokeList(const StrokeList* src, StrokeList* dst)
{
  if ((src == NULL) || (dst == NULL))
  {
    return -1;
  }
  mFreeStrokeList(dst);
  if (src->Size == 0)
  {
    return 0;
  }
  dst->Entries = (Stroke*)mMalloc(sizeof(Stroke) * src->Size, __FILE__, __LINE__);
  for (unsigned int i = 0; i < src->Size; i++)
  {
    // mCopyStroke allocates each point array; roll back on failure.
    if (mCopyStroke(&src->Entries[i], &dst->Entries[i]) != 0)
    {
      for (unsigned int j = 0; j < i; j++)
      {
        mFreeStroke(&dst->Entries[j]);
      }
      free(dst->Entries);
      dst->Entries = NULL;
      return -1;
    }
  }
  dst->Size     = src->Size;
  dst->Capacity = src->Size;
  return 0;
}

int mRemoveStrokeAt(StrokeList* list, unsigned int index)
{
  if ((list == NULL) || (list->Entries == NULL) || (index >= list->Size))
  {
    return -1;
  }
  mFreeStroke(&list->Entries[index]);
  for (unsigned int i = index + 1; i < list->Size; i++)
  {
    list->Entries[i - 1] = list->Entries[i];
  }
  list->Size--;
  return 0;
}

static double mPointSegDistSq(double px, double py, double ax, double ay, double bx, double by)
{
  double dx = bx - ax;
  double dy = by - ay;
  double lenSq = dx * dx + dy * dy;
  double t = 0.0;
  if (lenSq > 0.0)
  {
    t = ((px - ax) * dx + (py - ay) * dy) / lenSq;
    if (t < 0.0)
    {
      t = 0.0;
    }
    else if (t > 1.0)
    {
      t = 1.0;
    }
  }
  double cx = ax + t * dx - px;
  double cy = ay + t * dy - py;
  return cx * cx + cy * cy;
}

int mHitTestStroke(const Stroke* stroke, ULONG padX, ULONG padY, double radiusPad)
{
  if ((stroke == NULL) || (stroke->Points.Size == 0))
  {
    return 0;
  }
  double px = (double)((LONG)padX);
  double py = (double)((LONG)padY);
  double rSq = radiusPad * radiusPad;
  if (stroke->Points.Size == 1)
  {
    double dx = (double)((LONG)stroke->Points.Entries[0].X) - px;
    double dy = (double)((LONG)stroke->Points.Entries[0].Y) - py;
    // Single-point stroke: also account for its drawn dot radius.
    double r = radiusPad + (double)stroke->Width;
    return ((dx * dx + dy * dy) <= r * r) ? 1 : 0;
  }
  for (unsigned int i = 1; i < stroke->Points.Size; i++)
  {
    double ax = (double)((LONG)stroke->Points.Entries[i - 1].X);
    double ay = (double)((LONG)stroke->Points.Entries[i - 1].Y);
    double bx = (double)((LONG)stroke->Points.Entries[i].X);
    double by = (double)((LONG)stroke->Points.Entries[i].Y);
    if (mPointSegDistSq(px, py, ax, ay, bx, by) <= rSq)
    {
      return 1;
    }
  }
  return 0;
}
