#include <jni.h>
#include <string.h>
#include <cstring>

//#define LOG_NDEBUG 0
#define LOG_TAG "Mira_for_You_JNI"
#include <utils/Log.h>

#include <media/stagefright/foundation/AMessage.h>

#include "sink/WifiDisplaySink.h"
#include "source/WifiDisplaySource.h"

#include <binder/ProcessState.h>
#include <binder/IServiceManager.h>
#include <gui/ISurfaceComposer.h>
#include <gui/SurfaceComposerClient.h>
#include <ui/DisplayInfo.h>
#include <media/AudioSystem.h>
#include <media/IMediaPlayerService.h>
#include <media/IRemoteDisplay.h>
#include <media/IRemoteDisplayClient.h>
#include <media/stagefright/DataSource.h>
#include <media/stagefright/foundation/ADebug.h>

// base src:/frameworks/av/media/libstagefright/wifi-display/wfd.cpp

namespace android {

// Sink JNI
extern "C" void Java_com_example_mira4u_MainActivity_nativeInvokeSink(JNIEnv* env, jobject thiz, jstring ipaddr, jint port) {

    ProcessState::self()->startThreadPool();
    DataSource::RegisterDefaultSniffers();

    sp<ANetworkSession> session = new ANetworkSession;
    session->start();

    sp<ALooper> looper = new ALooper;

    // psw0523 add for surface
    sp<SurfaceComposerClient> composerClient = new SurfaceComposerClient;
    CHECK_EQ(composerClient->initCheck(), (status_t)OK);

    sp<IBinder> display(SurfaceComposerClient::getBuiltInDisplay(
            ISurfaceComposer::eDisplayIdMain));
    DisplayInfo info;
    SurfaceComposerClient::getDisplayInfo(display, &info);
    ssize_t displayWidth = info.w;
    ssize_t displayHeight = info.h;

    ALOGD("display is %d x %d\n", displayWidth, displayHeight);

    sp<SurfaceControl> control =
        composerClient->createSurface(
                String8("A Miracast Sink Surface"),
                displayWidth,
                displayHeight,
                //PIXEL_FORMAT_RGB_565,
                PIXEL_FORMAT_RGBA_8888,
                //HAL_PIXEL_FORMAT_YV12,
                0);

    CHECK(control != NULL);
    CHECK(control->isValid());

    SurfaceComposerClient::openGlobalTransaction();
    CHECK_EQ(control->setLayer(INT_MAX), (status_t)OK);
    CHECK_EQ(control->show(), (status_t)OK);
    SurfaceComposerClient::closeGlobalTransaction();

    sp<Surface> surface = control->getSurface();
    CHECK(surface != NULL);
    //ANativeWindow *anw = surface.get();
    //status_t result = native_window_api_connect(anw, NATIVE_WINDOW_API_EGL);
    //ALOGD("api connect result: %d", result);
    //result = native_window_set_buffers_transform(anw, NATIVE_WINDOW_TRANSFORM_ROT_90);
    //ALOGD("native_window_set_buffers_transform result: %d", result);
    //native_window_api_disconnect(anw, NATIVE_WINDOW_API_EGL);
    // end psw0523

    sp<WifiDisplaySink> sink = new WifiDisplaySink(0, session, surface->getIGraphicBufferProducer());
    looper->registerHandler(sink);

    const char *ip = env->GetStringUTFChars(ipaddr, NULL);
    ALOGD("Source[%s] Port[%d]", ip, port);
    sink->start(ip, port);
    //env->ReleaseStringUTFChars(ipaddr, ip);

    looper->start(true /* runOnCallingThread */);
}


// Source lib
struct RemoteDisplayClient : public BnRemoteDisplayClient {
    RemoteDisplayClient();

    virtual void onDisplayConnected(
            const sp<IGraphicBufferProducer>& bufferProducer,
            uint32_t width,
            uint32_t height,
            uint32_t flags,
            uint32_t session);

    virtual void onDisplayDisconnected();
    virtual void onDisplayError(int32_t error);

    void waitUntilDone();

protected:
    virtual ~RemoteDisplayClient();

private:
    Mutex mLock;
    Condition mCondition;

    bool mDone;

    sp<SurfaceComposerClient> mComposerClient;
    sp<IGraphicBufferProducer> mSurfaceTexture;
    sp<IBinder> mDisplayBinder;

    DISALLOW_EVIL_CONSTRUCTORS(RemoteDisplayClient);
};

RemoteDisplayClient::RemoteDisplayClient()
    : mDone(false) {
    mComposerClient = new SurfaceComposerClient;
    CHECK_EQ(mComposerClient->initCheck(), (status_t)OK);
}

RemoteDisplayClient::~RemoteDisplayClient() {
}

void RemoteDisplayClient::onDisplayConnected(
        const sp<IGraphicBufferProducer>& bufferProducer,
        uint32_t width,
        uint32_t height,
        uint32_t flags,
        uint32_t session) {
    ALOGI("onDisplayConnected width=%u, height=%u, flags = 0x%08x",
          width, height, flags);

    mSurfaceTexture = bufferProducer;
    mDisplayBinder = mComposerClient->createDisplay(
            String8("foo"), false /* secure */);

    SurfaceComposerClient::openGlobalTransaction();
    mComposerClient->setDisplaySurface(mDisplayBinder, mSurfaceTexture);

    DisplayInfo info;
    SurfaceComposerClient::getDisplayInfo(0, &info);
    Rect layerStackRect(info.w, info.h);
    Rect displayRect(info.w, info.h);

    mComposerClient->setDisplayProjection(
            mDisplayBinder, 0 /* 0 degree rotation */,
            layerStackRect,
            displayRect);

    SurfaceComposerClient::closeGlobalTransaction();
}

void RemoteDisplayClient::onDisplayDisconnected() {
    ALOGI("onDisplayDisconnected");

    Mutex::Autolock autoLock(mLock);
    mDone = true;
    mCondition.broadcast();
}

void RemoteDisplayClient::onDisplayError(int32_t error) {
    ALOGI("onDisplayError error=%d", error);

    Mutex::Autolock autoLock(mLock);
    mDone = true;
    mCondition.broadcast();
}

void RemoteDisplayClient::waitUntilDone() {
    Mutex::Autolock autoLock(mLock);
    while (!mDone) {
        mCondition.wait(mLock);
    }
}

static status_t enableAudioSubmix(bool enable) {
    status_t err = AudioSystem::setDeviceConnectionState(
            AUDIO_DEVICE_IN_REMOTE_SUBMIX,
            enable
                ? AUDIO_POLICY_DEVICE_STATE_AVAILABLE
                : AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE,
            NULL /* device_address */);

    if (err != OK) {
        return err;
    }

    err = AudioSystem::setDeviceConnectionState(
            AUDIO_DEVICE_OUT_REMOTE_SUBMIX,
            enable
                ? AUDIO_POLICY_DEVICE_STATE_AVAILABLE
                : AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE,
            NULL /* device_address */);

    return err;
}

static void createSource(const AString &addr, int32_t port) {
    sp<IServiceManager> sm = defaultServiceManager();
    sp<IBinder> binder = sm->getService(String16("media.player"));
    sp<IMediaPlayerService> service =
        interface_cast<IMediaPlayerService>(binder);

    CHECK(service.get() != NULL);

    enableAudioSubmix(true /* enable */);

    String8 iface;
    iface.append(addr.c_str());
    iface.append(StringPrintf(":%d", port).c_str());

    sp<RemoteDisplayClient> client = new RemoteDisplayClient;
    sp<IRemoteDisplay> display = service->listenForRemoteDisplay(client, iface);

    client->waitUntilDone();

    display->dispose();
    display.clear();

    enableAudioSubmix(false /* enable */);
}

// Source JNI
extern "C" void Java_com_example_mira4u_MainActivity_nativeInvokeSource(JNIEnv* env, jobject thiz, jstring ipaddr, jint port) {

    ProcessState::self()->startThreadPool();
    DataSource::RegisterDefaultSniffers();

    const char *ip = env->GetStringUTFChars(ipaddr, NULL);
    ALOGD("Source[%s] Port[%d]", ip, port);

    createSource(ip, port);
}

}  // namespace android

