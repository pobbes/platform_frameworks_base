/* 
**
** Copyright 2007 The Android Open Source Project
**
** Licensed under the Apache License Version 2.0(the "License"); 
** you may not use this file except in compliance with the License. 
** You may obtain a copy of the License at 
**
**     http://www.apache.org/licenses/LICENSE-2.0 
**
** Unless required by applicable law or agreed to in writing software 
** distributed under the License is distributed on an "AS IS" BASIS 
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND either express or implied. 
** See the License for the specific language governing permissions and 
** limitations under the License.
*/

#define LOG_TAG "FramebufferNativeWindow"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <cutils/log.h>
#include <cutils/atomic.h>
#include <utils/threads.h>

#include <ui/SurfaceComposerClient.h>
#include <ui/Rect.h>
#include <ui/FramebufferNativeWindow.h>

#include <EGL/egl.h>

#include <pixelflinger/format.h>
#include <pixelflinger/pixelflinger.h>

#include <hardware/hardware.h>
#include <hardware/gralloc.h>

#include <private/ui/android_natives_priv.h>

// ----------------------------------------------------------------------------
namespace android {
// ----------------------------------------------------------------------------

class NativeBuffer 
    : public EGLNativeBase<
        android_native_buffer_t, 
        NativeBuffer, 
        LightRefBase<NativeBuffer> >
{
public:
    NativeBuffer(int w, int h, int f, int u) : BASE() {
        android_native_buffer_t::width  = w;
        android_native_buffer_t::height = h;
        android_native_buffer_t::format = f;
        android_native_buffer_t::usage  = u;
    }
private:
    friend class LightRefBase<NativeBuffer>;    
    ~NativeBuffer() { }; // this class cannot be overloaded
};


/*
 * This implements the (main) framebuffer management. This class is used
 * mostly by SurfaceFlinger, but also by command line GL application.
 * 
 * In fact this is an implementation of android_native_window_t on top of
 * the framebuffer.
 * 
 * Currently it is pretty simple, it manages only two buffers (the front and 
 * back buffer).
 * 
 */

FramebufferNativeWindow::FramebufferNativeWindow() 
    : BASE(), fbDev(0), grDev(0), mUpdateOnDemand(false)
{
    hw_module_t const* module;
    if (hw_get_module(GRALLOC_HARDWARE_MODULE_ID, &module) == 0) {
        int stride;
        framebuffer_open(module, &fbDev);
        gralloc_open(module, &grDev);
        int err;

        
        mUpdateOnDemand = (fbDev->setUpdateRect != 0);
        
        // initialize the buffer FIFO
        mNumBuffers = 2;
        mNumFreeBuffers = 2;
        mBufferHead = mNumBuffers-1;
        buffers[0] = new NativeBuffer(
                fbDev->width, fbDev->height, fbDev->format, GRALLOC_USAGE_HW_FB);
        buffers[1] = new NativeBuffer(
                fbDev->width, fbDev->height, fbDev->format, GRALLOC_USAGE_HW_FB);
        
        err = grDev->alloc(grDev,
                fbDev->width, fbDev->height, fbDev->format, 
                GRALLOC_USAGE_HW_FB, &buffers[0]->handle, &buffers[0]->stride);

        LOGE_IF(err, "fb buffer 0 allocation failed w=%d, h=%d, err=%s",
                fbDev->width, fbDev->height, strerror(-err));

        err = grDev->alloc(grDev,
                fbDev->width, fbDev->height, fbDev->format, 
                GRALLOC_USAGE_HW_FB, &buffers[1]->handle, &buffers[1]->stride);

        LOGE_IF(err, "fb buffer 1 allocation failed w=%d, h=%d, err=%s",
                fbDev->width, fbDev->height, strerror(-err));
    }

    const_cast<uint32_t&>(android_native_window_t::flags) = fbDev->flags; 
    const_cast<float&>(android_native_window_t::xdpi) = fbDev->xdpi;
    const_cast<float&>(android_native_window_t::ydpi) = fbDev->ydpi;
    const_cast<int&>(android_native_window_t::minSwapInterval) = 
        fbDev->minSwapInterval;
    const_cast<int&>(android_native_window_t::maxSwapInterval) = 
        fbDev->maxSwapInterval;

    android_native_window_t::setSwapInterval = setSwapInterval;
    android_native_window_t::dequeueBuffer = dequeueBuffer;
    android_native_window_t::lockBuffer = lockBuffer;
    android_native_window_t::queueBuffer = queueBuffer;
    android_native_window_t::query = query;
}

FramebufferNativeWindow::~FramebufferNativeWindow() {
    grDev->free(grDev, buffers[0]->handle);
    grDev->free(grDev, buffers[1]->handle);
    gralloc_close(grDev);
    framebuffer_close(fbDev);
}

status_t FramebufferNativeWindow::setUpdateRectangle(const Rect& r) 
{
    if (!mUpdateOnDemand) {
        return INVALID_OPERATION;
    }
    return fbDev->setUpdateRect(fbDev, r.left, r.top, r.width(), r.height());
}

int FramebufferNativeWindow::setSwapInterval(
        android_native_window_t* window, int interval) 
{
    framebuffer_device_t* fb = getSelf(window)->fbDev;
    return fb->setSwapInterval(fb, interval);
}

int FramebufferNativeWindow::dequeueBuffer(android_native_window_t* window, 
        android_native_buffer_t** buffer)
{
    FramebufferNativeWindow* self = getSelf(window);
    Mutex::Autolock _l(self->mutex);
    framebuffer_device_t* fb = self->fbDev;

    // wait for a free buffer
    while (!self->mNumFreeBuffers) {
        self->mCondition.wait(self->mutex);
    }
    // get this buffer
    self->mNumFreeBuffers--;
    int index = self->mBufferHead++;
    if (self->mBufferHead >= self->mNumBuffers)
        self->mBufferHead = 0;

    *buffer = self->buffers[index].get();

    return 0;
}

int FramebufferNativeWindow::lockBuffer(android_native_window_t* window, 
        android_native_buffer_t* buffer)
{
    FramebufferNativeWindow* self = getSelf(window);
    Mutex::Autolock _l(self->mutex);

    // wait that the buffer we're locking is not front anymore
    while (self->front == buffer) {
        self->mCondition.wait(self->mutex);
    }

    return NO_ERROR;
}

int FramebufferNativeWindow::queueBuffer(android_native_window_t* window, 
        android_native_buffer_t* buffer)
{
    FramebufferNativeWindow* self = getSelf(window);
    Mutex::Autolock _l(self->mutex);
    framebuffer_device_t* fb = self->fbDev;
    buffer_handle_t handle = static_cast<NativeBuffer*>(buffer)->handle;
    int res = fb->post(fb, handle);
    self->front = static_cast<NativeBuffer*>(buffer);
    self->mNumFreeBuffers++;
    self->mCondition.broadcast();
    return res;
}

int FramebufferNativeWindow::query(android_native_window_t* window,
        int what, int* value) 
{
    FramebufferNativeWindow* self = getSelf(window);
    Mutex::Autolock _l(self->mutex);
    framebuffer_device_t* fb = self->fbDev;
    switch (what) {
        case NATIVE_WINDOW_WIDTH:
            *value = fb->width;
            return NO_ERROR;
        case NATIVE_WINDOW_HEIGHT:
            *value = fb->height;
            return NO_ERROR;
    }
    return BAD_VALUE;
}

// ----------------------------------------------------------------------------
}; // namespace android
// ----------------------------------------------------------------------------


EGLNativeWindowType android_createDisplaySurface(void)
{
    return new android::FramebufferNativeWindow();
}

