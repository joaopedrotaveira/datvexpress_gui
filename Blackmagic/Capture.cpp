/* -LICENSE-START-
** Copyright (c) 2013 Blackmagic Design
**
** Permission is hereby granted, free of charge, to any person or organization
** obtaining a copy of the software and accompanying documentation covered by
** this license (the "Software") to use, reproduce, display, distribute,
** execute, and transmit the Software, and to prepare derivative works of the
** Software, and to permit third-parties to whom the Software is furnished to
** do so, all subject to the following:
**
** The copyright notices in the Software and this entire statement, including
** the above license grant, this restriction and the following disclaimer,
** must be included in all copies of the Software, in whole or in part, and
** all derivative works of the Software, unless such copies or derivative
** works are solely in the form of machine-executable object code generated by
** a source language processor.
**
** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
** IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
** FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
** SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
** FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
** ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
** DEALINGS IN THE SOFTWARE.
** -LICENSE-END-
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <csignal>

#include "DeckLinkAPI.h"
#include "Capture.h"
#include "Config.h"
#include "an_capture.h"

IDeckLinkIterator* g_deckLinkIterator = NULL;
uint8_t *g_audio_buffer;
int g_audio_index;
#define AU_S_LEN (7680)

static pthread_mutex_t	g_sleepMutex;
static pthread_cond_t	g_sleepCond;
static int				g_videoOutputFile = -1;
static int				g_audioOutputFile = -1;
static bool				g_do_exit = false;

static BMDConfig		g_config;

static IDeckLinkInput*	g_deckLinkInput = NULL;
static DeckLinkCaptureDelegate* g_delegate = NULL;

static unsigned long	g_frameCount = 0;

DeckLinkCaptureDelegate::DeckLinkCaptureDelegate() : m_refCount(0)
{
	pthread_mutex_init(&m_mutex, NULL);
}

DeckLinkCaptureDelegate::~DeckLinkCaptureDelegate()
{
	pthread_mutex_destroy(&m_mutex);
}

ULONG DeckLinkCaptureDelegate::AddRef(void)
{
	pthread_mutex_lock(&m_mutex);
		m_refCount++;
	pthread_mutex_unlock(&m_mutex);

	return (ULONG)m_refCount;
}

ULONG DeckLinkCaptureDelegate::Release(void)
{
	pthread_mutex_lock(&m_mutex);
		m_refCount--;
	pthread_mutex_unlock(&m_mutex);

	if (m_refCount == 0)
	{
		delete this;
		return 0;
	}

	return (ULONG)m_refCount;
}

HRESULT DeckLinkCaptureDelegate::VideoInputFrameArrived(IDeckLinkVideoInputFrame* videoFrame, IDeckLinkAudioInputPacket* audioFrame)
{
    void*								videoFrameBytes;
	void*								audioFrameBytes;
	// Handle Video Frame
	if (videoFrame)
	{
        if(videoFrame->GetFlags() & bmdFrameHasNoInputSource){

        }else{
            videoFrame->GetBytes(&videoFrameBytes);
            int bytes = videoFrame->GetRowBytes();
            int len = videoFrame->GetRowBytes() * videoFrame->GetHeight();
            uint8_t *b = (uint8_t*)videoFrameBytes;
            //an_process_captured_video_buffer((uint8_t*)b,AV_PIX_FMT_YUV422P);
            an_process_captured_video_buffer((uint8_t*)videoFrameBytes,AV_PIX_FMT_UYVY422);
        }
	}

	// Handle Audio Frame
	if (audioFrame)
	{
        // Audio frames from the DeckLink are dependant on the frame rate
        // This means they are not the size required for the codec.
        // Therefore some buffering is required.
        audioFrame->GetBytes(&audioFrameBytes);
        int bytes = audioFrame->GetSampleFrameCount() * 2 * 2;
        switch(g_audio_index){
        case 0:
            memcpy(&g_audio_buffer[0],audioFrameBytes,bytes);
            an_process_capture_audio(g_audio_buffer, 4608);
            g_audio_index = 1;
            break;
        case 1:
            memcpy(&g_audio_buffer[AU_S_LEN],audioFrameBytes,bytes);
            an_process_capture_audio(&g_audio_buffer[4608], 4608);
            an_process_capture_audio(&g_audio_buffer[4608*2], 4608);
            g_audio_index = 2;
            break;
        case 2:
        default:
            memcpy(&g_audio_buffer[AU_S_LEN*2],audioFrameBytes,bytes);
            an_process_capture_audio(&g_audio_buffer[4608*3], 4608);
            an_process_capture_audio(&g_audio_buffer[4608*4], 4608);
            g_audio_index = 0;
            break;
        }
            //write(g_audioOutputFile, audioFrameBytes, audioFrame->GetSampleFrameCount() * g_config.m_audioChannels * (g_config.m_audioSampleDepth / 8));
	}
	return S_OK;
}

HRESULT DeckLinkCaptureDelegate::VideoInputFormatChanged(BMDVideoInputFormatChangedEvents events, IDeckLinkDisplayMode *mode, BMDDetectedVideoInputFormatFlags)
{
	// This only gets called if bmdVideoInputEnableFormatDetection was set
	// when enabling video input
	HRESULT	result;
	char*	displayModeName = NULL;

	if (!(events & bmdVideoInputDisplayModeChanged))
		return S_OK;

	mode->GetName((const char**)&displayModeName);
	printf("Video format changed to %s\n", displayModeName);

	if (displayModeName)
		free(displayModeName);

	if (g_deckLinkInput)
	{
		g_deckLinkInput->StopStreams();

		result = g_deckLinkInput->EnableVideoInput(mode->GetDisplayMode(), g_config.m_pixelFormat, g_config.m_inputFlags);
		if (result != S_OK)
		{
			fprintf(stderr, "Failed to switch video mode\n");
			goto bail;
		}

		g_deckLinkInput->StartStreams();
	}

bail:
	return S_OK;
}

static void sigfunc(int signum)
{
	if (signum == SIGINT || signum == SIGTERM)
		g_do_exit = true;

	pthread_cond_signal(&g_sleepCond);
}
//
// Interface functions
//
int dl_init(void){
    IDeckLink*		   deckLink = NULL;
    IDeckLinkAttributes* deckLinkAttributes = NULL;
    IDeckLinkDisplayModeIterator*	displayModeIterator = NULL;
    IDeckLinkDisplayMode*			displayMode = NULL;


    g_deckLinkIterator = CreateDeckLinkIteratorInstance();
    if (!g_deckLinkIterator)
    {
        //fprintf(stderr, "This application requires the DeckLink drivers installed.\n");
        return -1;
    }
    // Allocate memory for the audio re-buffering
    g_audio_buffer = (uint8_t*)malloc(7680*3);
    g_audio_index  = 0;

    if(g_deckLinkIterator->Next(&deckLink) == S_OK){
        if(deckLink->QueryInterface(IID_IDeckLinkInput, (void**)&g_deckLinkInput)==S_OK){
            // We have an input device
            g_deckLinkInput->GetDisplayModeIterator(&displayModeIterator);
            displayModeIterator->Next(&displayMode);
            // Configure the capture callback
            g_delegate = new DeckLinkCaptureDelegate();
            g_deckLinkInput->SetCallback(g_delegate);
            g_deckLinkInput->EnableVideoInput(displayMode->GetDisplayMode(), bmdFormat8BitYUV, bmdVideoInputEnableFormatDetection);
            g_deckLinkInput->EnableAudioInput(bmdAudioSampleRate48kHz, 16, 2);
            g_deckLinkInput->StartStreams();

            // Check the card supports format detection
           // if(deckLink->QueryInterface(IID_IDeckLinkAttributes, (void**)&deckLinkAttributes)==S_OK){

//            }
           return 0;
        }
    }
    return -1;
}
void dl_close(void){
    g_deckLinkIterator->Release();
    g_deckLinkInput->StopStreams();
    g_deckLinkInput->DisableAudioInput();
    g_deckLinkInput->DisableVideoInput();
    delete g_delegate;
}

int blackmagic_main(int argc, char *argv[])
{
	HRESULT							result;
	int								exitStatus = 1;
	int								idx;

	IDeckLinkIterator*				deckLinkIterator = NULL;
	IDeckLink*						deckLink = NULL;

	IDeckLinkAttributes*			deckLinkAttributes = NULL;
	bool							formatDetectionSupported;

	IDeckLinkDisplayModeIterator*	displayModeIterator = NULL;
	IDeckLinkDisplayMode*			displayMode = NULL;
	char*							displayModeName = NULL;
	BMDDisplayModeSupport			displayModeSupported;

	DeckLinkCaptureDelegate*		delegate = NULL;

	pthread_mutex_init(&g_sleepMutex, NULL);
	pthread_cond_init(&g_sleepCond, NULL);

	signal(SIGINT, sigfunc);
	signal(SIGTERM, sigfunc);
	signal(SIGHUP, sigfunc);

	// Process the command line arguments
	if (!g_config.ParseArguments(argc, argv))
	{
		g_config.DisplayUsage(exitStatus);
		goto bail;
	}

	// Get the DeckLink device
	deckLinkIterator = CreateDeckLinkIteratorInstance();
	if (!deckLinkIterator)
	{
		fprintf(stderr, "This application requires the DeckLink drivers installed.\n");
		goto bail;
	}

	idx = g_config.m_deckLinkIndex;

	while ((result = deckLinkIterator->Next(&deckLink)) == S_OK)
	{
		if (idx == 0)
			break;
		--idx;

		deckLink->Release();
	}

	if (result != S_OK || deckLink == NULL)
	{
		fprintf(stderr, "Unable to get DeckLink device %u\n", g_config.m_deckLinkIndex);
		goto bail;
	}

	// Get the input (capture) interface of the DeckLink device
	result = deckLink->QueryInterface(IID_IDeckLinkInput, (void**)&g_deckLinkInput);
	if (result != S_OK)
		goto bail;

	// Get the display mode
	if (g_config.m_displayModeIndex == -1)
	{
		// Check the card supports format detection
		result = deckLink->QueryInterface(IID_IDeckLinkAttributes, (void**)&deckLinkAttributes);
		if (result == S_OK)
		{
			result = deckLinkAttributes->GetFlag(BMDDeckLinkSupportsInputFormatDetection, &formatDetectionSupported);
			if (result != S_OK || !formatDetectionSupported)
			{
				fprintf(stderr, "Format detection is not supported on this device\n");
				goto bail;
			}
		}

		g_config.m_inputFlags |= bmdVideoInputEnableFormatDetection;

		// Format detection still needs a valid mode to start with
		idx = 0;
	}
	else
	{
		idx = g_config.m_displayModeIndex;
	}

	result = g_deckLinkInput->GetDisplayModeIterator(&displayModeIterator);
	if (result != S_OK)
		goto bail;

	while ((result = displayModeIterator->Next(&displayMode)) == S_OK)
	{
		if (idx == 0)
			break;
		--idx;

		displayMode->Release();
	}

	if (result != S_OK || displayMode == NULL)
	{
		fprintf(stderr, "Unable to get display mode %d\n", g_config.m_displayModeIndex);
		goto bail;
	}

	// Get display mode name
	result = displayMode->GetName((const char**)&displayModeName);
	if (result != S_OK)
	{
		displayModeName = (char *)malloc(32);
		snprintf(displayModeName, 32, "[index %d]", g_config.m_displayModeIndex);
	}

	// Check display mode is supported with given options
	result = g_deckLinkInput->DoesSupportVideoMode(displayMode->GetDisplayMode(), g_config.m_pixelFormat, bmdVideoInputFlagDefault, &displayModeSupported, NULL);
	if (result != S_OK)
		goto bail;

	if (displayModeSupported == bmdDisplayModeNotSupported)
	{
		fprintf(stderr, "The display mode %s is not supported with the selected pixel format\n", displayModeName);
		goto bail;
	}

	if (g_config.m_inputFlags & bmdVideoInputDualStream3D)
	{
		if (!(displayMode->GetFlags() & bmdDisplayModeSupports3D))
		{
			fprintf(stderr, "The display mode %s is not supported with 3D\n", displayModeName);
			goto bail;
		}
	}

	// Print the selected configuration
	g_config.DisplayConfiguration();

	// Configure the capture callback
	delegate = new DeckLinkCaptureDelegate();
	g_deckLinkInput->SetCallback(delegate);

	// Open output files
	if (g_config.m_videoOutputFile != NULL)
	{
		g_videoOutputFile = open(g_config.m_videoOutputFile, O_WRONLY|O_CREAT|O_TRUNC, 0664);
		if (g_videoOutputFile < 0)
		{
			fprintf(stderr, "Could not open video output file \"%s\"\n", g_config.m_videoOutputFile);
			goto bail;
		}
	}

	if (g_config.m_audioOutputFile != NULL)
	{
		g_audioOutputFile = open(g_config.m_audioOutputFile, O_WRONLY|O_CREAT|O_TRUNC, 0664);
		if (g_audioOutputFile < 0)
		{
			fprintf(stderr, "Could not open audio output file \"%s\"\n", g_config.m_audioOutputFile);
			goto bail;
		}
	}

	// Block main thread until signal occurs
	while (!g_do_exit)
	{
		// Start capturing
		result = g_deckLinkInput->EnableVideoInput(displayMode->GetDisplayMode(), g_config.m_pixelFormat, g_config.m_inputFlags);
		if (result != S_OK)
		{
			fprintf(stderr, "Failed to enable video input. Is another application using the card?\n");
			goto bail;
		}

		result = g_deckLinkInput->EnableAudioInput(bmdAudioSampleRate48kHz, g_config.m_audioSampleDepth, g_config.m_audioChannels);
		if (result != S_OK)
			goto bail;

		result = g_deckLinkInput->StartStreams();
		if (result != S_OK)
			goto bail;

		// All Okay.
		exitStatus = 0;

		pthread_mutex_lock(&g_sleepMutex);
		pthread_cond_wait(&g_sleepCond, &g_sleepMutex);
		pthread_mutex_unlock(&g_sleepMutex);

		fprintf(stderr, "Stopping Capture\n");
		g_deckLinkInput->StopStreams();
		g_deckLinkInput->DisableAudioInput();
		g_deckLinkInput->DisableVideoInput();
	}

bail:
	if (g_videoOutputFile != 0)
		close(g_videoOutputFile);

	if (g_audioOutputFile != 0)
		close(g_audioOutputFile);

	if (displayModeName != NULL)
		free(displayModeName);

	if (displayMode != NULL)
		displayMode->Release();

	if (displayModeIterator != NULL)
		displayModeIterator->Release();

	if (g_deckLinkInput != NULL)
	{
		g_deckLinkInput->Release();
		g_deckLinkInput = NULL;
	}

	if (deckLinkAttributes != NULL)
		deckLinkAttributes->Release();

	if (deckLink != NULL)
		deckLink->Release();

	if (deckLinkIterator != NULL)
		deckLinkIterator->Release();

	return exitStatus;
}
