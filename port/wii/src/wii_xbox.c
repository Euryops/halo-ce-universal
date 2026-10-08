/*
WII_XBOX.C

The XAPI calls the game makes while it starts, on the Wii: memory, time,
errors, events, mutexes and threads, and files on the SD card. The rest of the SDK (Direct3D, DirectSound, XInput,
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
#include <errno.h>
#include <fcntl.h>

#include "wii_os.h"

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

/* (completions_run is below, with the asynchronous procedure calls) */
static BOOL completions_run(void);

DWORD WINAPI SleepEx(DWORD milliseconds, BOOL alertable)
{
	if (alertable && completions_run())
		return WAIT_IO_COMPLETION;
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

/* ---------- handles

Every HANDLE this layer returns points at one of these: an event, a mutex
or a thread (wii_os.c's waitable objects), or an open file. */

enum
{
	_handle_waitable = 0x57414954, /* WAIT */
	_handle_file = 0x46494c45, /* FILE */
};

struct wii_handle
{
	unsigned long type;
	struct wii_waitable *waitable;
	int file;
};

static HANDLE handle_new(unsigned long type, struct wii_waitable *waitable, int file)
{
	struct wii_handle *handle;

	if (type == _handle_waitable && !waitable)
	{
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return NULL;
	}
	handle = malloc(sizeof(*handle));
	if (!handle)
	{
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return NULL;
	}
	handle->type = type;
	handle->waitable = waitable;
	handle->file = file;
	return handle;
}

static struct wii_handle *handle_get(HANDLE object, unsigned long type)
{
	struct wii_handle *handle = object;

	if (!handle || object == INVALID_HANDLE_VALUE || handle->type != type)
	{
		SetLastError(ERROR_INVALID_HANDLE);
		return NULL;
	}
	return handle;
}

static struct wii_waitable *waitable_get(HANDLE object)
{
	struct wii_handle *handle = handle_get(object, _handle_waitable);

	return handle ? handle->waitable : NULL;
}

BOOL WINAPI CloseHandle(HANDLE object)
{
	struct wii_handle *handle = object;

	if (!handle || object == INVALID_HANDLE_VALUE)
	{
		SetLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	if (handle->type == _handle_waitable)
		wii_waitable_delete(handle->waitable);
	else if (handle->type == _handle_file)
		close(handle->file);
	else
	{
		SetLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	handle->type = 0;
	free(handle);
	return TRUE;
}

/* ---------- asynchronous procedure calls

ReadFileEx and WriteFileEx finish at once; as on Win32, the completion
routine then runs on the thread that asked, at its next alertable wait
(WaitForSingleObjectEx or SleepEx with alertable TRUE), which then answers
WAIT_IO_COMPLETION. Only that thread queues or runs its calls, so the lock
guards the list against the other threads' entries only. */

struct completion
{
	unsigned long thread;
	LPOVERLAPPED_COMPLETION_ROUTINE routine;
	LPOVERLAPPED overlapped;
	struct completion *next;
};

static struct completion *completions;
static struct wii_waitable *completions_lock;

static void completions_lock_take(void)
{
	if (!completions_lock)
		completions_lock = wii_waitable_new(_wii_waitable_mutex, FALSE, FALSE);
	wii_waitable_wait(completions_lock, WII_INFINITE);
}

static void completion_queue(LPOVERLAPPED_COMPLETION_ROUTINE routine, LPOVERLAPPED overlapped)
{
	struct completion *entry = malloc(sizeof(*entry));
	struct completion **last;

	if (!entry)
		return;
	entry->thread = wii_thread_self();
	entry->routine = routine;
	entry->overlapped = overlapped;
	entry->next = NULL;
	completions_lock_take();
	for (last = &completions; *last; last = &(*last)->next);
	*last = entry;
	wii_mutex_release(completions_lock);
}

/* run the calling thread's queued completions, in order; TRUE if any ran */
static BOOL completions_run(void)
{
	unsigned long self = wii_thread_self();
	BOOL ran = FALSE;

	for (;;)
	{
		struct completion **link;
		struct completion *entry = NULL;

		completions_lock_take();
		for (link = &completions; *link; link = &(*link)->next)
		{
			if ((*link)->thread == self)
			{
				entry = *link;
				*link = entry->next;
				break;
			}
		}
		wii_mutex_release(completions_lock);
		if (!entry)
			return ran;
		entry->routine((DWORD)entry->overlapped->Internal, (DWORD)entry->overlapped->InternalHigh,
			entry->overlapped);
		free(entry);
		ran = TRUE;
	}
}

/* ---------- events, mutexes and threads */

HANDLE WINAPI CreateEventA(LPSECURITY_ATTRIBUTES attributes, BOOL manual_reset, BOOL initial_state, LPCSTR name)
{
	return handle_new(_handle_waitable, wii_waitable_new(_wii_waitable_event, manual_reset, initial_state), -1);
}

HANDLE WINAPI CreateMutexA(LPSECURITY_ATTRIBUTES attributes, BOOL initial_owner, LPCSTR name)
{
	HANDLE handle = handle_new(_handle_waitable, wii_waitable_new(_wii_waitable_mutex, FALSE, FALSE), -1);

	if (handle && initial_owner)
		WaitForSingleObject(handle, INFINITE);
	return handle;
}

BOOL WINAPI SetEvent(HANDLE event)
{
	struct wii_waitable *waitable = waitable_get(event);

	if (!waitable)
		return FALSE;
	wii_event_set(waitable);
	return TRUE;
}

BOOL WINAPI ResetEvent(HANDLE event)
{
	struct wii_waitable *waitable = waitable_get(event);

	if (!waitable)
		return FALSE;
	wii_event_reset(waitable);
	return TRUE;
}

BOOL WINAPI ReleaseMutex(HANDLE mutex)
{
	struct wii_waitable *waitable = waitable_get(mutex);

	if (!waitable || !wii_mutex_release(waitable))
	{
		SetLastError(ERROR_NOT_OWNER);
		return FALSE;
	}
	return TRUE;
}

DWORD WINAPI WaitForSingleObject(HANDLE handle, DWORD milliseconds)
{
	struct wii_waitable *waitable = waitable_get(handle);

	if (!waitable)
		return WAIT_FAILED;
	return wii_waitable_wait(waitable, milliseconds == INFINITE ? WII_INFINITE : milliseconds) ?
		WAIT_OBJECT_0 : WAIT_TIMEOUT;
}

DWORD WINAPI WaitForSingleObjectEx(HANDLE handle, DWORD milliseconds, BOOL alertable)
{
	if (alertable && completions_run())
		return WAIT_IO_COMPLETION;
	return WaitForSingleObject(handle, milliseconds);
}

HANDLE WINAPI CreateThread(LPSECURITY_ATTRIBUTES attributes, DWORD stack_size,
	LPTHREAD_START_ROUTINE start, LPVOID parameter, DWORD flags, LPDWORD thread_id)
{
	static DWORD next_thread_id = 1;
	HANDLE handle = handle_new(_handle_waitable,
		wii_thread_start(start, parameter, stack_size, (flags & CREATE_SUSPENDED) != 0), -1);

	if (handle && thread_id)
		*thread_id = next_thread_id++;
	return handle;
}

DWORD WINAPI ResumeThread(HANDLE thread)
{
	struct wii_waitable *waitable = waitable_get(thread);

	if (!waitable)
		return (DWORD)-1;
	wii_thread_resume(waitable);
	return 1;
}

/* every game thread runs at main's priority (wii_os.c) */
BOOL WINAPI SetThreadPriority(HANDLE thread, int priority)
{
	return TRUE;
}

BOOL WINAPI GetExitCodeThread(HANDLE thread, LPDWORD exit_code)
{
	struct wii_waitable *waitable = waitable_get(thread);
	unsigned long code;

	if (!waitable)
		return FALSE;
	*exit_code = wii_thread_exit_code(waitable, &code) ? code : STILL_ACTIVE;
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

/* ---------- files

The Xbox's drives are folders of the SD card's data root (wii_os.c): d:\ is
sd:/halo itself, where the maps go (sd:/halo/maps), and every other drive
is sd:/halo/<letter>, made on first use. FAT is case-insensitive as FATX
is, so a name needs no search. */

static BOOL translate_path(const char *xbox_path, char *path, size_t size)
{
	const char *root = wii_data_root();
	size_t length;
	BOOL separator;

	if (!root)
	{
		SetLastError(ERROR_PATH_NOT_FOUND);
		return FALSE;
	}
	if (((xbox_path[0] | 0x20) >= 'a' && (xbox_path[0] | 0x20) <= 'z') && xbox_path[1] == ':')
	{
		char drive = (char)(xbox_path[0] | 0x20);

		if (drive == 'd')
			snprintf(path, size, "%s", root);
		else
		{
			snprintf(path, size, "%s/%c", root, drive);
			wii_make_directory(path);
		}
		xbox_path += 2;
	}
	else
		snprintf(path, size, "%s", root);

	/* each component after a separator, the first after the root */
	length = strlen(path);
	for (separator = TRUE; *xbox_path && length + 2 < size; xbox_path++)
	{
		if (*xbox_path == '\\' || *xbox_path == '/')
		{
			separator = TRUE;
			continue;
		}
		if (separator)
		{
			path[length++] = '/';
			separator = FALSE;
		}
		path[length++] = *xbox_path;
	}
	path[length] = '\0';
	return TRUE;
}

#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)

static DWORD error_from_errno(void)
{
	switch (errno)
	{
	case ENOENT: return ERROR_FILE_NOT_FOUND;
	case ENOTDIR: return ERROR_PATH_NOT_FOUND;
	case EEXIST: return ERROR_ALREADY_EXISTS;
	case EACCES: case EPERM: case EROFS: return ERROR_ACCESS_DENIED;
	case ENOSPC: return ERROR_DISK_FULL;
	default: return ERROR_GEN_FAILURE;
	}
}

/* (wii_crt.c's fopen and open take the same paths) */
BOOL wii_translate_path(const char *xbox_path, char *path, size_t size)
{
	return translate_path(xbox_path, path, size);
}

HANDLE WINAPI CreateFileA(LPCSTR file_name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES attributes,
	DWORD disposition, DWORD flags_and_attributes, HANDLE template_file)
{
	char path[1024];
	int kind;
	BOOL existed;
	int flags;
	int file;
	HANDLE handle;

	if (!translate_path(file_name, path, sizeof(path)))
		return INVALID_HANDLE_VALUE;
	kind = wii_path_kind(path);
	existed = kind != _wii_path_missing;
	if ((access & GENERIC_READ) && (access & GENERIC_WRITE))
		flags = O_RDWR;
	else if (access & GENERIC_WRITE)
		flags = O_WRONLY;
	else
		flags = O_RDONLY;
	switch (disposition)
	{
	case CREATE_NEW: flags |= O_CREAT | O_EXCL; break;
	case CREATE_ALWAYS: flags |= O_CREAT | O_TRUNC; break;
	case OPEN_EXISTING: break;
	case OPEN_ALWAYS: flags |= O_CREAT; break;
	case TRUNCATE_EXISTING: flags |= O_TRUNC; break;
	default:
		SetLastError(ERROR_INVALID_PARAMETER);
		return INVALID_HANDLE_VALUE;
	}
	if (kind == _wii_path_directory)
	{
		SetLastError(ERROR_ACCESS_DENIED);
		return INVALID_HANDLE_VALUE;
	}
	file = open(path, flags, 0666);
	if (file < 0)
	{
		SetLastError(error_from_errno());
		return INVALID_HANDLE_VALUE;
	}
	handle = handle_new(_handle_file, NULL, file);
	if (!handle)
	{
		close(file);
		return INVALID_HANDLE_VALUE;
	}
	/* as Win32: success, but say whether it was there already */
	SetLastError(existed && (disposition == CREATE_ALWAYS || disposition == OPEN_ALWAYS) ?
		ERROR_ALREADY_EXISTS : ERROR_SUCCESS);
	return handle;
}

/* an overlapped request names its offset; a plain one goes on from the file
position */
static BOOL file_transfer(HANDLE object, void *buffer, DWORD count, LPDWORD done_out,
	LPOVERLAPPED overlapped, BOOL writing)
{
	struct wii_handle *handle = handle_get(object, _handle_file);
	DWORD done = 0;
	BOOL result = FALSE;

	if (handle && (!overlapped || lseek(handle->file, overlapped->Offset, SEEK_SET) >= 0))
	{
		result = TRUE;
		while (done < count)
		{
			int moved = writing ?
				write(handle->file, (const char *)buffer + done, count - done) :
				read(handle->file, (char *)buffer + done, count - done);

			if (moved < 0)
			{
				SetLastError(error_from_errno());
				result = FALSE;
				break;
			}
			if (!moved)
				break;
			done += moved;
		}
	}
	if (done_out)
		*done_out = done;
	if (overlapped)
	{
		overlapped->Internal = result ? 0 : GetLastError();
		overlapped->InternalHigh = done;
		if (overlapped->hEvent)
			SetEvent(overlapped->hEvent);
	}
	return result;
}

/* (see the asynchronous procedure calls) */
static BOOL file_transfer_ex(HANDLE handle, void *buffer, DWORD count, LPOVERLAPPED overlapped,
	LPOVERLAPPED_COMPLETION_ROUTINE routine, BOOL writing)
{
	DWORD done = 0;
	BOOL result;

	if (!handle_get(handle, _handle_file))
		return FALSE;
	if (!overlapped)
	{
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	/* the event is the caller's own word here (cache_files_windows.c), not
	a handle to signal */
	{
		HANDLE event = overlapped->hEvent;

		overlapped->hEvent = NULL;
		result = file_transfer(handle, buffer, count, &done, overlapped, writing);
		overlapped->hEvent = event;
	}
	overlapped->Internal = result ? ERROR_SUCCESS : GetLastError();
	if (result && !writing && done == 0 && count > 0)
		overlapped->Internal = ERROR_HANDLE_EOF;
	overlapped->InternalHigh = done;
	completion_queue(routine, overlapped);
	SetLastError(ERROR_SUCCESS);
	return TRUE;
}

BOOL WINAPI ReadFileEx(HANDLE handle, LPVOID buffer, DWORD count, LPOVERLAPPED overlapped,
	LPOVERLAPPED_COMPLETION_ROUTINE routine)
{
	return file_transfer_ex(handle, buffer, count, overlapped, routine, FALSE);
}

BOOL WINAPI WriteFileEx(HANDLE handle, LPCVOID buffer, DWORD count, LPOVERLAPPED overlapped,
	LPOVERLAPPED_COMPLETION_ROUTINE routine)
{
	return file_transfer_ex(handle, (void *)buffer, count, overlapped, routine, TRUE);
}

BOOL WINAPI ReadFile(HANDLE handle, LPVOID buffer, DWORD count, LPDWORD bytes_read, LPOVERLAPPED overlapped)
{
	return file_transfer(handle, buffer, count, bytes_read, overlapped, FALSE);
}

BOOL WINAPI WriteFile(HANDLE handle, LPCVOID buffer, DWORD count, LPDWORD bytes_written, LPOVERLAPPED overlapped)
{
	return file_transfer(handle, (void *)buffer, count, bytes_written, overlapped, TRUE);
}

DWORD WINAPI SetFilePointer(HANDLE object, LONG distance, PLONG distance_high, DWORD method)
{
	struct wii_handle *handle = handle_get(object, _handle_file);
	off_t position;

	if (!handle)
		return INVALID_SET_FILE_POINTER;
	position = lseek(handle->file, distance, method == FILE_BEGIN ? SEEK_SET : method == FILE_CURRENT ? SEEK_CUR : SEEK_END);
	if (position < 0)
	{
		SetLastError(error_from_errno());
		return INVALID_SET_FILE_POINTER;
	}
	if (distance_high)
		*distance_high = 0;
	return (DWORD)position;
}

DWORD WINAPI GetFileSize(HANDLE object, LPDWORD size_high)
{
	struct wii_handle *handle = handle_get(object, _handle_file);
	long size = handle ? wii_file_size(handle->file) : -1;

	if (size < 0)
		return INVALID_FILE_SIZE;
	if (size_high)
		*size_high = 0;
	return (DWORD)size;
}

BOOL WINAPI SetEndOfFile(HANDLE object)
{
	struct wii_handle *handle = handle_get(object, _handle_file);
	off_t position;

	if (!handle)
		return FALSE;
	position = lseek(handle->file, 0, SEEK_CUR);
	if (position < 0 || ftruncate(handle->file, position) != 0)
	{
		SetLastError(error_from_errno());
		return FALSE;
	}
	return TRUE;
}

DWORD WINAPI GetFileAttributesA(LPCSTR file_name)
{
	char path[1024];
	int kind;

	if (!translate_path(file_name, path, sizeof(path)))
		return INVALID_FILE_ATTRIBUTES;
	kind = wii_path_kind(path);
	if (kind == _wii_path_missing)
	{
		SetLastError(ERROR_FILE_NOT_FOUND);
		return INVALID_FILE_ATTRIBUTES;
	}
	return kind == _wii_path_directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
}

BOOL WINAPI CreateDirectoryA(LPCSTR path_name, LPSECURITY_ATTRIBUTES attributes)
{
	char path[1024];

	if (!translate_path(path_name, path, sizeof(path)))
		return FALSE;
	if (wii_make_directory(path) != 0)
	{
		SetLastError(error_from_errno());
		return FALSE;
	}
	return TRUE;
}

BOOL WINAPI DeleteFileA(LPCSTR file_name)
{
	char path[1024];

	if (!translate_path(file_name, path, sizeof(path)))
		return FALSE;
	if (unlink(path) != 0)
	{
		SetLastError(error_from_errno());
		return FALSE;
	}
	return TRUE;
}

BOOL WINAPI RemoveDirectoryA(LPCSTR path_name)
{
	char path[1024];

	if (!translate_path(path_name, path, sizeof(path)))
		return FALSE;
	if (rmdir(path) != 0)
	{
		SetLastError(error_from_errno());
		return FALSE;
	}
	return TRUE;
}

/* directory listings are stage 3 (the saved games and the map list) */
HANDLE WINAPI FindFirstFileA(LPCSTR pattern, LPWIN32_FIND_DATAA data)
{
	SetLastError(ERROR_FILE_NOT_FOUND);
	return INVALID_HANDLE_VALUE;
}
