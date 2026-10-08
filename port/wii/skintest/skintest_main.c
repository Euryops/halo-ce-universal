/*
SKINTEST_MAIN.C

The entry point of skintest.dol, the skinned characters and vertex-program
effects drawn by the GX device (skintest_scene.c). It brings up the video as
wii_main.c does, gives the scene a clock, and keeps the log: on the screen
until the device takes it, and written to sd:/skintest.txt at the end (and
as each line comes, while the card is there), where a run in Dolphin with
a card folder leaves it to read. This file sees libogc and not the XDK.
*/

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gccore.h>
#include <fat.h>
#include <ogc/lwp_watchdog.h>

void skintest_run(void);

static void *framebuffer;
static GXRModeObj *video_mode;
static FILE *log_file;

void *wii_video_mode(void)
{
	return video_mode;
}

unsigned long long skintest_ticks(void)
{
	return gettime();
}

double skintest_ticks_to_microseconds(unsigned long long ticks)
{
	return (double)ticks / (TB_TIMER_CLOCK / 1000.0);
}

void platform_log(const char *format, ...)
{
	va_list arguments;

	fputs("skintest: ", stdout);
	va_start(arguments, format);
	vprintf(format, arguments);
	va_end(arguments);
	fputs("\n", stdout);
	if (log_file)
	{
		va_start(arguments, format);
		vfprintf(log_file, format, arguments);
		va_end(arguments);
		fputs("\n", log_file);
		fflush(log_file);
	}
}

void platform_unimplemented(const char *name)
{
	platform_log("not implemented: %s", name);
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	VIDEO_Init();
	video_mode = VIDEO_GetPreferredMode(NULL);
	framebuffer = MEM_K0_TO_K1(SYS_AllocateFramebuffer(video_mode));
	console_init(framebuffer, 16, 16, video_mode->fbWidth, video_mode->xfbHeight,
		video_mode->fbWidth * VI_DISPLAY_PIX_SZ);
	VIDEO_Configure(video_mode);
	VIDEO_SetNextFramebuffer(framebuffer);
	VIDEO_SetBlack(FALSE);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if (video_mode->viTVMode & VI_NON_INTERLACE)
		VIDEO_WaitVSync();
	LWP_SetThreadPriority(LWP_GetSelf(), 64);
	if (fatInitDefault())
		log_file = fopen("sd:/skintest.txt", "w");
	platform_log("the log is %s", log_file ? "on the card, sd:/skintest.txt" : "not on a card (none)");

	skintest_run();

	if (log_file)
		fclose(log_file);
	log_file = NULL;
	for (;;)
		VIDEO_WaitVSync();
	return 0;
}
