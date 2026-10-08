/*
WII_OS.H

What the XDK half of the Wii layer (wii_xbox.c, wii_crt.c) needs from libogc,
in plain C types: the XDK's headers and libogc's cannot be read by one unit
(BOOL, u32 and others clash), so wii_os.c reads libogc and this is the seam.
*/

#ifndef __WII_OS_H
#define __WII_OS_H

/* ---------- storage */

/* mount the SD card and make the data root, sd:/halo; FALSE with no card */
int wii_storage_mount(void);

/* the data root (the Xbox's d:\), or NULL with no card */
const char *wii_data_root(void);

/* copy all console output to <data root>/debug.txt from now on */
void wii_console_log_to_storage(void);

/* what <sys/stat.h> answers, which the XDK half cannot include: the port's
own sys/stat.h is MSVC's */
enum
{
	_wii_path_missing = -1,
	_wii_path_file,
	_wii_path_directory,
};

int wii_path_kind(const char *path);
/* -1 when it cannot be had */
long wii_file_size(int file);
/* 0, or -1 with errno set */
int wii_make_directory(const char *path);

/* what stat answers about a path or an open file: size in bytes and the
last write as seconds since 1970; 0, or -1 with errno set */
struct wii_path_information
{
	int kind;
	unsigned long size;
	long long modified;
};

int wii_path_information(const char *path, struct wii_path_information *information);
int wii_file_information(int file, struct wii_path_information *information);

/* the card's free and total bytes; 0, or -1 with errno set */
int wii_disk_space(const char *path, unsigned long long *free_bytes, unsigned long long *total_bytes);

/* a directory's entries one by one, without "." and ".." told apart: NULL
with errno set when it cannot be opened, and 0 from next at the end */
struct wii_directory;

struct wii_directory *wii_directory_open(const char *path);
int wii_directory_next(struct wii_directory *directory, char *name, unsigned long name_size,
	struct wii_path_information *information);
void wii_directory_close(struct wii_directory *directory);

/* ---------- waitable objects: events, mutexes and threads */

struct wii_waitable;

enum
{
	_wii_waitable_event,
	_wii_waitable_mutex,
	_wii_waitable_thread,
};

struct wii_waitable *wii_waitable_new(int kind, int manual_reset, int signaled);
void wii_waitable_delete(struct wii_waitable *waitable);

/* TRUE when the object was had (an event's or a thread's signal, a mutex's
ownership) within the time; WII_INFINITE waits for ever */
#define WII_INFINITE 0xFFFFFFFFUL
int wii_waitable_wait(struct wii_waitable *waitable, unsigned long milliseconds);

void wii_event_set(struct wii_waitable *event);
void wii_event_reset(struct wii_waitable *event);
/* FALSE when the caller does not own it */
int wii_mutex_release(struct wii_waitable *mutex);

/* a thread running proc(parameter), held until wii_thread_resume when
suspended; the waitable is signalled when proc returns */
struct wii_waitable *wii_thread_start(unsigned long (*proc)(void *), void *parameter,
	unsigned long stack_size, int suspended);
void wii_thread_resume(struct wii_waitable *thread);
/* the calling thread, as a number */
unsigned long wii_thread_self(void);
/* FALSE while it runs */
int wii_thread_exit_code(struct wii_waitable *thread, unsigned long *code);

#endif /* __WII_OS_H */
