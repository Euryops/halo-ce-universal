/*
WII_XBOX.C

The XAPI calls the game makes while it starts, on the Wii: memory, time,
errors, events, mutexes and threads, and files on the SD card. Direct3D is
d3d8_gx.c. The rest of the SDK (DirectSound, XInput, XNet) is stubbed by wii_stubs.c, generated at build time from what the
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

/* ---------- memory

The Xbox hands out physically contiguous memory, some of it at fixed
addresses: the game state, and the tag cache that maps are linked to. On the
Wii those places are in port/wii/halo_wii_capacity.h, kept back from the
heap by wii_main.c; a request for one of them gets it, and anything else
comes from the heap. The texture and sound caches have places too, in MEM2,
so that the heap keeps MEM1 (the plan is in halo_wii_capacity.h). */

struct fixed_place
{
	unsigned long address;
	unsigned long end;
};

static const struct fixed_place fixed_places[] =
{
	{ HALO_PORT_GAME_STATE_BASE_ADDRESS, HALO_PORT_GAME_STATE_BASE_ADDRESS + HALO_PORT_GAME_STATE_SIZE },
	{ HALO_WII_TAG_CACHE_BASE_ADDRESS, HALO_WII_TAG_CACHE_END },
	{ HALO_WII_TEXTURE_CACHE_BASE_ADDRESS, HALO_WII_TEXTURE_CACHE_END },
	{ HALO_WII_SOUND_CACHE_BASE_ADDRESS, HALO_WII_SOUND_CACHE_END },
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
or a thread (wii_os.c's waitable objects), an open file, or a directory
listing (FindFirstFile, XFindFirstSaveGame). */

enum
{
	_handle_waitable = 0x57414954, /* WAIT */
	_handle_file = 0x46494c45, /* FILE */
	_handle_find = 0x46494e44, /* FIND: a directory listing, data is its find */
};

struct wii_handle
{
	unsigned long type;
	struct wii_waitable *waitable;
	int file;
	void *data;
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
	handle->data = NULL;
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

static void find_delete(void *find);

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
	else if (handle->type == _handle_find)
		find_delete(handle->data);
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
			{
				/* a write that moves nothing is a full card: libfat says
				so with 0 and no error */
				if (writing)
				{
					SetLastError(ERROR_DISK_FULL);
					result = FALSE;
				}
				break;
			}
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
	/* libfat stops a seek past the end where the card's free space runs
	out, where Win32 would go there: the game pre-sizes its z:\ cache files
	this way (cache_files_windows.c), and a short one is a full card */
	if (method == FILE_BEGIN && position != (off_t)(DWORD)distance)
	{
		SetLastError(ERROR_DISK_FULL);
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
	/* libfat writes a file's size to its folder only at close or fsync,
	and the game keeps its z:\ cache files open for good: without the fsync
	they are empty after the power goes, and made again at every start */
	if (position < 0 || ftruncate(handle->file, position) != 0 || fsync(handle->file) != 0)
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

/* seconds since 1970 as a FILETIME: 100 ns since 1601 */
static void filetime_from_seconds(long long seconds, FILETIME *time)
{
	unsigned long long ticks = (unsigned long long)(seconds + 11644473600LL) * 10000000ULL;

	time->dwLowDateTime = (DWORD)ticks;
	time->dwHighDateTime = (DWORD)(ticks >> 32);
}

static void attribute_data_from(const struct wii_path_information *information, WIN32_FILE_ATTRIBUTE_DATA *data)
{
	memset(data, 0, sizeof(*data));
	data->dwFileAttributes = information->kind == _wii_path_directory ?
		FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
	filetime_from_seconds(information->modified, &data->ftCreationTime);
	data->ftLastAccessTime = data->ftCreationTime;
	data->ftLastWriteTime = data->ftCreationTime;
	data->nFileSizeLow = information->size;
}

BOOL WINAPI GetFileAttributesExA(LPCSTR file_name, GET_FILEEX_INFO_LEVELS level, LPVOID file_information)
{
	struct wii_path_information information;
	char path[1024];

	if (level != GetFileExInfoStandard)
	{
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (!translate_path(file_name, path, sizeof(path)))
		return FALSE;
	if (wii_path_information(path, &information) != 0)
	{
		SetLastError(error_from_errno());
		return FALSE;
	}
	attribute_data_from(&information, file_information);
	return TRUE;
}

/* FAT keeps one time worth having, the last write: it answers all three */
BOOL WINAPI GetFileTime(HANDLE object, LPFILETIME creation_time, LPFILETIME last_access_time,
	LPFILETIME last_write_time)
{
	struct wii_handle *handle = handle_get(object, _handle_file);
	struct wii_path_information information;
	FILETIME time;

	if (!handle)
		return FALSE;
	if (wii_file_information(handle->file, &information) != 0)
	{
		SetLastError(error_from_errno());
		return FALSE;
	}
	filetime_from_seconds(information.modified, &time);
	if (creation_time)
		*creation_time = time;
	if (last_access_time)
		*last_access_time = time;
	if (last_write_time)
		*last_write_time = time;
	return TRUE;
}

BOOL WINAPI GetDiskFreeSpaceExA(LPCSTR directory_name, PULARGE_INTEGER free_bytes_available,
	PULARGE_INTEGER total_bytes, PULARGE_INTEGER total_free_bytes)
{
	char path[1024];
	unsigned long long free_bytes, all_bytes;

	if (!translate_path(directory_name ? directory_name : "d:\\", path, sizeof(path)))
		return FALSE;
	if (wii_disk_space(path, &free_bytes, &all_bytes) != 0)
	{
		SetLastError(error_from_errno());
		return FALSE;
	}
	if (free_bytes_available)
		free_bytes_available->QuadPart = free_bytes;
	if (total_free_bytes)
		total_free_bytes->QuadPart = free_bytes;
	if (total_bytes)
		total_bytes->QuadPart = all_bytes;
	return TRUE;
}

/* ---------- directory listings

FindFirstFile takes a folder and a pattern (files_windows.c asks for
<folder>\*.*), and answers each entry that matches, "." and ".." among
them as on Win32 when the card's FAT driver gives them. */

struct find
{
	struct wii_directory *directory;
	char pattern[MAX_PATH];
	/* a save-game listing's UDATA folder (XFindFirstSaveGame), or empty */
	char save_data_directory[MAX_PATH];
};

static void find_delete(void *data)
{
	struct find *find = data;

	wii_directory_close(find->directory);
	free(find);
}

static int lower(int character)
{
	return character >= 'A' && character <= 'Z' ? character + 0x20 : character;
}

/* Win32's wildcards: '*' any run, '?' one character, either case; "*.*"
also matches a name with no dot */
static BOOL wildcard_match(const char *pattern, const char *name)
{
	if (!strcmp(pattern, "*.*") || !strcmp(pattern, "*"))
		return TRUE;
	for (; *pattern; pattern++, name++)
	{
		if (*pattern == '*')
		{
			while (*pattern == '*')
				pattern++;
			if (!*pattern)
				return TRUE;
			for (; *name; name++)
			{
				if (wildcard_match(pattern, name))
					return TRUE;
			}
			return FALSE;
		}
		if (!*name || (*pattern != '?' && lower((unsigned char)*pattern) != lower((unsigned char)*name)))
			return FALSE;
	}
	return !*name;
}

static BOOL find_next(struct find *find, LPWIN32_FIND_DATAA data)
{
	char name[MAX_PATH];
	struct wii_path_information information;

	while (wii_directory_next(find->directory, name, sizeof(name), &information))
	{
		WIN32_FILE_ATTRIBUTE_DATA attributes;

		if (!wildcard_match(find->pattern, name))
			continue;
		attribute_data_from(&information, &attributes);
		memset(data, 0, sizeof(*data));
		data->dwFileAttributes = attributes.dwFileAttributes;
		data->ftCreationTime = attributes.ftCreationTime;
		data->ftLastAccessTime = attributes.ftLastAccessTime;
		data->ftLastWriteTime = attributes.ftLastWriteTime;
		data->nFileSizeLow = attributes.nFileSizeLow;
		snprintf(data->cFileName, sizeof(data->cFileName), "%s", name);
		return TRUE;
	}
	SetLastError(ERROR_NO_MORE_FILES);
	return FALSE;
}

HANDLE WINAPI FindFirstFileA(LPCSTR file_name, LPWIN32_FIND_DATAA data)
{
	char folder[MAX_PATH];
	char path[1024];
	const char *separator = strrchr(file_name, '\\');
	struct find *find;
	HANDLE handle;

	if (!separator)
		separator = strrchr(file_name, '/');
	snprintf(folder, sizeof(folder), "%.*s", separator ? (int)(separator - file_name) : 0, file_name);
	if (!translate_path(folder, path, sizeof(path)))
		return INVALID_HANDLE_VALUE;
	find = malloc(sizeof(*find));
	if (!find)
	{
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return INVALID_HANDLE_VALUE;
	}
	snprintf(find->pattern, sizeof(find->pattern), "%s", separator ? separator + 1 : file_name);
	find->save_data_directory[0] = '\0';
	find->directory = wii_directory_open(path);
	if (!find->directory)
	{
		SetLastError(error_from_errno());
		free(find);
		return INVALID_HANDLE_VALUE;
	}
	handle = handle_new(_handle_find, NULL, -1);
	if (!handle)
	{
		find_delete(find);
		return INVALID_HANDLE_VALUE;
	}
	((struct wii_handle *)handle)->data = find;
	if (!find_next(find, data))
	{
		CloseHandle(handle);
		SetLastError(ERROR_FILE_NOT_FOUND);
		return INVALID_HANDLE_VALUE;
	}
	return handle;
}

BOOL WINAPI FindNextFileA(HANDLE object, LPWIN32_FIND_DATAA data)
{
	struct wii_handle *handle = handle_get(object, _handle_find);

	return handle ? find_next(handle->data, data) : FALSE;
}

/* ---------- save games

As the Linux port (xbox_xapi.c), the Xbox's own layout: each save is a
folder <root>\UDATA\<id>\, its display name kept in SaveMeta.xbx as UTF-16,
which on this side is big-endian. The id is FNV-1a over the name's UTF-16
code units, so a name gets the same folder on both. */

#define SAVE_DATA_DIRECTORY "UDATA"
#define SAVE_META_FILE "SaveMeta.xbx"

static void save_data_directory_for(LPCSTR root, char *directory, size_t size)
{
	size_t length = strlen(root);

	snprintf(directory, size, "%s%s" SAVE_DATA_DIRECTORY "\\", root,
		length && (root[length - 1] == '\\' || root[length - 1] == '/') ? "" : "\\");
}

static void save_directory_for(LPCSTR root, LPCWSTR name, char *directory, size_t size)
{
	unsigned long long hash = 1469598103934665603ULL;
	char data_directory[MAX_PATH];

	for (; *name; name++)
	{
		hash ^= (unsigned long long)*name;
		hash *= 1099511628211ULL;
	}
	save_data_directory_for(root, data_directory, sizeof(data_directory));
	snprintf(directory, size, "%s%012llX\\", data_directory, hash & 0xffffffffffffULL);
}

static BOOL save_read_name(const char *directory, WCHAR *name, unsigned long name_count)
{
	char meta_path[MAX_PATH + 32];
	HANDLE file;
	DWORD read = 0;
	BOOL success;

	snprintf(meta_path, sizeof(meta_path), "%s" SAVE_META_FILE, directory);
	file = CreateFileA(meta_path, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return FALSE;
	memset(name, 0, name_count * sizeof(WCHAR));
	success = ReadFile(file, name, (name_count - 1) * sizeof(WCHAR), &read, NULL);
	CloseHandle(file);
	return success;
}

static size_t wide_length(LPCWSTR string)
{
	size_t length = 0;

	while (string[length])
		length++;
	return length;
}

DWORD WINAPI XCreateSaveGame(LPCSTR root_path_name, LPCWSTR save_game_name, DWORD creation_disposition,
	DWORD create_flags, LPSTR path_buffer, UINT buffer_size)
{
	char directory[MAX_PATH];
	char data_directory[MAX_PATH];
	char meta_path[MAX_PATH + 32];
	HANDLE file;
	DWORD written;

	save_directory_for(root_path_name, save_game_name, directory, sizeof(directory));
	if (GetFileAttributesA(directory) != INVALID_FILE_ATTRIBUTES)
	{
		if (creation_disposition == CREATE_NEW)
			return ERROR_ALREADY_EXISTS;
	}
	else
	{
		if (creation_disposition == OPEN_EXISTING)
			return ERROR_PATH_NOT_FOUND;
		save_data_directory_for(root_path_name, data_directory, sizeof(data_directory));
		CreateDirectoryA(root_path_name, NULL);
		CreateDirectoryA(data_directory, NULL);
		if (!CreateDirectoryA(directory, NULL))
			return GetLastError();
		snprintf(meta_path, sizeof(meta_path), "%s" SAVE_META_FILE, directory);
		file = CreateFileA(meta_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
		if (file == INVALID_HANDLE_VALUE)
			return GetLastError();
		WriteFile(file, save_game_name, (DWORD)(wide_length(save_game_name) * sizeof(WCHAR)), &written, NULL);
		CloseHandle(file);
	}
	if (path_buffer && buffer_size)
	{
		if (strlen(directory) + 1 > buffer_size)
			return ERROR_INSUFFICIENT_BUFFER;
		strcpy(path_buffer, directory);
	}
	return ERROR_SUCCESS;
}

static void delete_tree(const char *directory)
{
	char pattern[MAX_PATH + 4];
	WIN32_FIND_DATAA data;
	HANDLE find;

	snprintf(pattern, sizeof(pattern), "%s*", directory);
	find = FindFirstFileA(pattern, &data);
	if (find != INVALID_HANDLE_VALUE)
	{
		do
		{
			char child[MAX_PATH * 2];
			BOOL folder = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

			if (!strcmp(data.cFileName, ".") || !strcmp(data.cFileName, ".."))
				continue;
			/* (a path that does not fit is left) */
			if (snprintf(child, sizeof(child), "%s%s%s", directory, data.cFileName, folder ? "\\" : "") >=
				(int)sizeof(child))
				continue;
			if (folder)
				delete_tree(child);
			else
				DeleteFileA(child);
		} while (FindNextFileA(find, &data));
		CloseHandle(find);
	}
	RemoveDirectoryA(directory);
}

DWORD WINAPI XDeleteSaveGame(LPCSTR root_path_name, LPCWSTR save_game_name)
{
	char directory[MAX_PATH];

	save_directory_for(root_path_name, save_game_name, directory, sizeof(directory));
	if (GetFileAttributesA(directory) == INVALID_FILE_ATTRIBUTES)
		return ERROR_PATH_NOT_FOUND;
	delete_tree(directory);
	return ERROR_SUCCESS;
}

/* a save-game listing is a listing of UDATA: each folder with a readable
name is one save */
static BOOL save_game_fill(HANDLE listing, const char *data_directory, WIN32_FIND_DATAA *entry,
	PXGAME_FIND_DATA data)
{
	do
	{
		if (!(entry->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
			!strcmp(entry->cFileName, ".") || !strcmp(entry->cFileName, ".."))
			continue;
		memset(data, 0, sizeof(*data));
		data->wfd = *entry;
		snprintf(data->szSaveGameDirectory, sizeof(data->szSaveGameDirectory), "%s%s\\",
			data_directory, entry->cFileName);
		if (save_read_name(data->szSaveGameDirectory, data->szSaveGameName, MAX_GAMENAME))
			return TRUE;
	} while (FindNextFileA(listing, entry));
	SetLastError(ERROR_NO_MORE_FILES);
	return FALSE;
}

HANDLE WINAPI XFindFirstSaveGame(LPCSTR root_path_name, PXGAME_FIND_DATA find_game_data)
{
	char data_directory[MAX_PATH];
	char pattern[MAX_PATH + 4];
	WIN32_FIND_DATAA entry;
	HANDLE listing;

	save_data_directory_for(root_path_name, data_directory, sizeof(data_directory));
	snprintf(pattern, sizeof(pattern), "%s*", data_directory);
	listing = FindFirstFileA(pattern, &entry);
	if (listing == INVALID_HANDLE_VALUE)
	{
		SetLastError(ERROR_NO_MORE_FILES);
		return INVALID_HANDLE_VALUE;
	}
	if (!save_game_fill(listing, data_directory, &entry, find_game_data))
	{
		CloseHandle(listing);
		SetLastError(ERROR_NO_MORE_FILES);
		return INVALID_HANDLE_VALUE;
	}
	snprintf(((struct find *)((struct wii_handle *)listing)->data)->save_data_directory,
		MAX_PATH, "%s", data_directory);
	return listing;
}

BOOL WINAPI XFindNextSaveGame(HANDLE object, PXGAME_FIND_DATA find_game_data)
{
	struct wii_handle *handle = handle_get(object, _handle_find);
	struct find *find = handle ? handle->data : NULL;
	WIN32_FIND_DATAA entry;

	if (!find || !find->save_data_directory[0] || !FindNextFileA(object, &entry))
		return FALSE;
	return save_game_fill(object, find->save_data_directory, &entry, find_game_data);
}

BOOL WINAPI XFindClose(HANDLE object)
{
	return CloseHandle(object);
}

/* ---------- controllers, until stage 5

No pads yet, and no debug keyboard. The keyboard's queue must say it is
empty: the game reads keystrokes until the call fails (input_xbox.c), and
the generated stub's 0 is ERROR_SUCCESS, which held it in that loop for
ever once the GX device let it get that far. */

VOID WINAPI XInitDevices(DWORD preallocation_type_count, PXDEVICE_PREALLOC_TYPE preallocation_types)
{
	(void)preallocation_type_count;
	(void)preallocation_types;
}

BOOL WINAPI XGetDeviceChanges(PXPP_DEVICE_TYPE device_type, PDWORD insertions, PDWORD removals)
{
	(void)device_type;
	*insertions = 0;
	*removals = 0;
	return FALSE;
}

DWORD WINAPI XInputDebugInitKeyboardQueue(PXINPUT_DEBUG_KEYQUEUE_PARAMETERS parameters)
{
	(void)parameters;
	return ERROR_SUCCESS;
}

DWORD WINAPI XInputDebugGetKeystroke(PXINPUT_DEBUG_KEYSTROKE keystroke)
{
	(void)keystroke;
	return ERROR_HANDLE_EOF;
}
