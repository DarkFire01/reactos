/*
 * PROJECT:     ReactOS
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Table driven hierarchical state machine engine
 *
 * The tables are produced by a generator; see the *StateMachine.h headers
 * in drivers/network/netcx/adapter/statemachines. Every index below is into
 * one of those tables, with zero reserved for "none". The engine is in
 * sdk/lib/drivers/smfx.
 */

#pragma once

#ifndef __cplusplus
#error SmFx is C++ only
#endif

#include <ntddk.h>
#include <stdint.h>

/* The generated tables check this against the layout they were built for. */
#define SMFX_VER 2

namespace SmFx
{

typedef uint16_t EventId;
typedef uint16_t EventIndex;
typedef uint16_t StateId;
typedef uint16_t StateIndex;
typedef uint8_t SubmachineIndex;

/* A slot is an entry function, a transition array or an event list. */
typedef const void *StateSlot;

enum class StateFlags : uint8_t
{
    None = 0x00,
    RequiresPassiveLevel = 0x01,
    RequiresDedicatedThread = 0x02,
};

DEFINE_ENUM_FLAG_OPERATORS(StateFlags);

/* Sync states pick their next event themselves, Async ones wait, Call ones enter a submachine. */
enum class StateType : uint8_t
{
    Invalid = 0,
    Sync = 1,
    Async = 2,
    Call = 3,
};

/* A state's slot array holds one entry per set bit, in the order of the bits. */
enum class StateSlotType : uint16_t
{
    None = 0x0000,
    EntryFunction = 0x0001,
    ExternalTransitions = 0x0002,
    InternalTransitions = 0x0004,
    DeferredEvents = 0x0008,
    PopTransitions = 0x0010,
    PurgeEvents = 0x0020,
    StopTimerOnExit = 0x0040,
};

DEFINE_ENUM_FLAG_OPERATORS(StateSlotType);

enum class InternalTransitionFlags : uint8_t
{
    None = 0x00,
    RequiresPassiveLevel = 0x01,
    RequiresDedicatedThread = 0x02,
};

DEFINE_ENUM_FLAG_OPERATORS(InternalTransitionFlags);

/* What enqueueing does when the event is already waiting in the queue. */
enum class EventQueueingDisposition : uint8_t
{
    Invalid = 0,
    Unlimited = 1,
    RequeueAtEnd = 2,
    DropDuplicate = 3,
};

enum class StopTimerResult
{
    Invalid = 0,
    TimerStopped = 1,
    WaitForTimerFiredEventAndIgnoreIt = 2,
};

enum class TransitionType
{
    Invalid = 0,
    External = 1,
    Internal = 2,
    Pop = 3,
    Unwind = 4,
    Call = 5,
    Ignored = 6,
    Purged = 7,
};

enum class MachineException
{
    Invalid = 0,
    UnhandledEvent = 1,
    EventQueueFull = 2,
    StackFull = 3,
};

typedef EventIndex StateEntryFunction(void *context);
typedef void InternalTransitionAction(void *context);
typedef StopTimerResult StopTimerFunction(void *context);

typedef void EvtLogMachineExceptionFunc(void *context, MachineException exception, EventId relevantEvent, StateId relevantState);
typedef void EvtLogEventEnqueueFunc(void *context, EventId relevantEvent);
typedef void EvtLogTransitionFunc(void *context, TransitionType transitionType, StateId sourceState, EventId processedEvent, StateId targetState);
typedef void EvtMachineDestroyedFunc(void *context);

struct EVENT_SPECIFICATION
{
    EventId id;
    EventQueueingDisposition queueingDisposition;
};

struct SUBMACHINE_SPECIFICATION
{
    StateIndex initialStateIndex;
};

struct STATE_SPECIFICATION
{
    StateId id;
    StateFlags flags;
    StateType type;
    StateSlotType activeSlots;
    SubmachineIndex callSubmachine;
    const StateSlot *slots;
};

struct STATE_MACHINE_SPECIFICATION
{
    SubmachineIndex mainMachine;
    EventIndex defaultSyncEvent;
    const SUBMACHINE_SPECIFICATION *machineTable;
    const EVENT_SPECIFICATION *eventTable;
    const STATE_SPECIFICATION *stateTable;
};

/* Transition and event arrays end with an entry whose event index is zero. */
struct EXTERNAL_TRANSITION
{
    EventIndex triggeringEventIndex;
    StateIndex targetStateIndex;
};

struct INTERNAL_TRANSITION
{
    EventIndex triggeringEventIndex;
    InternalTransitionFlags flags;
    InternalTransitionAction *action;
};

/* Popping out of a submachine hands the return event to the state below. */
struct POP_TRANSITION
{
    EventIndex triggeringEventIndex;
    EventIndex returnEventIndex;
};

struct STOP_TIMER_ON_EXIT_DETAILS
{
    EventIndex timerFiredEvent;
    StopTimerFunction *stopTimerFunction;
};

struct STATE_MACHINE_ENGINE_CONFIG
{
    const STATE_MACHINE_SPECIFICATION *machineSpec;
    void *context;
    PDEVICE_OBJECT deviceObject;
    uint32_t poolTag;
    EvtLogMachineExceptionFunc *logExceptionCallback;
    EvtLogEventEnqueueFunc *logEventEnqueueCallback;
    EvtLogTransitionFunc *logTransitionCallback;
    EvtMachineDestroyedFunc *machineDestroyedCallback;
    bool isWorkerRequired;
    uint8_t stackSize;
};

/*
 * The engine lives in its own allocation and frees itself once the main
 * machine pops its last state, clearing m_impl on the way out.
 */
class StateMachineEngine
{
public:

    /* Also runs the initial transition, so machine operations can start before this returns. */
    _Must_inspect_result_
    _IRQL_requires_max_(DISPATCH_LEVEL)
    NTSTATUS
    Initialize(
        _In_ const STATE_MACHINE_ENGINE_CONFIG &config);

    bool
    IsInitialized(
        void)
    {
        return m_impl != nullptr;
    }

    _IRQL_requires_max_(DISPATCH_LEVEL)
    void
    EnqueueEvent(
        _In_ EventIndex event);

    class StateMachineEngineImpl;

private:

    StateMachineEngineImpl *m_impl = nullptr;
};

}
