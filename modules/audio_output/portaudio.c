/*****************************************************************************
 * portaudio.c : portaudio (v19) audio output plugin
 *****************************************************************************
 * Copyright (C) 2002 VideoLAN
 * $Id$
 *
 * Authors: Frederic Ruget <frederic.ruget@free.fr>
 *          Gildas Bazin <gbazin@videolan.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/
#include <string.h>
#include <stdlib.h>

#include <vlc/vlc.h>
#include <vlc/aout.h>
#include <portaudio.h>

#include "aout_internal.h"

#define FRAME_SIZE 1024              /* The size is in samples, not in bytes */

/*****************************************************************************
 * aout_sys_t: portaudio audio output method descriptor
 *****************************************************************************/
typedef struct pa_thread_t
{
    VLC_COMMON_MEMBERS
    aout_instance_t * p_aout;

} pa_thread_t;

struct aout_sys_t
{
    aout_instance_t *p_aout;
    PaStream *p_stream;

    PaDeviceIndex i_devices;
    int i_sample_size;
    PaDeviceIndex i_device_id;
    const PaDeviceInfo *deviceInfo;

    vlc_mutex_t lock;
    vlc_cond_t wait;

    pa_thread_t *pa_thread;
};

/*****************************************************************************
 * Local prototypes.
 *****************************************************************************/
static int  Open        ( vlc_object_t * );
static void Close       ( vlc_object_t * );
static void Play        ( aout_instance_t * );
static void PORTAUDIOThread( pa_thread_t * );

static int PAOpenDevice( aout_instance_t * );
static int PAOpenStream( aout_instance_t * );

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
#define DEVICE_TEXT N_("Output device")
#define DEVICE_LONGTEXT N_("Portaudio identifier for the output device")

vlc_module_begin();
    set_description( N_("PORTAUDIO audio output") );
    add_integer( "portaudio-device", 0, NULL,
                 DEVICE_TEXT, DEVICE_LONGTEXT, VLC_FALSE );
    set_capability( "audio output", 0 );
    set_callbacks( Open, Close );
vlc_module_end();

/* This routine will be called by the PortAudio engine when audio is needed.
 * It may called at interrupt level on some machines so don't do anything
 * that could mess up the system like calling malloc() or free().
 */
static int paCallback( const void *inputBuffer, void *outputBuffer,
                            unsigned long framesPerBuffer,
                            const PaStreamCallbackTimeInfo *paDate,
                            PaStreamCallbackFlags statusFlags,
                            void *p_cookie )
{
    struct aout_sys_t *p_sys = (struct aout_sys_t*) p_cookie;
    aout_instance_t   *p_aout = p_sys->p_aout;
    aout_buffer_t     *p_buffer;
    mtime_t out_date;

    out_date = mdate() + (mtime_t) ( 1000000 *
        ( paDate->outputBufferDacTime - paDate->currentTime ) );
    p_buffer = aout_OutputNextBuffer( p_aout, out_date, VLC_TRUE );

    if ( p_buffer != NULL )
    {
        p_aout->p_vlc->pf_memcpy( outputBuffer, p_buffer->p_buffer,
                                  framesPerBuffer * p_sys->i_sample_size );
        /* aout_BufferFree may be dangereous here, but then so is
         * aout_OutputNextBuffer (calls aout_BufferFree internally).
         * one solution would be to link the no longer useful buffers
         * in a second fifo (in aout_OutputNextBuffer too) and to
         * wait until we are in Play to do the actual free.
         */
        aout_BufferFree( p_buffer );
    }
    else
        /* Audio output buffer shortage -> stop the fill process and wait */
    {
        p_aout->p_vlc->pf_memset( outputBuffer, 0,
                                  framesPerBuffer * p_sys->i_sample_size );
    }
    return 0;
}

/*****************************************************************************
 * Open: open the audio device
 *****************************************************************************/
static int Open( vlc_object_t * p_this )
{
    aout_instance_t *p_aout = (aout_instance_t *)p_this;
    struct aout_sys_t * p_sys;
    vlc_value_t val;
    int i_err;

    msg_Dbg( p_aout, "Entering Open()");

    /* Allocate p_sys structure */
    p_sys = (aout_sys_t *)malloc( sizeof(aout_sys_t) );
    if( p_sys == NULL )
    {
        msg_Err( p_aout, "out of memory" );
        return VLC_ENOMEM;
    }
    p_sys->p_aout = p_aout;
    p_sys->p_stream = 0;
    p_aout->output.p_sys = p_sys;
    p_aout->output.pf_play = Play;

    /* Retrieve output device id from config */
    var_Create( p_aout, "portaudio-device", VLC_VAR_INTEGER|VLC_VAR_DOINHERIT);
    var_Get( p_aout, "portaudio-device", &val );
    p_sys->i_device_id = val.i_int;

    if( PAOpenDevice( p_aout ) != VLC_SUCCESS )
    {
        msg_Err( p_aout, "cannot open portaudio device" );
        free( p_sys );
        return VLC_EGENERIC;
    }

    /* Close device for now. We'll re-open it later on */
    if( ( i_err = Pa_Terminate() ) != paNoError )
    {
        msg_Err( p_aout, "Pa_Terminate returned %d", i_err );
    }

    /* Now we need to setup our DirectSound play notification structure */
    p_sys->pa_thread = vlc_object_create( p_aout, sizeof(pa_thread_t) );
    p_sys->pa_thread->p_aout = p_aout;

    vlc_mutex_init( p_aout, &p_sys->lock );
    vlc_cond_init( p_aout, &p_sys->wait );

    /* Create PORTAUDIOThread */
    if( vlc_thread_create( p_sys->pa_thread, "aout", PORTAUDIOThread,
                           VLC_THREAD_PRIORITY_OUTPUT, VLC_TRUE ) )
    {
        msg_Err( p_aout, "cannot create PORTAUDIO thread" );
        return VLC_EGENERIC;
    }

    if( p_sys->pa_thread->b_error )
    {
        msg_Err( p_aout, "PORTAUDIO thread failed" );
        Close( p_this );
        return VLC_EGENERIC;
    }

    return VLC_SUCCESS;
}

/*****************************************************************************
 * Close: close the audio device
 *****************************************************************************/
static void Close ( vlc_object_t *p_this )
{
    aout_instance_t *p_aout = (aout_instance_t *)p_this;
    aout_sys_t *p_sys = p_aout->output.p_sys;

    msg_Dbg( p_aout, "closing portaudio");

    vlc_mutex_lock( &p_sys->lock );
    p_sys->pa_thread->b_die = VLC_TRUE;
    vlc_cond_signal( &p_sys->wait );
    vlc_mutex_unlock( &p_sys->lock );

    vlc_thread_join( p_sys->pa_thread );
    vlc_cond_destroy( &p_sys->wait );
    vlc_mutex_destroy( &p_sys->lock );

    msg_Dbg( p_aout, "portaudio closed");
    free( p_sys );
}

/*****************************************************************************
 * Play: play sound
 *****************************************************************************/
static void Play( aout_instance_t * p_aout )
{
}

/*****************************************************************************
 * PORTAUDIOThread: all interactions with libportaudio.a are handled
 * in this single thread.  Otherwise libportaudio.a is _not_ happy :-(
 *****************************************************************************/
static void PORTAUDIOThread( pa_thread_t *pa_thread )
{
    aout_instance_t *p_aout = pa_thread->p_aout;
    aout_sys_t *p_sys = p_aout->output.p_sys;
    int i_err;

    if( PAOpenDevice( p_aout ) != VLC_SUCCESS )
    {
        msg_Err( p_aout, "cannot open portaudio device" );
        pa_thread->b_error = VLC_TRUE;
        vlc_thread_ready( pa_thread );
        return;
    }

    if( PAOpenStream( p_aout ) != VLC_SUCCESS )
    {
        msg_Err( p_aout, "cannot open portaudio device" );
        pa_thread->b_error = VLC_TRUE;
        vlc_thread_ready( pa_thread );
        goto end;
    }

    /* Tell the main thread that we are ready */
    vlc_thread_ready( pa_thread );

    vlc_mutex_lock( &p_sys->lock );
    if( !pa_thread->b_die ) vlc_cond_wait( &p_sys->wait, &p_sys->lock );
    vlc_mutex_unlock( &p_sys->lock );

    i_err = Pa_StopStream( p_sys->p_stream );
    if( i_err != paNoError )
    {
        msg_Err( p_aout, "Pa_StopStream: %d (%s)", i_err,
                 Pa_GetErrorText( i_err ) );
    }
    i_err = Pa_CloseStream( p_sys->p_stream );
    if( i_err != paNoError )
    {
        msg_Err( p_aout, "Pa_CloseStream: %d (%s)", i_err,
                 Pa_GetErrorText( i_err ) );
    }

 end:
    i_err = Pa_Terminate();
    if( i_err != paNoError )
    {
        msg_Err( p_aout, "Pa_Terminate: %d (%s)", i_err,
                 Pa_GetErrorText( i_err ) );
    }
}

static int PAOpenDevice( aout_instance_t *p_aout )
{
    aout_sys_t *p_sys = p_aout->output.p_sys;
    const PaDeviceInfo *p_pdi;
    PaError i_err;
    vlc_value_t val, text;
    int i;

    /* Initialize portaudio */
    i_err = Pa_Initialize();
    if( i_err != paNoError )
    {
        msg_Err( p_aout, "Pa_Initialize returned %d : %s",
                 i_err, Pa_GetErrorText( i_err ) );

        return VLC_EGENERIC;
    }

    p_sys->i_devices = Pa_GetDeviceCount();
    if( p_sys->i_devices < 0 )
    {
        i_err = p_sys->i_devices;
        msg_Err( p_aout, "Pa_GetDeviceCount returned %d : %s", i_err,
                 Pa_GetErrorText( i_err ) );

        goto error;
    }

    /* Display all devices info */
    msg_Dbg( p_aout, "number of devices = %d", p_sys->i_devices );
    for( i = 0; i < p_sys->i_devices; i++ )
    {
        p_pdi = Pa_GetDeviceInfo( i );
        msg_Dbg( p_aout, "------------------------------------- #%d", i );
        msg_Dbg( p_aout, "Name         = %s", p_pdi->name );
        msg_Dbg( p_aout, "Max Inputs   = %d, Max Outputs = %d",
                  p_pdi->maxInputChannels, p_pdi->maxOutputChannels );
    }
    msg_Dbg( p_aout, "-------------------------------------" );

    msg_Dbg( p_aout, "requested device is #%d", p_sys->i_device_id );
    if( p_sys->i_device_id >= p_sys->i_devices )
    {
        msg_Err( p_aout, "device %d does not exist", p_sys->i_device_id );
        goto error;
    }
    p_sys->deviceInfo = Pa_GetDeviceInfo( p_sys->i_device_id );

    if( p_sys->deviceInfo->maxOutputChannels < 1 )
    {
        msg_Err( p_aout, "no channel available" );
        goto error;
    }

    if( var_Type( p_aout, "audio-device" ) == 0 )
    {
        var_Create( p_aout, "audio-device", VLC_VAR_INTEGER|VLC_VAR_HASCHOICE);
        text.psz_string = _("Audio Device");
        var_Change( p_aout, "audio-device", VLC_VAR_SETTEXT, &text, NULL );

        if( p_sys->deviceInfo->maxOutputChannels >= 1 )
        {
            val.i_int = AOUT_VAR_MONO;
            text.psz_string = N_("Mono");
            var_Change( p_aout, "audio-device", VLC_VAR_ADDCHOICE,
                        &val, &text );
            msg_Dbg( p_aout, "device supports 1 channel" );
        }
        if( p_sys->deviceInfo->maxOutputChannels >= 2 )
        {
            val.i_int = AOUT_VAR_STEREO;
            text.psz_string = N_("Stereo");
            var_Change( p_aout, "audio-device", VLC_VAR_ADDCHOICE,
                        &val, &text );
            var_Change( p_aout, "audio-device", VLC_VAR_SETDEFAULT,
                        &val, NULL );
            msg_Dbg( p_aout, "device supports 2 channels" );
        }
        if( p_sys->deviceInfo->maxOutputChannels >= 4 )
        {
            val.i_int = AOUT_VAR_2F2R;
            text.psz_string = N_("2 Front 2 Rear");
            var_Change( p_aout, "audio-device", VLC_VAR_ADDCHOICE,
                        &val, &text );
            msg_Dbg( p_aout, "device supports 4 channels" );
        }
        if( p_sys->deviceInfo->maxOutputChannels >= 5 )
        {
            val.i_int = AOUT_VAR_3F2R;
            text.psz_string = N_("3 Front 2 Rear");
            var_Change( p_aout, "audio-device",
                        VLC_VAR_ADDCHOICE, &val, &text );
            msg_Dbg( p_aout, "device supports 5 channels" );
        }
        if( p_sys->deviceInfo->maxOutputChannels >= 6 )
        {
            val.i_int = AOUT_VAR_5_1;
            text.psz_string = N_("5.1");
            var_Change( p_aout, "audio-device", VLC_VAR_ADDCHOICE,
                        &val, &text );
            msg_Dbg( p_aout, "device supports 5.1 channels" );
        }

        var_AddCallback( p_aout, "audio-device", aout_ChannelsRestart, NULL );

        val.b_bool = VLC_TRUE;
        var_Set( p_aout, "intf-change", val );
    }

    /* Audio format is paFloat32 (always supported by portaudio v19) */
    p_aout->output.output.i_format = VLC_FOURCC('f','l','3','2');

    return VLC_SUCCESS;

 error:
    if( ( i_err = Pa_Terminate() ) != paNoError )
    {
        msg_Err( p_aout, "Pa_Terminate returned %d", i_err );
    }
    return VLC_EGENERIC;
}

static int PAOpenStream( aout_instance_t *p_aout )
{
    aout_sys_t *p_sys = p_aout->output.p_sys;
    const PaHostErrorInfo* paLastHostErrorInfo = Pa_GetLastHostErrorInfo();
    PaStreamParameters paStreamParameters;
    vlc_value_t val;
    int i_channels, i_err;

    if( var_Get( p_aout, "audio-device", &val ) < 0 )
    {
        return VLC_EGENERIC;
    }

    if( val.i_int == AOUT_VAR_5_1 )
    {
        p_aout->output.output.i_physical_channels
            = AOUT_CHAN_LEFT | AOUT_CHAN_RIGHT | AOUT_CHAN_CENTER
              | AOUT_CHAN_REARLEFT | AOUT_CHAN_REARRIGHT
              | AOUT_CHAN_LFE;
    }
    else if( val.i_int == AOUT_VAR_3F2R )
    {
        p_aout->output.output.i_physical_channels
            = AOUT_CHAN_LEFT | AOUT_CHAN_RIGHT | AOUT_CHAN_CENTER
            | AOUT_CHAN_REARLEFT | AOUT_CHAN_REARRIGHT;
    }
    else if( val.i_int == AOUT_VAR_2F2R )
    {
        p_aout->output.output.i_physical_channels
            = AOUT_CHAN_LEFT | AOUT_CHAN_RIGHT
            | AOUT_CHAN_REARLEFT | AOUT_CHAN_REARRIGHT;
    }
    else if( val.i_int == AOUT_VAR_MONO )
    {
        p_aout->output.output.i_physical_channels = AOUT_CHAN_CENTER;
    }
    else
    {
        p_aout->output.output.i_physical_channels
            = AOUT_CHAN_LEFT | AOUT_CHAN_RIGHT;
    }

    i_channels = aout_FormatNbChannels( &p_aout->output.output );
    msg_Dbg( p_aout, "nb_channels requested = %d", i_channels );

    /* Calculate the frame size in bytes */
    p_sys->i_sample_size = 4 * i_channels;
    p_aout->output.i_nb_samples = FRAME_SIZE;
    aout_FormatPrepare( &p_aout->output.output );
    aout_VolumeSoftInit( p_aout );

    paStreamParameters.device = p_sys->i_device_id;
    paStreamParameters.channelCount = i_channels;
    paStreamParameters.sampleFormat = paFloat32;
    paStreamParameters.suggestedLatency =
        p_sys->deviceInfo->defaultLowOutputLatency;
    paStreamParameters.hostApiSpecificStreamInfo = NULL;

    i_err = Pa_OpenStream( &p_sys->p_stream, NULL /* no input */,
                &paStreamParameters, (double)p_aout->output.output.i_rate,
                FRAME_SIZE, paClipOff, paCallback, p_sys );
    if( i_err != paNoError )
    {
        msg_Err( p_aout, "Pa_OpenStream returns %d : %s", i_err,
                 Pa_GetErrorText( i_err ) );
        if( i_err == paUnanticipatedHostError )
        {
            msg_Err( p_aout, "type %d code %ld : %s",
                     paLastHostErrorInfo->hostApiType,
                     paLastHostErrorInfo->errorCode,
                     paLastHostErrorInfo->errorText );
        }
        p_sys->p_stream = 0;
        return VLC_EGENERIC;
    }

    i_err = Pa_StartStream( p_sys->p_stream );
    if( i_err != paNoError )
    {
        msg_Err( p_aout, "Pa_StartStream() failed" );
        Pa_CloseStream( p_sys->p_stream );
        return VLC_EGENERIC;
    }

    return VLC_SUCCESS;
}
