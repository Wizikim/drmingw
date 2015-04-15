/*
 * Copyright 2002-2013 Jose Fonseca
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */


#include <map>

#include <assert.h>
#include <stdlib.h>

#include <windows.h>
#include <tchar.h>
#include <ntstatus.h>

#include "debugger.h"
#include "log.h"
#include "outdbg.h"
#include "symbols.h"


typedef struct {
    HANDLE hThread;
}
THREAD_INFO, * PTHREAD_INFO;

typedef std::map< DWORD, THREAD_INFO > THREAD_INFO_LIST;

typedef struct {
    HANDLE hProcess;
    THREAD_INFO_LIST Threads;
}
PROCESS_INFO, * PPROCESS_INFO;

typedef std::map< DWORD, PROCESS_INFO> PROCESS_INFO_LIST;
static PROCESS_INFO_LIST g_Processes;


BOOL ObtainSeDebugPrivilege(void)
{
    HANDLE hToken;
    PTOKEN_PRIVILEGES NewPrivileges;
    BYTE OldPriv[1024];
    PBYTE pbOldPriv;
    ULONG cbNeeded;
    BOOLEAN fRc;
    LUID LuidPrivilege;

    // Make sure we have access to adjust and to get the old token privileges
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
    {
        OutputDebug("OpenProcessToken failed with 0x%08lx\n", GetLastError());

        return FALSE;
    }

    cbNeeded = 0;

    // Initialize the privilege adjustment structure
    LookupPrivilegeValue( NULL, SE_DEBUG_NAME, &LuidPrivilege );

    NewPrivileges = (PTOKEN_PRIVILEGES) LocalAlloc(
        LMEM_ZEROINIT,
        sizeof(TOKEN_PRIVILEGES) + (1 - ANYSIZE_ARRAY)*sizeof(LUID_AND_ATTRIBUTES)
    );
    if(NewPrivileges == NULL) {
        return FALSE;
    }

    NewPrivileges->PrivilegeCount = 1;
    NewPrivileges->Privileges[0].Luid = LuidPrivilege;
    NewPrivileges->Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    // Enable the privilege
    pbOldPriv = OldPriv;
    fRc = AdjustTokenPrivileges(
        hToken,
        FALSE,
        NewPrivileges,
        1024,
        (PTOKEN_PRIVILEGES)pbOldPriv,
        &cbNeeded
    );

    if (!fRc) {

        // If the stack was too small to hold the privileges
        // then allocate off the heap
        if (GetLastError() == ERROR_INSUFFICIENT_BUFFER) {

            pbOldPriv = (PBYTE)LocalAlloc(LMEM_FIXED, cbNeeded);
            if (pbOldPriv == NULL) {
                return FALSE;
            }

            fRc = AdjustTokenPrivileges(
                hToken,
                FALSE,
                NewPrivileges,
                cbNeeded,
                (PTOKEN_PRIVILEGES)pbOldPriv,
                &cbNeeded
            );
        }
    }
    return fRc;
}


static BOOL CALLBACK
symCallback(HANDLE hProcess,
            ULONG ActionCode,
            ULONG64 CallbackData,
            ULONG64 UserContext)
{
    if (ActionCode == CBA_DEBUG_INFO) {
        lprintf("%s", (LPCSTR)(UINT_PTR)CallbackData);
        return TRUE;
    }

    return FALSE;
}


static void
loadModule(HANDLE hProcess, HANDLE hFile, LPVOID lpBaseOfDll)
{
    if (!SymLoadModuleEx(hProcess, hFile, NULL, NULL, (UINT_PTR)lpBaseOfDll, 0, NULL, 0)) {
        OutputDebug("warning: SymLoadModule64 failed: 0x%08lx\n", GetLastError());
    }

    if (hFile) {
        CloseHandle(hFile);
    }
}


static char *
readProcessString(HANDLE hProcess, LPCVOID lpBaseAddress, SIZE_T nSize)
{
    LPSTR lpszBuffer = (LPSTR)malloc(nSize + 1);
    SIZE_T NumberOfBytesRead = 0;

    if (!ReadProcessMemory(hProcess, lpBaseAddress, lpszBuffer, nSize, &NumberOfBytesRead)) {
        lpszBuffer[0] = '\0';
    }

    assert(NumberOfBytesRead <= nSize);
    lpszBuffer[NumberOfBytesRead] = '\0';
    return lpszBuffer;
}


BOOL DebugMainLoop(const DebugOptions *pOptions)
{
    BOOL fFinished = FALSE;
    BOOL fBreakpointSignalled = FALSE;
    BOOL fWowBreakpointSignalled = FALSE;

    while(!fFinished)
    {
        DEBUG_EVENT DebugEvent;            // debugging event information
        DWORD dwContinueStatus = DBG_CONTINUE;    // exception continuation
        PPROCESS_INFO pProcessInfo;
        PTHREAD_INFO pThreadInfo;
        HANDLE hProcess;

        // Wait for a debugging event to occur. The second parameter indicates
        // that the function does not return until a debugging event occurs.
        if(!WaitForDebugEvent(&DebugEvent, INFINITE))
        {
            OutputDebug("WaitForDebugEvent: 0x%08lx", GetLastError());

            return FALSE;
        }

        // Process the debugging event code.
        switch (DebugEvent.dwDebugEventCode) {
        case EXCEPTION_DEBUG_EVENT: {
            PEXCEPTION_RECORD pExceptionRecord = &DebugEvent.u.Exception.ExceptionRecord;
            NTSTATUS ExceptionCode = pExceptionRecord->ExceptionCode;

            // Process the exception code. When handling
            // exceptions, remember to set the continuation
            // status parameter (dwContinueStatus). This value
            // is used by the ContinueDebugEvent function.
            if (pOptions->debug_flag) {
                lprintf("EXCEPTION PID=%lu TID=%lu ExceptionCode=0x%lx dwFirstChance=%lu\r\n",
                        DebugEvent.dwProcessId,
                        DebugEvent.dwThreadId,
                        pExceptionRecord->ExceptionCode,
                        DebugEvent.u.Exception.dwFirstChance
                );
            }

            dwContinueStatus = DBG_EXCEPTION_NOT_HANDLED;

            if (DebugEvent.u.Exception.dwFirstChance) {
                if (pExceptionRecord->ExceptionCode == (DWORD)STATUS_BREAKPOINT) {
                    // Signal the aedebug event
                    if (!fBreakpointSignalled) {
                        fBreakpointSignalled = TRUE;

                        if (pOptions->hEvent) {
                            SetEvent(pOptions->hEvent);
                            CloseHandle(pOptions->hEvent);
                        }

                        /*
                         * We ignore first-chance breakpoints by default.
                         *
                         * We get one of these whenever we attach to a process.
                         * But in some cases, we never get a second-chance, e.g.,
                         * when we're attached through MSVCRT's abort().
                         */
                        if (!pOptions->breakpoint_flag) {
                            dwContinueStatus = DBG_CONTINUE;
                            break;
                        }
                    }
                }

                if (ExceptionCode == STATUS_WX86_BREAKPOINT) {
                    if (!fWowBreakpointSignalled) {
                        fWowBreakpointSignalled = TRUE;
                        dwContinueStatus = DBG_CONTINUE;
                    }
                }

                if (!pOptions->first_chance) {
                    break;
                }
            }

            // Find the process in the process list
            pProcessInfo = &g_Processes[DebugEvent.dwProcessId];

            SymRefreshModuleList(pProcessInfo->hProcess);

            dumpException(pProcessInfo->hProcess,
                          &DebugEvent.u.Exception.ExceptionRecord);

            // Find the thread in the thread list
            THREAD_INFO_LIST::const_iterator it;
            for (it = pProcessInfo->Threads.begin(); it != pProcessInfo->Threads.end(); ++it) {
                DWORD dwThreadId = it->first;
                if (dwThreadId != DebugEvent.dwThreadId &&
                    ExceptionCode != STATUS_BREAKPOINT &&
                    !pOptions->verbose_flag) {
                        continue;
                }

                dumpStack(pProcessInfo->hProcess,
                          pThreadInfo->hThread, NULL);
            }

            if (!DebugEvent.u.Exception.dwFirstChance) {
                /*
                 * Terminate the process. As continuing would cause the JIT debugger
                 * to be invoked again.
                 */
                TerminateProcess(pProcessInfo->hProcess, (UINT)ExceptionCode);
            }

            break;
        }

        case CREATE_THREAD_DEBUG_EVENT:
            if (pOptions->debug_flag) {
                lprintf("CREATE_THREAD PID=%lu TID=%lu\r\n",
                        DebugEvent.dwProcessId,
                        DebugEvent.dwThreadId
                );
            }

            // Add the thread to the thread list
            pProcessInfo = &g_Processes[DebugEvent.dwProcessId];
            pThreadInfo = &pProcessInfo->Threads[DebugEvent.dwThreadId];
            pThreadInfo->hThread = DebugEvent.u.CreateThread.hThread;
            break;

        case CREATE_PROCESS_DEBUG_EVENT: {
            if (pOptions->debug_flag) {
                lprintf("CREATE_PROCESS PID=%lu TID=%lu\r\n",
                        DebugEvent.dwProcessId,
                        DebugEvent.dwThreadId
                );
            }

            hProcess = DebugEvent.u.CreateProcessInfo.hProcess;

            pProcessInfo = &g_Processes[DebugEvent.dwProcessId];
            pProcessInfo->hProcess = hProcess;

            pThreadInfo = &pProcessInfo->Threads[DebugEvent.dwThreadId];
            pThreadInfo->hThread = DebugEvent.u.CreateProcessInfo.hThread;

            DWORD dwSymOptions = SymGetOptions();
            dwSymOptions |=
                SYMOPT_UNDNAME |
                SYMOPT_LOAD_LINES |
                SYMOPT_OMAP_FIND_NEAREST;

            // FIXME: This prevents catchsegv from resolving symbols upon
            // EXIT_PROCESS_DEBUG_EVENT
            if (0) {
                dwSymOptions |= SYMOPT_DEFERRED_LOADS;
            }

            if (pOptions->debug_flag) {
                dwSymOptions |= SYMOPT_DEBUG;
            }

#ifdef _WIN64
            BOOL bWow64 = FALSE;
            IsWow64Process(hProcess, &bWow64);
            if (bWow64) {
                dwSymOptions |= SYMOPT_INCLUDE_32BIT_MODULES;
            }
#endif

            SymSetOptions(dwSymOptions);
            if (!InitializeSym(hProcess, FALSE)) {
                OutputDebug("error: SymInitialize failed: 0x%08lx\n", GetLastError());
                exit(EXIT_FAILURE);
            }

            SymRegisterCallback64(hProcess, &symCallback, 0);

            loadModule(hProcess, DebugEvent.u.CreateProcessInfo.hFile, DebugEvent.u.CreateProcessInfo.lpBaseOfImage);

            break;
        }

        case EXIT_THREAD_DEBUG_EVENT:
            if (pOptions->debug_flag) {
                lprintf("EXIT_THREAD PID=%lu TID=%lu dwExitCode=0x%lx\r\n",
                        DebugEvent.dwProcessId,
                        DebugEvent.dwThreadId,
                        DebugEvent.u.ExitThread.dwExitCode
                );
            }

            // Remove the thread from the thread list
            pProcessInfo = &g_Processes[DebugEvent.dwProcessId];
            pProcessInfo->Threads.erase(DebugEvent.dwThreadId);
            break;

        case EXIT_PROCESS_DEBUG_EVENT: {
            if (pOptions->debug_flag) {
                lprintf("EXIT_PROCESS PID=%lu TID=%lu dwExitCode=0x%lx\r\n",
                        DebugEvent.dwProcessId,
                        DebugEvent.dwThreadId,
                        DebugEvent.u.ExitProcess.dwExitCode
                );
            }

            pProcessInfo = &g_Processes[DebugEvent.dwProcessId];
            hProcess = pProcessInfo->hProcess;

            // Dump the stack on abort()
            if (DebugEvent.u.ExitProcess.dwExitCode == 3) {
                pThreadInfo = &pProcessInfo->Threads[DebugEvent.dwThreadId];
                dumpStack(hProcess, pThreadInfo->hThread, NULL);
            }

            // Remove the process from the process list
            g_Processes.erase(DebugEvent.dwProcessId);

            if (!SymCleanup(hProcess)) {
                OutputDebug("SymCleanup failed with 0x%08lx\n", GetLastError());
            }

            if (g_Processes.empty()) {
                fFinished = TRUE;
            }

            break;
        }

        case LOAD_DLL_DEBUG_EVENT:
            if (pOptions->debug_flag) {
                lprintf("LOAD_DLL PID=%lu TID=%lu lpBaseOfDll=%p\r\n",
                        DebugEvent.dwProcessId,
                        DebugEvent.dwThreadId,
                        DebugEvent.u.LoadDll.lpBaseOfDll
                );
            }

            pProcessInfo = &g_Processes[DebugEvent.dwProcessId];
            hProcess = pProcessInfo->hProcess;

            loadModule(hProcess, DebugEvent.u.LoadDll.hFile, DebugEvent.u.LoadDll.lpBaseOfDll);

            break;

        case UNLOAD_DLL_DEBUG_EVENT:
            if (pOptions->debug_flag) {
                lprintf("UNLOAD_DLL PID=%lu TID=%lu lpBaseOfDll=%p\r\n",
                        DebugEvent.dwProcessId,
                        DebugEvent.dwThreadId,
                        DebugEvent.u.UnloadDll.lpBaseOfDll
                );
            }

            pProcessInfo = &g_Processes[DebugEvent.dwProcessId];
            hProcess = pProcessInfo->hProcess;

            SymUnloadModule64(hProcess, (UINT_PTR)DebugEvent.u.UnloadDll.lpBaseOfDll);

            break;

        case OUTPUT_DEBUG_STRING_EVENT: {
            if (pOptions->debug_flag) {
                lprintf("OUTPUT_DEBUG_STRING PID=%lu TID=%lu\r\n",
                        DebugEvent.dwProcessId,
                        DebugEvent.dwThreadId
                );
            }

            pProcessInfo = &g_Processes[DebugEvent.dwProcessId];

            assert(!DebugEvent.u.DebugString.fUnicode);

            LPSTR lpDebugStringData = readProcessString(pProcessInfo->hProcess,
                                                        DebugEvent.u.DebugString.lpDebugStringData,
                                                        DebugEvent.u.DebugString.nDebugStringLength);

            fputs(lpDebugStringData, stderr);

            free(lpDebugStringData);
            break;
        }

        case RIP_EVENT:
            if (pOptions->debug_flag) {
                lprintf("RIP PID=%lu TID=%lu\r\n",
                        DebugEvent.dwProcessId,
                        DebugEvent.dwThreadId
                );
            }
            break;

        default:
            if (pOptions->debug_flag) {
                lprintf("EVENT%lu PID=%lu TID=%lu\r\n",
                    DebugEvent.dwDebugEventCode,
                    DebugEvent.dwProcessId,
                    DebugEvent.dwThreadId
                );
            }
            break;
        }

        // Resume executing the thread that reported the debugging event.
        ContinueDebugEvent(
            DebugEvent.dwProcessId,
            DebugEvent.dwThreadId,
            dwContinueStatus
        );
    }

    return TRUE;
}