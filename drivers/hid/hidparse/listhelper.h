/*
 * PROJECT:     ReactOS HID Parser Library
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     common dual linked list operattions
 * COPYRIGHT:   Copyright 2022 Roman Masanin <36927roma@gmail.com>
 */

#include "parser.h"

typedef struct _HIDPARSER_LIST
{
    struct _HIDPARSER_LIST* Prev;
    struct _HIDPARSER_LIST* Next;
} HIDPARSER_LIST, *PHIDPARSER_LIST;

VOID HidParser_ListAddLast (PHIDPARSER_LIST* Head, PHIDPARSER_LIST* Tail, PHIDPARSER_LIST Entry);
VOID HidParser_ListAddFirst (PHIDPARSER_LIST* Head, PHIDPARSER_LIST* Tail, PHIDPARSER_LIST Entry);
VOID HidParser_ListFree (PHIDPARSER_LIST* Head, PHIDPARSER_LIST* Tail);
VOID HidParser_ListConnect (PHIDPARSER_LIST* HeadA, PHIDPARSER_LIST* TailA, PHIDPARSER_LIST* HeadB, PHIDPARSER_LIST* TailB);

#define LIST_ADD_LAST(head, tail, entry) HidParser_ListAddLast((PHIDPARSER_LIST*)&(head), (PHIDPARSER_LIST*)&(tail), (PHIDPARSER_LIST)(entry));
#define LIST_ADD_FIRST(head, tail, entry) HidParser_ListAddFirst((PHIDPARSER_LIST*)&(head), (PHIDPARSER_LIST*)&(tail), (PHIDPARSER_LIST)(entry));
#define LIST_FREE(head, tail) HidParser_ListFree((PHIDPARSER_LIST*)&(head), (PHIDPARSER_LIST*)&(tail));
#define LIST_CONNECT(headA, tailA, headB, tailB) HidParser_ListConnect((PHIDPARSER_LIST*)&(headA), (PHIDPARSER_LIST*)&(tailA), (PHIDPARSER_LIST*)&(headB), (PHIDPARSER_LIST*)&(tailB));
