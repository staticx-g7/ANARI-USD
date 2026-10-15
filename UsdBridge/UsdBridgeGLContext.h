// Copyright 2020 The Khronos Group

#ifndef UsdBridgeGLContext_h
#define UsdBridgeGLContext_h

// Ensures an offscreen OpenGL context is available on the current thread.

// Thread safety: must be called from the thread that will do rendering.

bool EnsureOpenGLContext();

#endif
