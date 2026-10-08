/*
WII_CRT.C

The Microsoft C runtime names the game calls that newlib does not have, and
the Xbox-path fopen family the game's headers redirect to
(port/linux/include/stdio.h). The counterpart of port/linux/src/msvc_crt.c,
cut down to what the Wii needs so far.
*/

#include "platform.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* ---------- strings */

int _stricmp(const char *string1, const char *string2)
{
	for (;; string1++, string2++)
	{
		int c1 = tolower((unsigned char)*string1);
		int c2 = tolower((unsigned char)*string2);

		if (c1 != c2 || !c1)
			return c1 - c2;
	}
}

int _strnicmp(const char *string1, const char *string2, size_t count)
{
	for (; count; count--, string1++, string2++)
	{
		int c1 = tolower((unsigned char)*string1);
		int c2 = tolower((unsigned char)*string2);

		if (c1 != c2 || !c1)
			return c1 - c2;
	}
	return 0;
}

/* ---------- floating point and compiler intrinsics */

/* the x87 control word: the Broadway FPU has none to set, and the game only
asks for the default (round to nearest, exceptions masked) */
unsigned int _control87(unsigned int new_value, unsigned int mask)
{
	return 0x0009001f;
}

void _ReadWriteBarrier(void)
{
	__asm__ volatile("sync" ::: "memory");
}

/* ---------- printf with MSVC length modifiers (as msvc_crt.c) */

/* rewrite %I64 as %ll and drop %I32 */
static const char *translate_format(const char *format, char *buffer, size_t size)
{
	const char *cursor;
	size_t length = 0;

	if (!strstr(format, "I64") && !strstr(format, "I32"))
		return format;
	for (cursor = format; *cursor && length + 3 < size; cursor++)
	{
		buffer[length++] = *cursor;
		if (*cursor != '%')
			continue;
		if (cursor[1] == '%')
		{
			buffer[length++] = *++cursor;
			continue;
		}
		while (cursor[1] && strchr("-+ #0123456789.*", cursor[1]) && length + 3 < size)
			buffer[length++] = *++cursor;
		if (cursor[1] == 'I' && cursor[2] == '6' && cursor[3] == '4')
		{
			buffer[length++] = 'l';
			buffer[length++] = 'l';
			cursor += 3;
		}
		else if (cursor[1] == 'I' && cursor[2] == '3' && cursor[3] == '2')
		{
			cursor += 3;
		}
	}
	buffer[length] = '\0';
	return buffer;
}

int halo_linux_vsnprintf(char *buffer, size_t count, const char *format, va_list arguments)
{
	char translated_format[1024];

	return vsnprintf(buffer, count, translate_format(format, translated_format, sizeof(translated_format)), arguments);
}

int halo_linux_snprintf(char *buffer, size_t count, const char *format, ...)
{
	va_list arguments;
	int result;

	va_start(arguments, format);
	result = halo_linux_vsnprintf(buffer, count, format, arguments);
	va_end(arguments);
	return result;
}

int halo_linux_vsprintf(char *buffer, const char *format, va_list arguments)
{
	char translated_format[1024];

	return vsprintf(buffer, translate_format(format, translated_format, sizeof(translated_format)), arguments);
}

int halo_linux_sprintf(char *buffer, const char *format, ...)
{
	va_list arguments;
	int result;

	va_start(arguments, format);
	result = halo_linux_vsprintf(buffer, format, arguments);
	va_end(arguments);
	return result;
}

int halo_linux_vfprintf(FILE *stream, const char *format, va_list arguments)
{
	char translated_format[1024];

	return vfprintf(stream, translate_format(format, translated_format, sizeof(translated_format)), arguments);
}

int halo_linux_fprintf(FILE *stream, const char *format, ...)
{
	va_list arguments;
	int result;

	va_start(arguments, format);
	result = halo_linux_vfprintf(stream, format, arguments);
	va_end(arguments);
	return result;
}

int halo_linux_vprintf(const char *format, va_list arguments)
{
	return halo_linux_vfprintf(stdout, format, arguments);
}

int halo_linux_printf(const char *format, ...)
{
	va_list arguments;
	int result;

	va_start(arguments, format);
	result = halo_linux_vfprintf(stdout, format, arguments);
	va_end(arguments);
	return result;
}

/* ---------- files by Xbox path

The game's log, d:\\debug.txt, is the console (which wii_os.c copies to the
card's debug.txt); every other path goes to the SD card as CreateFile's do
(wii_xbox.c). */

BOOL wii_translate_path(const char *xbox_path, char *path, size_t size);

FILE *halo_linux_fopen(const char *path, const char *mode)
{
	char card_path[1024];

	if (!_stricmp(path, "d:\\debug.txt"))
		return stdout;
	if (!wii_translate_path(path, card_path, sizeof(card_path)))
	{
		errno = ENOENT;
		return NULL;
	}
	return fopen(card_path, mode);
}

int halo_linux_remove(const char *path)
{
	char card_path[1024];

	if (!wii_translate_path(path, card_path, sizeof(card_path)))
	{
		errno = ENOENT;
		return -1;
	}
	return remove(card_path);
}

int halo_linux_open(const char *path, int flags, ...)
{
	char card_path[1024];
	int mode = 0666;

	if (flags & O_CREAT)
	{
		va_list arguments;

		va_start(arguments, flags);
		mode = va_arg(arguments, int);
		va_end(arguments);
	}
	if (!wii_translate_path(path, card_path, sizeof(card_path)))
	{
		errno = ENOENT;
		return -1;
	}
	return open(card_path, flags, mode);
}

int _close(int handle)
{
	return close(handle);
}

int _write(int handle, const void *buffer, unsigned int count)
{
	return write(handle, buffer, count);
}

/* MSVC's _fstat, whose struct _stat is not newlib's (which declares its own
_fstat), for libtiff's file size; nothing reaches it yet */
int msvc_fstat(int handle, void *buffer) __asm__("_fstat");

int msvc_fstat(int handle, void *buffer)
{
	errno = EBADF;
	return -1;
}
