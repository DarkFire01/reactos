/*
 * PROJECT:     ReactOS
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Table driven hierarchical state machine engine
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * Events go into a small ring and are handed one at a time to the state on
 * top of a stack of active states. A state that needs passive level or a
 * dedicated thread which the current caller cannot give it is parked, and a
 * work item picks the machine back up from there.
 */

#include <smfx.h>

namespace SmFx
{

static const ULONG DefaultPoolTag = 'xFmS';
static const uint8_t DefaultStackSize = 16;
static const uint8_t EventQueueSize = 16;
static const uint8_t TransitionHistorySize = 64;
static const uint8_t ExceptionHistorySize = 8;
static const uint8_t NoStackDepth = 0xFF;

class StateMachineEngine::StateMachineEngineImpl
{
public:

    /* Also the result of running an action or a state, telling the caller what comes next. */
    enum class EngineState
    {
        Invalid = 0,
        Idle = 1,
        Running = 2,
        NeedsWorkerForTransition = 3,
        NeedsWorkerForAction = 4,
        WaitingForTimerFired = 5,
    };

    enum class EventAction
    {
        None = 0,
        Unhandled = 1,
        External = 2,
        Internal = 3,
        Pop = 4,
        Defer = 5,
        Call = 6,
    };

    struct EVENT_ACTION
    {
        EventAction action;
        EventIndex eventIndex;

        /* Depth of the state that handles the event, NoStackDepth when none does. */
        uint8_t stackDepth;

        union
        {
            const EXTERNAL_TRANSITION *external;
            const INTERNAL_TRANSITION *internal;
            const POP_TRANSITION *pop;
            const SUBMACHINE_SPECIFICATION *submachine;
        };
    };

    /* Always constructed in place, in memory from ExAllocatePool2. */
    static
    void *
    operator new(
        _In_ size_t Size,
        _In_ void *Memory) noexcept
    {
        UNREFERENCED_PARAMETER(Size);
        return Memory;
    }

    static
    void
    operator delete(
        _In_ void *Object,
        _In_ void *Memory) noexcept
    {
        UNREFERENCED_PARAMETER(Object);
        UNREFERENCED_PARAMETER(Memory);
    }

    StateMachineEngineImpl(
        _In_ StateMachineEngineImpl **OwnerPointer) :
        m_ownerPointer(OwnerPointer)
    {
    }

    ~StateMachineEngineImpl(
        void);

    NTSTATUS
    Initialize(
        _In_ const STATE_MACHINE_ENGINE_CONFIG &Config);

    void
    EnqueueEvent(
        _In_ EventIndex Event);

private:

    struct TRANSITION_HISTORY_ENTRY
    {
        TransitionType transitionType;
        EventIndex processedEvent;
        StateIndex stateIndex;
        uint8_t stackDepth;
    };

    struct EXCEPTION_HISTORY_ENTRY
    {
        MachineException exception;
        EventIndex relevantEvent;
        StateIndex relevantState;
    };

    void
    Lock(
        void)
    {
        KeAcquireSpinLock(&m_lock, &m_lockIrql);
    }

    void
    Unlock(
        void)
    {
        KeReleaseSpinLock(&m_lock, m_lockIrql);
    }

    StateIndex
    CurrentState(
        void) const
    {
        return m_numStatesOnStack != 0 ? m_stack[m_numStatesOnStack - 1] : 0;
    }

    const STATE_SPECIFICATION &
    StateSpec(
        _In_ StateIndex Index) const
    {
        return m_config.machineSpec->stateTable[Index];
    }

    StateId
    StateIdOf(
        _In_ StateIndex Index) const
    {
        return Index != 0 ? StateSpec(Index).id : 0;
    }

    EventId
    EventIdOf(
        _In_ EventIndex Index) const
    {
        return Index != 0 ? m_config.machineSpec->eventTable[Index].id : 0;
    }

    /* The slot array only has entries for the slot types that are present. */
    static
    StateSlot
    SlotOf(
        _In_ const STATE_SPECIFICATION &State,
        _In_ StateSlotType Type)
    {
        uint16_t const active = static_cast<uint16_t>(State.activeSlots);
        uint16_t const bit = static_cast<uint16_t>(Type);
        uint16_t below = active & (bit - 1);
        ULONG index = 0;

        if ((active & bit) == 0)
            return nullptr;

        while (below != 0)
        {
            below &= below - 1;
            index++;
        }

        return State.slots[index];
    }

    bool
    CanRunHere(
        _In_ uint8_t Flags) const
    {
        if ((Flags & static_cast<uint8_t>(StateFlags::RequiresDedicatedThread)) && !m_isRunningOnDedicatedThread)
            return false;

        if ((Flags & static_cast<uint8_t>(StateFlags::RequiresPassiveLevel)) && m_runningIrql != PASSIVE_LEVEL)
            return false;

        return true;
    }

    void
    NoteRunningContext(
        _In_ KIRQL Irql,
        _In_ bool DedicatedThread);

    bool
    QueueEvent(
        _In_ EventIndex Event);

    bool
    AppendEvent(
        _In_ EventIndex Event);

    bool
    RemoveQueuedEvent(
        _In_ EventIndex Event);

    void
    InitiateFirstRun(
        void);

    void
    ProcessEventQueue(
        void);

    EngineState
    ExecuteCurrentState(
        void);

    void
    PurgeEvents(
        _In_ StateIndex State,
        _In_ const EventIndex *List);

    void
    FindActionForEvent(
        _In_ EventIndex Event,
        _Out_ EVENT_ACTION *Action);

    EngineState
    ExecuteEventAction(
        _In_ const EVENT_ACTION &Action);

    EngineState
    ExecutePop(
        _In_ const EVENT_ACTION &Action);

    EngineState
    PopToStackDepth(
        _In_ const EVENT_ACTION &Action);

    EngineState
    ExitCurrentState(
        _In_ const EVENT_ACTION &Cause);

    void
    UpdateEngineState(
        _In_ EngineState NewState);

    void
    LogTransition(
        _In_ TransitionType Type,
        _In_ StateIndex Source,
        _In_ EventIndex Event,
        _In_ StateIndex Target,
        _In_ uint8_t StackDepth);

    void
    ReportException(
        _In_ MachineException Exception,
        _In_ EventIndex Event,
        _In_ StateIndex State);

    void
    ReportExceptionWithLockHeld(
        _In_ MachineException Exception,
        _In_ EventIndex Event,
        _In_ StateIndex State);

    void
    Destroy(
        _In_ bool InvokeDestroyCallback);

    static
    void
    WorkerRoutine(
        _In_ void *Context);

    static IO_WORKITEM_ROUTINE WorkItemRoutine;

    TRANSITION_HISTORY_ENTRY m_transitionHistory[TransitionHistorySize] = {};
    EXCEPTION_HISTORY_ENTRY m_exceptionHistory[ExceptionHistorySize] = {};
    EventIndex m_eventQueue[EventQueueSize] = {};
    uint8_t m_stackSize = 0;
    StateIndex *m_stack = nullptr;

    /* The event a stopping timer will still deliver, which is dropped when it comes. */
    EventIndex m_timerFiredEvent = 0;

    STATE_MACHINE_ENGINE_CONFIG m_config = {};
    EVENT_ACTION m_resumeAction = {};
    uint8_t m_transitionHistoryIndex = 0;
    uint8_t m_exceptionHistoryIndex = 0;
    uint8_t m_eventQueueHead = 0;
    uint8_t m_eventQueueTail = 0;
    uint8_t m_numStatesOnStack = 0;
    bool m_currentStateAlreadyExited = true;
    bool m_isRunningOnDedicatedThread = true;
    KIRQL m_runningIrql = DISPATCH_LEVEL;

    /* Who is driving the machine right now, for the debugger. */
    union
    {
        PKTHREAD m_thread;
        ULONG m_processorNumber;
    };

    EngineState m_engineState = EngineState::Invalid;
    KSPIN_LOCK m_lock = 0;
    KIRQL m_lockIrql = PASSIVE_LEVEL;
    bool m_lockInitialized = false;
    PIO_WORKITEM m_workItem = nullptr;
    StateMachineEngineImpl **m_ownerPointer;
};

using Impl = StateMachineEngine::StateMachineEngineImpl;

Impl::~StateMachineEngineImpl(
    void)
{
    *m_ownerPointer = nullptr;

    Lock();
    m_engineState = EngineState::Invalid;
    Unlock();

    if (m_workItem != nullptr)
    {
        IoFreeWorkItem(m_workItem);
        m_workItem = nullptr;
    }

    if (m_stack != nullptr)
    {
        ExFreePool(m_stack);
        m_stack = nullptr;
    }

    m_lockInitialized = false;
}

NTSTATUS
Impl::Initialize(
    _In_ const STATE_MACHINE_ENGINE_CONFIG &Config)
{
    m_config = Config;

    if (!m_lockInitialized)
    {
        KeInitializeSpinLock(&m_lock);
        m_lockInitialized = true;
    }

    m_stackSize = m_config.stackSize != 0 ? m_config.stackSize : DefaultStackSize;

    ULONG const tag = m_config.poolTag != 0 ? m_config.poolTag : DefaultPoolTag;
    m_stack = static_cast<StateIndex *>(ExAllocatePool2(POOL_FLAG_NON_PAGED,
                                                        m_stackSize * sizeof(*m_stack),
                                                        tag));
    if (m_stack == nullptr)
        return STATUS_INSUFFICIENT_RESOURCES;

    if (m_config.isWorkerRequired)
    {
        if (m_workItem != nullptr)
            return STATUS_INVALID_DEVICE_STATE;

        m_workItem = IoAllocateWorkItem(Config.deviceObject);
        if (m_workItem == nullptr)
            return STATUS_INSUFFICIENT_RESOURCES;
    }

    InitiateFirstRun();
    return STATUS_SUCCESS;
}

void
Impl::NoteRunningContext(
    _In_ KIRQL Irql,
    _In_ bool DedicatedThread)
{
    m_runningIrql = Irql;
    m_isRunningOnDedicatedThread = DedicatedThread;

    if (Irql >= DISPATCH_LEVEL)
        m_processorNumber = KeGetCurrentProcessorNumber();
    else
        m_thread = KeGetCurrentThread();
}

/* Entering the main machine is a call like any other, just from an empty stack. */
void
Impl::InitiateFirstRun(
    void)
{
    Lock();
    m_engineState = EngineState::Running;
    KIRQL const irql = m_lockIrql;
    Unlock();

    NoteRunningContext(irql, false);

    EVENT_ACTION entry = {};
    entry.action = EventAction::Call;
    entry.submachine = &m_config.machineSpec->machineTable[m_config.machineSpec->mainMachine];

    EngineState result = ExecuteEventAction(entry);
    if (result == EngineState::Running)
        result = ExecuteCurrentState();

    Lock();

    if (result == EngineState::Idle)
        ProcessEventQueue();
    else
        UpdateEngineState(result);
}

void
Impl::EnqueueEvent(
    _In_ EventIndex Event)
{
    Lock();
    KIRQL const irql = m_lockIrql;
    bool startProcessing = false;

    if (m_engineState == EngineState::WaitingForTimerFired && Event == m_timerFiredEvent)
    {
        /* The event being waited for only unblocks the machine; it is never queued. */
        if (m_config.logEventEnqueueCallback != nullptr)
            m_config.logEventEnqueueCallback(m_config.context, EventIdOf(Event));

        m_timerFiredEvent = 0;
    }
    else
    {
        startProcessing = m_engineState == EngineState::Idle;

        if (!QueueEvent(Event))
            return;

        if (!startProcessing)
        {
            Unlock();
            return;
        }
    }

    NoteRunningContext(irql, false);
    ProcessEventQueue();
}

/* Returns false, with the lock dropped, when the event did not go into the queue. */
bool
Impl::QueueEvent(
    _In_ EventIndex Event)
{
    switch (m_config.machineSpec->eventTable[Event].queueingDisposition)
    {
        case EventQueueingDisposition::Unlimited:
            return AppendEvent(Event);

        case EventQueueingDisposition::RequeueAtEnd:
        {
            uint8_t write = m_eventQueueHead;
            bool found = false;

            for (uint8_t read = m_eventQueueHead; read != m_eventQueueTail; read = (read + 1) % EventQueueSize)
            {
                if (m_eventQueue[read] == Event)
                {
                    found = true;
                    continue;
                }

                m_eventQueue[write] = m_eventQueue[read];
                write = (write + 1) % EventQueueSize;
            }

            if (!found)
                return AppendEvent(Event);

            /* Same count as before, with the event moved to the back. */
            m_eventQueue[(m_eventQueueTail + EventQueueSize - 1) % EventQueueSize] = Event;
            break;
        }

        case EventQueueingDisposition::DropDuplicate:
        {
            bool found = false;

            for (uint8_t i = m_eventQueueHead; i != m_eventQueueTail; i = (i + 1) % EventQueueSize)
            {
                if (m_eventQueue[i] == Event)
                {
                    found = true;
                    break;
                }
            }

            if (!found)
                return AppendEvent(Event);

            break;
        }

        default:
            break;
    }

    Unlock();
    return false;
}

bool
Impl::AppendEvent(
    _In_ EventIndex Event)
{
    uint8_t const next = (m_eventQueueTail + 1) % EventQueueSize;

    if (next == m_eventQueueHead)
    {
        ReportExceptionWithLockHeld(MachineException::EventQueueFull, Event, 0);
        return false;
    }

    m_eventQueue[m_eventQueueTail] = Event;
    m_eventQueueTail = next;

    if (m_config.logEventEnqueueCallback != nullptr)
        m_config.logEventEnqueueCallback(m_config.context, EventIdOf(Event));

    return true;
}

/* Takes out the first instance of the event, keeping the rest in order. */
bool
Impl::RemoveQueuedEvent(
    _In_ EventIndex Event)
{
    uint8_t write = m_eventQueueHead;
    bool found = false;

    for (uint8_t read = m_eventQueueHead; read != m_eventQueueTail; read = (read + 1) % EventQueueSize)
    {
        m_eventQueue[write] = m_eventQueue[read];

        if (!found && m_eventQueue[read] == Event)
            found = true;
        else
            write = (write + 1) % EventQueueSize;
    }

    m_eventQueueTail = write;
    return found;
}

/* Entered and left with the lock held; the lock is dropped around each action. */
void
Impl::ProcessEventQueue(
    void)
{
    bool resume = m_engineState == EngineState::WaitingForTimerFired;
    EngineState result = EngineState::Idle;

    m_engineState = EngineState::Running;

    for (;;)
    {
        EVENT_ACTION action = {};

        if (resume)
        {
            resume = false;
            action = m_resumeAction;
        }
        else
        {
            /* Deferred events stay where they are and the first one that is not goes next. */
            uint8_t position = m_eventQueueHead;
            EventIndex event = 0;
            bool found = false;

            while (position != m_eventQueueTail)
            {
                event = m_eventQueue[position];
                action = {};
                FindActionForEvent(event, &action);

                if (action.action != EventAction::Defer)
                {
                    found = true;
                    break;
                }

                position = (position + 1) % EventQueueSize;
            }

            if (!found)
            {
                result = EngineState::Idle;
                break;
            }

            while (position != m_eventQueueHead)
            {
                uint8_t const previous = (position + EventQueueSize - 1) % EventQueueSize;
                m_eventQueue[position] = m_eventQueue[previous];
                position = previous;
            }

            m_eventQueueHead = (m_eventQueueHead + 1) % EventQueueSize;

            if (event == 0)
            {
                result = EngineState::Idle;
                break;
            }
        }

        Unlock();

        result = ExecuteEventAction(action);
        if (result == EngineState::Running)
            result = ExecuteCurrentState();

        if (result == EngineState::Idle)
            result = EngineState::Running;

        Lock();

        if (result == EngineState::WaitingForTimerFired)
        {
            /* If the timer's event already arrived there is nothing left to wait for. */
            if (!RemoveQueuedEvent(m_timerFiredEvent))
                break;

            m_timerFiredEvent = 0;
            resume = true;
        }
        else if (result != EngineState::Running)
        {
            break;
        }
    }

    UpdateEngineState(result);
}

Impl::EngineState
Impl::ExecuteCurrentState(
    void)
{
    for (;;)
    {
        StateIndex const stateIndex = CurrentState();
        const STATE_SPECIFICATION &state = StateSpec(stateIndex);
        EventIndex nextEvent = 0;

        StateSlot const entry = SlotOf(state, StateSlotType::EntryFunction);
        if (entry != nullptr)
            nextEvent = reinterpret_cast<StateEntryFunction *>(const_cast<void *>(entry))(m_config.context);
        else if (state.type == StateType::Sync)
            nextEvent = m_config.machineSpec->defaultSyncEvent;

        StateSlot const purge = SlotOf(StateSpec(CurrentState()), StateSlotType::PurgeEvents);
        if (purge != nullptr)
            PurgeEvents(CurrentState(), static_cast<const EventIndex *>(purge));

        EVENT_ACTION action = {};

        if (state.type == StateType::Sync)
        {
            FindActionForEvent(nextEvent, &action);
        }
        else if (state.type == StateType::Call)
        {
            action.action = EventAction::Call;
            action.submachine = &m_config.machineSpec->machineTable[state.callSubmachine];
        }
        else
        {
            return EngineState::Idle;
        }

        EngineState const result = ExecuteEventAction(action);
        if (result != EngineState::Running)
            return result;
    }
}

void
Impl::PurgeEvents(
    _In_ StateIndex State,
    _In_ const EventIndex *List)
{
    Lock();

    uint8_t write = m_eventQueueHead;

    for (uint8_t read = m_eventQueueHead; read != m_eventQueueTail; read = (read + 1) % EventQueueSize)
    {
        EventIndex const event = m_eventQueue[read];
        bool purged = false;

        for (const EventIndex *entry = List; *entry != 0; entry++)
        {
            if (*entry == event)
            {
                purged = true;
                break;
            }
        }

        if (purged)
        {
            LogTransition(TransitionType::Purged, State, event, 0, m_numStatesOnStack - 1);
        }
        else
        {
            m_eventQueue[write] = event;
            write = (write + 1) % EventQueueSize;
        }
    }

    m_eventQueueTail = write;
    Unlock();
}

/*
 * Looks for a handler from the top of the stack down. A Sync state has the
 * last word on its own events, so the search never goes below one.
 */
void
Impl::FindActionForEvent(
    _In_ EventIndex Event,
    _Out_ EVENT_ACTION *Action)
{
    Action->eventIndex = Event;
    Action->action = EventAction::Unhandled;
    Action->stackDepth = NoStackDepth;

    for (int depth = m_numStatesOnStack - 1; depth >= 0; depth--)
    {
        const STATE_SPECIFICATION &state = StateSpec(m_stack[depth]);

        Action->stackDepth = NoStackDepth;

        auto const external = static_cast<const EXTERNAL_TRANSITION *>(SlotOf(state, StateSlotType::ExternalTransitions));
        for (auto t = external; t != nullptr && t->triggeringEventIndex != 0; t++)
        {
            if (t->triggeringEventIndex == Event)
            {
                Action->action = EventAction::External;
                Action->external = t;
                Action->stackDepth = static_cast<uint8_t>(depth);
                return;
            }
        }

        auto const internal = static_cast<const INTERNAL_TRANSITION *>(SlotOf(state, StateSlotType::InternalTransitions));
        for (auto t = internal; t != nullptr && t->triggeringEventIndex != 0; t++)
        {
            if (t->triggeringEventIndex == Event)
            {
                Action->action = EventAction::Internal;
                Action->internal = t;
                Action->stackDepth = static_cast<uint8_t>(depth);
                return;
            }
        }

        auto const deferred = static_cast<const EventIndex *>(SlotOf(state, StateSlotType::DeferredEvents));
        for (auto e = deferred; e != nullptr && *e != 0; e++)
        {
            if (*e == Event)
            {
                Action->action = EventAction::Defer;
                Action->stackDepth = static_cast<uint8_t>(depth);
                return;
            }
        }

        auto const pop = static_cast<const POP_TRANSITION *>(SlotOf(state, StateSlotType::PopTransitions));
        for (auto t = pop; t != nullptr && t->triggeringEventIndex != 0; t++)
        {
            if (t->triggeringEventIndex == Event)
            {
                Action->action = EventAction::Pop;
                Action->pop = t;
                Action->stackDepth = static_cast<uint8_t>(depth);
                return;
            }
        }

        Action->action = EventAction::Unhandled;

        if (state.type == StateType::Sync)
            return;
    }
}

Impl::EngineState
Impl::ExecuteEventAction(
    _In_ const EVENT_ACTION &Action)
{
    switch (Action.action)
    {
        case EventAction::Unhandled:
            ReportException(MachineException::UnhandledEvent, Action.eventIndex, CurrentState());
            return EngineState::Idle;

        case EventAction::External:
        {
            EngineState result = PopToStackDepth(Action);
            if (result != EngineState::Idle)
                return result;

            result = ExitCurrentState(Action);
            if (result != EngineState::Idle)
                return result;

            StateIndex const target = Action.external->targetStateIndex;

            if (!CanRunHere(static_cast<uint8_t>(StateSpec(target).flags)))
            {
                m_resumeAction = Action;
                return EngineState::NeedsWorkerForTransition;
            }

            LogTransition(TransitionType::External, CurrentState(), Action.eventIndex, target, Action.stackDepth);
            m_stack[m_numStatesOnStack - 1] = target;
            m_currentStateAlreadyExited = false;
            return EngineState::Running;
        }

        case EventAction::Internal:
        {
            const INTERNAL_TRANSITION *transition = Action.internal;
            StateIndex const handler = m_stack[Action.stackDepth];

            if (transition->action == nullptr)
            {
                LogTransition(TransitionType::Ignored, CurrentState(), Action.eventIndex, handler, Action.stackDepth);
                return EngineState::Idle;
            }

            if (!CanRunHere(static_cast<uint8_t>(transition->flags)))
            {
                m_resumeAction = Action;
                return EngineState::NeedsWorkerForAction;
            }

            LogTransition(TransitionType::Internal, CurrentState(), Action.eventIndex, handler, Action.stackDepth);
            transition->action(m_config.context);
            return EngineState::Idle;
        }

        case EventAction::Pop:
            return ExecutePop(Action);

        case EventAction::Call:
        {
            StateIndex const current = CurrentState();

            if (m_numStatesOnStack == m_stackSize)
            {
                ReportException(MachineException::StackFull, 0, current);
                return EngineState::Idle;
            }

            StateIndex const target = Action.submachine->initialStateIndex;

            if (!CanRunHere(static_cast<uint8_t>(StateSpec(target).flags)))
            {
                m_resumeAction = Action;
                return EngineState::NeedsWorkerForTransition;
            }

            LogTransition(TransitionType::Call, current, 0, target, m_numStatesOnStack);
            m_stack[m_numStatesOnStack] = target;
            m_numStatesOnStack++;
            m_currentStateAlreadyExited = false;
            return EngineState::Running;
        }

        default:
            return EngineState::Idle;
    }
}

/* The return event of a pop is handled by the state below, and may pop it in turn. */
Impl::EngineState
Impl::ExecutePop(
    _In_ const EVENT_ACTION &Action)
{
    EVENT_ACTION current = Action;

    for (;;)
    {
        EngineState result = PopToStackDepth(current);
        if (result != EngineState::Idle)
            return result;

        result = ExitCurrentState(current);
        if (result != EngineState::Idle)
            return result;

        uint8_t const count = m_numStatesOnStack;
        StateIndex const source = count != 0 ? m_stack[count - 1] : 0;
        StateIndex const target = count > 1 ? m_stack[count - 2] : 0;

        LogTransition(TransitionType::Pop, source, current.eventIndex, target, current.stackDepth);

        m_stack[count - 1] = 0;
        m_numStatesOnStack = count - 1;
        m_currentStateAlreadyExited = false;

        FindActionForEvent(current.pop->returnEventIndex, &current);

        if (current.action == EventAction::Pop)
            continue;

        if (m_numStatesOnStack == 0)
            return EngineState::Idle;

        /* A state being returned to cannot put its return event off. */
        if (current.action == EventAction::Defer)
            current.action = EventAction::Unhandled;

        return ExecuteEventAction(current);
    }
}

/* Leaves every state above the one that handles the event. */
Impl::EngineState
Impl::PopToStackDepth(
    _In_ const EVENT_ACTION &Action)
{
    ULONG const keep = static_cast<ULONG>(Action.stackDepth) + 1;

    while (m_numStatesOnStack > keep)
    {
        EngineState const result = ExitCurrentState(Action);
        if (result != EngineState::Idle)
            return result;

        uint8_t const count = m_numStatesOnStack;
        StateIndex const target = count > 1 ? m_stack[count - 2] : 0;

        LogTransition(TransitionType::Unwind, m_stack[count - 1], Action.eventIndex, target, count - 1);

        m_stack[count - 1] = 0;
        m_numStatesOnStack = count - 1;
        m_currentStateAlreadyExited = false;
    }

    return EngineState::Idle;
}

/*
 * A state may own a timer that has to be stopped before it is left. If the
 * timer can no longer be stopped, the machine waits for its fired event and
 * swallows it, unless that event is what is taking the state out.
 */
Impl::EngineState
Impl::ExitCurrentState(
    _In_ const EVENT_ACTION &Cause)
{
    if (m_currentStateAlreadyExited)
        return EngineState::Idle;

    m_currentStateAlreadyExited = true;

    auto const details = static_cast<const STOP_TIMER_ON_EXIT_DETAILS *>(
        SlotOf(StateSpec(CurrentState()), StateSlotType::StopTimerOnExit));

    if (details == nullptr || Cause.eventIndex == details->timerFiredEvent)
        return EngineState::Idle;

    if (details->stopTimerFunction(m_config.context) == StopTimerResult::TimerStopped)
        return EngineState::Idle;

    m_resumeAction = Cause;
    m_timerFiredEvent = details->timerFiredEvent;
    return EngineState::WaitingForTimerFired;
}

/* Entered with the lock held, which is always dropped. */
void
Impl::UpdateEngineState(
    _In_ EngineState NewState)
{
    m_engineState = NewState;

    switch (NewState)
    {
        case EngineState::NeedsWorkerForTransition:
        case EngineState::NeedsWorkerForAction:
            Unlock();
            IoQueueWorkItem(m_workItem, WorkItemRoutine, DelayedWorkQueue, this);
            break;

        case EngineState::WaitingForTimerFired:
            Unlock();
            break;

        default:
        {
            /* An empty stack means the main machine is done. */
            bool const finished = CurrentState() == 0;

            Unlock();

            if (finished)
                Destroy(true);

            break;
        }
    }
}

void
Impl::LogTransition(
    _In_ TransitionType Type,
    _In_ StateIndex Source,
    _In_ EventIndex Event,
    _In_ StateIndex Target,
    _In_ uint8_t StackDepth)
{
    TRANSITION_HISTORY_ENTRY &entry = m_transitionHistory[m_transitionHistoryIndex];

    /* Transitions that stay in a state are recorded against the handling state. */
    bool const stays = Type == TransitionType::Internal || Type == TransitionType::Ignored;

    entry.transitionType = Type;
    entry.processedEvent = Event;
    entry.stateIndex = stays ? Target : Source;
    entry.stackDepth = StackDepth;
    m_transitionHistoryIndex = (m_transitionHistoryIndex + 1) % TransitionHistorySize;

    if (m_config.logTransitionCallback != nullptr)
    {
        m_config.logTransitionCallback(m_config.context,
                                       Type,
                                       StateIdOf(Source),
                                       EventIdOf(Event),
                                       StateIdOf(Target));
    }
}

void
Impl::ReportException(
    _In_ MachineException Exception,
    _In_ EventIndex Event,
    _In_ StateIndex State)
{
    Lock();
    ReportExceptionWithLockHeld(Exception, Event, State);
}

/* Drops the lock before calling out. */
void
Impl::ReportExceptionWithLockHeld(
    _In_ MachineException Exception,
    _In_ EventIndex Event,
    _In_ StateIndex State)
{
    EXCEPTION_HISTORY_ENTRY &entry = m_exceptionHistory[m_exceptionHistoryIndex];

    entry.exception = Exception;
    entry.relevantEvent = Event;
    entry.relevantState = State;
    m_exceptionHistoryIndex = (m_exceptionHistoryIndex + 1) % ExceptionHistorySize;

    Unlock();

    if (m_config.logExceptionCallback != nullptr)
        m_config.logExceptionCallback(m_config.context, Exception, EventIdOf(Event), StateIdOf(State));
}

void
Impl::Destroy(
    _In_ bool InvokeDestroyCallback)
{
    EvtMachineDestroyedFunc *const callback = m_config.machineDestroyedCallback;
    void *const context = m_config.context;

    this->~StateMachineEngineImpl();
    ExFreePool(this);

    if (InvokeDestroyCallback && callback != nullptr)
        callback(context);
}

_Use_decl_annotations_
void
NTAPI
Impl::WorkItemRoutine(
    PDEVICE_OBJECT DeviceObject,
    PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    WorkerRoutine(Context);
}

/* Runs the parked action on a system thread at passive level, then carries on. */
void
Impl::WorkerRoutine(
    _In_ void *Context)
{
    auto const engine = static_cast<Impl *>(Context);

    engine->Lock();
    EngineState const parked = engine->m_engineState;
    engine->m_engineState = EngineState::Running;
    engine->Unlock();

    engine->NoteRunningContext(PASSIVE_LEVEL, true);

    if (parked == EngineState::NeedsWorkerForTransition)
    {
        engine->ExecuteEventAction(engine->m_resumeAction);
        engine->m_resumeAction = {};

        EngineState const result = engine->ExecuteCurrentState();

        engine->Lock();

        if (result == EngineState::Idle)
            engine->ProcessEventQueue();
        else
            engine->UpdateEngineState(result);
    }
    else if (parked == EngineState::NeedsWorkerForAction)
    {
        engine->ExecuteEventAction(engine->m_resumeAction);
        engine->m_resumeAction = {};

        engine->Lock();
        engine->ProcessEventQueue();
    }
}

_Use_decl_annotations_
NTSTATUS
StateMachineEngine::Initialize(
    const STATE_MACHINE_ENGINE_CONFIG &config)
{
    ULONG const tag = config.poolTag != 0 ? config.poolTag : DefaultPoolTag;

    void *memory = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(Impl), tag);
    if (memory == nullptr)
        return STATUS_INSUFFICIENT_RESOURCES;

    Impl *impl = new (memory) Impl(&m_impl);
    m_impl = impl;

    NTSTATUS const status = impl->Initialize(config);
    if (!NT_SUCCESS(status))
    {
        impl->~StateMachineEngineImpl();
        ExFreePool(memory);
        m_impl = nullptr;
        return status;
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
void
StateMachineEngine::EnqueueEvent(
    EventIndex event)
{
    m_impl->EnqueueEvent(event);
}

}
