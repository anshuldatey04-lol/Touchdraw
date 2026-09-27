#ifndef __HISTORY_H__
#define __HISTORY_H__
#include "stroke.h"

// Minimum guaranteed undo depth (PRD FR7 asks for 50).
#define HIST_MAX_DEPTH 64

struct HistoryStack
{
  StrokeList* Snaps;
  unsigned int Size;
  unsigned int Capacity;
};

typedef struct HistoryStack HistoryStack;

struct History
{
  HistoryStack Undo;
  HistoryStack Redo;
};

typedef struct History History;

void Hist_Init(History* h);
void Hist_Free(History* h);
// Push a snapshot of the CURRENT canvas onto the undo stack (call BEFORE
// mutating: pen-down, first-erase-of-gesture, clear). Drops the oldest
// snapshot past HIST_MAX_DEPTH. Does not touch the redo stack.
void Hist_PushUndo(History* h, const StrokeList* current);
// A new user action invalidates the redo stack.
void Hist_ClearRedo(History* h);
// Restore helpers: move current state to the other stack, pop into current.
// Return 0 on success, -1 when the respective stack is empty.
int Hist_Undo(History* h, StrokeList* current);
int Hist_Redo(History* h, StrokeList* current);
int Hist_CanUndo(const History* h);
int Hist_CanRedo(const History* h);
#endif  // __HISTORY_H__
