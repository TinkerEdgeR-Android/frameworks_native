/*
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 0
#undef LOG_TAG
#define LOG_TAG "BufferLayer"
#define ATRACE_TAG ATRACE_TAG_GRAPHICS

#include "BufferLayer.h"
#include "Colorizer.h"
#include "DisplayDevice.h"
#include "LayerRejecter.h"
#include "clz.h"

#include "RenderEngine/RenderEngine.h"

#include <gui/BufferItem.h>
#include <gui/BufferQueue.h>
#include <gui/LayerDebugInfo.h>
#include <gui/Surface.h>

#include <ui/DebugUtils.h>

#include <utils/Errors.h>
#include <utils/Log.h>
#include <utils/NativeHandle.h>
#include <utils/StopWatch.h>
#include <utils/Trace.h>

#include <cutils/compiler.h>
#include <cutils/native_handle.h>
#include <cutils/properties.h>

#include <math.h>
#include <stdlib.h>
#include <mutex>

static const gralloc_module_t *g_gralloc = NULL;

#if RK_NV12_10_TO_NV12_BY_RGA
#define UN_NEED_GL
#include <RockchipRga.h>
#endif

#if (RK_NV12_10_TO_NV12_BY_NENO | RK_HDR)
#include <dlfcn.h>
#endif

namespace android {

#if (RK_NV12_10_TO_NV12_BY_RGA | RK_NV12_10_TO_NV12_BY_NENO | RK_HDR)
typedef struct
{
     sp<GraphicBuffer> yuvTexBuffer;
     EGLImageKHR img;
} TexBufferImag;

#define TexBufferMax  2
#define TexKey 0x524f434b
static TexBufferImag yuvTeximg[TexBufferMax] = {{NULL,EGL_NO_IMAGE_KHR},{NULL,EGL_NO_IMAGE_KHR}};
#endif

BufferLayer::BufferLayer(SurfaceFlinger* flinger, const sp<Client>& client, const String8& name,
                         uint32_t w, uint32_t h, uint32_t flags)
      : Layer(flinger, client, name, w, h, flags),
        mConsumer(nullptr),
        mTextureName(UINT32_MAX),
        mFormat(PIXEL_FORMAT_NONE),
        mCurrentScalingMode(NATIVE_WINDOW_SCALING_MODE_FREEZE),
        mBufferLatched(false),
        mPreviousFrameNumber(0),
        mUpdateTexImageFailed(false),
        mRefreshPending(false) {
    ALOGV("Creating Layer %s", name.string());

    mTextureName = mFlinger->getNewTexture();
    mTexture.init(Texture::TEXTURE_EXTERNAL, mTextureName);

    if (flags & ISurfaceComposerClient::eNonPremultiplied) mPremultipliedAlpha = false;

    mCurrentState.requested = mCurrentState.active;

    // drawing state & current state are identical
    mDrawingState = mCurrentState;

    // Use rk ashmem -------
    int ret = hw_get_module(GRALLOC_HARDWARE_MODULE_ID,
                      (const hw_module_t **)&g_gralloc);
    if (ret) {
        ALOGE("Failed to open gralloc module %d", ret);
    }
    // Use rk ashmem -------
}

BufferLayer::~BufferLayer() {
    mFlinger->deleteTextureAsync(mTextureName);

    if (!getBE().mHwcLayers.empty()) {
        ALOGE("Found stale hardware composer layers when destroying "
              "surface flinger layer %s",
              mName.string());
        destroyAllHwcLayers();
    }
}

// Use rk ashmem -------
int BufferLayer::get_handle_displayStereo(buffer_handle_t hnd)
{
    int ret = 0;
    int op = GRALLOC_MODULE_PERFORM_GET_RK_ASHMEM;
    struct rk_ashmem_t rk_ashmem;

    if(g_gralloc && g_gralloc->perform)
        ret = g_gralloc->perform(g_gralloc, op, hnd, &rk_ashmem);
    else
        ret = -EINVAL;

    if(ret != 0)
    {
        ALOGE("%s:cann't get value from gralloc", __FUNCTION__);
    }

    return rk_ashmem.displayStereo;
}

int BufferLayer::set_handle_displayStereo(buffer_handle_t hnd, int32_t displayStereo)
{
    int ret = 0;
    int op = GRALLOC_MODULE_PERFORM_GET_RK_ASHMEM;
    struct rk_ashmem_t rk_ashmem;

    if(g_gralloc && g_gralloc->perform)
        ret = g_gralloc->perform(g_gralloc, op, hnd, &rk_ashmem);
    else
        ret = -EINVAL;

    if(ret != 0)
    {
        ALOGE("%s:cann't get value from gralloc", __FUNCTION__);
        goto exit;
    }

    if(displayStereo != rk_ashmem.displayStereo)
    {
        op = GRALLOC_MODULE_PERFORM_SET_RK_ASHMEM;
        rk_ashmem.displayStereo = displayStereo;

        if(g_gralloc && g_gralloc->perform)
            ret = g_gralloc->perform(g_gralloc, op, hnd, &rk_ashmem);
        else
            ret = -EINVAL;

        if(ret != 0)
        {
            ALOGE("%s:cann't get value from gralloc", __FUNCTION__);
        }
    }

exit:
    return ret;
}

int BufferLayer::get_handle_alreadyStereo(buffer_handle_t hnd)
{
    int ret = 0;
    int op = GRALLOC_MODULE_PERFORM_GET_RK_ASHMEM;
    struct rk_ashmem_t rk_ashmem;

    if(g_gralloc && g_gralloc->perform)
        ret = g_gralloc->perform(g_gralloc, op, hnd, &rk_ashmem);
    else
        ret = -EINVAL;

    if(ret != 0)
    {
        ALOGE("%s:cann't get value from gralloc", __FUNCTION__);
    }

    return rk_ashmem.alreadyStereo;
}

int BufferLayer::set_handle_alreadyStereo(buffer_handle_t hnd, int32_t alreadyStereo)
{
    int ret = 0;
    int op = GRALLOC_MODULE_PERFORM_GET_RK_ASHMEM;
    struct rk_ashmem_t rk_ashmem;

    if(g_gralloc && g_gralloc->perform)
        ret = g_gralloc->perform(g_gralloc, op, hnd, &rk_ashmem);
    else
        ret = -EINVAL;

    if(ret != 0)
    {
        ALOGE("%s:cann't get value from gralloc", __FUNCTION__);
        goto exit;
    }

    if(alreadyStereo != rk_ashmem.alreadyStereo )
    {
        op = GRALLOC_MODULE_PERFORM_SET_RK_ASHMEM;
        rk_ashmem.alreadyStereo = alreadyStereo;

        if(g_gralloc && g_gralloc->perform)
            ret = g_gralloc->perform(g_gralloc, op, hnd, &rk_ashmem);
        else
            ret = -EINVAL;

        if(ret != 0)
        {
            ALOGE("%s:cann't get value from gralloc", __FUNCTION__);
        }
    }

exit:
    return ret;
}

int BufferLayer::get_handle_layername(buffer_handle_t hnd, char* layername, unsigned long len)
{
    int ret = 0;
    int op = GRALLOC_MODULE_PERFORM_GET_RK_ASHMEM;
    struct rk_ashmem_t rk_ashmem;
    unsigned long str_size;

    if(!layername)
        return -EINVAL;

    if(g_gralloc && g_gralloc->perform)
        ret = g_gralloc->perform(g_gralloc, op, hnd, &rk_ashmem);
    else
        ret = -EINVAL;

    if(ret != 0)
    {
        ALOGE("%s:cann't get value from gralloc", __FUNCTION__);
        goto exit;
    }

    str_size = strlen(rk_ashmem.LayerName)+1;
    str_size = str_size > len ? len:str_size;
    memcpy(layername,rk_ashmem.LayerName,str_size);

exit:
    return ret;
}

int BufferLayer::set_handle_layername(buffer_handle_t hnd, const char* layername)
{
    int ret = 0;
    int op = GRALLOC_MODULE_PERFORM_GET_RK_ASHMEM;
    struct rk_ashmem_t rk_ashmem;
    unsigned long str_size;

    if(!layername)
        return -EINVAL;

    if(g_gralloc && g_gralloc->perform)
        ret = g_gralloc->perform(g_gralloc, op, hnd, &rk_ashmem);
    else
        ret = -EINVAL;

    if(ret != 0)
    {
        ALOGE("%s:cann't get value from gralloc", __FUNCTION__);
        goto exit;
    }

    op = GRALLOC_MODULE_PERFORM_SET_RK_ASHMEM;

    str_size = strlen(layername)+1;
    str_size = str_size > sizeof(rk_ashmem.LayerName) ? sizeof(rk_ashmem.LayerName):str_size;
    memcpy(rk_ashmem.LayerName,layername,str_size);

    if(g_gralloc && g_gralloc->perform)
        ret = g_gralloc->perform(g_gralloc, op, hnd, &rk_ashmem);
    else
        ret = -EINVAL;

    if(ret != 0)
    {
        ALOGE("%s:cann't get value from gralloc", __FUNCTION__);
    }

exit:
    return ret;
}
// Use rk ashmem -------



void BufferLayer::useSurfaceDamage() {
    if (mFlinger->mForceFullDamage) {
        surfaceDamageRegion = Region::INVALID_REGION;
    } else {
        surfaceDamageRegion = mConsumer->getSurfaceDamage();
    }
}

void BufferLayer::useEmptyDamage() {
    surfaceDamageRegion.clear();
}

bool BufferLayer::isProtected() const {
    const sp<GraphicBuffer>& buffer(getBE().compositionInfo.mBuffer);
    return (buffer != 0) &&
            (buffer->getUsage() & GRALLOC_USAGE_PROTECTED);
}

bool BufferLayer::isVisible() const {
    return !(isHiddenByPolicy()) && getAlpha() > 0.0f &&
            (getBE().compositionInfo.mBuffer != nullptr ||
             getBE().compositionInfo.hwc.sidebandStream != nullptr);
}

bool BufferLayer::isFixedSize() const {
    return getEffectiveScalingMode() != NATIVE_WINDOW_SCALING_MODE_FREEZE;
}

status_t BufferLayer::setBuffers(uint32_t w, uint32_t h, PixelFormat format, uint32_t flags) {
    uint32_t const maxSurfaceDims =
            min(mFlinger->getMaxTextureSize(), mFlinger->getMaxViewportDims());

    // never allow a surface larger than what our underlying GL implementation
    // can handle.
    if ((uint32_t(w) > maxSurfaceDims) || (uint32_t(h) > maxSurfaceDims)) {
        ALOGE("dimensions too large %u x %u", uint32_t(w), uint32_t(h));
        return BAD_VALUE;
    }

    mFormat = format;

    mPotentialCursor = (flags & ISurfaceComposerClient::eCursorWindow) ? true : false;
    mProtectedByApp = (flags & ISurfaceComposerClient::eProtectedByApp) ? true : false;
    mCurrentOpacity = getOpacityForFormat(format);

    mConsumer->setDefaultBufferSize(w, h);
    mConsumer->setDefaultBufferFormat(format);
    mConsumer->setConsumerUsageBits(getEffectiveUsage(0));

    return NO_ERROR;
}

static constexpr mat4 inverseOrientation(uint32_t transform) {
    const mat4 flipH(-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0, 1);
    const mat4 flipV(1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1, 0, 0, 1, 0, 1);
    const mat4 rot90(0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 1);
    mat4 tr;

    if (transform & NATIVE_WINDOW_TRANSFORM_ROT_90) {
        tr = tr * rot90;
    }
    if (transform & NATIVE_WINDOW_TRANSFORM_FLIP_H) {
        tr = tr * flipH;
    }
    if (transform & NATIVE_WINDOW_TRANSFORM_FLIP_V) {
        tr = tr * flipV;
    }
    return inverse(tr);
}

#if (RK_NV12_10_TO_NV12_BY_RGA | RK_NV12_10_TO_NV12_BY_NENO | RK_HDR)
  /* print time macros. */
#define PRINT_TIME_START        \
    struct timeval tpend1, tpend2;\
    long usec1 = 0;\
    gettimeofday(&tpend1,NULL);\

#define PRINT_TIME_END(tag)        \
    gettimeofday(&tpend2,NULL);\
    usec1 = 1000*(tpend2.tv_sec - tpend1.tv_sec) + (tpend2.tv_usec- tpend1.tv_usec)/1000;\
    if (property_get_bool("sys.hwc.time", 1)) \
    ALOGD_IF(1,"%s use time=%ld ms",tag,usec1);\

#if RK_NV12_10_TO_NV12_BY_RGA
static int rgaCopyBit(sp<GraphicBuffer> src_buf, sp<GraphicBuffer> dst_buf, const Rect& rect)
{
    rga_info_t src, dst;
    int src_l,src_t,src_r,src_b,src_h,src_stride,src_format;
    int dst_l,dst_t,dst_r,dst_b,dst_h,dst_stride,dst_format;
    RockchipRga& mRga = RockchipRga::get();
    int ret = 0;

    memset(&src, 0, sizeof(rga_info_t));
    memset(&dst, 0, sizeof(rga_info_t));
    src.fd = -1;
    dst.fd = -1;

    src_stride = src_buf->getStride();
    src_format = src_buf->getPixelFormat();
    src_h = src_buf->getHeight();

    dst_stride = dst_buf->getStride();
    dst_format = dst_buf->getPixelFormat();
    dst_h = dst_buf->getHeight();

    dst_l = src_l = rect.left;
    dst_t = src_t = rect.top;
    dst_r = src_r = rect.right;
    dst_b = src_b = rect.bottom;
    rga_set_rect(&src.rect, src_l, src_t, src_r - src_l, src_b - src_t, src_stride, src_h, src_format);
    rga_set_rect(&dst.rect, dst_l, dst_t, dst_buf->getWidth(), dst_buf->getHeight(), dst_stride, dst_h, dst_format);

    src.hnd = src_buf->handle;
    dst.hnd = dst_buf->handle;
//  src.rotation = rga_transform;
//PRINT_TIME_START
    ret = mRga.RkRgaBlit(&src, &dst, NULL);
//PRINT_TIME_END("rgaCopyBit")
    if(ret) {
        ALOGD_IF(1,"rgaCopyBit  : src[x=%d,y=%d,w=%d,h=%d,ws=%d,hs=%d,format=0x%x],dst[x=%d,y=%d,w=%d,h=%d,ws=%d,hs=%d,format=0x%x]",
            src.rect.xoffset, src.rect.yoffset, src.rect.width, src.rect.height, src.rect.wstride, src.rect.hstride, src.rect.format,
            dst.rect.xoffset, dst.rect.yoffset, dst.rect.width, dst.rect.height, dst.rect.wstride, dst.rect.hstride, dst.rect.format);
        ALOGD_IF(1,"rgaCopyBit : src hnd=%p,dst hnd=%p, src_format=0x%x ==> dst_format=0x%x\n",
            (void*)src_buf->handle, (void*)(dst_buf->handle), src_format, dst_format);
        return ret;
    }

    return ret;
}
#endif
#endif


#if RK_HDR
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef signed char s8;
typedef signed short s16;
typedef signed int s32;
#define ARM_P010            0x4000000
#define HDRUSAGE            0x3000000
#define RK_XXX_PATH         "/system/lib64/librockchipxxx.so"
typedef void (*__rockchipxxx)(u8 *src, u8 *dst, int w, int h, int srcStride, int dstStride, int area);

static void* dso = NULL;
static __rockchipxxx rockchipxxx = NULL;

#elif RK_NV12_10_TO_NV12_BY_NENO

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef signed char s8;
typedef signed short s16;
typedef signed int s32;
#define RK_XXX_PATH         "/system/lib/librockchipxxx.so"
typedef void (*__rockchipxxx3288)(u8 *src, u8 *dst, int w, int h, int srcStride, int dstStride, int area);

static void* dso = NULL;
static __rockchipxxx3288 rockchipxxx3288 = NULL;
#endif

#define ALIGN(val, align) (((val) + ((align) - 1)) & ~((align) - 1))

/*
 * onDraw will draw the current layer onto the presentable buffer
 */
void BufferLayer::onDraw(const RenderArea& renderArea, const Region& clip,
                         bool useIdentityTransform) const {
    ATRACE_CALL();
    auto& engine(mFlinger->getRenderEngine());

    if (CC_UNLIKELY(getBE().compositionInfo.mBuffer == 0)) {
        // the texture has not been created yet, this Layer has
        // in fact never been drawn into. This happens frequently with
        // SurfaceView because the WindowManager can't know when the client
        // has drawn the first time.

        // If there is nothing under us, we paint the screen in black, otherwise
        // we just skip this update.

        // figure out if there is something below us
        Region under;
        bool finished = false;
        mFlinger->mDrawingState.traverseInZOrder([&](Layer* layer) {
            if (finished || layer == static_cast<BufferLayer const*>(this)) {
                finished = true;
                return;
            }
            under.orSelf(renderArea.getTransform().transform(layer->visibleRegion));
        });
        // if not everything below us is covered, we plug the holes!
        Region holes(clip.subtract(under));
        if (!holes.isEmpty()) {
            clearWithOpenGL(renderArea, 0, 0, 0, 1);
        }
        return;
    }

    // Bind the current buffer to the GL texture, and wait for it to be
    // ready for us to draw into.
    status_t err = NO_ERROR;

#if (RK_NV12_10_TO_NV12_BY_RGA | RK_NV12_10_TO_NV12_BY_NENO | RK_HDR)
    if(mActiveBuffer != NULL &&
       mActiveBuffer->getPixelFormat() == HAL_PIXEL_FORMAT_YCrCb_NV12_10 )
    {
#if RK_HDR
        const int yuvTexUsage = GraphicBuffer::USAGE_HW_TEXTURE | GRALLOC_USAGE_TO_USE_ARM_P010;
        const int yuvTexFormat = HAL_PIXEL_FORMAT_YCrCb_NV12_10;
#elif (RK_NV12_10_TO_NV12_BY_NENO | RK_NV12_10_TO_NV12_BY_RGA)
        const int yuvTexUsage = GraphicBuffer::USAGE_HW_TEXTURE;
        const int yuvTexFormat = HAL_PIXEL_FORMAT_YCrCb_NV12;
#endif

        static int yuvcnt;
        int yuvIndex ;
        yuvcnt ++;
        yuvIndex = yuvcnt%2;
#if (RK_HDR | RK_NV12_10_TO_NV12_BY_NENO)
        int src_l,src_t,src_r,src_b,src_stride;
        void *src_vaddr;
        void *dst_vaddr;
        src_l = mCurrentCrop.left;
        src_t = mCurrentCrop.top;
        src_r = mCurrentCrop.right;
        src_b = mCurrentCrop.bottom;
        src_stride = mActiveBuffer->getStride();
        uint32_t w = src_r - src_l;
#elif RK_NV12_10_TO_NV12_BY_RGA
        //Since rga cann't support scalet to bigger than 4096 limit to 4096
        uint32_t w = (mCurrentCrop.getWidth() + 31) & (~31);
#endif
        if((yuvTeximg[yuvIndex].yuvTexBuffer != NULL) &&
                   (yuvTeximg[yuvIndex].yuvTexBuffer->getWidth() != w ||
                    yuvTeximg[yuvIndex].yuvTexBuffer->getHeight() != mActiveBuffer->getHeight()))
        {
            yuvTeximg[yuvIndex].yuvTexBuffer = NULL;
            eglDestroyImageKHR(engine.getEGLDisplayBase(), yuvTeximg[yuvIndex].img);
        }
        if(yuvTeximg[yuvIndex].yuvTexBuffer == NULL)
        {
            yuvTeximg[yuvIndex].yuvTexBuffer = new GraphicBuffer(w, mActiveBuffer->getHeight(),
                                             yuvTexFormat, yuvTexUsage);
            EGLClientBuffer clientBuffer = (EGLClientBuffer)yuvTeximg[yuvIndex].yuvTexBuffer->getNativeBuffer();
            yuvTeximg[yuvIndex].img = eglCreateImageKHR(engine.getEGLDisplayBase(), EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
                                clientBuffer, 0);
            if (yuvTeximg[yuvIndex].img == EGL_NO_IMAGE_KHR) {
                ALOGE("%s:line=%d,EGL_NO_IMAGE_KHR",__FUNCTION__,__LINE__);
                return ;
            }
        }

#if (RK_HDR | RK_NV12_10_TO_NV12_BY_NENO)
        mActiveBuffer->lock(GRALLOC_USAGE_SW_READ_OFTEN,&src_vaddr);
        yuvTeximg[yuvIndex].yuvTexBuffer->lock(GRALLOC_USAGE_SW_WRITE_OFTEN,&dst_vaddr);

        //PRINT_TIME_START
        if(dso == NULL)
            dso = dlopen(RK_XXX_PATH, RTLD_NOW | RTLD_LOCAL);

        if (dso == 0) {
            ALOGE("rk_debug can't not find /system/lib64/librockchipxxx.so ! error=%s \n",
                dlerror());
            return;
        }
#if RK_HDR
        if(rockchipxxx == NULL)
            rockchipxxx = (__rockchipxxx)dlsym(dso, "_Z11rockchipxxxPhS_iiiii");
        if(rockchipxxx == NULL)
        {
            ALOGE("rk_debug can't not find target function in /system/lib64/librockchipxxx.so ! \n");
            dlclose(dso);
            return;
        }
		/* align w to 64 */
		w = ALIGN(w, 64);
		ALOGD("DEBUG_lb Stride=%d",yuvTeximg[yuvIndex].yuvTexBuffer->getStride());
		if(w <= yuvTeximg[yuvIndex].yuvTexBuffer->getStride()/2)
		{
		    rockchipxxx((u8*)src_vaddr, (u8*)dst_vaddr, w, src_b - src_t, src_stride, yuvTeximg[yuvIndex].yuvTexBuffer->getStride(), 0);
		}else
			ALOGE("%s(%d):unsupport resolution for 4k", __FUNCTION__, __LINE__);
#elif RK_NV12_10_TO_NV12_BY_NENO
        if(rockchipxxx3288 == NULL)
            rockchipxxx3288 = (__rockchipxxx3288)dlsym(dso, "_Z15rockchipxxx3288PhS_iiiii");
        if(rockchipxxx3288 == NULL)
        {
            ALOGE("rk_debug can't not find target function in /system/lib64/librockchipxxx.so ! \n");
            dlclose(dso);
            return;
        }
        rockchipxxx3288((u8*)src_vaddr, (u8*)dst_vaddr, src_r - src_l, src_b - src_t, src_stride, (src_r - src_l), 0);
#endif
        //PRINT_TIME_END("convert10to16_highbit_arm64_neon")
        ALOGD("src_vaddr=%p,dst_vaddr=%p,crop_w=%d,crop_h=%d,stride=%f, src_stride=%d,raw_w=%d,raw_h=%d",
                src_vaddr, dst_vaddr, src_r - src_l,src_b - src_t,
                (src_r - src_l)*1.25+64,src_stride,mActiveBuffer->getWidth(),mActiveBuffer->getHeight());
    //dump data
    static int i =0;
    char pro_value[PROPERTY_VALUE_MAX];

    property_get("sys.dump_out_neon",pro_value,0);
    if(i<10 && !strcmp(pro_value,"true"))
    {
        char data_name[100];

        sprintf(data_name,"/data/dump/dmlayer%d_%d_%d.bin", i,
                yuvTeximg[yuvIndex].yuvTexBuffer->getWidth(),yuvTeximg[yuvIndex].yuvTexBuffer->getHeight());
#if RK_HDR
        int n = yuvTeximg[yuvIndex].yuvTexBuffer->getHeight() * yuvTeximg[yuvIndex].yuvTexBuffer->getStride();
#else
        int n = yuvTeximg[yuvIndex].yuvTexBuffer->getHeight() * yuvTeximg[yuvIndex].yuvTexBuffer->getStride() * 1.5;
#endif
        ALOGD("dump %s size=%d", data_name, n );
        FILE *fp;
        if ((fp = fopen(data_name, "w+")) == NULL)
        {
            printf("can't open output.bin!!!!!\n");
        }
        fwrite(dst_vaddr, n, 1, fp);
        fclose(fp);
        i++;
    }
#elif RK_NV12_10_TO_NV12_BY_RGA
        rgaCopyBit(mActiveBuffer, yuvTeximg[yuvIndex].yuvTexBuffer, mCurrentCrop);
#endif

        if (yuvTeximg[yuvIndex].img == EGL_NO_IMAGE_KHR) {
            ALOGE("%s:line=%d,EGL_NO_IMAGE_KHR",__FUNCTION__,__LINE__);
            return ;
        }

        engine.bindyuvimg(yuvTeximg[yuvIndex].img, mTextureName);
        ALOGV("glEGLImageTargetTexture2DOES,index=%d,img=%p,name=%d",yuvIndex,yuvTeximg[yuvIndex].img,mTextureName);
    }
    else
#endif
    {
        err = mConsumer->bindTextureImage();
    }

    if (err != NO_ERROR) {
        ALOGW("onDraw: bindTextureImage failed (err=%d)", err);
        // Go ahead and draw the buffer anyway; no matter what we do the screen
        // is probably going to have something visibly wrong.
    }

    bool blackOutLayer = isProtected() || (isSecure() && !renderArea.isSecure());

    if (!blackOutLayer) {
        // TODO: we could be more subtle with isFixedSize()
        const bool useFiltering = needsFiltering(renderArea) || isFixedSize();

        // Query the texture matrix given our current filtering mode.
        float textureMatrix[16];
        mConsumer->setFilteringEnabled(useFiltering);
        mConsumer->getTransformMatrix(textureMatrix);

        if (getTransformToDisplayInverse()) {
            /*
             * the code below applies the primary display's inverse transform to
             * the texture transform
             */
            uint32_t transform = DisplayDevice::getPrimaryDisplayOrientationTransform();
            mat4 tr = inverseOrientation(transform);

            /**
             * TODO(b/36727915): This is basically a hack.
             *
             * Ensure that regardless of the parent transformation,
             * this buffer is always transformed from native display
             * orientation to display orientation. For example, in the case
             * of a camera where the buffer remains in native orientation,
             * we want the pixels to always be upright.
             */
            sp<Layer> p = mDrawingParent.promote();
            if (p != nullptr) {
                const auto parentTransform = p->getTransform();
                tr = tr * inverseOrientation(parentTransform.getOrientation());
            }

            // and finally apply it to the original texture matrix
            const mat4 texTransform(mat4(static_cast<const float*>(textureMatrix)) * tr);
            memcpy(textureMatrix, texTransform.asArray(), sizeof(textureMatrix));
        }

        // Set things up for texturing.
        mTexture.setDimensions(getBE().compositionInfo.mBuffer->getWidth(),
                               getBE().compositionInfo.mBuffer->getHeight());
        mTexture.setFiltering(useFiltering);
        mTexture.setMatrix(textureMatrix);

        engine.setupLayerTexturing(mTexture);
    } else {
        engine.setupLayerBlackedOut();
    }
    drawWithOpenGL(renderArea, useIdentityTransform);
    engine.disableTexturing();
}

#if RK_STEREO
void setStereoDraw(const RenderArea& renderArea, RE::RenderEngine& engine,
    Mesh& mMesh, int alreadyStereo, int displayStereo)
{
   Mesh::VertexArray<vec2> position(mMesh.getPositionArray<vec2>());

   if(1==displayStereo && !alreadyStereo) {
       position[0].x /= 2;
       position[1].x /= 2;
       position[2].x /= 2;
       position[3].x /= 2;
       engine.drawMesh(mMesh);

       position[0].x += (renderArea.getWidth()/2);
       position[1].x += (renderArea.getWidth()/2);
       position[2].x += (renderArea.getWidth()/2);
       position[3].x += (renderArea.getWidth()/2);
   }

   if(2==displayStereo && !alreadyStereo) {
       position[0].y /= 2;
       position[1].y /= 2;
       position[2].y /= 2;
       position[3].y /= 2;
       engine.drawMesh(mMesh);

       position[0].y += (renderArea.getHeight()/2);
       position[1].y += (renderArea.getHeight()/2);
       position[2].y += (renderArea.getHeight()/2);
       position[3].y += (renderArea.getHeight()/2);
   }
}
#endif

void BufferLayer::onLayerDisplayed(const sp<Fence>& releaseFence) {
    mConsumer->setReleaseFence(releaseFence);
}

void BufferLayer::abandon() {
    mConsumer->abandon();
}

bool BufferLayer::shouldPresentNow(const DispSync& dispSync) const {
    if (mSidebandStreamChanged || mAutoRefresh) {
        return true;
    }

    Mutex::Autolock lock(mQueueItemLock);
    if (mQueueItems.empty()) {
        return false;
    }
    auto timestamp = mQueueItems[0].mTimestamp;
    nsecs_t expectedPresent = mConsumer->computeExpectedPresent(dispSync);

    // Ignore timestamps more than a second in the future
    bool isPlausible = timestamp < (expectedPresent + s2ns(1));
    ALOGW_IF(!isPlausible,
             "[%s] Timestamp %" PRId64 " seems implausible "
             "relative to expectedPresent %" PRId64,
             mName.string(), timestamp, expectedPresent);

    bool isDue = timestamp < expectedPresent;
    return isDue || !isPlausible;
}

void BufferLayer::setTransformHint(uint32_t orientation) const {
    mConsumer->setTransformHint(orientation);
}

bool BufferLayer::onPreComposition(nsecs_t refreshStartTime) {
    if (mBufferLatched) {
        Mutex::Autolock lock(mFrameEventHistoryMutex);
        mFrameEventHistory.addPreComposition(mCurrentFrameNumber,
                                             refreshStartTime);
    }
    mRefreshPending = false;
    return mQueuedFrames > 0 || mSidebandStreamChanged ||
            mAutoRefresh;
}
bool BufferLayer::onPostComposition(const std::shared_ptr<FenceTime>& glDoneFence,
                                    const std::shared_ptr<FenceTime>& presentFence,
                                    const CompositorTiming& compositorTiming) {
    // mFrameLatencyNeeded is true when a new frame was latched for the
    // composition.
    if (!mFrameLatencyNeeded) return false;

    // Update mFrameEventHistory.
    {
        Mutex::Autolock lock(mFrameEventHistoryMutex);
        mFrameEventHistory.addPostComposition(mCurrentFrameNumber, glDoneFence,
                                              presentFence, compositorTiming);
    }

    // Update mFrameTracker.
    nsecs_t desiredPresentTime = mConsumer->getTimestamp();
    mFrameTracker.setDesiredPresentTime(desiredPresentTime);

    const std::string layerName(getName().c_str());
    mTimeStats.setDesiredTime(layerName, mCurrentFrameNumber, desiredPresentTime);

    std::shared_ptr<FenceTime> frameReadyFence = mConsumer->getCurrentFenceTime();
    if (frameReadyFence->isValid()) {
        mFrameTracker.setFrameReadyFence(std::move(frameReadyFence));
    } else {
        // There was no fence for this frame, so assume that it was ready
        // to be presented at the desired present time.
        mFrameTracker.setFrameReadyTime(desiredPresentTime);
    }

    if (presentFence->isValid()) {
        mTimeStats.setPresentFence(layerName, mCurrentFrameNumber, presentFence);
        mFrameTracker.setActualPresentFence(std::shared_ptr<FenceTime>(presentFence));
    } else {
        // The HWC doesn't support present fences, so use the refresh
        // timestamp instead.
        const nsecs_t actualPresentTime =
                mFlinger->getHwComposer().getRefreshTimestamp(HWC_DISPLAY_PRIMARY);
        mTimeStats.setPresentTime(layerName, mCurrentFrameNumber, actualPresentTime);
        mFrameTracker.setActualPresentTime(actualPresentTime);
    }

    mFrameTracker.advanceFrame();
    mFrameLatencyNeeded = false;
    return true;
}

std::vector<OccupancyTracker::Segment> BufferLayer::getOccupancyHistory(bool forceFlush) {
    std::vector<OccupancyTracker::Segment> history;
    status_t result = mConsumer->getOccupancyHistory(forceFlush, &history);
    if (result != NO_ERROR) {
        ALOGW("[%s] Failed to obtain occupancy history (%d)", mName.string(), result);
        return {};
    }
    return history;
}

bool BufferLayer::getTransformToDisplayInverse() const {
    return mConsumer->getTransformToDisplayInverse();
}

void BufferLayer::releasePendingBuffer(nsecs_t dequeueReadyTime) {
    if (!mConsumer->releasePendingBuffer()) {
        return;
    }

    auto releaseFenceTime =
            std::make_shared<FenceTime>(mConsumer->getPrevFinalReleaseFence());
    mReleaseTimeline.updateSignalTimes();
    mReleaseTimeline.push(releaseFenceTime);

    Mutex::Autolock lock(mFrameEventHistoryMutex);
    if (mPreviousFrameNumber != 0) {
        mFrameEventHistory.addRelease(mPreviousFrameNumber, dequeueReadyTime,
                                      std::move(releaseFenceTime));
    }
}

Region BufferLayer::latchBuffer(bool& recomputeVisibleRegions, nsecs_t latchTime) {
    ATRACE_CALL();

    if (android_atomic_acquire_cas(true, false, &mSidebandStreamChanged) == 0) {
        // mSidebandStreamChanged was true
        mSidebandStream = mConsumer->getSidebandStream();
        // replicated in LayerBE until FE/BE is ready to be synchronized
        getBE().compositionInfo.hwc.sidebandStream = mSidebandStream;
        if (getBE().compositionInfo.hwc.sidebandStream != nullptr) {
            setTransactionFlags(eTransactionNeeded);
            mFlinger->setTransactionFlags(eTraversalNeeded);
        }
        recomputeVisibleRegions = true;

        const State& s(getDrawingState());
        return getTransform().transform(Region(Rect(s.active.w, s.active.h)));
    }

    Region outDirtyRegion;
    if (mQueuedFrames <= 0 && !mAutoRefresh) {
        return outDirtyRegion;
    }

    // if we've already called updateTexImage() without going through
    // a composition step, we have to skip this layer at this point
    // because we cannot call updateTeximage() without a corresponding
    // compositionComplete() call.
    // we'll trigger an update in onPreComposition().
    if (mRefreshPending) {
        return outDirtyRegion;
    }

    // If the head buffer's acquire fence hasn't signaled yet, return and
    // try again later
    if (!headFenceHasSignaled()) {
        mFlinger->signalLayerUpdate();
        return outDirtyRegion;
    }

    // Capture the old state of the layer for comparisons later
    const State& s(getDrawingState());
    const bool oldOpacity = isOpaque(s);
    sp<GraphicBuffer> oldBuffer = getBE().compositionInfo.mBuffer;

    if (!allTransactionsSignaled()) {
        mFlinger->signalLayerUpdate();
        return outDirtyRegion;
    }

    // This boolean is used to make sure that SurfaceFlinger's shadow copy
    // of the buffer queue isn't modified when the buffer queue is returning
    // BufferItem's that weren't actually queued. This can happen in shared
    // buffer mode.
    bool queuedBuffer = false;
    LayerRejecter r(mDrawingState, getCurrentState(), recomputeVisibleRegions,
                    getProducerStickyTransform() != 0, mName.string(),
                    mOverrideScalingMode, mFreezeGeometryUpdates);
    status_t updateResult =
            mConsumer->updateTexImage(&r, mFlinger->mPrimaryDispSync,
                                                    &mAutoRefresh, &queuedBuffer,
                                                    mLastFrameNumberReceived);
    if (updateResult == BufferQueue::PRESENT_LATER) {
        // Producer doesn't want buffer to be displayed yet.  Signal a
        // layer update so we check again at the next opportunity.
        mFlinger->signalLayerUpdate();
        return outDirtyRegion;
    } else if (updateResult == BufferLayerConsumer::BUFFER_REJECTED) {
        // If the buffer has been rejected, remove it from the shadow queue
        // and return early
        if (queuedBuffer) {
            Mutex::Autolock lock(mQueueItemLock);
            mTimeStats.removeTimeRecord(getName().c_str(), mQueueItems[0].mFrameNumber);
            mQueueItems.removeAt(0);
            android_atomic_dec(&mQueuedFrames);
        }
        return outDirtyRegion;
    } else if (updateResult != NO_ERROR || mUpdateTexImageFailed) {
        // This can occur if something goes wrong when trying to create the
        // EGLImage for this buffer. If this happens, the buffer has already
        // been released, so we need to clean up the queue and bug out
        // early.
        if (queuedBuffer) {
            Mutex::Autolock lock(mQueueItemLock);
            mQueueItems.clear();
            android_atomic_and(0, &mQueuedFrames);
            mTimeStats.clearLayerRecord(getName().c_str());
        }

        // Once we have hit this state, the shadow queue may no longer
        // correctly reflect the incoming BufferQueue's contents, so even if
        // updateTexImage starts working, the only safe course of action is
        // to continue to ignore updates.
        mUpdateTexImageFailed = true;

        return outDirtyRegion;
    }

    if (queuedBuffer) {
        // Autolock scope
        auto currentFrameNumber = mConsumer->getFrameNumber();

        Mutex::Autolock lock(mQueueItemLock);

        // Remove any stale buffers that have been dropped during
        // updateTexImage
        while (mQueueItems[0].mFrameNumber != currentFrameNumber) {
            mTimeStats.removeTimeRecord(getName().c_str(), mQueueItems[0].mFrameNumber);
            mQueueItems.removeAt(0);
            android_atomic_dec(&mQueuedFrames);
        }

        const std::string layerName(getName().c_str());
        mTimeStats.setAcquireFence(layerName, currentFrameNumber, mQueueItems[0].mFenceTime);
        mTimeStats.setLatchTime(layerName, currentFrameNumber, latchTime);

        mQueueItems.removeAt(0);
    }

    // Decrement the queued-frames count.  Signal another event if we
    // have more frames pending.
    if ((queuedBuffer && android_atomic_dec(&mQueuedFrames) > 1) ||
        mAutoRefresh) {
        mFlinger->signalLayerUpdate();
    }

    // update the active buffer
    getBE().compositionInfo.mBuffer =
            mConsumer->getCurrentBuffer(&getBE().compositionInfo.mBufferSlot);
    // replicated in LayerBE until FE/BE is ready to be synchronized
    mActiveBuffer = getBE().compositionInfo.mBuffer;
    if (getBE().compositionInfo.mBuffer == nullptr) {
        // this can only happen if the very first buffer was rejected.
        return outDirtyRegion;
    }

    mBufferLatched = true;
    mPreviousFrameNumber = mCurrentFrameNumber;
    mCurrentFrameNumber = mConsumer->getFrameNumber();

    {
        Mutex::Autolock lock(mFrameEventHistoryMutex);
        mFrameEventHistory.addLatch(mCurrentFrameNumber, latchTime);
    }

    mRefreshPending = true;
    mFrameLatencyNeeded = true;
    if (oldBuffer == nullptr) {
        // the first time we receive a buffer, we need to trigger a
        // geometry invalidation.
        recomputeVisibleRegions = true;
    }

    ui::Dataspace dataSpace = mConsumer->getCurrentDataSpace();
    // treat modern dataspaces as legacy dataspaces whenever possible, until
    // we can trust the buffer producers
    switch (dataSpace) {
        case ui::Dataspace::V0_SRGB:
            dataSpace = ui::Dataspace::SRGB;
            break;
        case ui::Dataspace::V0_SRGB_LINEAR:
            dataSpace = ui::Dataspace::SRGB_LINEAR;
            break;
        case ui::Dataspace::V0_JFIF:
            dataSpace = ui::Dataspace::JFIF;
            break;
        case ui::Dataspace::V0_BT601_625:
            dataSpace = ui::Dataspace::BT601_625;
            break;
        case ui::Dataspace::V0_BT601_525:
            dataSpace = ui::Dataspace::BT601_525;
            break;
        case ui::Dataspace::V0_BT709:
            dataSpace = ui::Dataspace::BT709;
            break;
        default:
            break;
    }
    mCurrentDataSpace = dataSpace;

    Rect crop(mConsumer->getCurrentCrop());
    const uint32_t transform(mConsumer->getCurrentTransform());
    const uint32_t scalingMode(mConsumer->getCurrentScalingMode());
    if ((crop != mCurrentCrop) ||
        (transform != mCurrentTransform) ||
        (scalingMode != mCurrentScalingMode)) {
        mCurrentCrop = crop;
        mCurrentTransform = transform;
        mCurrentScalingMode = scalingMode;
        recomputeVisibleRegions = true;
    }

    if (oldBuffer != nullptr) {
        uint32_t bufWidth = getBE().compositionInfo.mBuffer->getWidth();
        uint32_t bufHeight = getBE().compositionInfo.mBuffer->getHeight();
        if (bufWidth != uint32_t(oldBuffer->width) ||
            bufHeight != uint32_t(oldBuffer->height)) {
            recomputeVisibleRegions = true;
        }
    }

    mCurrentOpacity = getOpacityForFormat(getBE().compositionInfo.mBuffer->format);
    if (oldOpacity != isOpaque(s)) {
        recomputeVisibleRegions = true;
    }

    // Remove any sync points corresponding to the buffer which was just
    // latched
    {
        Mutex::Autolock lock(mLocalSyncPointMutex);
        auto point = mLocalSyncPoints.begin();
        while (point != mLocalSyncPoints.end()) {
            if (!(*point)->frameIsAvailable() || !(*point)->transactionIsApplied()) {
                // This sync point must have been added since we started
                // latching. Don't drop it yet.
                ++point;
                continue;
            }

            if ((*point)->getFrameNumber() <= mCurrentFrameNumber) {
                point = mLocalSyncPoints.erase(point);
            } else {
                ++point;
            }
        }
    }

    // FIXME: postedRegion should be dirty & bounds
    Region dirtyRegion(Rect(s.active.w, s.active.h));

    // transform the dirty region to window-manager space
    outDirtyRegion = (getTransform().transform(dirtyRegion));

    return outDirtyRegion;
}

void BufferLayer::setDefaultBufferSize(uint32_t w, uint32_t h) {
    mConsumer->setDefaultBufferSize(w, h);
}

void BufferLayer::setPerFrameData(const sp<const DisplayDevice>& displayDevice) {
    // Apply this display's projection's viewport to the visible region
    // before giving it to the HWC HAL.
    const Transform& tr = displayDevice->getTransform();
    const auto& viewport = displayDevice->getViewport();
    Region visible = tr.transform(visibleRegion.intersect(viewport));
    auto hwcId = displayDevice->getHwcDisplayId();
    auto& hwcInfo = getBE().mHwcLayers[hwcId];
    auto& hwcLayer = hwcInfo.layer;
    auto error = hwcLayer->setVisibleRegion(visible);
    if (error != HWC2::Error::None) {
        ALOGE("[%s] Failed to set visible region: %s (%d)", mName.string(),
              to_string(error).c_str(), static_cast<int32_t>(error));
        visible.dump(LOG_TAG);
    }

    error = hwcLayer->setSurfaceDamage(surfaceDamageRegion);
    if (error != HWC2::Error::None) {
        ALOGE("[%s] Failed to set surface damage: %s (%d)", mName.string(),
              to_string(error).c_str(), static_cast<int32_t>(error));
        surfaceDamageRegion.dump(LOG_TAG);
    }

    // Sideband layers
    if (getBE().compositionInfo.hwc.sidebandStream.get()) {
        setCompositionType(hwcId, HWC2::Composition::Sideband);
        ALOGV("[%s] Requesting Sideband composition", mName.string());
        error = hwcLayer->setSidebandStream(getBE().compositionInfo.hwc.sidebandStream->handle());
        if (error != HWC2::Error::None) {
            ALOGE("[%s] Failed to set sideband stream %p: %s (%d)", mName.string(),
                  getBE().compositionInfo.hwc.sidebandStream->handle(), to_string(error).c_str(),
                  static_cast<int32_t>(error));
        }
        return;
    }

    // Device or Cursor layers
    if (mPotentialCursor) {
        ALOGV("[%s] Requesting Cursor composition", mName.string());
        setCompositionType(hwcId, HWC2::Composition::Cursor);
    } else {
        ALOGV("[%s] Requesting Device composition", mName.string());
        setCompositionType(hwcId, HWC2::Composition::Device);
    }

    ALOGV("setPerFrameData: dataspace = %d", mCurrentDataSpace);
    error = hwcLayer->setDataspace(mCurrentDataSpace);
    if (error != HWC2::Error::None) {
        ALOGE("[%s] Failed to set dataspace %d: %s (%d)", mName.string(), mCurrentDataSpace,
              to_string(error).c_str(), static_cast<int32_t>(error));
    }

    const HdrMetadata& metadata = mConsumer->getCurrentHdrMetadata();
    error = hwcLayer->setPerFrameMetadata(displayDevice->getSupportedPerFrameMetadata(), metadata);
    if (error != HWC2::Error::None && error != HWC2::Error::Unsupported) {
        ALOGE("[%s] Failed to set hdrMetadata: %s (%d)", mName.string(),
              to_string(error).c_str(), static_cast<int32_t>(error));
    }

    uint32_t hwcSlot = 0;
    sp<GraphicBuffer> hwcBuffer;
    hwcInfo.bufferCache.getHwcBuffer(getBE().compositionInfo.mBufferSlot,
                                     getBE().compositionInfo.mBuffer, &hwcSlot, &hwcBuffer);

    auto acquireFence = mConsumer->getCurrentFence();
    error = hwcLayer->setBuffer(hwcSlot, hwcBuffer, acquireFence);
    if (error != HWC2::Error::None) {
        ALOGE("[%s] Failed to set buffer %p: %s (%d)", mName.string(),
              getBE().compositionInfo.mBuffer->handle, to_string(error).c_str(),
              static_cast<int32_t>(error));
    }
#if RK_LAYER_NAME
    // Use rk ashmem -------
    set_handle_layername(getBE().compositionInfo.mBuffer->handle, mName.string());
#endif

#if RK_STEREO
    set_handle_alreadyStereo(getBE().compositionInfo.mBuffer->handle, mConsumer->getAlreadyStereo());
    set_handle_displayStereo(getBE().compositionInfo.mBuffer->handle, 0);
#endif
    // Use rk ashmem -------


}
#if RK_STEREO
void BufferLayer::setDisplayStereo(){
    if(getBE().compositionInfo.mBuffer != nullptr && getBE().compositionInfo.mBuffer->handle)
    {
        displayStereo = get_handle_displayStereo(getBE().compositionInfo.mBuffer->handle);
    }
    else
        displayStereo = 0;
}
#endif

bool BufferLayer::isOpaque(const Layer::State& s) const {
    // if we don't have a buffer or sidebandStream yet, we're translucent regardless of the
    // layer's opaque flag.
    if ((getBE().compositionInfo.hwc.sidebandStream == nullptr) && (getBE().compositionInfo.mBuffer == nullptr)) {
        return false;
    }

    // if the layer has the opaque flag, then we're always opaque,
    // otherwise we use the current buffer's format.
    return ((s.flags & layer_state_t::eLayerOpaque) != 0) || mCurrentOpacity;
}

void BufferLayer::onFirstRef() {
    Layer::onFirstRef();

    // Creates a custom BufferQueue for SurfaceFlingerConsumer to use
    sp<IGraphicBufferProducer> producer;
    sp<IGraphicBufferConsumer> consumer;
    BufferQueue::createBufferQueue(&producer, &consumer, true);
    mProducer = new MonitoredProducer(producer, mFlinger, this);
    {
        // Grab the SF state lock during this since it's the only safe way to access RenderEngine
        Mutex::Autolock lock(mFlinger->mStateLock);
        mConsumer = new BufferLayerConsumer(consumer, mFlinger->getRenderEngine(), mTextureName,
                                            this);
    }
    mConsumer->setConsumerUsageBits(getEffectiveUsage(0));
    mConsumer->setContentsChangedListener(this);
    mConsumer->setName(mName);

    if (mFlinger->isLayerTripleBufferingDisabled()) {
        mProducer->setMaxDequeuedBufferCount(2);
    }

    const sp<const DisplayDevice> hw(mFlinger->getDefaultDisplayDevice());
    updateTransformHint(hw);
}

// ---------------------------------------------------------------------------
// Interface implementation for SurfaceFlingerConsumer::ContentsChangedListener
// ---------------------------------------------------------------------------

void BufferLayer::onFrameAvailable(const BufferItem& item) {
    // Add this buffer from our internal queue tracker
    { // Autolock scope
        Mutex::Autolock lock(mQueueItemLock);
        mFlinger->mInterceptor->saveBufferUpdate(this, item.mGraphicBuffer->getWidth(),
                                                 item.mGraphicBuffer->getHeight(),
                                                 item.mFrameNumber);
        // Reset the frame number tracker when we receive the first buffer after
        // a frame number reset
        if (item.mFrameNumber == 1) {
            mLastFrameNumberReceived = 0;
        }

        // Ensure that callbacks are handled in order
        while (item.mFrameNumber != mLastFrameNumberReceived + 1) {
            status_t result = mQueueItemCondition.waitRelative(mQueueItemLock,
                                                               ms2ns(500));
            if (result != NO_ERROR) {
                ALOGE("[%s] Timed out waiting on callback", mName.string());
            }
        }

        mQueueItems.push_back(item);
        android_atomic_inc(&mQueuedFrames);

        // Wake up any pending callbacks
        mLastFrameNumberReceived = item.mFrameNumber;
        mQueueItemCondition.broadcast();
    }

    mFlinger->signalLayerUpdate();
}

void BufferLayer::onFrameReplaced(const BufferItem& item) {
    { // Autolock scope
        Mutex::Autolock lock(mQueueItemLock);

        // Ensure that callbacks are handled in order
        while (item.mFrameNumber != mLastFrameNumberReceived + 1) {
            status_t result = mQueueItemCondition.waitRelative(mQueueItemLock,
                                                               ms2ns(500));
            if (result != NO_ERROR) {
                ALOGE("[%s] Timed out waiting on callback", mName.string());
            }
        }

        if (mQueueItems.empty()) {
            ALOGE("Can't replace a frame on an empty queue");
            return;
        }
        mQueueItems.editItemAt(mQueueItems.size() - 1) = item;

        // Wake up any pending callbacks
        mLastFrameNumberReceived = item.mFrameNumber;
        mQueueItemCondition.broadcast();
    }
}

void BufferLayer::onSidebandStreamChanged() {
    if (android_atomic_release_cas(false, true, &mSidebandStreamChanged) == 0) {
        // mSidebandStreamChanged was false
        mFlinger->signalLayerUpdate();
    }
}

bool BufferLayer::needsFiltering(const RenderArea& renderArea) const {
    return mNeedsFiltering || renderArea.needsFiltering();
}

// As documented in libhardware header, formats in the range
// 0x100 - 0x1FF are specific to the HAL implementation, and
// are known to have no alpha channel
// TODO: move definition for device-specific range into
// hardware.h, instead of using hard-coded values here.
#define HARDWARE_IS_DEVICE_FORMAT(f) ((f) >= 0x100 && (f) <= 0x1FF)

bool BufferLayer::getOpacityForFormat(uint32_t format) {
    if (HARDWARE_IS_DEVICE_FORMAT(format)) {
        return true;
    }
    switch (format) {
        case HAL_PIXEL_FORMAT_RGBA_8888:
        case HAL_PIXEL_FORMAT_BGRA_8888:
        case HAL_PIXEL_FORMAT_RGBA_FP16:
        case HAL_PIXEL_FORMAT_RGBA_1010102:
            return false;
    }
    // in all other case, we have no blending (also for unknown formats)
    return true;
}

bool BufferLayer::isHdrY410() const {
    // pixel format is HDR Y410 masquerading as RGBA_1010102
    return (mCurrentDataSpace == ui::Dataspace::BT2020_ITU_PQ &&
            mConsumer->getCurrentApi() == NATIVE_WINDOW_API_MEDIA &&
            getBE().compositionInfo.mBuffer->getPixelFormat() == HAL_PIXEL_FORMAT_RGBA_1010102);
}

void BufferLayer::drawWithOpenGL(const RenderArea& renderArea, bool useIdentityTransform) const {
    ATRACE_CALL();
    const State& s(getDrawingState());

    computeGeometry(renderArea, getBE().mMesh, useIdentityTransform);

    /*
     * NOTE: the way we compute the texture coordinates here produces
     * different results than when we take the HWC path -- in the later case
     * the "source crop" is rounded to texel boundaries.
     * This can produce significantly different results when the texture
     * is scaled by a large amount.
     *
     * The GL code below is more logical (imho), and the difference with
     * HWC is due to a limitation of the HWC API to integers -- a question
     * is suspend is whether we should ignore this problem or revert to
     * GL composition when a buffer scaling is applied (maybe with some
     * minimal value)? Or, we could make GL behave like HWC -- but this feel
     * like more of a hack.
     */
    const Rect bounds{computeBounds()}; // Rounds from FloatRect

    Transform t = getTransform();
    Rect win = bounds;
    if (!s.finalCrop.isEmpty()) {
        win = t.transform(win);
        if (!win.intersect(s.finalCrop, &win)) {
            win.clear();
        }
        win = t.inverse().transform(win);
        if (!win.intersect(bounds, &win)) {
            win.clear();
        }
    }

    float left = float(win.left) / float(s.active.w);
    float top = float(win.top) / float(s.active.h);
    float right = float(win.right) / float(s.active.w);
    float bottom = float(win.bottom) / float(s.active.h);

    // TODO: we probably want to generate the texture coords with the mesh
    // here we assume that we only have 4 vertices
    Mesh::VertexArray<vec2> texCoords(getBE().mMesh.getTexCoordArray<vec2>());
    texCoords[0] = vec2(left, 1.0f - top);
    texCoords[1] = vec2(left, 1.0f - bottom);
    texCoords[2] = vec2(right, 1.0f - bottom);
    texCoords[3] = vec2(right, 1.0f - top);

    auto& engine(mFlinger->getRenderEngine());
    engine.setupLayerBlending(mPremultipliedAlpha, isOpaque(s), false /* disableTexture */,
                              getColor());
    engine.setSourceDataSpace(mCurrentDataSpace);

#if RK_STEREO
    setStereoDraw(renderArea, engine, getBE().mMesh,
        mConsumer->getAlreadyStereo(), displayStereo);
#endif

#if RK_HDR
    if(mActiveBuffer->getPixelFormat() == HAL_PIXEL_FORMAT_YCrCb_NV12_10
        && (mActiveBuffer->getUsage() & HDRUSAGE)) {
        engine.setupHdr(true);
        engine.setupMRatioTexturing();
    }
#endif

    if (isHdrY410()) {
        engine.setSourceY410BT2020(true);
    }

    engine.drawMesh(getBE().mMesh);
    engine.disableBlending();

#if RK_HDR
        engine.setupHdr(false);
#endif

    engine.setSourceY410BT2020(false);
}

uint32_t BufferLayer::getProducerStickyTransform() const {
    int producerStickyTransform = 0;
    int ret = mProducer->query(NATIVE_WINDOW_STICKY_TRANSFORM, &producerStickyTransform);
    if (ret != OK) {
        ALOGW("%s: Error %s (%d) while querying window sticky transform.", __FUNCTION__,
              strerror(-ret), ret);
        return 0;
    }
    return static_cast<uint32_t>(producerStickyTransform);
}

bool BufferLayer::latchUnsignaledBuffers() {
    static bool propertyLoaded = false;
    static bool latch = false;
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    if (!propertyLoaded) {
        char value[PROPERTY_VALUE_MAX] = {};
        property_get("debug.sf.latch_unsignaled", value, "0");
        latch = atoi(value);
        propertyLoaded = true;
    }
    return latch;
}

uint64_t BufferLayer::getHeadFrameNumber() const {
    Mutex::Autolock lock(mQueueItemLock);
    if (!mQueueItems.empty()) {
        return mQueueItems[0].mFrameNumber;
    } else {
        return mCurrentFrameNumber;
    }
}

bool BufferLayer::headFenceHasSignaled() const {
    if (latchUnsignaledBuffers()) {
        return true;
    }

    Mutex::Autolock lock(mQueueItemLock);
    if (mQueueItems.empty()) {
        return true;
    }
    if (mQueueItems[0].mIsDroppable) {
        // Even though this buffer's fence may not have signaled yet, it could
        // be replaced by another buffer before it has a chance to, which means
        // that it's possible to get into a situation where a buffer is never
        // able to be latched. To avoid this, grab this buffer anyway.
        return true;
    }
    return mQueueItems[0].mFenceTime->getSignalTime() !=
            Fence::SIGNAL_TIME_PENDING;
}

uint32_t BufferLayer::getEffectiveScalingMode() const {
    if (mOverrideScalingMode >= 0) {
        return mOverrideScalingMode;
    }
    return mCurrentScalingMode;
}

// ----------------------------------------------------------------------------
// transaction
// ----------------------------------------------------------------------------

void BufferLayer::notifyAvailableFrames() {
    auto headFrameNumber = getHeadFrameNumber();
    bool headFenceSignaled = headFenceHasSignaled();
    Mutex::Autolock lock(mLocalSyncPointMutex);
    for (auto& point : mLocalSyncPoints) {
        if (headFrameNumber >= point->getFrameNumber() && headFenceSignaled) {
            point->setFrameAvailable();
        }
    }
}

sp<IGraphicBufferProducer> BufferLayer::getProducer() const {
    return mProducer;
}

// ---------------------------------------------------------------------------
// h/w composer set-up
// ---------------------------------------------------------------------------

bool BufferLayer::allTransactionsSignaled() {
    auto headFrameNumber = getHeadFrameNumber();
    bool matchingFramesFound = false;
    bool allTransactionsApplied = true;
    Mutex::Autolock lock(mLocalSyncPointMutex);

    for (auto& point : mLocalSyncPoints) {
        if (point->getFrameNumber() > headFrameNumber) {
            break;
        }
        matchingFramesFound = true;

        if (!point->frameIsAvailable()) {
            // We haven't notified the remote layer that the frame for
            // this point is available yet. Notify it now, and then
            // abort this attempt to latch.
            point->setFrameAvailable();
            allTransactionsApplied = false;
            break;
        }

        allTransactionsApplied = allTransactionsApplied && point->transactionIsApplied();
    }
    return !matchingFramesFound || allTransactionsApplied;
}

} // namespace android

#if defined(__gl_h_)
#error "don't include gl/gl.h in this file"
#endif

#if defined(__gl2_h_)
#error "don't include gl2/gl2.h in this file"
#endif
