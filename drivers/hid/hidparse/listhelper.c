/*
 * PROJECT:     ReactOS HID Parser Library
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     common dual linked list operattions
 * COPYRIGHT:   Copyright 2022 Roman Masanin <36927roma@gmail.com>
 */

#include "listhelper.h"
#include <hidpmem.h>

#define NDEBUG
#include <debug.h>

VOID HidParser_ListAddFirst (PHIDPARSER_LIST* Head, PHIDPARSER_LIST* Tail, PHIDPARSER_LIST Entry)
{
    if (*Head == NULL)
    {
        ASSERT(*Head == *Tail);
        *Head = *Tail = Entry;
    }
    else
    {
        Entry->Next = *Head;
        (*Head)->Prev = Entry;
        *Head = Entry;
    }
}

VOID HidParser_ListAddLast (PHIDPARSER_LIST* Head, PHIDPARSER_LIST* Tail, PHIDPARSER_LIST Entry)
{
    if (*Tail == NULL)
    {
        ASSERT(*Head == *Tail);
        *Head = *Tail = Entry;
    }
    else
    {
        Entry->Prev = *Tail;
        (*Tail)->Next = Entry;
        *Tail = Entry;
    }
}

VOID HidParser_ListConnect (PHIDPARSER_LIST* HeadA, PHIDPARSER_LIST* TailA, PHIDPARSER_LIST* HeadB, PHIDPARSER_LIST* TailB)
{
    if (*HeadB == NULL)
    {
        ASSERT(*HeadB == *TailB);
        return;
    }

    if (*TailA == NULL)
    {
        ASSERT(*HeadA == *TailA);
        *HeadA = *HeadB;
        *TailA = *TailB;
    }
    else
    {
        (*TailA)->Next = *HeadB;
        (*HeadB)->Prev = *TailA;
        *TailA = *TailB;
    }
}


VOID HidParser_ListFree (PHIDPARSER_LIST* Head, PHIDPARSER_LIST* Tail)
{
    PHIDPARSER_LIST current, next;

    if (*Head == NULL)
    {
        ASSERT(*Tail == NULL);
        return;
    }

    for (current = *Head; current != NULL; current = next)
    {
        next = current->Next;
        FreeFunction(current);
    }
    *Head = NULL;
    *Tail = NULL;
}
