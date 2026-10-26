#!/bin/sh
# Verifies that the given PE files can be loaded on Windows XP:
# - PE header: OS and subsystem version at most 5.1 (XP 32 bit) or 5.2 (XP 64 bit),
# - imports: only DLLs that ship with Windows XP,
# - imports: none of the well-known functions introduced after Windows XP.
# Usage: checkxp.sh file...
[ $# -gt 0 ] || { echo "usage: $0 file..." >&2; exit 2; }

# DLLs which do not exist on Windows XP.
badDlls='^(api-ms-.*|ext-ms-.*|ucrtbase|vcruntime.*|msvcp.*|concrt.*|bcrypt|bcryptprimitives|cfgmgr32|shcore|dwmapi|d2d1|d3d11|dxgi)\.dll$'
# Functions introduced after Windows XP (Vista or later).
badFuncs='^(InitializeConditionVariable|SleepConditionVariable(CS|SRW)|WakeConditionVariable|WakeAllConditionVariable|InitializeSRWLock|(Acquire|Release|TryAcquire)SRWLock(Exclusive|Shared)|InitOnce(Initialize|BeginInitialize|Complete|ExecuteOnce)|GetTickCount64|CreateSymbolicLink[AW]|GetFinalPathNameByHandle[AW]|GetLogicalProcessorInformationEx|Create(Event|Mutex|Semaphore|WaitableTimer)Ex[AW]|SetFileInformationByHandle|GetFileInformationByHandleEx|CancelIoEx|GetSystemTimePreciseAsFileTime|SetThreadErrorMode|GetThreadErrorMode|GetThreadId|QueryThreadCycleTime|QueryProcessCycleTime|InitializeCriticalSectionEx|InitializeProcThreadAttributeList|UpdateProcThreadAttribute|DeleteProcThreadAttributeList|GetCurrentProcessorNumberEx|WaitOnAddress|WakeByAddress(Single|All)|CreateThreadpool.*|CloseThreadpool.*|SubmitThreadpoolWork|WaitForThreadpool.*|(Get|Set)ThreadPreferredUILanguages|GetUserDefaultLocaleName|GetSystemDefaultLocaleName|GetLocaleInfoEx|LCMapStringEx|CompareStringEx|GetDateFormatEx|GetTimeFormatEx|IsValidLocaleName|LocaleNameToLCID|LCIDToLocaleName|FindNLSStringEx|GetNLSVersionEx|GetUserPreferredUILanguages|GetFileVersionInfo(Size)?Ex[AW]?|RegGetValue[AW]|RegDeleteKeyEx[AW]|RegisterApplicationRestart|InterlockedCompareExchange64|SetThreadStackGuarantee|AddVectoredContinuationHandler|GetProductInfo|ConvertThreadToFiberEx|RtlCaptureStackBackTrace)$'

status=0
for f in "$@"; do
	failed=0
	hdr=$(objdump -p "$f") || { echo "$f: objdump failed"; status=1; continue; }
	for field in MajorOSystemVersion MajorSubsystemVersion; do
		major=$(echo "$hdr" | awk -v k="$field" '$1 == k { print $2 }')
		if [ -z "$major" ] || [ "$major" -gt 5 ]; then
			echo "$f: $field is '$major', Windows XP requires 5 or lower"
			status=1; failed=1
		fi
	done
	minor=$(echo "$hdr" | awk '$1 == "MinorSubsystemVersion" { print $2 }')
	if [ "$minor" -gt 2 ]; then
		echo "$f: MinorSubsystemVersion is '$minor', Windows XP requires 2 or lower"
		status=1
		failed=1
	fi
	dlls=$(echo "$hdr" | awk '/^\tDLL Name:/ { print tolower($3) }' | sort -u)
	for d in $dlls; do
		if echo "$d" | grep -Eq "$badDlls"; then
			echo "$f: imports $d, which does not exist on Windows XP"
			status=1; failed=1
		fi
	done
	bad=$(echo "$hdr" | awk '/^	[0-9a-f]+  (<none>|[0-9]+)  [0-9a-f]+  [A-Za-z_]/ { print $4 }' | grep -E "$badFuncs" | sort -u)
	for fn in $bad; do
		echo "$f: imports $fn, which requires Windows Vista or later"
		status=1
		failed=1
	done
	[ $failed -eq 0 ] && echo "$f: OK"
done
exit $status
