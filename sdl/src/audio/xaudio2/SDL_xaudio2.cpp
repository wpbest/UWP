/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2015 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

/* WinRT NOTICE:

   A few changes to SDL's XAudio2 backend were warranted by API
   changes to Windows.  Many, but not all of these are documented by Microsoft
   at:
   http://blogs.msdn.com/b/chuckw/archive/2012/04/02/xaudio2-and-windows-8-consumer-preview.aspx

   1. Windows' thread synchronization function, CreateSemaphore, was removed
      from WinRT.  SDL's semaphore API was substituted instead.
   2. The method calls, IXAudio2::GetDeviceCount and IXAudio2::GetDeviceDetails
      were removed from the XAudio2 API.  Microsoft is telling developers to
      use APIs in Windows::Foundation instead.
      For SDL, the missing methods were reimplemented using the APIs Microsoft
      said to use.
   3. CoInitialize and CoUninitialize are not available in WinRT.
      These calls were removed, as COM will have been initialized earlier,
      at least by the call to the WinRT app's main function
      (aka 'int main(Platform::Array<Platform::String^>^)).  (DLudwig:
      This was my understanding of how WinRT: the 'main' function uses
      a tag of [MTAThread], which should initialize COM.  My understanding
      of COM is somewhat limited, and I may be incorrect here.)
   4. IXAudio2::CreateMasteringVoice changed its integer-based 'DeviceIndex'
      argument to a string-based one, 'szDeviceId'.  In WinRT, the
      string-based argument will be used.
*/
#include "../../SDL_internal.h"

#if SDL_AUDIO_DRIVER_XAUDIO2

extern "C" {
#include "../../core/windows/SDL_windows.h"
#include "SDL_audio.h"
#include "../SDL_audio_c.h"
#include "../SDL_sysaudio.h"
#include "SDL_assert.h"
}

#ifdef __GNUC__
/* The configure script already did any necessary checking */
#  define SDL_XAUDIO2_HAS_SDK 1
#elif defined(__WINRT__)
/* WinRT always has access to the XAudio 2 SDK */
#  define SDL_XAUDIO2_HAS_SDK
#else
/* XAudio2 exists as of the March 2008 DirectX SDK 
   The XAudio2 implementation available in the Windows 8 SDK targets Windows 8 and newer.
   If you want to build SDL with XAudio2 support you should install the DirectX SDK.
 */
#include <dxsdkver.h>
#if (!defined(_DXSDK_BUILD_MAJOR) || (_DXSDK_BUILD_MAJOR < 1284))
#  pragma message("Your DirectX SDK is too old. Disabling XAudio2 support.")
#else
#  define SDL_XAUDIO2_HAS_SDK 1
#endif
#endif

#ifdef SDL_XAUDIO2_HAS_SDK

/* Check to see if we're compiling for XAudio 2.8, or higher. */
#ifdef WINVER
#if WINVER >= 0x0602  /* Windows 8 SDK or higher? */
#define SDL_XAUDIO2_WIN8 1
#endif
#endif

#define INITGUID 1
#include <xaudio2.h>

/* Hidden "this" pointer for the audio functions */
#define _THIS   SDL_AudioDevice *_this

#ifdef __WINRT__
#include "SDL_xaudio2_winrthelpers.h"
#endif

/* Fixes bug 1210 where some versions of gcc need named parameters */
#ifdef __GNUC__
#ifdef THIS
#undef THIS
#endif
#define THIS    INTERFACE *p
#ifdef THIS_
#undef THIS_
#endif
#define THIS_   INTERFACE *p,
#endif

struct SDL_PrivateAudioData
{
    IXAudio2 *ixa2;
    IXAudio2SourceVoice *source;
    IXAudio2MasteringVoice *mastering;
    SDL_sem * semaphore;
    Uint8 *mixbuf;
    int mixlen;
    Uint8 *nextbuf;
};


static void
XAUDIO2_DetectDevices(void)
{
    IXAudio2 *ixa2 = NULL;
    UINT32 devcount = 0;
    UINT32 i = 0;

    if (XAudio2Create(&ixa2, 0, XAUDIO2_DEFAULT_PROCESSOR) != S_OK) {
        SDL_SetError("XAudio2: XAudio2Create() failed at detection.");
        return;
    } else if (IXAudio2_GetDeviceCount(ixa2, &devcount) != S_OK) {
        SDL_SetError("XAudio2: IXAudio2::GetDeviceCount() failed.");
        ixa2->Release();
        return;
    }

    for (i = 0; i < devcount; i++) {
        XAUDIO2_DEVICE_DETAILS details;
        if (IXAudio2_GetDeviceDetails(ixa2, i, &details) == S_OK) {
            char *str = WIN_StringToUTF8(details.DisplayName);
            if (str != NULL) {
                SDL_AddAudioDevice(SDL_FALSE, str, (void *) ((size_t) i+1));
                SDL_free(str);  /* SDL_AddAudioDevice made a copy of the string. */
            }
        }
    }

    ixa2->Release();
}


class SDL_XAudio2VoiceCallbacks : public IXAudio2VoiceCallback
{
    void STDMETHODCALLTYPE
    OnBufferEnd(void *data)
    {
        /* Just signal the SDL audio thread and get out of XAudio2's way. */
        SDL_AudioDevice *_this = (SDL_AudioDevice *) data;
        SDL_SemPost(_this->hidden->semaphore);
    }

    void STDMETHODCALLTYPE
    OnVoiceError(void *data, HRESULT Error)
    {
        SDL_AudioDevice *_this = (SDL_AudioDevice *) data;
        SDL_OpenedAudioDeviceDisconnected(_this);
    }

    /* no-op callbacks... */
    void STDMETHODCALLTYPE OnStreamEnd() {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32 b) {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() {}
    void STDMETHODCALLTYPE OnBufferStart(void *data) {}
    void STDMETHODCALLTYPE OnLoopEnd(void *data) {}
};

static Uint8 *
XAUDIO2_GetDeviceBuf(_THIS)
{
    return _this->hidden->nextbuf;
}

static void
XAUDIO2_PlayDevice(_THIS)
{
    XAUDIO2_BUFFER buffer;
    Uint8 *mixbuf = _this->hidden->mixbuf;
    Uint8 *nextbuf = _this->hidden->nextbuf;
    const int mixlen = _this->hidden->mixlen;
    IXAudio2SourceVoice *source = _this->hidden->source;
    HRESULT result = S_OK;

    if (!_this->enabled) { /* shutting down? */
        return;
    }

    /* Submit the next filled buffer */
    SDL_zero(buffer);
    buffer.AudioBytes = mixlen;
    buffer.pAudioData = nextbuf;
    buffer.pContext = _this;

    if (nextbuf == mixbuf) {
        nextbuf += mixlen;
    } else {
        nextbuf = mixbuf;
    }
    _this->hidden->nextbuf = nextbuf;

    result = source->SubmitSourceBuffer(&buffer, NULL);
    if (result == XAUDIO2_E_DEVICE_INVALIDATED) {
        /* !!! FIXME: possibly disconnected or temporary lost. Recover? */
    }

    if (result != S_OK) {  /* uhoh, panic! */
        source->FlushSourceBuffers();
        SDL_OpenedAudioDeviceDisconnected(_this);
    }
}

static void
XAUDIO2_WaitDevice(_THIS)
{
    if (_this->enabled) {
        SDL_SemWait(_this->hidden->semaphore);
    }
}

static void
XAUDIO2_WaitDone(_THIS)
{
    IXAudio2SourceVoice *source = _this->hidden->source;
    XAUDIO2_VOICE_STATE state;
    SDL_assert(!_this->enabled);  /* flag that stops playing. */
    source->Discontinuity();
#if SDL_XAUDIO2_WIN8
    source->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
#else
    IXAudio2SourceVoice_GetState(source, &state);
#endif
    while (state.BuffersQueued > 0) {
        SDL_SemWait(_this->hidden->semaphore);
#if SDL_XAUDIO2_WIN8
        source->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
#else
        source->GetState(&state);
#endif
    }
}


static void
XAUDIO2_CloseDevice(_THIS)
{
    if (_this->hidden != NULL) {
        IXAudio2 *ixa2 = _this->hidden->ixa2;
        IXAudio2SourceVoice *source = _this->hidden->source;
        IXAudio2MasteringVoice *mastering = _this->hidden->mastering;

        if (source != NULL) {
            source->Stop(0, XAUDIO2_COMMIT_NOW);
            source->FlushSourceBuffers();
            source->DestroyVoice();
        }
        if (ixa2 != NULL) {
            ixa2->StopEngine();
        }
        if (mastering != NULL) {
            mastering->DestroyVoice();
        }
        if (ixa2 != NULL) {
            ixa2->Release();
        }
        SDL_free(_this->hidden->mixbuf);
        if (_this->hidden->semaphore != NULL) {
            SDL_DestroySemaphore(_this->hidden->semaphore);
        }

        SDL_free(_this->hidden);
        _this->hidden = NULL;
    }
}

static int
XAUDIO2_OpenDevice(_THIS, void *handle, const char *devname, int iscapture)
{
    HRESULT result = S_OK;
    WAVEFORMATEX waveformat;
    int valid_format = 0;
    SDL_AudioFormat test_format = SDL_FirstAudioFormat(_this->spec.format);
    IXAudio2 *ixa2 = NULL;
    IXAudio2SourceVoice *source = NULL;
#if defined(SDL_XAUDIO2_WIN8)
    LPCWSTR devId = NULL;
#else
    UINT32 devId = 0;  /* 0 == system default device. */
#endif

    static SDL_XAudio2VoiceCallbacks callbacks;

#if defined(SDL_XAUDIO2_WIN8)
    /* !!! FIXME: hook up hotplugging. */
#else
    if (handle != NULL) {  /* specific device requested? */
        /* -1 because we increment the original value to avoid NULL. */
        const size_t val = ((size_t) handle) - 1;
        devId = (UINT32) val;
    }
#endif

    if (XAudio2Create(&ixa2, 0, XAUDIO2_DEFAULT_PROCESSOR) != S_OK) {
        return SDL_SetError("XAudio2: XAudio2Create() failed at open.");
    }

    /*
    XAUDIO2_DEBUG_CONFIGURATION debugConfig;
    debugConfig.TraceMask = XAUDIO2_LOG_ERRORS; //XAUDIO2_LOG_WARNINGS | XAUDIO2_LOG_DETAIL | XAUDIO2_LOG_FUNC_CALLS | XAUDIO2_LOG_TIMING | XAUDIO2_LOG_LOCKS | XAUDIO2_LOG_MEMORY | XAUDIO2_LOG_STREAMING;
    debugConfig.BreakMask = XAUDIO2_LOG_ERRORS; //XAUDIO2_LOG_WARNINGS;
    debugConfig.LogThreadID = TRUE;
    debugConfig.LogFileline = TRUE;
    debugConfig.LogFunctionName = TRUE;
    debugConfig.LogTiming = TRUE;
    ixa2->SetDebugConfiguration(&debugConfig);
    */

    /* Initialize all variables that we clean on shutdown */
    _this->hidden = (struct SDL_PrivateAudioData *)
        SDL_malloc((sizeof *_this->hidden));
    if (_this->hidden == NULL) {
        ixa2->Release();
        return SDL_OutOfMemory();
    }
    SDL_memset(_this->hidden, 0, (sizeof *_this->hidden));

    _this->hidden->ixa2 = ixa2;
    _this->hidden->semaphore = SDL_CreateSemaphore(1);
    if (_this->hidden->semaphore == NULL) {
        XAUDIO2_CloseDevice(_this);
        return SDL_SetError("XAudio2: CreateSemaphore() failed!");
    }

    while ((!valid_format) && (test_format)) {
        switch (test_format) {
        case AUDIO_U8:
        case AUDIO_S16:
        case AUDIO_S32:
        case AUDIO_F32:
            _this->spec.format = test_format;
            valid_format = 1;
            break;
        }
        test_format = SDL_NextAudioFormat();
    }

    if (!valid_format) {
        XAUDIO2_CloseDevice(_this);
        return SDL_SetError("XAudio2: Unsupported audio format");
    }

    /* Update the fragment size as size in bytes */
    SDL_CalculateAudioSpec(&_this->spec);

    /* We feed a Source, it feeds the Mastering, which feeds the device. */
    _this->hidden->mixlen = _this->spec.size;
    _this->hidden->mixbuf = (Uint8 *) SDL_malloc(2 * _this->hidden->mixlen);
    if (_this->hidden->mixbuf == NULL) {
        XAUDIO2_CloseDevice(_this);
        return SDL_OutOfMemory();
    }
    _this->hidden->nextbuf = _this->hidden->mixbuf;
    SDL_memset(_this->hidden->mixbuf, 0, 2 * _this->hidden->mixlen);

    /* We use XAUDIO2_DEFAULT_CHANNELS instead of _this->spec.channels. On
       Xbox360, _this means 5.1 output, but on Windows, it means "figure out
       what the system has." It might be preferable to let XAudio2 blast
       stereo output to appropriate surround sound configurations
       instead of clamping to 2 channels, even though we'll configure the
       Source Voice for whatever number of channels you supply. */
#if SDL_XAUDIO2_WIN8
    result = ixa2->CreateMasteringVoice(&_this->hidden->mastering,
                                        XAUDIO2_DEFAULT_CHANNELS,
                                        _this->spec.freq, 0, devId, NULL, AudioCategory_GameEffects);
#else
    result = ixa2->CreateMasteringVoice(&_this->hidden->mastering,
                                        XAUDIO2_DEFAULT_CHANNELS,
                                        _this->spec.freq, 0, devId, NULL);
#endif
    if (result != S_OK) {
        XAUDIO2_CloseDevice(_this);
        return SDL_SetError("XAudio2: Couldn't create mastering voice");
    }

    SDL_zero(waveformat);
    if (SDL_AUDIO_ISFLOAT(_this->spec.format)) {
        waveformat.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    } else {
        waveformat.wFormatTag = WAVE_FORMAT_PCM;
    }
    waveformat.wBitsPerSample = SDL_AUDIO_BITSIZE(_this->spec.format);
    waveformat.nChannels = _this->spec.channels;
    waveformat.nSamplesPerSec = _this->spec.freq;
    waveformat.nBlockAlign =
        waveformat.nChannels * (waveformat.wBitsPerSample / 8);
    waveformat.nAvgBytesPerSec =
        waveformat.nSamplesPerSec * waveformat.nBlockAlign;
    waveformat.cbSize = sizeof(waveformat);

#ifdef __WINRT__
    // DLudwig: for now, make XAudio2 do sample rate conversion, just to
    // get the loopwave test to work.
    //
    // TODO, WinRT: consider removing WinRT-specific source-voice creation code from SDL_xaudio2.c
    result = ixa2->CreateSourceVoice(&source, &waveformat,
                                     0,
                                     1.0f, &callbacks, NULL, NULL);
#else
    result = ixa2->CreateSourceVoice(&source, &waveformat,
                                     XAUDIO2_VOICE_NOSRC |
                                     XAUDIO2_VOICE_NOPITCH,
                                     1.0f, &callbacks, NULL, NULL);

#endif
    if (result != S_OK) {
        XAUDIO2_CloseDevice(_this);
        return SDL_SetError("XAudio2: Couldn't create source voice");
    }
    _this->hidden->source = source;

    /* Start everything playing! */
    result = ixa2->StartEngine();
    if (result != S_OK) {
        XAUDIO2_CloseDevice(_this);
        return SDL_SetError("XAudio2: Couldn't start engine");
    }

    result = source->Start(0, XAUDIO2_COMMIT_NOW);
    if (result != S_OK) {
        XAUDIO2_CloseDevice(_this);
        return SDL_SetError("XAudio2: Couldn't start source voice");
    }

    return 0; /* good to go. */
}

static void
XAUDIO2_Deinitialize(void)
{
#if defined(__WIN32__)
    WIN_CoUninitialize();
#endif
}

#endif  /* SDL_XAUDIO2_HAS_SDK */


static int
XAUDIO2_Init(SDL_AudioDriverImpl * impl)
{
#ifndef SDL_XAUDIO2_HAS_SDK
    SDL_SetError("XAudio2: SDL was built without XAudio2 support (old DirectX SDK).");
    return 0;  /* no XAudio2 support, ever. Update your SDK! */
#else
    /* XAudio2Create() is a macro that uses COM; we don't load the .dll */
    IXAudio2 *ixa2 = NULL;
#if defined(__WIN32__)
    // TODO, WinRT: Investigate using CoInitializeEx here
    if (FAILED(WIN_CoInitialize())) {
        SDL_SetError("XAudio2: CoInitialize() failed");
        return 0;
    }
#endif

    if (XAudio2Create(&ixa2, 0, XAUDIO2_DEFAULT_PROCESSOR) != S_OK) {
#if defined(__WIN32__)
        WIN_CoUninitialize();
#endif
        SDL_SetError("XAudio2: XAudio2Create() failed at initialization");
        return 0;  /* not available. */
    }
    ixa2->Release();

    /* Set the function pointers */
    impl->DetectDevices = XAUDIO2_DetectDevices;
    impl->OpenDevice = XAUDIO2_OpenDevice;
    impl->PlayDevice = XAUDIO2_PlayDevice;
    impl->WaitDevice = XAUDIO2_WaitDevice;
    impl->WaitDone = XAUDIO2_WaitDone;
    impl->GetDeviceBuf = XAUDIO2_GetDeviceBuf;
    impl->CloseDevice = XAUDIO2_CloseDevice;
    impl->Deinitialize = XAUDIO2_Deinitialize;

    /* !!! FIXME: We can apparently use a C++ interface on Windows 8
     * !!! FIXME: (Windows::Devices::Enumeration::DeviceInformation) for device
     * !!! FIXME: detection, but it's not implemented here yet.
     * !!! FIXME:  see http://blogs.msdn.com/b/chuckw/archive/2012/04/02/xaudio2-and-windows-8-consumer-preview.aspx
     * !!! FIXME:  for now, force the default device.
     */
#if defined(SDL_XAUDIO2_WIN8) || defined(__WINRT__)
    impl->OnlyHasDefaultOutputDevice = 1;
#endif

    return 1;   /* _this audio target is available. */
#endif
}

extern "C"
AudioBootStrap XAUDIO2_bootstrap = {
    "xaudio2", "XAudio2", XAUDIO2_Init, 0
};

#endif  /* SDL_AUDIO_DRIVER_XAUDIO2 */

/* vi: set ts=4 sw=4 expandtab: */
