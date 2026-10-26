/**
 * @file xpcompat.cpp
 * @author Daniel Starke
 * @date 2026-10-25
 * @version 2026-10-25
 *
 * Windows XP compatibility of the statically linked GCC runtime.
 *
 * The win32 thread model of libgcc (GCC 13 and newer) imports `GetThreadId`
 * and the condition variable functions, which exist since Windows Vista only.
 * The imports alone prevent loading on Windows XP. This file defines the
 * import pointers itself, so the linker does not take the ones of `kernel32`.
 * Each replacement calls the system function if available and falls back to
 * an implementation for Windows XP otherwise.
 */
#include <windows.h>


/* Name of the import pointer which the linker resolves for a `kernel32` function. */
#ifdef _WIN64
#define XP_IMPORT(name, argBytes) __asm__("__imp_" #name)
#else
#define XP_IMPORT(name, argBytes) __asm__("__imp__" #name "@" #argBytes)
#endif


namespace {


/** `THREAD_BASIC_INFORMATION` of `NtQueryInformationThread`. */
struct ThreadBasicInformation {
	LONG exitStatus;
	PVOID tebBaseAddress;
	HANDLE uniqueProcess;
	HANDLE uniqueThread;
	ULONG_PTR affinityMask;
	LONG priority;
	LONG basePriority;
};


/** Pointer sized `CONDITION_VARIABLE`. */
struct CondVar {
	PVOID ptr;
};


typedef DWORD (WINAPI * GetThreadIdFn)(HANDLE);
typedef LONG (WINAPI * NtQueryInformationThreadFn)(HANDLE, int, PVOID, ULONG, PULONG);
typedef VOID (WINAPI * InitializeConditionVariableFn)(CondVar *);
typedef BOOL (WINAPI * SleepConditionVariableCSFn)(CondVar *, CRITICAL_SECTION *, DWORD);
typedef VOID (WINAPI * WakeConditionVariableFn)(CondVar *);


/**
 * Returns a function of a system DLL.
 *
 * @param[in] module - DLL name
 * @param[in] name - function name
 * @return function or `nullptr`
 * @tparam Fn - function pointer type
 */
template <typename Fn>
Fn systemFunction(const wchar_t * module, const char * name) {
	const HMODULE handle = GetModuleHandleW(module);
	return (handle != nullptr) ? reinterpret_cast<Fn>(reinterpret_cast<void *>(GetProcAddress(handle, name))) : nullptr;
}


/**
 * Replacement of `GetThreadId`.
 *
 * @param[in] thread - thread handle
 * @return thread ID or 0 on error
 */
DWORD WINAPI compatGetThreadId(HANDLE thread) {
	const GetThreadIdFn getThreadId = systemFunction<GetThreadIdFn>(L"kernel32.dll", "GetThreadId");
	if (getThreadId != nullptr) {
		return getThreadId(thread);
	}
	const NtQueryInformationThreadFn query = systemFunction<NtQueryInformationThreadFn>(L"ntdll.dll", "NtQueryInformationThread");
	ThreadBasicInformation info;
	if (query == nullptr || query(thread, 0 /* ThreadBasicInformation */, &info, sizeof(info), nullptr) < 0) {
		SetLastError(ERROR_INVALID_HANDLE);
		return 0;
	}
	return DWORD(reinterpret_cast<ULONG_PTR>(info.uniqueThread));
}


/** Condition variable functions of the system, if all of them are available. */
struct SystemCondVar {
	InitializeConditionVariableFn initialize;
	SleepConditionVariableCSFn sleep;
	WakeConditionVariableFn wake;
	WakeConditionVariableFn wakeAll;

	/**
	 * Returns whether the system implementation is used.
	 *
	 * @return `true` if available
	 */
	bool available() const noexcept {
		return this->initialize != nullptr && this->sleep != nullptr && this->wake != nullptr && this->wakeAll != nullptr;
	}
};


/**
 * Returns the condition variable functions of the system.
 * No function local static with a constructor is used here, because its
 * thread-safe initialization waits on a condition variable itself. The lookup
 * gives the same result every time, so concurrent first calls are harmless.
 *
 * @return functions
 */
const SystemCondVar & getSystemCondVar() noexcept {
	static SystemCondVar functions;
	static volatile LONG resolved;
	if (InterlockedCompareExchange(&resolved, 0, 0) == 0) {
		functions.initialize = systemFunction<InitializeConditionVariableFn>(L"kernel32.dll", "InitializeConditionVariable");
		functions.sleep = systemFunction<SleepConditionVariableCSFn>(L"kernel32.dll", "SleepConditionVariableCS");
		functions.wake = systemFunction<WakeConditionVariableFn>(L"kernel32.dll", "WakeConditionVariable");
		functions.wakeAll = systemFunction<WakeConditionVariableFn>(L"kernel32.dll", "WakeAllConditionVariable");
		InterlockedExchange(&resolved, 1);
	}
	return functions;
}


/** Thread waiting for a condition variable of the fallback implementation. */
struct CondWaiter {
	HANDLE event; /**< Set when woken. */
	CondWaiter * next; /**< Next waiter in the queue. */
};


/**
 * State of a condition variable of the fallback implementation.
 * It is created on first use and never released, because condition variables
 * have no destroy function.
 */
struct CondState {
	CRITICAL_SECTION lock; /**< Protects the queue. */
	CondWaiter * head; /**< First waiter. */
	CondWaiter * tail; /**< Last waiter. */
};


/**
 * Returns the state of a condition variable of the fallback implementation.
 *
 * @param[in,out] cond - condition variable
 * @return state or `nullptr` if out of memory
 */
CondState * getCondState(CondVar * cond) noexcept {
	CondState * state = static_cast<CondState *>(InterlockedCompareExchangePointer(&cond->ptr, nullptr, nullptr));
	if (state != nullptr) {
		return state;
	}
	CondState * created = static_cast<CondState *>(HeapAlloc(GetProcessHeap(), 0, sizeof(CondState)));
	if (created == nullptr) {
		return nullptr;
	}
	InitializeCriticalSection(&created->lock);
	created->head = nullptr;
	created->tail = nullptr;
	state = static_cast<CondState *>(InterlockedCompareExchangePointer(&cond->ptr, created, nullptr));
	if (state != nullptr) {
		/* Another thread was faster. */
		DeleteCriticalSection(&created->lock);
		HeapFree(GetProcessHeap(), 0, created);
		return state;
	}
	return created;
}


/**
 * Removes a waiter from the queue if it is still queued.
 *
 * @param[in,out] state - condition variable state (locked)
 * @param[in] waiter - waiter to remove
 * @return `true` if it was removed, `false` if it was already woken
 */
bool removeWaiter(CondState * state, const CondWaiter * waiter) noexcept {
	CondWaiter * prev = nullptr;
	for (CondWaiter * it = state->head; it != nullptr; prev = it, it = it->next) {
		if (it == waiter) {
			if (prev == nullptr) {
				state->head = it->next;
			} else {
				prev->next = it->next;
			}
			if (state->tail == it) {
				state->tail = prev;
			}
			return true;
		}
	}
	return false;
}


/**
 * Replacement of `InitializeConditionVariable`.
 *
 * @param[out] cond - condition variable
 */
VOID WINAPI compatInitializeConditionVariable(CondVar * cond) {
	const SystemCondVar & sys = getSystemCondVar();
	if ( sys.available() ) {
		sys.initialize(cond);
		return;
	}
	cond->ptr = nullptr;
}


/**
 * Replacement of `SleepConditionVariableCS`.
 *
 * @param[in,out] cond - condition variable
 * @param[in,out] cs - critical section owned by the caller
 * @param[in] milliseconds - timeout
 * @return `TRUE` on success, `FALSE` on timeout or error
 */
BOOL WINAPI compatSleepConditionVariableCS(CondVar * cond, CRITICAL_SECTION * cs, DWORD milliseconds) {
	const SystemCondVar & sys = getSystemCondVar();
	if ( sys.available() ) {
		return sys.sleep(cond, cs, milliseconds);
	}
	CondState * state = getCondState(cond);
	CondWaiter waiter;
	waiter.event = (state != nullptr) ? CreateEventW(nullptr, TRUE, FALSE, nullptr) : nullptr;
	waiter.next = nullptr;
	if (waiter.event == nullptr) {
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return FALSE;
	}
	/* Queue up before releasing the critical section, so no wake is lost. */
	EnterCriticalSection(&state->lock);
	if (state->tail == nullptr) {
		state->head = &waiter;
	} else {
		state->tail->next = &waiter;
	}
	state->tail = &waiter;
	LeaveCriticalSection(&state->lock);
	LeaveCriticalSection(cs);
	const DWORD res = WaitForSingleObject(waiter.event, milliseconds);
	EnterCriticalSection(&state->lock);
	/* A waiter which is no longer queued has been woken, even on a timeout. */
	const bool timedOut = removeWaiter(state, &waiter);
	LeaveCriticalSection(&state->lock);
	CloseHandle(waiter.event);
	EnterCriticalSection(cs);
	if ( timedOut ) {
		SetLastError((res == WAIT_TIMEOUT) ? ERROR_TIMEOUT : GetLastError());
		return FALSE;
	}
	return TRUE;
}


/**
 * Replacement of `WakeConditionVariable`.
 *
 * @param[in,out] cond - condition variable
 */
VOID WINAPI compatWakeConditionVariable(CondVar * cond) {
	const SystemCondVar & sys = getSystemCondVar();
	if ( sys.available() ) {
		sys.wake(cond);
		return;
	}
	CondState * state = getCondState(cond);
	if (state == nullptr) {
		return;
	}
	EnterCriticalSection(&state->lock);
	CondWaiter * waiter = state->head;
	if (waiter != nullptr) {
		state->head = waiter->next;
		if (state->head == nullptr) {
			state->tail = nullptr;
		}
		/* The waiter frees the event only after taking the lock again. */
		SetEvent(waiter->event);
	}
	LeaveCriticalSection(&state->lock);
}


/**
 * Replacement of `WakeAllConditionVariable`.
 *
 * @param[in,out] cond - condition variable
 */
VOID WINAPI compatWakeAllConditionVariable(CondVar * cond) {
	const SystemCondVar & sys = getSystemCondVar();
	if ( sys.available() ) {
		sys.wakeAll(cond);
		return;
	}
	CondState * state = getCondState(cond);
	if (state == nullptr) {
		return;
	}
	EnterCriticalSection(&state->lock);
	CondWaiter * waiter = state->head;
	state->head = nullptr;
	state->tail = nullptr;
	while (waiter != nullptr) {
		CondWaiter * const next = waiter->next;
		SetEvent(waiter->event);
		waiter = next;
	}
	LeaveCriticalSection(&state->lock);
}


} /* anonymous namespace */


/* Import pointers as referenced by libgcc. */
extern "C" GetThreadIdFn compatImpGetThreadId XP_IMPORT(GetThreadId, 4);
extern "C" InitializeConditionVariableFn compatImpInitializeConditionVariable XP_IMPORT(InitializeConditionVariable, 4);
extern "C" SleepConditionVariableCSFn compatImpSleepConditionVariableCS XP_IMPORT(SleepConditionVariableCS, 12);
extern "C" WakeConditionVariableFn compatImpWakeConditionVariable XP_IMPORT(WakeConditionVariable, 4);
extern "C" WakeConditionVariableFn compatImpWakeAllConditionVariable XP_IMPORT(WakeAllConditionVariable, 4);
__attribute__((used)) GetThreadIdFn compatImpGetThreadId = compatGetThreadId;
__attribute__((used)) InitializeConditionVariableFn compatImpInitializeConditionVariable = compatInitializeConditionVariable;
__attribute__((used)) SleepConditionVariableCSFn compatImpSleepConditionVariableCS = compatSleepConditionVariableCS;
__attribute__((used)) WakeConditionVariableFn compatImpWakeConditionVariable = compatWakeConditionVariable;
__attribute__((used)) WakeConditionVariableFn compatImpWakeAllConditionVariable = compatWakeAllConditionVariable;
