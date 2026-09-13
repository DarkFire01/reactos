/*
 * PROJECT:     uACPI-NT
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Reference-counted ACPI power resources (_PRx, _ON/_OFF)
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "acpipriv.h"
#include <uacpi/internal/namespace.h>   // uacpi_namespace_node_get_object

// PowerResource list, built once. The mutex serializes _ON/_OFF and refcounts.
static LIST_ENTRY g_PowerResList;
static KMUTEX     g_PowerResMutex;
static BOOLEAN    g_PowerResReady = FALSE;

// Trace a node's absolute path; the path string must be freed.
static VOID
UacpiPowerResTrace(const char *what, uacpi_namespace_node *node)
{
    const uacpi_char *p = uacpi_namespace_node_generate_absolute_path(node);
    UacpiTrace("[acpi] powerres: %s %s\n", what, p ? p : "?");
    if (p != NULL)
    {
        uacpi_free_absolute_path(p);
    }
}

// Enumerate every PowerResource in the namespace (once).
static uacpi_iteration_decision
UacpiPowerResCollect(void *user, uacpi_namespace_node *node, uacpi_u32 depth)
{
    PUACPI_POWER_RESOURCE r;
    uacpi_object *obj;
    uacpi_power_resource_info info;
    uacpi_u64 sta = 1;

    UNREFERENCED_PARAMETER(user);
    UNREFERENCED_PARAMETER(depth);

    obj = uacpi_namespace_node_get_object(node);
    if (obj == NULL)
    {
        return UACPI_ITERATION_DECISION_CONTINUE;
    }

    r = (PUACPI_POWER_RESOURCE)ExAllocatePoolWithTag(NonPagedPool,
                                                    sizeof(*r), UACPI_POOL_TAG);
    if (r == NULL)
    {
        return UACPI_ITERATION_DECISION_CONTINUE;   // best effort
    }
    RtlZeroMemory(r, sizeof(*r));
    r->Node   = node;
    r->Object = obj;

    if (!uacpi_unlikely_error(uacpi_object_get_power_resource_info(obj, &info)))
    {
        r->SystemLevel   = info.system_level;
        r->ResourceOrder = info.resource_order;
    }

    // Seed On from _STA; without _STA assume off (_ON is idempotent).
    r->On = FALSE;
    if (!uacpi_unlikely_error(uacpi_eval_simple_integer(node, "_STA", &sta)))
    {
        r->On = (sta != 0);
    }

    InsertTailList(&g_PowerResList, &r->Link);
    UacpiPowerResTrace(r->On ? "found (on)" : "found (off)", node);
    return UACPI_ITERATION_DECISION_CONTINUE;
}

VOID
UacpiPowerResInit(void)
{
    if (g_PowerResReady)
    {
        return;
    }
    InitializeListHead(&g_PowerResList);
    KeInitializeMutex(&g_PowerResMutex, 0);

    (void)uacpi_namespace_for_each_child(
        uacpi_namespace_root(), UacpiPowerResCollect, UACPI_NULL,
        UACPI_OBJECT_POWER_RESOURCE_BIT, UACPI_MAX_DEPTH_ANY, UACPI_NULL);

    g_PowerResReady = TRUE;
}

// Small helpers.
static PUACPI_POWER_RESOURCE
UacpiPowerResFindByObject(uacpi_object *obj)
{
    PLIST_ENTRY e;
    for (e = g_PowerResList.Flink; e != &g_PowerResList; e = e->Flink)
    {
        PUACPI_POWER_RESOURCE r = CONTAINING_RECORD(e, UACPI_POWER_RESOURCE, Link);
        if (r->Object == obj)
        {
            return r;
        }
    }
    return NULL;
}

static BOOLEAN
UacpiPowerResInSet(PUACPI_POWER_RESOURCE r, PUACPI_POWER_RESOURCE *set, ULONG count)
{
    ULONG i;
    for (i = 0; i < count; i++)
    {
        if (set[i] == r)
        {
            return TRUE;
        }
    }
    return FALSE;
}

// Resolve package elements [startIdx..] to known power resources, deduped and
// sorted ascending by resource order; returns the count placed in 'out'.
static ULONG
UacpiPowerResResolveArray(const uacpi_object_array *arr, ULONG startIdx,
                         const char *what, PUACPI_POWER_RESOURCE *out, ULONG maxOut)
{
    ULONG n = 0, i, j;

    for (i = startIdx; i < arr->count && n < maxOut; i++)
    {
        uacpi_object *target = NULL;
        PUACPI_POWER_RESOURCE r;

        // Dereference the element to the node's object; else match it as is.
        if (uacpi_unlikely_error(uacpi_object_get_dereferenced(arr->objects[i], &target))
            || target == NULL)
            {
            target = NULL;
            r = UacpiPowerResFindByObject(arr->objects[i]);
        }
        else
        {
            r = UacpiPowerResFindByObject(target);
            uacpi_object_unref(target);
        }

        if (r == NULL)
        {
            UacpiTrace("[acpi] powerres: %s element %u unresolved\n", what, i);
            continue;
        }
        if (!UacpiPowerResInSet(r, out, n))   // dedup
        {
            out[n++] = r;
        }
    }

    // Insertion-sort ascending by resource order.
    for (i = 1; i < n; i++)
    {
        PUACPI_POWER_RESOURCE key = out[i];
        j = i;
        while (j > 0 && out[j - 1]->ResourceOrder > key->ResourceOrder)
        {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = key;
    }
    return n;
}

// Resolve a _PRx package to resources sorted by resource order; returns count.
static ULONG
UacpiPowerResEvalList(uacpi_namespace_node *dev, const char *prName,
                     PUACPI_POWER_RESOURCE *out, ULONG maxOut)
{
    uacpi_object *pkg = NULL;
    uacpi_object_array arr;
    ULONG n;

    if (dev == NULL || !g_PowerResReady)
    {
        return 0;
    }
    if (uacpi_unlikely_error(uacpi_eval(dev, prName, UACPI_NULL, &pkg)) || pkg == NULL)
    {
        return 0;   // no _PRx: device has no rails for this state
    }
    if (uacpi_unlikely_error(uacpi_object_get_package(pkg, &arr)))
    {
        uacpi_object_unref(pkg);
        return 0;
    }

    n = UacpiPowerResResolveArray(&arr, 0, prName, out, maxOut);
    uacpi_object_unref(pkg);
    return n;
}

static const char *
UacpiPowerResPrName(DEVICE_POWER_STATE d)
{
    switch (d)
    {
    case PowerDeviceD0: return "_PR0";
    case PowerDeviceD1: return "_PR1";
    case PowerDeviceD2: return "_PR2";
    case PowerDeviceD3: return "_PR3";
    default:            return NULL;
    }
}

// Acquire the new D-state's rails this PDO does not hold yet. Before _PSx.
VOID
UacpiPowerResAcquireForState(PUACPI_PDO Pdo, DEVICE_POWER_STATE DState,
                            PUACPI_POWER_RESOURCE *newList, PULONG newCount)
{
    const char *prName = UacpiPowerResPrName(DState);
    ULONG n, i;

    *newCount = 0;
    if (prName == NULL || !g_PowerResReady)
    {
        return;
    }

    n = UacpiPowerResEvalList(Pdo->Node, prName, newList, UACPI_MAX_PR_PER_DEV);
    *newCount = n;
    if (n == 0)
    {
        return;
    }

    KeWaitForSingleObject(&g_PowerResMutex, Executive, KernelMode, FALSE, NULL);
    for (i = 0; i < n; i++)   // ascending resource order
    {
        PUACPI_POWER_RESOURCE r = newList[i];
        if (UacpiPowerResInSet(r, Pdo->HeldRes, Pdo->HeldCount))
        {
            continue;                                // this PDO already holds it
        }
        r->RefCount++;
        if (r->RefCount == 1 && !r->On)
        {
            if (!uacpi_unlikely_error(uacpi_eval(r->Node, "_ON", UACPI_NULL, UACPI_NULL)))
            {
                r->On = TRUE;
                UacpiPowerResTrace("_ON", r->Node);
            }
        }
    }
    KeReleaseMutex(&g_PowerResMutex, FALSE);
}

// Drop rails the new state does not need, then record the new set. After _PSx.
VOID
UacpiPowerResReleaseDelta(PUACPI_PDO Pdo, PUACPI_POWER_RESOURCE *newList, ULONG newCount)
{
    ULONG i;

    if (!g_PowerResReady)
    {
        return;
    }

    KeWaitForSingleObject(&g_PowerResMutex, Executive, KernelMode, FALSE, NULL);

    // Walk the previously-held set in reverse resource order for _OFF.
    for (i = Pdo->HeldCount; i > 0; i--)
    {
        PUACPI_POWER_RESOURCE r = Pdo->HeldRes[i - 1];
        if (UacpiPowerResInSet(r, newList, newCount))
        {
            continue;                                // still needed
        }
        if (r->RefCount > 0)
        {
            r->RefCount--;
        }
        if (r->RefCount == 0 && r->On)
        {
            if (!uacpi_unlikely_error(uacpi_eval(r->Node, "_OFF", UACPI_NULL, UACPI_NULL)))
            {
                r->On = FALSE;
                UacpiPowerResTrace("_OFF", r->Node);
            }
        }
    }

    // Commit the new holdings.
    for (i = 0; i < newCount && i < UACPI_MAX_PR_PER_DEV; i++)
    {
        Pdo->HeldRes[i] = newList[i];
    }
    Pdo->HeldCount = (newCount < UACPI_MAX_PR_PER_DEV) ? newCount : UACPI_MAX_PR_PER_DEV;

    KeReleaseMutex(&g_PowerResMutex, FALSE);
}

// Turn on a device's _PRW[2..] wake power rails and hold them while it is armed
// for wake. These are separate from the D-state rails (HeldRes[]); a device can
// be in D3 with its wake circuit still powered. Returns the held set in 'out'.
ULONG
UacpiPowerResAcquireWake(uacpi_namespace_node *dev, PUACPI_POWER_RESOURCE *out,
                        ULONG maxOut)
{
    uacpi_object *pkg = NULL;
    uacpi_object_array arr;
    ULONG n, i;

    if (dev == NULL || !g_PowerResReady)
    {
        return 0;
    }
    if (uacpi_unlikely_error(uacpi_eval(dev, "_PRW", UACPI_NULL, &pkg)) || pkg == NULL)
    {
        return 0;
    }
    if (uacpi_unlikely_error(uacpi_object_get_package(pkg, &arr)) || arr.count < 2)
    {
        uacpi_object_unref(pkg);
        return 0;   // no wake rails beyond [0]=GPE and [1]=deepest state
    }

    n = UacpiPowerResResolveArray(&arr, 2, "_PRW", out, maxOut);
    uacpi_object_unref(pkg);
    if (n == 0)
    {
        return 0;
    }

    KeWaitForSingleObject(&g_PowerResMutex, Executive, KernelMode, FALSE, NULL);
    for (i = 0; i < n; i++)   // ascending resource order
    {
        PUACPI_POWER_RESOURCE r = out[i];
        r->RefCount++;
        if (r->RefCount == 1 && !r->On)
        {
            if (!uacpi_unlikely_error(uacpi_eval(r->Node, "_ON", UACPI_NULL, UACPI_NULL)))
            {
                r->On = TRUE;
                UacpiPowerResTrace("wake _ON", r->Node);
            }
        }
    }
    KeReleaseMutex(&g_PowerResMutex, FALSE);
    return n;
}

// Release the wake rails acquired above, at final disarm.
VOID
UacpiPowerResReleaseWake(PUACPI_POWER_RESOURCE *list, ULONG count)
{
    ULONG i;

    if (count == 0 || !g_PowerResReady)
    {
        return;
    }

    KeWaitForSingleObject(&g_PowerResMutex, Executive, KernelMode, FALSE, NULL);
    for (i = count; i > 0; i--)   // reverse resource order for _OFF
    {
        PUACPI_POWER_RESOURCE r = list[i - 1];
        if (r->RefCount > 0)
        {
            r->RefCount--;
        }
        if (r->RefCount == 0 && r->On)
        {
            if (!uacpi_unlikely_error(uacpi_eval(r->Node, "_OFF", UACPI_NULL, UACPI_NULL)))
            {
                r->On = FALSE;
                UacpiPowerResTrace("wake _OFF", r->Node);
            }
        }
    }
    KeReleaseMutex(&g_PowerResMutex, FALSE);
}

// Re-run _ON on every resource marked on; S4 loses rails without D3 IRPs.
VOID
UacpiPowerResResume(void)
{
    PLIST_ENTRY e;

    if (!g_PowerResReady)
    {
        return;
    }

    KeWaitForSingleObject(&g_PowerResMutex, Executive, KernelMode, FALSE, NULL);
    for (e = g_PowerResList.Flink; e != &g_PowerResList; e = e->Flink)
    {
        PUACPI_POWER_RESOURCE r = CONTAINING_RECORD(e, UACPI_POWER_RESOURCE, Link);
        if (r->On)
        {
            if (!uacpi_unlikely_error(uacpi_eval(r->Node, "_ON", UACPI_NULL, UACPI_NULL)))
            {
                UacpiPowerResTrace("resume re-_ON", r->Node);
            }
        }
    }
    KeReleaseMutex(&g_PowerResMutex, FALSE);
}
