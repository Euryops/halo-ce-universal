/*
LIBEGL.C

libEGL.dll for ANGLE, which the build for graphics hardware without OpenGL
4.5 makes next to the game (configure.py --gles, ../README.md). SDL3 gets an
OpenGL ES context through the EGL functions of this library.

ANGLE's code is all in its libGLESv2.dll, which has each EGL function under
the name EGL_...; ANGLE's own libEGL.dll only passes the standard names to
those. This one does the same with no code: each name is exported as a
forwarder, which Windows resolves in libGLESv2.dll. Thus libGLESv2.dll is
the only file of ANGLE that the game needs, and a distribution of ANGLE that
has no libEGL.dll will do.
*/

#define FORWARD(name) __pragma(comment(linker, "/export:egl" #name "=libGLESv2.EGL_" #name))

/* EGL 1.0 */
FORWARD(ChooseConfig)
FORWARD(CopyBuffers)
FORWARD(CreateContext)
FORWARD(CreatePbufferSurface)
FORWARD(CreatePixmapSurface)
FORWARD(CreateWindowSurface)
FORWARD(DestroyContext)
FORWARD(DestroySurface)
FORWARD(GetConfigAttrib)
FORWARD(GetConfigs)
FORWARD(GetCurrentDisplay)
FORWARD(GetCurrentSurface)
FORWARD(GetDisplay)
FORWARD(GetError)
FORWARD(GetProcAddress)
FORWARD(Initialize)
FORWARD(MakeCurrent)
FORWARD(QueryContext)
FORWARD(QueryString)
FORWARD(QuerySurface)
FORWARD(SwapBuffers)
FORWARD(Terminate)
FORWARD(WaitGL)
FORWARD(WaitNative)

/* EGL 1.1 */
FORWARD(BindTexImage)
FORWARD(ReleaseTexImage)
FORWARD(SurfaceAttrib)
FORWARD(SwapInterval)

/* EGL 1.2 */
FORWARD(BindAPI)
FORWARD(CreatePbufferFromClientBuffer)
FORWARD(QueryAPI)
FORWARD(ReleaseThread)
FORWARD(WaitClient)

/* EGL 1.4 */
FORWARD(GetCurrentContext)

/* EGL 1.5 */
FORWARD(ClientWaitSync)
FORWARD(CreateImage)
FORWARD(CreatePlatformPixmapSurface)
FORWARD(CreatePlatformWindowSurface)
FORWARD(CreateSync)
FORWARD(DestroyImage)
FORWARD(DestroySync)
FORWARD(GetPlatformDisplay)
FORWARD(GetSyncAttrib)
FORWARD(WaitSync)

/* extensions */
FORWARD(ClientWaitSyncKHR)
FORWARD(CreateDeviceANGLE)
FORWARD(CreateImageKHR)
FORWARD(CreatePlatformPixmapSurfaceEXT)
FORWARD(CreatePlatformWindowSurfaceEXT)
FORWARD(CreateSyncKHR)
FORWARD(DebugMessageControlKHR)
FORWARD(DestroyImageKHR)
FORWARD(DestroySyncKHR)
FORWARD(GetPlatformDisplayEXT)
FORWARD(GetSyncAttribKHR)
FORWARD(LabelObjectKHR)
FORWARD(PostSubBufferNV)
FORWARD(ProgramCacheGetAttribANGLE)
FORWARD(ProgramCachePopulateANGLE)
FORWARD(ProgramCacheQueryANGLE)
FORWARD(ProgramCacheResizeANGLE)
FORWARD(QueryDebugKHR)
FORWARD(QueryDeviceAttribEXT)
FORWARD(QueryDeviceStringEXT)
FORWARD(QueryDisplayAttribANGLE)
FORWARD(QueryDisplayAttribEXT)
FORWARD(QueryStringiANGLE)
FORWARD(QuerySurfacePointerANGLE)
FORWARD(ReleaseDeviceANGLE)
FORWARD(SetBlobCacheFuncsANDROID)
FORWARD(SetDamageRegionKHR)
FORWARD(SignalSyncKHR)
FORWARD(SwapBuffersWithDamageKHR)
FORWARD(WaitSyncKHR)
