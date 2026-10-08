/*
GXTEST_MAIN.C

The entry point of gxtest.dol, the GX device's test scene (gxtest_scene.c).
It brings up the video as wii_main.c does, gives the device the few things
the game's platform layer would (the video mode, the log), and runs the
scene. This file sees libogc and not the XDK.
*/

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <gccore.h>

void gxtest_run(void);

static void *framebuffer;
static GXRModeObj *video_mode;

void *wii_video_mode(void)
{
	return video_mode;
}

void platform_log(const char *format, ...)
{
	va_list arguments;

	fputs("gxtest: ", stdout);
	va_start(arguments, format);
	vprintf(format, arguments);
	va_end(arguments);
	fputs("\n", stdout);
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

	gxtest_run();

	for (;;)
		VIDEO_WaitVSync();
	return 0;
}
