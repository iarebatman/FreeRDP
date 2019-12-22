/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Audio Output Virtual Channel
 *
 * Copyright (c) 2015 Rozhuk Ivan <rozhuk.im@gmail.com>
 * Copyright 2015 Thincast Technologies GmbH
 * Copyright 2015 DI (FH) Martin Haimberger <martin.haimberger@thincast.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <winpr/crt.h>
#include <winpr/string.h>
#include <winpr/cmdline.h>
#include <winpr/sysinfo.h>
#include <winpr/collections.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <sndio.h>

#include <freerdp/types.h>
#include <freerdp/channels/log.h>

#include "rdpsnd_main.h"

typedef struct rdpsnd_sndio_plugin rdpsndSndioPlugin;

struct rdpsnd_sndio_plugin
{
	rdpsndDevicePlugin device;

  struct sio_hdl* device_handle;
  UINT32 volume;


	UINT32 latency;
	AUDIO_FORMAT format;
};

#define SNDIO_LOG_ERR(_text, _error)                                         \
	{                                                                      \
		if (_error != 0)                                                   \
			WLog_ERR(TAG, "%s: %i - %s", _text, _error, strerror(_error)); \
	}

static void onvol_callback(void *device, UINT32 volume)
{
	rdpsndSndioPlugin* sndio = (rdpsndSndioPlugin*)device;
  if(sndio != NULL)
  {
    sndio->volume = volume;
  }
}

static BOOL rdpsnd_sndio_format_supported(rdpsndDevicePlugin* device, const AUDIO_FORMAT* format)
{
  //sndiod allows us to pretty much support any format.
	switch (format->wFormatTag)
    {
		case WAVE_FORMAT_PCM:
			if (format->cbSize == 0 && format->nSamplesPerSec <= 48000 &&
			    (format->wBitsPerSample == 8 || format->wBitsPerSample == 16) &&
			    (format->nChannels == 1 || format->nChannels == 2))
        {
          return TRUE;
        }

			break;
    }

	return FALSE;
}

static BOOL rdpsnd_sndio_set_format(rdpsndDevicePlugin* device, const AUDIO_FORMAT* format,
                                  UINT32 latency)
{
	rdpsndSndioPlugin* sndio = (rdpsndSndioPlugin*)device;

	if (device == NULL || sndio->device_handle == NULL || format == NULL)
		return FALSE;

  struct sio_par par;
  sio_initpar(&par);
  par.bits = format->wBitsPerSample;
  par.rate = format->nSamplesPerSec;
  par.pchan = format->nChannels;

  sio_setpar(sndio->device_handle, &par);

	return TRUE;
}

static BOOL rdpsnd_sndio_open(rdpsndDevicePlugin* device, const AUDIO_FORMAT* format, UINT32 latency)
{
	rdpsndSndioPlugin* sndio = (rdpsndSndioPlugin*)device;

	if (device == NULL || sndio->device_handle != NULL)
		return TRUE;

	WLog_INFO(TAG, "open");

  sndio->device_handle = sio_open(SIO_DEVANY, SIO_PLAY, 0);
  if(sndio->device_handle == NULL)
  {
		SNDIO_LOG_ERR("sound dev open failed", errno);
    return FALSE;
  }
	rdpsnd_sndio_set_format(device, format, latency);
  //sio_onvol(sndio->device_handle, onvol_callback, (void*)device);
  sio_start(sndio->device_handle);
	return TRUE;
}

static void rdpsnd_sndio_close(rdpsndDevicePlugin* device)
{
	rdpsndSndioPlugin* sndio = (rdpsndSndioPlugin*)device;

	if (device == NULL)
		return;

	if (sndio->device_handle != NULL)
	{
		WLog_INFO(TAG, "close: dsp");
		sio_stop(sndio->device_handle);
	}
}

static void rdpsnd_sndio_free(rdpsndDevicePlugin* device)
{
	rdpsndSndioPlugin* sndio = (rdpsndSndioPlugin*)device;

	if (device == NULL || sndio->device_handle == NULL)
		return;

  sio_close(sndio->device_handle);
}

static UINT32 rdpsnd_sndio_get_volume(rdpsndDevicePlugin* device)
{
	rdpsndSndioPlugin* sndio = (rdpsndSndioPlugin*)device;

	if (device == NULL || sndio->device_handle == NULL)
		return -1;

  return sndio->volume;
}

static BOOL rdpsnd_sndio_set_volume(rdpsndDevicePlugin* device, UINT32 value)
{
	rdpsndSndioPlugin* sndio = (rdpsndSndioPlugin*)device;

	if (device == NULL || sndio->device_handle == NULL)
		return FALSE;

  sio_setvol(sndio->device_handle, value);
	return TRUE;
}

static UINT rdpsnd_sndio_play(rdpsndDevicePlugin* device, const BYTE* data, size_t size)
{
	rdpsndSndioPlugin* sndio = (rdpsndSndioPlugin*)device;

	if (device == NULL || sndio->device_handle == NULL)
		return 0;

	while (size > 0)
	{
		size_t status = sio_write(sndio->device_handle, data, size);

		if (status < 0)
		{
			SNDIO_LOG_ERR("write fail", errno);
			rdpsnd_sndio_close(device);
			rdpsnd_sndio_open(device, NULL, sndio->latency);
			break;
		}

		data += status;

		if ((size_t)status <= size)
			size -= (size_t)status;
		else
			size = 0;
	}

	return 10; /* TODO: Get real latency in [ms] */
}


#ifdef BUILTIN_CHANNELS
#define freerdp_rdpsnd_client_subsystem_entry sndio_freerdp_rdpsnd_client_subsystem_entry
#else
#define freerdp_rdpsnd_client_subsystem_entry FREERDP_API freerdp_rdpsnd_client_subsystem_entry
#endif

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
UINT freerdp_rdpsnd_client_subsystem_entry(PFREERDP_RDPSND_DEVICE_ENTRY_POINTS pEntryPoints)
{
	ADDIN_ARGV* args;
	rdpsndSndioPlugin* sndio;
	sndio = (rdpsndSndioPlugin*)calloc(1, sizeof(rdpsndSndioPlugin));

	if (!sndio)
		return CHANNEL_RC_NO_MEMORY;

	sndio->device.Open = rdpsnd_sndio_open;
	sndio->device.FormatSupported = rdpsnd_sndio_format_supported;
	sndio->device.GetVolume = rdpsnd_sndio_get_volume;
	sndio->device.SetVolume = rdpsnd_sndio_set_volume;
	sndio->device.Play = rdpsnd_sndio_play;
	sndio->device.Close = rdpsnd_sndio_close;
	sndio->device.Free = rdpsnd_sndio_free;
	sndio->device_handle = NULL;
	args = pEntryPoints->args;
	pEntryPoints->pRegisterRdpsndDevice(pEntryPoints->rdpsnd, (rdpsndDevicePlugin*)sndio);
	return CHANNEL_RC_OK;
}
