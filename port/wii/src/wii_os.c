/*
WII_OS.C

libogc's side of the seam in wii_os.h: the SD card, the console's copy on
it, and the waitable objects the XAPI half builds events, mutexes and
threads on.
*/

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/iosupport.h>
#include <sys/stat.h>
#include <unistd.h>
#include <gccore.h>
#include <ogc/cond.h>
#include <ogc/lwp_watchdog.h>
#include <fat.h>

#include "wii_os.h"

/* the priority of the game's own threads and of main: equal, so none of them
starves the others while it runs */
#define THREAD_PRIORITY 64
#define MINIMUM_STACK_SIZE 0x10000

/* ---------- storage */

#define DATA_ROOT "sd:/halo"

static int storage_mounted;

int wii_storage_mount(void)
{
	if (!fatInitDefault())
		return 0;
	mkdir(DATA_ROOT, 0777);
	storage_mounted = 1;
	return 1;
}

const char *wii_data_root(void)
{
	return storage_mounted ? DATA_ROOT : NULL;
}

int wii_path_kind(const char *path)
{
	struct stat information;

	if (stat(path, &information) != 0)
		return _wii_path_missing;
	return S_ISDIR(information.st_mode) ? _wii_path_directory : _wii_path_file;
}

long wii_file_size(int file)
{
	struct stat information;

	return fstat(file, &information) == 0 ? (long)information.st_size : -1;
}

int wii_make_directory(const char *path)
{
	return mkdir(path, 0777);
}

/* stdout and stderr are the console's devoptab; the copy wraps its write */
static devoptab_t console_copy;
static ssize_t (*console_write)(struct _reent *r, void *fd, const char *ptr, size_t len);
static int log_file = -1;

static ssize_t console_and_log_write(struct _reent *r, void *fd, const char *ptr, size_t len)
{
	ssize_t result = console_write(r, fd, ptr, len);

	if (log_file >= 0)
	{
		write(log_file, ptr, len);
		/* the card's copy is whole however the power goes */
		fsync(log_file);
	}
	return result;
}

void wii_console_log_to_storage(void)
{
	if (!storage_mounted || log_file >= 0)
		return;
	log_file = open(DATA_ROOT "/debug.txt", O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (log_file < 0)
		return;
	fflush(stdout);
	console_copy = *devoptab_list[STD_OUT];
	console_write = console_copy.write_r;
	console_copy.write_r = console_and_log_write;
	devoptab_list[STD_OUT] = &console_copy;
	devoptab_list[STD_ERR] = &console_copy;
}

/* ---------- waitable objects

One lock guards every object's state; each object has its own condition. */

struct wii_waitable
{
	int kind;
	int manual_reset;
	int signaled;
	cond_t condition;
	/* mutexes */
	lwp_t owner;
	unsigned long recursion;
	/* threads */
	lwp_t thread;
	unsigned long (*proc)(void *);
	void *parameter;
	int suspended;
	unsigned long exit_code;
};

static mutex_t lock = LWP_MUTEX_NULL;

static void lock_state(void)
{
	/* the first object is made by main before any thread exists */
	if (lock == LWP_MUTEX_NULL)
		LWP_MutexInit(&lock, false);
	LWP_MutexLock(lock);
}

struct wii_waitable *wii_waitable_new(int kind, int manual_reset, int signaled)
{
	struct wii_waitable *waitable = calloc(1, sizeof(*waitable));

	if (!waitable)
		return NULL;
	lock_state();
	waitable->kind = kind;
	waitable->manual_reset = manual_reset;
	waitable->signaled = signaled;
	waitable->owner = LWP_THREAD_NULL;
	LWP_CondInit(&waitable->condition);
	LWP_MutexUnlock(lock);
	return waitable;
}

void wii_waitable_delete(struct wii_waitable *waitable)
{
	/* a thread's record is still read by the thread itself until it ends */
	if (waitable->kind == _wii_waitable_thread && !waitable->signaled)
		return;
	LWP_CondDestroy(waitable->condition);
	free(waitable);
}

/* with the lock held */
static int try_acquire(struct wii_waitable *waitable)
{
	lwp_t self = LWP_GetSelf();

	switch (waitable->kind)
	{
	case _wii_waitable_mutex:
		if (waitable->owner == LWP_THREAD_NULL || waitable->owner == self)
		{
			waitable->owner = self;
			waitable->recursion++;
			return 1;
		}
		return 0;
	case _wii_waitable_event:
		if (!waitable->signaled)
			return 0;
		if (!waitable->manual_reset)
			waitable->signaled = 0;
		return 1;
	default:
		return waitable->signaled;
	}
}

int wii_waitable_wait(struct wii_waitable *waitable, unsigned long milliseconds)
{
	int acquired;

	lock_state();
	if (milliseconds == WII_INFINITE)
	{
		while (!(acquired = try_acquire(waitable)))
			LWP_CondWait(waitable->condition, lock);
	}
	else
	{
		u64 deadline = gettime() + millisecs_to_ticks(milliseconds);

		while (!(acquired = try_acquire(waitable)))
		{
			u64 now = gettime();
			struct timespec left;
			u64 left_us;

			if (now >= deadline)
				break;
			left_us = ticks_to_microsecs(deadline - now);
			left.tv_sec = left_us / 1000000;
			left.tv_nsec = (left_us % 1000000) * 1000;
			LWP_CondTimedWait(waitable->condition, lock, &left);
		}
	}
	LWP_MutexUnlock(lock);
	return acquired;
}

void wii_event_set(struct wii_waitable *event)
{
	lock_state();
	event->signaled = 1;
	LWP_CondBroadcast(event->condition);
	LWP_MutexUnlock(lock);
}

void wii_event_reset(struct wii_waitable *event)
{
	lock_state();
	event->signaled = 0;
	LWP_MutexUnlock(lock);
}

int wii_mutex_release(struct wii_waitable *mutex)
{
	int owned;

	lock_state();
	owned = mutex->owner == LWP_GetSelf() && mutex->recursion;
	if (owned && !--mutex->recursion)
	{
		mutex->owner = LWP_THREAD_NULL;
		LWP_CondBroadcast(mutex->condition);
	}
	LWP_MutexUnlock(lock);
	return owned;
}

static void *thread_entry(void *context)
{
	struct wii_waitable *thread = context;
	unsigned long code;

	lock_state();
	while (thread->suspended)
		LWP_CondWait(thread->condition, lock);
	LWP_MutexUnlock(lock);

	code = thread->proc(thread->parameter);

	lock_state();
	thread->exit_code = code;
	thread->signaled = 1;
	LWP_CondBroadcast(thread->condition);
	LWP_MutexUnlock(lock);
	return NULL;
}

struct wii_waitable *wii_thread_start(unsigned long (*proc)(void *), void *parameter,
	unsigned long stack_size, int suspended)
{
	struct wii_waitable *thread = wii_waitable_new(_wii_waitable_thread, 1, 0);

	if (!thread)
		return NULL;
	thread->proc = proc;
	thread->parameter = parameter;
	thread->suspended = suspended;
	if (LWP_CreateThread(&thread->thread, thread_entry, thread, NULL,
		stack_size > MINIMUM_STACK_SIZE ? stack_size : MINIMUM_STACK_SIZE, THREAD_PRIORITY) < 0)
	{
		LWP_CondDestroy(thread->condition);
		free(thread);
		return NULL;
	}
	return thread;
}

void wii_thread_resume(struct wii_waitable *thread)
{
	lock_state();
	thread->suspended = 0;
	LWP_CondBroadcast(thread->condition);
	LWP_MutexUnlock(lock);
}

int wii_thread_exit_code(struct wii_waitable *thread, unsigned long *code)
{
	int ended;

	lock_state();
	ended = thread->signaled;
	if (ended)
		*code = thread->exit_code;
	LWP_MutexUnlock(lock);
	return ended;
}

unsigned long wii_thread_self(void)
{
	return (unsigned long)LWP_GetSelf();
}
