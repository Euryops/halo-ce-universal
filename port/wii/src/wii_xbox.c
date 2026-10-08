/*
WII_XBOX.C

The XAPI calls the game makes while it starts, on the Wii: memory, time,
errors and the waitable objects, enough for shell_initialize to run through
and log what it does. The rest of the SDK (Direct3D, DirectSound, XInput,
XNet, files) is stubbed by wii_stubs.c, generated at build time from what the
link is missing, until a stage of docs/plans/2026-10-09-halo-on-the-wii.md
replaces each part.
*/

#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

/* newlib has these; the port's own malloc.h (port/linux/include) hides them */
void *memalign(size_t alignment, size_t size);
size_t malloc_usable_size(void *pointer);
struct mallinfo { size_t arena, ordblks, smblks, hblks, hblkhd, usmblks, fsmblks, uordblks, fordblks, keepcost; };
struct mallinfo mallinfo(void);

/* ---------- data the game links against */

XPP_DEVICE_TYPE XDEVICE_TYPE_GAMEPAD_TABLE;
XPP_DEVICE_TYPE XDEVICE_TYPE_MEMORY_UNIT_TABLE;
XPP_DEVICE_TYPE XDEVICE_TYPE_DEBUG_KEYBOARD_TABLE;

DWORD D3D__RenderState[D3DRS_MAX];
DWORD D3D__TextureState[D3DTSS_MAXSTAGES][D3DTSS_MAX];
WORD *D3D__IndexData;

/* ---------- memory

The Xbox hands out physically contiguous memory, some of it at fixed
addresses: the game state, and the tag cache that maps are linked to. On the
Wii those places are in port/wii/halo_wii_capacity.h, kept back from the
heap by wii_main.c; a request for one of them gets it, and anything else
comes from the heap. The full plan across MEM1 and MEM2 is stage 3. */

struct fixed_place
{
	unsigned long address;
	unsigned long end;
};

static const struct fixed_place fixed_places[] =
{
	{ HALO_PORT_GAME_STATE_BASE_ADDRESS, HALO_PORT_GAME_STATE_BASE_ADDRESS + HALO_PORT_GAME_STATE_SIZE },
	{ HALO_WII_TAG_CACHE_BASE_ADDRESS, HALO_WII_TAG_CACHE_END },
};

LPVOID WINAPI XPhysicalAlloc(SIZE_T size, ULONG_PTR physical_address, ULONG_PTR alignment, DWORD protect)
{
	void *result;
	int index;

	/* the game asks for a place by its highest acceptable physical address,
	which is the place itself less the cached segment's top bit */
	if (physical_address != (ULONG_PTR)-1)
	{
		unsigned long address = physical_address | 0x80000000;

		for (index = 0; index < (int)(sizeof(fixed_places) / sizeof(fixed_places[0])); index++)
		{
			if (fixed_places[index].address == address && address + size <= fixed_places[index].end)
			{
				platform_log("XPhysicalAlloc: %lu KB at 0x%08lx", (unsigned long)size >> 10, address);
				return (void *)address;
			}
		}
		platform_log("XPhysicalAlloc: no place for %lu KB at 0x%08lx", (unsigned long)size >> 10, address);
	}

	result = memalign(alignment > 32 ? alignment : 32, size);
	if (!result)
	{
		platform_log("XPhysicalAlloc: no room for %lu KB", (unsigned long)size >> 10);
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return NULL;
	}
	platform_log("XPhysicalAlloc: %lu KB at %p", (unsigned long)size >> 10, result);
	return result;
}

void WINAPI XPhysicalFree(LPVOID address)
{
	int index;

	for (index = 0; index < (int)(sizeof(fixed_places) / sizeof(fixed_places[0])); index++)
	{
		if ((unsigned long)address == fixed_places[index].address)
			return;
	}
	free(address);
}

void WINAPI XPhysicalProtect(LPVOID address, SIZE_T size, DWORD protect)
{
}

DWORD WINAPI XQueryMemoryProtect(LPVOID address)
{
	return PAGE_READWRITE;
}

HGLOBAL WINAPI GlobalAlloc(UINT flags, SIZE_T size)
{
	return calloc(1, size ? size : 1);
}

HGLOBAL WINAPI GlobalReAlloc(HGLOBAL memory, SIZE_T size, UINT flags)
{
	return realloc(memory, size);
}

HLOCAL WINAPI LocalFree(HLOCAL memory)
{
	free(memory);
	return NULL;
}

SIZE_T WINAPI LocalSize(HLOCAL memory)
{
	return memory ? malloc_usable_size(memory) : 0;
}

void WINAPI GlobalMemoryStatus(LPMEMORYSTATUS status)
{
	struct mallinfo information = mallinfo();

	memset(status, 0, sizeof(*status));
	status->dwLength = sizeof(*status);
	status->dwTotalPhys = 88 << 20;
	status->dwAvailPhys = status->dwTotalPhys > (DWORD)information.uordblks ?
		status->dwTotalPhys - information.uordblks : 0;
	status->dwTotalVirtual = status->dwTotalPhys;
	status->dwAvailVirtual = status->dwAvailPhys;
}

/* ---------- time */

static unsigned long long microseconds(void)
{
	struct timeval now;

	gettimeofday(&now, NULL);
	return (unsigned long long)now.tv_sec * 1000000 + now.tv_usec;
}

DWORD WINAPI GetTickCount(void)
{
	return (DWORD)(microseconds() / 1000);
}

BOOL WINAPI QueryPerformanceCounter(LARGE_INTEGER *count)
{
	count->QuadPart = (LONGLONG)microseconds();
	return TRUE;
}

BOOL WINAPI QueryPerformanceFrequency(LARGE_INTEGER *frequency)
{
	frequency->QuadPart = 1000000;
	return TRUE;
}

void WINAPI Sleep(DWORD milliseconds)
{
	usleep(milliseconds * 1000);
}

DWORD WINAPI SleepEx(DWORD milliseconds, BOOL alertable)
{
	Sleep(milliseconds);
	return 0;
}

BOOL WINAPI SwitchToThread(void)
{
	return FALSE;
}

/* ---------- errors */

static DWORD last_error;

DWORD WINAPI GetLastError(void)
{
	return last_error;
}

void WINAPI SetLastError(DWORD error)
{
	last_error = error;
}

void WINAPI OutputDebugStringA(LPCSTR string)
{
	fputs(string, stdout);
}

/* ---------- waitable objects

There is one thread in stage 1, so an event or a mutex is always there to be
had: each is a distinct handle that every wait succeeds on at once. */

static HANDLE new_handle(void)
{
	return calloc(1, 16);
}

HANDLE WINAPI CreateEventA(LPSECURITY_ATTRIBUTES attributes, BOOL manual_reset, BOOL initial_state, LPCSTR name)
{
	return new_handle();
}

HANDLE WINAPI CreateMutexA(LPSECURITY_ATTRIBUTES attributes, BOOL initial_owner, LPCSTR name)
{
	return new_handle();
}

BOOL WINAPI SetEvent(HANDLE event)
{
	return TRUE;
}

BOOL WINAPI ResetEvent(HANDLE event)
{
	return TRUE;
}

BOOL WINAPI ReleaseMutex(HANDLE mutex)
{
	return TRUE;
}

DWORD WINAPI WaitForSingleObject(HANDLE handle, DWORD milliseconds)
{
	return WAIT_OBJECT_0;
}

DWORD WINAPI WaitForSingleObjectEx(HANDLE handle, DWORD milliseconds, BOOL alertable)
{
	return WAIT_OBJECT_0;
}

BOOL WINAPI CloseHandle(HANDLE handle)
{
	if (handle && handle != INVALID_HANDLE_VALUE)
		free(handle);
	return TRUE;
}

/* ---------- the title */

/* started from the loader, which the game takes as the dashboard: no
title-specific launch data (as xbox_xapi.c) */
DWORD WINAPI XGetLaunchInfo(PDWORD type, PLAUNCH_DATA data)
{
	if (type)
		*type = LDT_FROM_DASHBOARD;
	if (data)
		memset(data, 0, sizeof(*data));
	return ERROR_SUCCESS;
}

DWORD WINAPI XGetLanguage(void)
{
	return XC_LANGUAGE_ENGLISH;
}

/* ---------- files: none until the SD card (stage 3) */

HANDLE WINAPI CreateFileA(LPCSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES attributes,
	DWORD disposition, DWORD flags, HANDLE template_file)
{
	platform_log("CreateFile %s: no file system yet", path);
	SetLastError(ERROR_FILE_NOT_FOUND);
	return INVALID_HANDLE_VALUE;
}

DWORD WINAPI GetFileAttributesA(LPCSTR path)
{
	SetLastError(ERROR_FILE_NOT_FOUND);
	return (DWORD)-1;
}

HANDLE WINAPI FindFirstFileA(LPCSTR pattern, LPWIN32_FIND_DATAA data)
{
	SetLastError(ERROR_FILE_NOT_FOUND);
	return INVALID_HANDLE_VALUE;
}
