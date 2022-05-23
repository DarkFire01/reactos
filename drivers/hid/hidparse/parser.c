/*
 * PROJECT:     ReactOS HID Parser Library
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * FILE:        drivers/hid/hidparse/parser.c
 * PURPOSE:     HID Parser
 * COPYRIGHT:   Copyright (C) Roman Masanin (36927roma@gmail.com) 2022
 */

/* Purpose of this file is to parse the HID report descriptor into preparsed data
 * structure. The idea about this structure arragement is taken from wine, and extended
 * based on results of tests(see ).
 * TODO:
 * Delimeters
 * PoolType
 * DebugField
 */

#include "parser.h"
#include "listhelper.h"
#include <hidpmem.h>

#define NDEBUG
#include <debug.h>

/* HidParser_CombinePreparsedData takes the ValueCaps, Nodes and ReportIDs
 *  to combine into actual preparsedData structure
 */
NTSTATUS
HidParser_CombinePreparsedData(
    IN PHIDPARSER_VALUE_CAPS_LIST OutputListHead,
    IN PHIDPARSER_NODES_LIST NodesListTail,
    IN PHIDPARSER_REPORT_IDS_STACK ReportIDsStack,
    OUT PHIDPARSER_COLLECTION_STACK CurrentCollection)
{
    UINT32 valuesCount = 0, nodesCount = 0, dataCount = 0;
    USHORT inputReportMax = 0, outputReportMax = 0, featureReportMax = 0;
    PUCHAR currentOffset = 0;

    PHIDPARSER_VALUE_CAPS_LIST outputListTmp = NULL;
    PHIDPARSER_NODES_LIST nodesListTemp = NULL;
    PHIDPARSER_REPORT_IDS_STACK reportIDsTemp;
    PHIDPARSER_PREPARSED_DATA preparsedData = NULL;

    if (NodesListTail == NULL)
    {
        return STATUS_COULD_NOT_INTERPRET;
    }

    for (outputListTmp = OutputListHead; outputListTmp != NULL; outputListTmp = outputListTmp->Next)
    {
        valuesCount++;
    }

    for (nodesListTemp = NodesListTail; nodesListTemp != NULL; nodesListTemp = nodesListTemp->Prev)
    {
        nodesCount++;
    }

    preparsedData = AllocFunction(
        sizeof(HIDPARSER_PREPARSED_DATA) + sizeof(HIDPARSER_VALUE_CAPS) * (valuesCount - 1) +
        sizeof(HIDPARSER_LINK_COLLECTION_NODE) * nodesCount);
    if (preparsedData == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    currentOffset = (PUCHAR)preparsedData->ValueCaps;

    for (reportIDsTemp = ReportIDsStack; reportIDsTemp != NULL; reportIDsTemp = reportIDsTemp->Next)
    {
        if (reportIDsTemp->Value.CollectionNumber == CurrentCollection->Index)
        {
            if (reportIDsTemp->Value.InputLength > inputReportMax)
            {
                inputReportMax = reportIDsTemp->Value.InputLength;
            }
            if (reportIDsTemp->Value.OutputLength > outputReportMax)
            {
                outputReportMax = reportIDsTemp->Value.OutputLength;
            }
            if (reportIDsTemp->Value.FeatureLength > featureReportMax)
            {
                featureReportMax = reportIDsTemp->Value.FeatureLength;
            }
        }
    }
    inputReportMax = inputReportMax > 0 ? (inputReportMax / 8) + 1 : 0;
    outputReportMax = outputReportMax > 0 ? (outputReportMax / 8) + 1 : 0;
    featureReportMax = featureReportMax > 0 ? (featureReportMax / 8) + 1 : 0;

    preparsedData->CapsByteLength = 0;

    preparsedData->InputCapsStart = 0;
    preparsedData->InputCapsCount = 0;
    preparsedData->InputCapsEnd = 0;
    preparsedData->InputReportByteLength = inputReportMax;
    dataCount = 0;
    for (outputListTmp = OutputListHead; outputListTmp != NULL; outputListTmp = outputListTmp->Next)
    {
        if (outputListTmp->ReportType == HidP_Input)
        {
            outputListTmp->Value.DataIndexMin = dataCount;
            dataCount += outputListTmp->Value.UsageMax - outputListTmp->Value.UsageMin;
            outputListTmp->Value.DataIndexMax = dataCount;
            dataCount++;

            CopyFunction(currentOffset, &outputListTmp->Value, sizeof(HIDPARSER_VALUE_CAPS));

            preparsedData->InputCapsCount++;
            preparsedData->InputCapsEnd++;
            preparsedData->CapsByteLength += sizeof(HIDPARSER_VALUE_CAPS);
            currentOffset += sizeof(HIDPARSER_VALUE_CAPS);
        }
    }

    preparsedData->OutputCapsStart = preparsedData->InputCapsEnd;
    preparsedData->OutputCapsCount = 0;
    preparsedData->OutputCapsEnd = preparsedData->InputCapsEnd;
    preparsedData->OutputReportByteLength = outputReportMax;
    dataCount = 0;
    for (outputListTmp = OutputListHead; outputListTmp != NULL; outputListTmp = outputListTmp->Next)
    {
        if (outputListTmp->ReportType == HidP_Output)
        {
            outputListTmp->Value.DataIndexMin = dataCount;
            dataCount += outputListTmp->Value.UsageMax - outputListTmp->Value.UsageMin;
            outputListTmp->Value.DataIndexMax = dataCount;
            dataCount++;

            CopyFunction(currentOffset, &outputListTmp->Value, sizeof(HIDPARSER_VALUE_CAPS));

            preparsedData->OutputCapsCount++;
            preparsedData->OutputCapsEnd++;
            preparsedData->CapsByteLength += sizeof(HIDPARSER_VALUE_CAPS);
            currentOffset += sizeof(HIDPARSER_VALUE_CAPS);
        }
    }

    preparsedData->FeatureCapsStart = preparsedData->OutputCapsEnd;
    preparsedData->FeatureCapsCount = 0;
    preparsedData->FeatureCapsEnd = preparsedData->OutputCapsEnd;
    preparsedData->FeatureReportByteLength = featureReportMax;
    dataCount = 0;
    for (outputListTmp = OutputListHead; outputListTmp != NULL; outputListTmp = outputListTmp->Next)
    {
        if (outputListTmp->ReportType == HidP_Feature)
        {
            outputListTmp->Value.DataIndexMin = dataCount;
            dataCount += outputListTmp->Value.UsageMax - outputListTmp->Value.UsageMin;
            outputListTmp->Value.DataIndexMax = dataCount;
            dataCount++;

            CopyFunction(currentOffset, &outputListTmp->Value, sizeof(HIDPARSER_VALUE_CAPS));

            preparsedData->FeatureCapsCount++;
            preparsedData->FeatureCapsEnd++;
            preparsedData->CapsByteLength += sizeof(HIDPARSER_VALUE_CAPS);
            currentOffset += sizeof(HIDPARSER_VALUE_CAPS);
        }
    }

    preparsedData->LinkCollectionCount = 0;
    for (nodesListTemp = NodesListTail; nodesListTemp != NULL; nodesListTemp = nodesListTemp->Prev)
    {
        CopyFunction(currentOffset, &nodesListTemp->Value, sizeof(HIDPARSER_LINK_COLLECTION_NODE));

        preparsedData->LinkCollectionCount++;
        currentOffset += sizeof(HIDPARSER_LINK_COLLECTION_NODE);
    }
    preparsedData->Usage = NodesListTail->Value.LinkUsage;
    preparsedData->UsagePage = NodesListTail->Value.LinkUsagePage;

    // return created data
    CopyFunction(preparsedData->Magic, PreparsedMagic, sizeof(CurrentCollection->PreparsedData->Magic));
    CurrentCollection->PreparsedData = preparsedData;
    CurrentCollection->PreparsedSize = sizeof(HIDPARSER_PREPARSED_DATA) + sizeof(HIDPARSER_VALUE_CAPS) * (valuesCount - 1) +
        sizeof(HIDPARSER_LINK_COLLECTION_NODE) * nodesCount;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
HidParser_GetCollectionDescription(
    IN PHIDP_REPORT_DESCRIPTOR ReportDesc,
    IN ULONG DescLength,
    IN POOL_TYPE PoolType,
    OUT PHIDP_DEVICE_DESC DeviceDescription)
{
    NTSTATUS status;
    PUCHAR CurrentOffset, ReportEnd;

    ULONG CurrentItemSize;
    PREPORT_ITEM CurrentItem;
    ITEM_DATA Data;
    HIDP_REPORT_TYPE reportType;
    USHORT* reportSizeBits;

    PHIDPARSER_COLLECTION_STACK collectionStack, collectionTemp;
    PHIDPARSER_GLOBAL_STACK globalStack, globalStackTemp;
    PHIDPARSER_REPORT_IDS_STACK reportIDsStack, reportIDsTemp;

    PHIDPARSER_VALUE_CAPS_LIST workingListHead, workingListTail, workingListTemp;
    PHIDPARSER_VALUE_CAPS_LIST outputListHead, outputListTail;
    PHIDPARSER_NODES_LIST nodesListHead, nodesListTail, nodesListTemp, currentNode;

    INT32 nodeDepth, workingCount;

    if (DeviceDescription == NULL)
    {
        return STATUS_NO_DATA_DETECTED;
    }

    ZeroFunction(DeviceDescription, sizeof(HIDP_DEVICE_DESC));

    CurrentOffset = ReportDesc;
    ReportEnd = ReportDesc + DescLength;

    if (DescLength == 0 || ReportDesc >= ReportEnd)
    {
        return STATUS_NO_DATA_DETECTED;
    }

    globalStack = AllocFunction(sizeof(HIDPARSER_GLOBAL_STACK));
    if (globalStack == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    workingListHead = workingListTail = AllocFunction(sizeof(HIDPARSER_VALUE_CAPS_LIST));
    if (workingListHead == NULL)
    {
        FreeFunction(globalStack);
        return STATUS_INSUFFICIENT_RESOURCES;
    };

    status = STATUS_SUCCESS;

    collectionStack = NULL;
    reportIDsStack = NULL;
    outputListHead = outputListTail = NULL;
    nodesListHead = nodesListTail = NULL;
    currentNode = NULL;
    nodeDepth = 0;
    workingCount = 0;
    do
    {
        // get current item
        CurrentItem = (PREPORT_ITEM)CurrentOffset;

        // get item size
        CurrentItemSize = 0;
        Data.Raw.UData = 0;
        Data.Raw.SData = 0;

        // get associated data
        switch (CurrentItem->Size)
        {
            case ITEM_SIZE_0:
                // Data is zero already
                break;
            case ITEM_SIZE_1:
                CurrentItemSize = 1;
                Data.Raw.UData = CurrentItem->Data.Short8.Value;
                Data.Raw.SData = (CHAR)CurrentItem->Data.Short8.Value;
                break;
            case ITEM_SIZE_2:
                CurrentItemSize = 2;
                Data.Raw.UData = CurrentItem->Data.Short16.Value;
                Data.Raw.SData = (SHORT)CurrentItem->Data.Short16.Value;
                break;
            case ITEM_SIZE_4:
                CurrentItemSize = 4;
                Data.Raw.UData = CurrentItem->Data.Short32.Value;
                Data.Raw.SData = (LONG)CurrentItem->Data.Short32.Value;
                break;
        }

        // handle items
        switch ((ITEM_TYPE)CurrentItem->Type)
        {
            case ITEM_TYPE_MAIN:
            {

                switch ((MAIN_ITEM_TYPE)CurrentItem->Tag)
                {
                    case MAIN_ITEM_TAG_COLLECTION:
                    {
                        DPRINT("[HIDPARSE] ITEM_TAG_MAIN_COLLECTION 0x%x\n", Data.UData16.Value);
                        ASSERT(workingListHead != NULL);

                        // process new collection
                        if (nodeDepth == 0)
                        {
                            // allocate new collection
                            collectionTemp = AllocFunction(sizeof(HIDPARSER_COLLECTION_STACK));
                            if (collectionTemp == NULL)
                            {
                                status = STATUS_INSUFFICIENT_RESOURCES;
                                goto freeContext;
                            }

                            // calculate collection index
                            collectionTemp->Index = collectionStack != NULL ? collectionStack->Index + 1 : 0;

                            // add new item to the stack
                            collectionTemp->Next = collectionStack;
                            collectionStack = collectionTemp;
                        }

                        currentNode = AllocFunction(sizeof(HIDPARSER_NODES_LIST));
                        if (currentNode == NULL)
                        {
                            status = STATUS_INSUFFICIENT_RESOURCES;
                            goto freeContext;
                        }

                        if (workingListHead->Value.UsagePage == 0)
                        {
                            workingListHead->Value.UsagePage = globalStack->UsagePage;
                        }

                        currentNode->Value.LinkUsage = workingListHead->Value.UsageMin;
                        currentNode->Value.LinkUsagePage = workingListHead->Value.UsagePage;
                        currentNode->Value.CollectionType = Data.UData16.Value;
                        currentNode->Value.Parent = 0;
                        currentNode->Value.NextSibling = 0;
                        currentNode->Depth = nodeDepth + 1;
                        currentNode->Index = nodesListHead != NULL ? nodesListHead->Index + 1 : 0;

                        if (nodeDepth > 0)
                        {
                            // find parent
                            for (nodesListTemp = nodesListHead; nodesListTemp != NULL; nodesListTemp = nodesListTemp->Next)
                            {
                                if (nodesListTemp->Depth == nodeDepth)
                                {
                                    currentNode->Value.Parent = nodesListTemp->Index;
                                    currentNode->Value.NextSibling = nodesListTemp->Value.FirstChild;
                                    nodesListTemp->Value.NumberOfChildren++;
                                    nodesListTemp->Value.FirstChild = currentNode->Index;
                                    break;
                                }
                            }
                            ASSERT(nodesListTemp != NULL);
                        }

                        LIST_ADD_FIRST(nodesListHead, nodesListTail, currentNode);
                        nodeDepth++;
                    }
                    break;
                    case MAIN_ITEM_TAG_END_COLLECTION:
                    {
                        DPRINT("[HIDPARSE] ITEM_TAG_MAIN_END_COLLECTION 0x%x\n", Data.UData16.Value);
                        nodeDepth--;
                        if (nodeDepth < 0)
                        {
                            DPRINT("[HIDPARSE] Collection was closed, but no open was associated with it");
                            status = STATUS_COULD_NOT_INTERPRET;
                            goto freeContext;
                        }

                        // the top level collection was closed, combine all information about it.
                        if (nodeDepth == 0)
                        {
                            // put everything toogether
                            status = HidParser_CombinePreparsedData(outputListHead, nodesListTail, reportIDsStack, collectionStack);
                            if (status != STATUS_SUCCESS)
                            {
                                goto freeContext;
                            }
                            ASSERT(collectionStack->PreparsedData != NULL);

                            // free output and nodes stacks
                            LIST_FREE(outputListHead, outputListTail);
                            LIST_FREE(nodesListHead, nodesListTail);
                        }
                        else
                        {
                            // find parent
                            for (nodesListTemp = nodesListHead; nodesListTemp != NULL; nodesListTemp = nodesListTemp->Next)
                            {
                                if (nodesListTemp->Depth == nodeDepth)
                                {
                                    currentNode = nodesListTemp;
                                    break;
                                }
                            }
                            if (nodesListTemp == NULL)
                            {
                                DPRINT("[HIDPARSE] Error, no parent found for node");
                                ASSERT(FALSE);
                            }
                            currentNode = nodesListTemp;
                        }
                    }
                    break;
                    case MAIN_ITEM_TAG_INPUT:
                    case MAIN_ITEM_TAG_OUTPUT:
                    case MAIN_ITEM_TAG_FEATURE:
                    {
                        DPRINT("[HIDPARSE] ITEM_TAG_MAIN_INPUT 0x%x\n", Data.UData16.Value);
                        // we must have atleast 1 usages in stack
                        ASSERT(workingListTail != NULL);
                        ASSERT(currentNode != NULL);

                        reportSizeBits = NULL;

                        // process reports stack
                        for (reportIDsTemp = reportIDsStack; reportIDsTemp != NULL; reportIDsTemp = reportIDsTemp->Next)
                        {
                            if (reportIDsTemp->Value.CollectionNumber == collectionStack->Index)
                            {
                                if (reportIDsTemp->Value.ReportID == globalStack->ReportID)
                                {
                                    break;
                                }
                            }
                        }
                        if (reportIDsTemp == NULL)
                        {
                            reportIDsTemp = AllocFunction(sizeof(HIDPARSER_REPORT_IDS_STACK));
                            if (reportIDsTemp == NULL)
                            {
                                status = STATUS_INSUFFICIENT_RESOURCES;
                                goto freeContext;
                            }

                            reportIDsTemp->Value.ReportID = globalStack->ReportID;
                            // actually hid spec does not define reports limit
                            reportIDsTemp->Value.CollectionNumber = (UCHAR)collectionStack->Index;

                            reportIDsTemp->Next = reportIDsStack;
                            reportIDsStack = reportIDsTemp;
                        }
                        switch (CurrentItem->Tag)
                        {
                            case MAIN_ITEM_TAG_INPUT:
                                reportSizeBits = &reportIDsTemp->Value.InputLength;
                                reportType = HidP_Input;
                                break;
                            case MAIN_ITEM_TAG_OUTPUT:
                                reportSizeBits = &reportIDsTemp->Value.OutputLength;
                                reportType = HidP_Output;
                                break;
                            case MAIN_ITEM_TAG_FEATURE:
                                reportSizeBits = &reportIDsTemp->Value.FeatureLength;
                                reportType = HidP_Feature;
                                break;
                            default:
                                reportType = -1;
                                ASSERT(FALSE);
                                break;
                        }

                        workingListTail->Value.BitField.Raw = Data.UData32.Value;

                        // if usage was not setted up, and type is constant
                        if (workingCount < 1)
                        {
                            if (workingListTail->Value.BitField.Flags.IsConstant == TRUE)
                            {
                                // skip bits and exit
                                *reportSizeBits += globalStack->ReportSize * globalStack->ReportCount;
                                break;
                            }
                            else
                            {
                                // variable with no usage
                                ASSERT(FALSE);
                            }
                        }

                        // process usages
                        for (workingListTemp = workingListTail; workingListTemp != NULL; workingListTemp = workingListTemp->Prev)
                        {
                            // copy common items
                            workingListTemp->ReportType = reportType;
                            workingListTemp->Value.BitField.Raw = Data.UData32.Value;
                            // copy global items
                            workingListTemp->Value.Flags.UnknownGlobalCount = globalStack->UnknownGlobalCount;
                            CopyFunction(workingListTemp->Value.UnknownGlobals, globalStack->UnknownGlobals, sizeof(workingListTemp->Value.UnknownGlobals));
                            workingListTemp->Value.Units = globalStack->Units;
                            workingListTemp->Value.UnitsExp = globalStack->UnitsExp;
                            workingListTemp->Value.ReportSize = globalStack->ReportSize;
                            workingListTemp->Value.ReportID = globalStack->ReportID;
                            workingListTemp->Value.ReportCount = globalStack->ReportCount;
                            if (workingListTemp->Value.UsagePage == 0)
                            {
                                workingListTemp->Value.UsagePage = globalStack->UsagePage;
                            }

                            // setup node linkage
                            workingListTemp->Value.LinkCollection = currentNode->Index;
                            workingListTemp->Value.LinkUsagePage = currentNode->Value.LinkUsagePage;
                            workingListTemp->Value.LinkUsage = currentNode->Value.LinkUsage;

                            // process size
                            if (workingCount > 1)
                            {
                                if (workingListTemp->Value.ReportCount > 1)
                                {
                                    ASSERT(workingListTemp->Value.ReportCount == workingCount);
                                    workingListTemp->Value.ReportCount = 1;
                                }
                            }
                            workingListTemp->Value.StartByte = (*reportSizeBits / 8) + 1;
                            workingListTemp->Value.StartBit = *reportSizeBits % 8;
                            workingListTemp->Value.TotalBits =
                                workingListTemp->Value.ReportSize * workingListTemp->Value.ReportCount;
                            workingListTemp->Value.EndByte =
                                ((*reportSizeBits + 7 + workingListTemp->Value.TotalBits) / 8) + 1;
                            if (workingListTemp->Value.BitField.Flags.IsVariable == TRUE || workingListTemp->Prev == NULL)
                            {
                                // always add for variable and add once for arrays
                                *reportSizeBits += workingListTemp->Value.TotalBits;
                            }

                            // process flags
                            if (workingListTemp->Value.BitField.Flags.IsVariable == FALSE)
                            {
                                workingListTemp->Value.Flags.IsButton = TRUE;
                                workingListTemp->Value.Logical.Button.Min = globalStack->LogicalMin;
                                workingListTemp->Value.Logical.Button.Max = globalStack->LogicalMax;
                                // if it's usages array setup hasMore flag
                                if (workingListTemp->Next != NULL) // not last
                                {
                                    workingListTemp->Value.Flags.ArrayHasMore = TRUE;
                                }
                            }
                            else if (workingListTemp->Value.ReportSize == 1)
                            {
                                // variable with size 1 is button too
                                workingListTemp->Value.Flags.IsButton = TRUE;
                            }
                            else
                            {
                                workingListTemp->Value.Logical.Value.Min = globalStack->LogicalMin;
                                workingListTemp->Value.Logical.Value.Max = globalStack->LogicalMax;
                                workingListTemp->Value.Logical.Value.NullValue = workingListTemp->Value.BitField.Flags.NullState;
                                workingListTemp->Value.PhysicalMax = globalStack->PhysicalMax;
                                workingListTemp->Value.PhysicalMin = globalStack->PhysicalMin;
                            }

                            workingListTemp->Value.Flags.IsAbsolute = !workingListTemp->Value.BitField.Flags.IsRelative;
                            workingListTemp->Value.Flags.IsConstant = workingListTemp->Value.BitField.Flags.IsConstant;
                        }

                        LIST_CONNECT(outputListHead, outputListTail, workingListHead, workingListTail);
                        workingListHead = workingListTail = AllocFunction(sizeof(HIDPARSER_VALUE_CAPS_LIST));
                        if (workingListHead == NULL)
                        {
                            status = STATUS_INSUFFICIENT_RESOURCES;
                            goto freeContext;
                        }
                    }
                    break;
                    default:
                        DPRINT(
                            "[HIDPARSE] Unknown ReportType Tag %x Type %x Size %x CurrentItemSize %x\n",
                            CurrentItem->Tag, CurrentItem->Type, CurrentItem->Size, CurrentItemSize);
                        status = STATUS_ILLEGAL_INSTRUCTION;
                        goto freeContext;
                        break;
                }

                // main item should reset local items
                workingListTemp = workingListHead->Next;
                // free all but first one (to avoid allocation)
                if (workingListTemp != NULL)
                {
                    // unlink from the list
                    workingListTemp->Prev = NULL;
                    // free all but workingListHead
                    LIST_FREE(workingListTemp, workingListTail);
                }
                ZeroFunction(workingListHead, sizeof(HIDPARSER_VALUE_CAPS_LIST));
                workingListTail = workingListHead;
                workingCount = 0;
                break;
            }
            case ITEM_TYPE_GLOBAL:
            {
                switch (CurrentItem->Tag)
                {
                    case GLOBAL_ITEM_TAG_USAGE_PAGE:
                        DPRINT("[HIDPARSE] ITEM_TAG_GLOBAL_USAGE_PAGE 0x%x\n", Data.UData16.Value);
                        globalStack->UsagePage = Data.UData16.Value;
                        break;
                    case GLOBAL_ITEM_TAG_LOGICAL_MINIMUM:
                        DPRINT("[HIDPARSE] ITEM_TAG_GLOBAL_LOGICAL_MINIMUM 0x%x\n", Data.SData32.Value);
                        globalStack->LogicalMin = Data.SData32.Value;
                        break;

                    case GLOBAL_ITEM_TAG_LOGICAL_MAXIMUM:
                        DPRINT("[HIDPARSE] ITEM_TAG_GLOBAL_LOCAL_MAXIMUM 0x%x\n", Data.SData32.Value);
                        globalStack->LogicalMax = Data.SData32.Value;
                        break;

                    case GLOBAL_ITEM_TAG_PHYSICAL_MINIMUM:
                        DPRINT("[HIDPARSE] ITEM_TAG_GLOBAL_PHYSICAL_MINIMUM 0x%x\n", Data.SData32.Value);
                        globalStack->PhysicalMin = Data.SData32.Value;
                        break;

                    case GLOBAL_ITEM_TAG_PHYSICAL_MAXIMUM:
                        DPRINT("[HIDPARSE] ITEM_TAG_GLOBAL_PHYSICAL_MAXIMUM 0x%x\n", Data.SData32.Value);
                        globalStack->PhysicalMax = Data.SData32.Value;
                        break;

                    case GLOBAL_ITEM_TAG_UNIT_EXPONENT:
                        DPRINT("[HIDPARSE] ITEM_TAG_GLOBAL_REPORT_UNIT_EXPONENT 0x%x\n", Data.SData32.Value);
                        globalStack->UnitsExp = Data.SData32.Value;
                        break;

                    case GLOBAL_ITEM_TAG_UNIT:
                        DPRINT("[HIDPARSE] ITEM_TAG_GLOBAL_REPORT_UNIT 0x%x\n", Data.SData32.Value);
                        globalStack->Units = Data.SData32.Value;
                        break;

                    case GLOBAL_ITEM_TAG_REPORT_SIZE:
                        DPRINT("[HIDPARSE] ITEM_TAG_GLOBAL_REPORT_SIZE 0x%x\n", Data.UData16.Value);
                        globalStack->ReportSize = Data.UData16.Value;
                        break;

                    case GLOBAL_ITEM_TAG_REPORT_ID:
                        DPRINT("[HIDPARSE] ITEM_TAG_GLOBAL_REPORT_ID 0x%x\n", Data.UData8.Value);
                        globalStack->ReportID = Data.UData8.Value;
                        if (globalStack->ReportID == 0)
                        {
                            status = HIDP_STATUS_INVALID_REPORT_TYPE;
                            goto freeContext;
                        }
                        break;

                    case GLOBAL_ITEM_TAG_REPORT_COUNT:
                        DPRINT("[HIDPARSE] ITEM_TAG_GLOBAL_REPORT_COUNT 0x%x\n", Data.UData16.Value);
                        globalStack->ReportCount = Data.UData16.Value;
                        break;

                    case GLOBAL_ITEM_TAG_PUSH:
                    {
                        DPRINT("[HIDPARSE] ITEM_TAG_GLOBAL_PUSH\n");
                        // allocate global item state
                        globalStackTemp = (PHIDPARSER_GLOBAL_STACK)AllocFunction(sizeof(HIDPARSER_GLOBAL_STACK));
                        ASSERT(globalStackTemp);

                        // copy global item state
                        CopyFunction(globalStackTemp, globalStack, sizeof(HIDPARSER_GLOBAL_STACK));

                        // store pushed item in link member
                        globalStackTemp->Next = globalStack;
                        globalStack = globalStackTemp;
                        break;
                    }
                    case GLOBAL_ITEM_TAG_POP:
                    {
                        DPRINT("[HIDPARSE] ITEM_TAG_GLOBAL_POP\n");
                        if (globalStack->Next == NULL)
                        {
                            // pop without push
                            DPRINT("[HIDPARSE] ITEM_TAG_GLOBAL_POP Stack underflow!!!\n");
                            status = STATUS_COULD_NOT_INTERPRET;
                            goto freeContext;
                        }

                        // get link
                        globalStackTemp = globalStack->Next;

                        // free item
                        FreeFunction(globalStack);

                        // replace current item with linked one
                        globalStack = globalStackTemp;

                        break;
                    }

                    default:
                        ASSERT(globalStack->UnknownGlobalCount < 5);
                        if (globalStack->UnknownGlobalCount < 4)
                        {
                            globalStack->UnknownGlobalCount++;
                        }
                        globalStack->UnknownGlobals[globalStack->UnknownGlobalCount - 1].Token = CurrentItem->Tag;
                        globalStack->UnknownGlobals[globalStack->UnknownGlobalCount - 1].BitField = Data.UData32.Value;
                        break;
                }

                break;
            }
            case ITEM_TYPE_LOCAL:
            {
                switch (CurrentItem->Tag)
                {
                    case ITEM_TAG_LOCAL_USAGE:
                    case ITEM_TAG_LOCAL_USAGE_MAXIMUM:
                    {
                        DPRINT("[HIDPARSE] ITEM_TAG_LOCAL_USAGE Data 0x%x\n", Data.UData16.Value);
                        if (workingCount > 0)
                        {
                            workingListTemp = AllocFunction(sizeof(HIDPARSER_VALUE_CAPS_LIST));
                            if (workingListTemp == NULL)
                            {
                                status = STATUS_INSUFFICIENT_RESOURCES;
                                goto freeContext;
                            }
                            CopyFunction(&workingListTemp->Value, &workingListHead->Value, sizeof(HIDPARSER_VALUE_CAPS));
                            LIST_ADD_FIRST(workingListHead, workingListTail, workingListTemp);
                        }
                        workingCount++;

                        if (CurrentItem->Tag == ITEM_TAG_LOCAL_USAGE)
                        {
                            workingListHead->Value.UsageMin = Data.UData16.Value;
                        }
                        else
                        {
                            workingListHead->Value.Flags.IsRange = TRUE;
                        }
                        workingListHead->Value.UsageMax = Data.UData16.Value;
                        if (CurrentItemSize == 4)
                        {
                            workingListHead->Value.UsagePage = Data.UData16.Value2;
                        }
                    }
                    break;

                    case ITEM_TAG_LOCAL_USAGE_MINIMUM:
                        DPRINT("[HIDPARSE] ITEM_TAG_LOCAL_USAGE_MINIMUM 0x%x\n", Data.UData16.Value);
                        // usage min alone cannot be used to define an usage, usage max is must be called anyway
                        workingListHead->Value.UsageMin = Data.UData16.Value;
                        if (CurrentItemSize == 4)
                        {
                            workingListHead->Value.UsagePage = Data.UData16.Value2;
                        }
                        break;
                    case ITEM_TAG_LOCAL_DESIGNATOR_INDEX:
                        DPRINT("[HIDPARSE] ITEM_TAG_LOCAL_DESIGNATOR_INDEX 0x%x\n", Data.UData16.Value);
                        workingListHead->Value.DesignatorMin = Data.UData16.Value;
                        workingListHead->Value.DesignatorMax = Data.UData16.Value;
                        break;

                    case ITEM_TAG_LOCAL_DESIGNATOR_MINIMUM:
                        DPRINT("[HIDPARSE] ITEM_TAG_LOCAL_DESIGNATOR_MINIMUM 0x%x\n", Data.UData16.Value);
                        workingListHead->Value.DesignatorMin = Data.UData16.Value;
                        break;

                    case ITEM_TAG_LOCAL_DESIGNATOR_MAXIMUM:
                        DPRINT("[HIDPARSE] ITEM_TAG_LOCAL_DESIGNATOR_MAXIMUM 0x%x\n", Data.UData16.Value);
                        workingListHead->Value.DesignatorMax = Data.UData16.Value;
                        workingListHead->Value.Flags.IsDesignatorRange = TRUE;
                        break;

                    case ITEM_TAG_LOCAL_STRING_INDEX:
                        DPRINT("[HIDPARSE] ITEM_TAG_LOCAL_STRING_INDEX 0x%x\n", Data.UData16.Value);
                        workingListHead->Value.StringMin = Data.UData16.Value;
                        workingListHead->Value.StringMax = Data.UData16.Value;
                        break;

                    case ITEM_TAG_LOCAL_STRING_MINIMUM:
                        DPRINT("[HIDPARSE] ITEM_TAG_LOCAL_STRING_MINIMUM 0x%x\n", Data.UData16.Value);
                        workingListHead->Value.StringMin = Data.UData16.Value;
                        break;

                    case ITEM_TAG_LOCAL_STRING_MAXIMUM:
                        DPRINT("[HIDPARSE] ITEM_TAG_LOCAL_STRING_MAXIMUM 0x%x\n", Data.UData16.Value);
                        workingListHead->Value.StringMax = Data.UData16.Value;
                        workingListHead->Value.Flags.IsStringRange = TRUE;
                        break;

                    default:
                        DPRINT("Unknown Local Item Tag 0x%x\n", CurrentItem->Tag);
                        status = STATUS_ILLEGAL_INSTRUCTION;
                        goto freeContext;
                        break;
                }

                break;
            }
            case ITEM_TYPE_RESERVED:
            {
                switch (CurrentItem->Tag)
                {
                    case ITEM_TAG_LONG:
                        // due hid specefication size of long item must be 2
                        if (CurrentItemSize != 2)
                        {
                            DPRINT(
                                "ITEM_TYPE_LONG Sanity check failed, size is 0x%x but 0x2 expected\n",
                                CurrentItem->Data.Long.LongItemTag);
                        }
                        CurrentItemSize += CurrentItem->Data.Long.LongDataSize;
                        DPRINT("Unsupported ITEM_TYPE_LONG Tag %x\n", CurrentItem->Data.Long.LongItemTag);
                        // status = STATUS_COULD_NOT_INTERPRET;
                        // goto freeContext;
                        break;
                    default:
                        DPRINT("Unsupported ITEM_TYPE_RESERVED Tag %x\n", CurrentItem->Data.Long.LongItemTag);
                        status = STATUS_ILLEGAL_INSTRUCTION;
                        goto freeContext;
                        break;
                }
                break;
            }
        }

        // move to next item
        CurrentOffset += CurrentItemSize + REPORT_ITEM_PREFIX_SIZE;

    }
    while (CurrentOffset < ReportEnd);

    // check if all collection's was closed
    if (nodeDepth > 0)
    {
        status = STATUS_BUFFER_TOO_SMALL;
    }

freeContext:
    // free working stacks and lists
    LIST_FREE(workingListHead, workingListTail);
    LIST_FREE(outputListHead, outputListTail);
    LIST_FREE(nodesListHead, nodesListTail);
    while (globalStack != NULL)
    {
        globalStackTemp = globalStack->Next;
        FreeFunction(globalStack);
        globalStack = globalStackTemp;
    }
    if (status != STATUS_SUCCESS)
    {
        goto exit;
    }

    // Cunstruct the device description
    for (collectionTemp = collectionStack; collectionTemp != NULL; collectionTemp = collectionTemp->Next)
    {
        DeviceDescription->CollectionDescLength++;
    }
    if (DeviceDescription->CollectionDescLength == 0)
    {
        // no top level collections found
        ASSERT(FALSE);
        status = STATUS_NO_DATA_DETECTED;
        goto exit;
    }

    // allocate collection
    DeviceDescription->CollectionDesc =
        (PHIDP_COLLECTION_DESC)AllocFunction(sizeof(HIDP_COLLECTION_DESC) * DeviceDescription->CollectionDescLength);
    if (DeviceDescription->CollectionDesc == NULL)
    {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto exit;
    }

    // copy collections and free
    for (collectionTemp = collectionStack; collectionTemp != NULL; collectionTemp = collectionTemp->Next)
    {
        ASSERT(collectionTemp->Index < DeviceDescription->CollectionDescLength);
        DeviceDescription->CollectionDesc[collectionTemp->Index].PreparsedData = collectionTemp->PreparsedData;
        DeviceDescription->CollectionDesc[collectionTemp->Index].PreparsedDataLength = collectionTemp->PreparsedSize;
        DeviceDescription->CollectionDesc[collectionTemp->Index].UsagePage = collectionTemp->PreparsedData->UsagePage;
        DeviceDescription->CollectionDesc[collectionTemp->Index].Usage = collectionTemp->PreparsedData->Usage;
        DeviceDescription->CollectionDesc[collectionTemp->Index].CollectionNumber = collectionTemp->Index;
        DeviceDescription->CollectionDesc[collectionTemp->Index].InputLength =
            collectionTemp->PreparsedData->InputReportByteLength;
        DeviceDescription->CollectionDesc[collectionTemp->Index].OutputLength =
            collectionTemp->PreparsedData->OutputReportByteLength;
        DeviceDescription->CollectionDesc[collectionTemp->Index].FeatureLength =
            collectionTemp->PreparsedData->FeatureReportByteLength;
    }

    for (reportIDsTemp = reportIDsStack; reportIDsTemp != NULL; reportIDsTemp = reportIDsTemp->Next)
    {
        DeviceDescription->ReportIDsLength++;
    }

    // allocate report description
    DeviceDescription->ReportIDs =
        (PHIDP_REPORT_IDS)AllocFunction(sizeof(HIDP_REPORT_IDS) * DeviceDescription->ReportIDsLength);
    if (DeviceDescription->ReportIDs == NULL)
    {
        // no memory
        FreeFunction(DeviceDescription->CollectionDesc);
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto exit;
    }

    // copy reports and free
    workingCount = 0;
    for (reportIDsTemp = reportIDsStack; reportIDsTemp != NULL; reportIDsTemp = reportIDsTemp->Next)
    {
        reportIDsTemp->Value.FeatureLength /= 8;
        reportIDsTemp->Value.InputLength /= 8;
        reportIDsTemp->Value.OutputLength /= 8;

        //
        // if desctiptor has only one report with reportID == 0
        // report will not contain reportID at zero's byte
        //
        if (reportIDsStack->Next != NULL || reportIDsTemp->Value.ReportID > 0)
        {
            if (reportIDsTemp->Value.FeatureLength > 0)
            {
                reportIDsTemp->Value.FeatureLength++;
            }
            if (reportIDsTemp->Value.InputLength > 0)
            {
                reportIDsTemp->Value.InputLength++;
            }
            if (reportIDsTemp->Value.OutputLength > 0)
            {
                reportIDsTemp->Value.OutputLength++;
            }
        }
        CopyFunction(&DeviceDescription->ReportIDs[workingCount], &reportIDsTemp->Value, sizeof(HIDP_REPORT_IDS));
        workingCount++;
    }

exit:
    while (collectionStack != NULL)
    {
        collectionTemp = collectionStack->Next;
        FreeFunction(collectionStack);
        collectionStack = collectionTemp;
    }

    while (reportIDsStack != NULL)
    {
        reportIDsTemp = reportIDsStack->Next;
        FreeFunction(reportIDsStack);
        reportIDsStack = reportIDsTemp;
    }

    return status;
}
