#include <stdio.h>

#include "history.h"
#include "termcolor.h"
#include "utils.h"

static void HistStack_Free(HistoryStack* s)
{
  if (s == NULL)
  {
    return;
  }
  if (s->Snaps != NULL)
  {
    for (unsigned int i = 0; i < s->Size; i++)
    {
      mFreeStrokeList(&s->Snaps[i]);
    }
    free(s->Snaps);
    s->Snaps = NULL;
  }
  s->Size     = 0;
  s->Capacity = 0;
}

static void HistStack_Clear(HistoryStack* s)
{
  HistStack_Free(s);
}

static int HistStack_PushCopy(HistoryStack* s, const StrokeList* snap, char* filePath, int lineNumber)
{
  if (s->Size >= HIST_MAX_DEPTH)
  {
    // Drop the oldest snapshot (index 0), shift the rest down.
    mFreeStrokeList(&s->Snaps[0]);
    for (unsigned int i = 1; i < s->Size; i++)
    {
      s->Snaps[i - 1] = s->Snaps[i];
    }
    s->Size--;
  }
  if (s->Size >= s->Capacity)
  {
    unsigned int newCapacity = (s->Capacity == 0) ? 8 : (s->Capacity * 2);
    if (newCapacity > HIST_MAX_DEPTH)
    {
      newCapacity = HIST_MAX_DEPTH;
    }
    StrokeList* grown = (StrokeList*)mMalloc(sizeof(StrokeList) * newCapacity, filePath, lineNumber);
    for (unsigned int i = 0; i < s->Size; i++)
    {
      grown[i] = s->Snaps[i];
    }
    free(s->Snaps);
    s->Snaps    = grown;
    s->Capacity = newCapacity;
  }
  {
    StrokeList* slot = &s->Snaps[s->Size];
    slot->Entries  = NULL;
    slot->Size     = 0;
    slot->Capacity = 0;
    if (mCopyStrokeList(snap, slot) != 0)
    {
      printf(FG_RED);
      printf("HistStack_PushCopy: snapshot copy failed at %s:%d\n", filePath, lineNumber);
      printf(RESET_COLOR);
      return -1;
    }
    s->Size++;
  }
  return 0;
}

void Hist_Init(History* h)
{
  if (h == NULL)
  {
    return;
  }
  h->Undo.Snaps = NULL;
  h->Undo.Size = 0;
  h->Undo.Capacity = 0;
  h->Redo.Snaps = NULL;
  h->Redo.Size = 0;
  h->Redo.Capacity = 0;
}

void Hist_Free(History* h)
{
  if (h == NULL)
  {
    return;
  }
  HistStack_Free(&h->Undo);
  HistStack_Free(&h->Redo);
}

void Hist_PushUndo(History* h, const StrokeList* current)
{
  if ((h == NULL) || (current == NULL))
  {
    return;
  }
  HistStack_PushCopy(&h->Undo, current, __FILE__, __LINE__);
}

void Hist_ClearRedo(History* h)
{
  if (h == NULL)
  {
    return;
  }
  HistStack_Clear(&h->Redo);
}

int Hist_Undo(History* h, StrokeList* current)
{
  StrokeList tmp = {0};
  if ((h == NULL) || (current == NULL) || (h->Undo.Size == 0))
  {
    return -1;
  }
  // Stash current state on the redo stack, then restore the newest undo snap.
  if (HistStack_PushCopy(&h->Redo, current, __FILE__, __LINE__) != 0)
  {
    return -1;
  }
  tmp = h->Undo.Snaps[h->Undo.Size - 1];
  h->Undo.Size--;
  mFreeStrokeList(current);
  *current   = tmp;
  tmp.Entries = NULL;  // ownership moved; keep the analyzer calm
  tmp.Size = 0;
  tmp.Capacity = 0;
  return 0;
}

int Hist_Redo(History* h, StrokeList* current)
{
  StrokeList tmp = {0};
  if ((h == NULL) || (current == NULL) || (h->Redo.Size == 0))
  {
    return -1;
  }
  if (HistStack_PushCopy(&h->Undo, current, __FILE__, __LINE__) != 0)
  {
    return -1;
  }
  tmp = h->Redo.Snaps[h->Redo.Size - 1];
  h->Redo.Size--;
  mFreeStrokeList(current);
  *current   = tmp;
  tmp.Entries = NULL;
  tmp.Size = 0;
  tmp.Capacity = 0;
  // Redo restores a state that was already counted against the depth cap;
  // re-apply the cap in case the push above exceeded it.
  while (h->Undo.Size > HIST_MAX_DEPTH)
  {
    mFreeStrokeList(&h->Undo.Snaps[0]);
    for (unsigned int i = 1; i < h->Undo.Size; i++)
    {
      h->Undo.Snaps[i - 1] = h->Undo.Snaps[i];
    }
    h->Undo.Size--;
  }
  return 0;
}

int Hist_CanUndo(const History* h)
{
  return (h != NULL) && (h->Undo.Size > 0);
}

int Hist_CanRedo(const History* h)
{
  return (h != NULL) && (h->Redo.Size > 0);
}
