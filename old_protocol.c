/* Copyright (C)
* 2015 - John Melton, G0ORX/N6LYT
*
* This program is free software; you can redistribute it and/or
* modify it under the terms of the GNU General Public License
* as published by the Free Software Foundation; either version 2
* of the License, or (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*
*/

#include <gtk/gtk.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <net/if_arp.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <semaphore.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <wdsp.h>

#include "audio.h"
#include "band.h"
#include "channel.h"
#include "discovered.h"
#include "mode.h"
#include "filter.h"
#include "old_protocol.h"
#include "radio.h"
#include "receiver.h"
#include "transmitter.h"
#include "signal.h"
#include "toolbar.h"
#include "vfo.h"
#ifdef FREEDV
#include "freedv.h"
#endif
#ifdef PSK
#include "psk.h"
#endif
#include "vox.h"

#define min(x,y) (x<y?x:y)

#define SYNC0 0
#define SYNC1 1
#define SYNC2 2
#define C0 3
#define C1 4
#define C2 5
#define C3 6
#define C4 7

#define DATA_PORT 1024

#define SYNC 0x7F
#define OZY_BUFFER_SIZE 512
//#define OUTPUT_BUFFER_SIZE 1024

// ozy command and control
#define MOX_DISABLED    0x00
#define MOX_ENABLED     0x01

#define MIC_SOURCE_JANUS 0x00
#define MIC_SOURCE_PENELOPE 0x80
#define CONFIG_NONE     0x00
#define CONFIG_PENELOPE 0x20
#define CONFIG_MERCURY  0x40
#define CONFIG_BOTH     0x60
#define PENELOPE_122_88MHZ_SOURCE 0x00
#define MERCURY_122_88MHZ_SOURCE  0x10
#define ATLAS_10MHZ_SOURCE        0x00
#define PENELOPE_10MHZ_SOURCE     0x04
#define MERCURY_10MHZ_SOURCE      0x08
#define SPEED_48K                 0x00
#define SPEED_96K                 0x01
#define SPEED_192K                0x02
#define SPEED_384K                0x03
#define MODE_CLASS_E              0x01
#define MODE_OTHERS               0x00
#define ALEX_ATTENUATION_0DB      0x00
#define ALEX_ATTENUATION_10DB     0x01
#define ALEX_ATTENUATION_20DB     0x02
#define ALEX_ATTENUATION_30DB     0x03
#define LT2208_GAIN_OFF           0x00
#define LT2208_GAIN_ON            0x04
#define LT2208_DITHER_OFF         0x00
#define LT2208_DITHER_ON          0x08
#define LT2208_RANDOM_OFF         0x00
#define LT2208_RANDOM_ON          0x10

//static int buffer_size=BUFFER_SIZE;

static int display_width;

static int speed;

static int dsp_rate=48000;
static int output_rate=48000;

static int data_socket;
static struct sockaddr_in data_addr;
static int data_addr_length;

static int output_buffer_size;

static unsigned char control_in[5]={0x00,0x00,0x00,0x00,0x00};

static double tuning_phase;
static double phase=0.0;

static int running;
static long ep4_sequence;

static int current_rx=0;

static int samples=0;
static int mic_samples=0;
static int mic_sample_divisor=1;
#ifdef FREEDV
static int freedv_samples=0;
static int freedv_divisor=6;
#endif
#ifdef PSK
static int psk_samples=0;
static int psk_divisor=6;
#endif

//static float left_input_buffer[BUFFER_SIZE];
//static float right_input_buffer[BUFFER_SIZE];
//static double iqinputbuffer[MAX_RECEIVERS][MAX_BUFFER_SIZE*2];

//static float mic_left_buffer[MAX_BUFFER_SIZE];
//static float mic_right_buffer[MAX_BUFFER_SIZE];
static double micinputbuffer[MAX_BUFFER_SIZE*2];

//static float left_output_buffer[OUTPUT_BUFFER_SIZE];
//static float right_output_buffer[OUTPUT_BUFFER_SIZE];
//static double audiooutputbuffer[MAX_BUFFER_SIZE*2];

//static float left_subrx_output_buffer[OUTPUT_BUFFER_SIZE];
//static float right_subrx_output_buffer[OUTPUT_BUFFER_SIZE];

//static float left_tx_buffer[OUTPUT_BUFFER_SIZE];
//static float right_tx_buffer[OUTPUT_BUFFER_SIZE];
//static double iqoutputbuffer[MAX_BUFFER_SIZE*2];

static int left_rx_sample;
static int right_rx_sample;
static int left_tx_sample;
static int right_tx_sample;

static unsigned char output_buffer[OZY_BUFFER_SIZE];
static int output_buffer_index=8;

static int command=0;

static GThread *receive_thread_id;
static void start_receive_thread();
static gpointer receive_thread(gpointer arg);
static void process_ozy_input_buffer(char  *buffer);
static void process_bandscope_buffer(char  *buffer);
void ozy_send_buffer();

static unsigned char metis_buffer[1032];
static long send_sequence=-1;
static int metis_offset=8;

static int frequencyChanged=0;
static sem_t frequency_changed_sem;

static int metis_write(unsigned char ep,char* buffer,int length);
static void metis_start_stop(int command);
static void metis_send_buffer(char* buffer,int length);
static void metis_restart();

#define COMMON_MERCURY_FREQUENCY 0x80
#define PENELOPE_MIC 0x80

#ifdef USBOZY
//
// additional defines if we include USB Ozy support
//
#include "ozyio.h"

static GThread *ozy_EP4_rx_thread_id;
static GThread *ozy_EP6_rx_thread_id;
static gpointer ozy_ep4_rx_thread(gpointer arg);
static gpointer ozy_ep6_rx_thread(gpointer arg);
static void start_usb_receive_threads();
static int ozyusb_write(char* buffer,int length);
#define EP6_IN_ID  0x86                         // end point = 6, direction toward PC
#define EP2_OUT_ID  0x02                        // end point = 2, direction from PC
#define EP6_BUFFER_SIZE 2048
static unsigned char usb_output_buffer[EP6_BUFFER_SIZE];
static unsigned char ep6_inbuffer[EP6_BUFFER_SIZE];
static unsigned char usb_buffer_block = 0;
#endif

void schedule_frequency_changed() {
    frequencyChanged=1;
}

void old_protocol_stop() {
  metis_start_stop(0);
}

void old_protocol_run() {
  metis_restart();
}

void old_protocol_set_mic_sample_rate(int rate) {
  mic_sample_divisor=rate/48000;
}

void old_protocol_init(int rx,int pixels,int rate) {
  int i;

  fprintf(stderr,"old_protocol_init\n");

  old_protocol_set_mic_sample_rate(rate);

  if(transmitter->local_microphone) {
    if(audio_open_input()!=0) {
      fprintf(stderr,"audio_open_input failed\n");
      transmitter->local_microphone=0;
    }
  }

  display_width=pixels;
 
#ifdef USBOZY
//
// if we have a USB interfaced Ozy device:
//
  if (device == DEVICE_OZY) {
    fprintf(stderr,"old_protocol_init: initialise ozy on USB\n");
    ozy_initialise();
    start_usb_receive_threads();
  }
  else
#endif
  start_receive_thread();

  fprintf(stderr,"old_protocol_init: prime radio\n");
  for(i=8;i<OZY_BUFFER_SIZE;i++) {
    output_buffer[i]=0;
  }

  metis_restart();

}

#ifdef USBOZY
//
// starts the threads for USB receive
// EP4 is the bandscope endpoint (not yet used)
// EP6 is the "normal" USB frame endpoint
//
static void start_usb_receive_threads()
{
  int rc;

  fprintf(stderr,"old_protocol starting USB receive thread: buffer_size=%d\n",buffer_size);

  ozy_EP6_rx_thread_id = g_thread_new( "OZY EP6 RX", ozy_ep6_rx_thread, NULL);
  if( ! ozy_EP6_rx_thread_id )
  {
    fprintf(stderr,"g_thread_new failed for ozy_ep6_rx_thread\n");
    exit( -1 );
  }
}

//
// receive threat for USB EP4 (bandscope) not currently used.
//
static gpointer ozy_ep4_rx_thread(gpointer arg)
{
}

//
// receive threat for USB EP6 (512 byte USB Ozy frames)
// this function loops reading 4 frames at a time through USB
// then processes them one at a time.
//
static gpointer ozy_ep6_rx_thread(gpointer arg) {
  int bytes;
  unsigned char buffer[2048];

  fprintf(stderr, "old_protocol: USB EP6 receive_thread\n");
  running=1;
 
  while (running)
  {
    bytes = ozy_read(EP6_IN_ID,ep6_inbuffer,EP6_BUFFER_SIZE); // read a 2K buffer at a time

    if (bytes == 0)
    {
      fprintf(stderr,"old_protocol_ep6_read: ozy_read returned 0 bytes... retrying\n");
      continue;
    }
    else if (bytes != EP6_BUFFER_SIZE)
    {
      fprintf(stderr,"old_protocol_ep6_read: OzyBulkRead failed %d bytes\n",bytes);
      perror("ozy_read(EP6 read failed");
      //exit(1);
    }
    else
// process the received data normally
    {
      process_ozy_input_buffer(&ep6_inbuffer[0]);
      process_ozy_input_buffer(&ep6_inbuffer[512]);
      process_ozy_input_buffer(&ep6_inbuffer[1024]);
      process_ozy_input_buffer(&ep6_inbuffer[1024+512]);
    }

  }
}
#endif

static void start_receive_thread() {
  int i;
  int rc;
  struct hostent *h;

  fprintf(stderr,"old_protocol starting receive thread: buffer_size=%d output_buffer_size=%d\n",buffer_size,output_buffer_size);

  switch(device) {
#ifdef USBOZY
    case DEVICE_OZY:
      break;
#endif
    default:
      data_socket=socket(PF_INET,SOCK_DGRAM,IPPROTO_UDP);
      if(data_socket<0) {
        perror("old_protocol: create socket failed for data_socket\n");
        exit(-1);
      }

      int optval = 1;
      setsockopt(data_socket, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

      // bind to the interface
      if(bind(data_socket,(struct sockaddr*)&radio->info.network.interface_address,radio->info.network.interface_length)<0) {
        perror("old_protocol: bind socket failed for data_socket\n");
        exit(-1);
      }

      memcpy(&data_addr,&radio->info.network.address,radio->info.network.address_length);
      data_addr_length=radio->info.network.address_length;
      data_addr.sin_port=htons(DATA_PORT);
      break;
  }

  receive_thread_id = g_thread_new( "old protocol", receive_thread, NULL);
  if( ! receive_thread_id )
  {
    fprintf(stderr,"g_thread_new failed on receive_thread\n");
    exit( -1 );
  }
  fprintf(stderr, "receive_thread: id=%p\n",receive_thread_id);

}

static gpointer receive_thread(gpointer arg) {
  struct sockaddr_in addr;
  int length;
  unsigned char buffer[2048];
  int bytes_read;
  int ep;
  long sequence;

  fprintf(stderr, "old_protocol: receive_thread\n");
  running=1;

  length=sizeof(addr);
  while(running) {

    switch(device) {
#ifdef USBOZY
      case DEVICE_OZY:
        // should not happen
        break;
#endif

      default:
        bytes_read=recvfrom(data_socket,buffer,sizeof(buffer),0,(struct sockaddr*)&addr,&length);
        if(bytes_read<0) {
          perror("recvfrom socket failed for old_protocol: receive_thread");
          exit(1);
        }

        if(buffer[0]==0xEF && buffer[1]==0xFE) {
          switch(buffer[2]) {
            case 1:
              // get the end point
              ep=buffer[3]&0xFF;

              // get the sequence number
              sequence=((buffer[4]&0xFF)<<24)+((buffer[5]&0xFF)<<16)+((buffer[6]&0xFF)<<8)+(buffer[7]&0xFF);

              switch(ep) {
                case 6: // EP6
                  // process the data
                  process_ozy_input_buffer(&buffer[8]);
                  process_ozy_input_buffer(&buffer[520]);
                  break;
                case 4: // EP4
/*
                  ep4_sequence++;
                  if(sequence!=ep4_sequence) {
                    ep4_sequence=sequence;
                  } else {
                    int seq=(int)(sequence%32L);
                    if((sequence%32L)==0L) {
                      reset_bandscope_buffer_index();
                    }
                    process_bandscope_buffer(&buffer[8]);
                    process_bandscope_buffer(&buffer[520]);
                  }
*/
                  break;
                default:
                  fprintf(stderr,"unexpected EP %d length=%d\n",ep,bytes_read);
                  break;
              }
              break;
            case 2:  // response to a discovery packet
              fprintf(stderr,"unexepected discovery response when not in discovery mode\n");
              break;
            default:
              fprintf(stderr,"unexpected packet type: 0x%02X\n",buffer[2]);
              break;
          }
        } else {
          fprintf(stderr,"received bad header bytes on data port %02X,%02X\n",buffer[0],buffer[1]);
        }
        break;
    }
  }
}

static void process_ozy_input_buffer(char  *buffer) {
  int i,j;
  int r;
  int b=0;
  unsigned char ozy_samples[8*8];
  int bytes;
  int previous_ptt;
  int previous_dot;
  int previous_dash;
  int left_sample;
  int right_sample;
  short mic_sample;
  double left_sample_double;
  double right_sample_double;
  double mic_sample_double;
  double gain=pow(10.0, mic_gain / 20.0);

  if(buffer[b++]==SYNC && buffer[b++]==SYNC && buffer[b++]==SYNC) {
    // extract control bytes
    control_in[0]=buffer[b++];
    control_in[1]=buffer[b++];
    control_in[2]=buffer[b++];
    control_in[3]=buffer[b++];
    control_in[4]=buffer[b++];

    previous_ptt=ptt;
    previous_dot=dot;
    previous_dash=dash;
    ptt=(control_in[0]&0x01)==0x01;
    dash=(control_in[0]&0x02)==0x02;
    dot=(control_in[0]&0x04)==0x04;

if(ptt!=previous_ptt) {
  fprintf(stderr,"ptt=%d\n",ptt);
}
if(dot!=previous_dot) {
  fprintf(stderr,"dot=%d\n",dot);
}
if(dash!=previous_dash) {
  fprintf(stderr,"dash=%d\n",dash);
}

    if(previous_ptt!=ptt || dot!=previous_dot || dash!=previous_dash) {
      g_idle_add(ptt_update,(gpointer)(ptt | dot | dash));
    }

    switch((control_in[0]>>3)&0x1F) {
      case 0:
        adc_overload=control_in[1]&0x01;
        IO1=(control_in[1]&0x02)?0:1;
        IO2=(control_in[1]&0x04)?0:1;
        IO3=(control_in[1]&0x08)?0:1;
        if(mercury_software_version!=control_in[2]) {
          mercury_software_version=control_in[2];
          fprintf(stderr,"  Mercury Software version: %d (0x%0X)\n",mercury_software_version,mercury_software_version);
        }
        if(penelope_software_version!=control_in[3]) {
          penelope_software_version=control_in[3];
          fprintf(stderr,"  Penelope Software version: %d (0x%0X)\n",penelope_software_version,penelope_software_version);
        }
        if(ozy_software_version!=control_in[4]) {
          ozy_software_version=control_in[4];
          fprintf(stderr,"FPGA firmware version: %d.%d\n",ozy_software_version/10,ozy_software_version%10);
        }
        break;
      case 1:
        exciter_power=((control_in[1]&0xFF)<<8)|(control_in[2]&0xFF); // from Penelope or Hermes
        alex_forward_power=((control_in[3]&0xFF)<<8)|(control_in[4]&0xFF); // from Alex or Apollo
        break;
      case 2:
        alex_reverse_power=((control_in[1]&0xFF)<<8)|(control_in[2]&0xFF); // from Alex or Apollo
        AIN3=(control_in[3]<<8)+control_in[4]; // from Pennelope or Hermes
        break;
      case 3:
        AIN4=(control_in[1]<<8)+control_in[2]; // from Pennelope or Hermes
        AIN6=(control_in[3]<<8)+control_in[4]; // from Pennelope or Hermes
        break;
    }


    int iq_samples=(512-8)/((RECEIVERS*6)+2);

    for(i=0;i<iq_samples;i++) {
      for(r=0;r<RECEIVERS;r++) {
        left_sample   = (int)((signed char) buffer[b++])<<16;
        left_sample  |= (int)((((unsigned char)buffer[b++])<<8)&0xFF00);
        left_sample  |= (int)((unsigned char)buffer[b++]&0xFF);
        right_sample  = (int)((signed char)buffer[b++]) << 16;
        right_sample |= (int)((((unsigned char)buffer[b++])<<8)&0xFF00);
        right_sample |= (int)((unsigned char)buffer[b++]&0xFF);

        left_sample_double=(double)left_sample/8388607.0; // 24 bit sample 2^23-1
        right_sample_double=(double)right_sample/8388607.0; // 24 bit sample 2^23-1

        add_iq_samples(receiver[r], left_sample_double,right_sample_double);
      }
      mic_sample  = (short)(buffer[b++]<<8);
      mic_sample |= (short)(buffer[b++]&0xFF);
      if(!transmitter->local_microphone) {
        mic_samples++;
        if(mic_samples>=mic_sample_divisor) { // reduce to 48000
          add_mic_sample(transmitter,mic_sample);
          mic_samples=0;
        }
      }
    }
  } else {
    time_t t;
    struct tm* gmt;
    time(&t);
    gmt=gmtime(&t);

    fprintf(stderr,"%s: process_ozy_input_buffer: did not find sync: restarting\n",
            asctime(gmt));


    metis_start_stop(0);
    metis_restart();
  }
}

void old_protocol_audio_samples(RECEIVER *rx,short left_audio_sample,short right_audio_sample) {
  if(!isTransmitting()) {
    output_buffer[output_buffer_index++]=left_audio_sample>>8;
    output_buffer[output_buffer_index++]=left_audio_sample;
    output_buffer[output_buffer_index++]=right_audio_sample>>8;
    output_buffer[output_buffer_index++]=right_audio_sample;
    output_buffer[output_buffer_index++]=0;
    output_buffer[output_buffer_index++]=0;
    output_buffer[output_buffer_index++]=0;
    output_buffer[output_buffer_index++]=0;
    if(output_buffer_index>=OZY_BUFFER_SIZE) {
      ozy_send_buffer();
      output_buffer_index=8;
    }
  }
}

void old_protocol_iq_samples(int isample,int qsample) {
  if(isTransmitting()) {
    output_buffer[output_buffer_index++]=0;
    output_buffer[output_buffer_index++]=0;
    output_buffer[output_buffer_index++]=0;
    output_buffer[output_buffer_index++]=0;
    output_buffer[output_buffer_index++]=isample>>8;
    output_buffer[output_buffer_index++]=isample;
    output_buffer[output_buffer_index++]=qsample>>8;
    output_buffer[output_buffer_index++]=qsample;
    if(output_buffer_index>=OZY_BUFFER_SIZE) {
      ozy_send_buffer();
      output_buffer_index=8;
    }
  }
}


void old_protocol_process_local_mic(unsigned char *buffer,int le) {
  int b;
  short mic_sample;
// always 48000 samples per second
  b=0;
  int i,j,s;
  for(i=0;i<720;i++) {
    if(le) {
      mic_sample = (short)((buffer[b++]&0xFF) | (buffer[b++]<<8));
    } else {
      mic_sample = (short)((buffer[b++]<<8) | (buffer[b++]&0xFF));
    }
    add_mic_sample(transmitter,mic_sample);
  }
}

/*
static void process_bandscope_buffer(char  *buffer) {
}
*/



void ozy_send_buffer() {

  int mode;
  int i;
  BAND *band;

  output_buffer[SYNC0]=SYNC;
  output_buffer[SYNC1]=SYNC;
  output_buffer[SYNC2]=SYNC;

  switch(command) {
    case 0:
      {
   
      output_buffer[C0]=0x00;

      output_buffer[C1]=0x00;
      switch(active_receiver->sample_rate) {
        case 48000:
          output_buffer[C1]|=SPEED_48K;
          break;
        case 96000:
          output_buffer[C1]|=SPEED_96K;
          break;
        case 192000:
          output_buffer[C1]|=SPEED_192K;
          break;
        case 384000:
          output_buffer[C1]|=SPEED_384K;
          break;
      }

// set more bits for Atlas based device
// CONFIG_BOTH seems to be critical to getting ozy to respond
#ifdef USBOZY
      if ((device == DEVICE_OZY) || (device == DEVICE_METIS))
#else
      if (device == DEVICE_METIS)
#endif
      {
        if (atlas_mic_source)
          output_buffer[C1] |= PENELOPE_MIC;
        output_buffer[C1] |= CONFIG_BOTH;
        if (atlas_clock_source_128mhz)
          output_buffer[C1] |= MERCURY_122_88MHZ_SOURCE;
        output_buffer[C1] |= ((atlas_clock_source_10mhz & 3) << 2);
      }

      output_buffer[C2]=0x00;
      if(classE) {
        output_buffer[C2]|=0x01;
      }
      band=band_get_band(vfo[VFO_A].band);
      if(isTransmitting()) {
        if(split) {
          band=band_get_band(vfo[VFO_B].band);
        }
        output_buffer[C2]|=band->OCtx<<1;
        if(tune) {
          if(OCmemory_tune_time!=0) {
            struct timeval te;
            gettimeofday(&te,NULL);
            long long now=te.tv_sec*1000LL+te.tv_usec/1000;
            if(tune_timeout>now) {
              output_buffer[C2]|=OCtune<<1;
            }
          } else {
            output_buffer[C2]|=OCtune<<1;
          }
        }
      } else {
        output_buffer[C2]|=band->OCrx<<1;
      }

// TODO - add Alex Attenuation and Alex Antenna
      output_buffer[C3]=0x00;
      if(active_receiver->random) {
        output_buffer[C3]|=LT2208_RANDOM_ON;
      }
      if(active_receiver->dither) {
        output_buffer[C3]|=LT2208_DITHER_ON;
      }
      if(active_receiver->preamp) {
        output_buffer[C3]|=LT2208_GAIN_ON;
      }

      output_buffer[C3] |= active_receiver->alex_attenuation;

      switch(receiver[0]->alex_antenna) {
        case 0:  // ANT 1
          break;
        case 1:  // ANT 2
          break;
        case 2:  // ANT 3
          break;
        case 3:  // EXT 1
          //output_buffer[C3]|=0xA0;
          output_buffer[C3]|=0xC0;
          break;
        case 4:  // EXT 2
          //output_buffer[C3]|=0xC0;
          output_buffer[C3]|=0xA0;
          break;
        case 5:  // XVTR
          output_buffer[C3]|=0xE0;
          break;
        default:
          break;
      }


// TODO - add Alex TX relay, duplex, receivers Mercury board frequency
      output_buffer[C4]=0x04;  // duplex
      output_buffer[C4]|=(RECEIVERS-1)<<3;
    
      if(isTransmitting()) {
        switch(transmitter->alex_antenna) {
          case 0:  // ANT 1
            output_buffer[C4]|=0x00;
            break;
          case 1:  // ANT 2
            output_buffer[C4]|=0x01;
            break;
          case 2:  // ANT 3
            output_buffer[C4]|=0x02;
            break;
          default:
            break;
        }
      } else {
        switch(receiver[0]->alex_antenna) {
          case 0:  // ANT 1
            output_buffer[C4]|=0x00;
            break;
          case 1:  // ANT 2
            output_buffer[C4]|=0x01;
            break;
          case 2:  // ANT 3
            output_buffer[C4]|=0x02;
            break;
          case 3:  // EXT 1
          case 4:  // EXT 2
          case 5:  // XVTR
            switch(transmitter->alex_antenna) {
              case 0:  // ANT 1
                output_buffer[C4]|=0x00;
                break;
              case 1:  // ANT 2
                output_buffer[C4]|=0x01;
                break;
              case 2:  // ANT 3
                output_buffer[C4]|=0x02;
                break;
            }
            break;
        }
      }
      }
      break;
    case 1: // tx frequency
      output_buffer[C0]=0x02;
      long long txFrequency;
      if(split) {
        txFrequency=vfo[VFO_B].frequency-vfo[VFO_A].lo+vfo[VFO_B].offset;
      } else {
        txFrequency=vfo[VFO_A].frequency-vfo[VFO_B].lo+vfo[VFO_A].offset;
      }
      output_buffer[C1]=txFrequency>>24;
      output_buffer[C2]=txFrequency>>16;
      output_buffer[C3]=txFrequency>>8;
      output_buffer[C4]=txFrequency;
      break;
    case 2: // rx frequency
      if(current_rx<receivers) {
        output_buffer[C0]=0x04+(current_rx*2);
        int v=receiver[current_rx]->id;
        long long rxFrequency=vfo[v].frequency-vfo[v].lo;
        if(vfo[v].rit_enabled) {
          rxFrequency+=vfo[v].rit;
        }
        if(vfo[active_receiver->id].mode==modeCWU) {
          rxFrequency-=(long long)cw_keyer_sidetone_frequency;
        } else if(vfo[active_receiver->id].mode==modeCWL) {
          rxFrequency+=(long long)cw_keyer_sidetone_frequency;
        }
        output_buffer[C1]=rxFrequency>>24;
        output_buffer[C2]=rxFrequency>>16;
        output_buffer[C3]=rxFrequency>>8;
        output_buffer[C4]=rxFrequency;
        current_rx++;
      }
      if(current_rx>=receivers) {
        current_rx=0;
      }
      break;
    case 3:
      {
      BAND *band=band_get_current_band();
      int power=0;
      if(isTransmitting()) {
        if(tune) {
          power=tune_drive_level;
        } else {
          power=drive_level;
        }
      }

      output_buffer[C0]=0x12;
      output_buffer[C1]=power&0xFF;
      output_buffer[C2]=0x00;
      if(mic_boost) {
        output_buffer[C2]|=0x01;
      }
      if(mic_linein) {
        output_buffer[C2]|=0x02;
      }
      if(filter_board==APOLLO) {
        output_buffer[C2]|=0x2C;
      }
      if((filter_board==APOLLO) && tune) {
        output_buffer[C2]|=0x10;
      }
      output_buffer[C3]=0x00;
      if(band_get_current()==band6) {
        output_buffer[C3]=output_buffer[C3]|0x40; // Alex 6M low noise amplifier
      }
      if(band->disablePA) {
        output_buffer[C3]=output_buffer[C3]|0x80; // disable PA
      }
      output_buffer[C4]=0x00;
      }
      break;
    case 4:
      output_buffer[C0]=0x14;
      output_buffer[C1]=0x00;
      for(i=0;i<receivers;i++) {
        output_buffer[C1]|=(receiver[i]->preamp<<i);
      }
      if(mic_ptt_enabled==0) {
        output_buffer[C1]|=0x40;
      }
      if(mic_bias_enabled) {
        output_buffer[C1]|=0x20;
      }
      if(mic_ptt_tip_bias_ring) {
        output_buffer[C1]|=0x10;
      }
      output_buffer[C2]=linein_gain;
      output_buffer[C3]=0x00;

      if(radio->device==DEVICE_HERMES || radio->device==DEVICE_ANGELIA || radio->device==DEVICE_ORION || radio->device==DEVICE_ORION2) {
        output_buffer[C4]=0x20|receiver[0]->attenuation;
      } else {
        output_buffer[C4]=0x00;
      }
      break;
    case 5:
      // need to add adc 2 and 3 attenuation
      output_buffer[C0]=0x16;
      output_buffer[C1]=0x00;
      if(receivers==2) {
        if(radio->device==DEVICE_HERMES || radio->device==DEVICE_ANGELIA || radio->device==DEVICE_ORION || radio->device==DEVICE_ORION2) {
          output_buffer[C1]=0x20|receiver[1]->attenuation;
        }
      }
      output_buffer[C2]=0x00;
      if(cw_keys_reversed!=0) {
        output_buffer[C2]|=0x40;
      }
      output_buffer[C3]=cw_keyer_speed | (cw_keyer_mode<<6);
      output_buffer[C4]=cw_keyer_weight | (cw_keyer_spacing<<7);
      break;
    case 6:
      // need to add tx attenuation and rx ADC selection
      output_buffer[C0]=0x1C;
      output_buffer[C1]=0x00;
      output_buffer[C2]=0x00;
      output_buffer[C3]=0x00;
      output_buffer[C4]=0x00;
      break;
    case 7:
      output_buffer[C0]=0x1E;
      if(split) {
        mode=vfo[1].mode;
      } else {
        mode=vfo[0].mode;
      }
      if(mode!=modeCWU && mode!=modeCWL) {
        // output_buffer[C1]|=0x00;
      } else {
        if((tune==1) || (vox==1) || (cw_keyer_internal==0)) {
          output_buffer[C1]|=0x00;
        } else if(mox==1) {
          output_buffer[C1]|=0x01;
        }
      }
      output_buffer[C2]=cw_keyer_sidetone_volume;
      output_buffer[C3]=cw_keyer_ptt_delay;
      output_buffer[C4]=0x00;
      break;
    case 8:
      output_buffer[C0]=0x20;
      output_buffer[C1]=(cw_keyer_hang_time>>2) & 0xFF;
      output_buffer[C2]=cw_keyer_hang_time & 0x03;
      output_buffer[C3]=(cw_keyer_sidetone_frequency>>4) & 0xFF;
      output_buffer[C4]=cw_keyer_sidetone_frequency & 0x0F;
      break;
    case 9:
      output_buffer[C0]=0x22;
      output_buffer[C1]=(eer_pwm_min>>2) & 0xFF;
      output_buffer[C2]=eer_pwm_min & 0x03;
      output_buffer[C3]=(eer_pwm_max>>3) & 0xFF;
      output_buffer[C4]=eer_pwm_max & 0x03;
      break;
  }

  // set mox
  if(split) {
    mode=vfo[1].mode;
  } else {
    mode=vfo[0].mode;
  }
  if(mode==modeCWU || mode==modeCWL) {
    if(tune) {
      output_buffer[C0]|=0x01;
    }
  } else {
    if(isTransmitting()) {
      output_buffer[C0]|=0x01;
    }
  }

#ifdef USBOZY
//
// if we have a USB interfaced Ozy device:
//
  if (device == DEVICE_OZY)
        ozyusb_write(output_buffer,OZY_BUFFER_SIZE);
  else
#endif
  metis_write(0x02,output_buffer,OZY_BUFFER_SIZE);

  command++;
  if(command>9) {
    command=0;
  }

  //fprintf(stderr,"C0=%02X C1=%02X C2=%02X C3=%02X C4=%02X\n",
  //                output_buffer[C0],output_buffer[C1],output_buffer[C2],output_buffer[C3],output_buffer[C4]);
}

#ifdef USBOZY
static int ozyusb_write(char* buffer,int length)
{
  int i;

// batch up 4 USB frames (2048 bytes) then do a USB write
  switch(usb_buffer_block++)
  {
    case 0:
    default:
      memcpy(usb_output_buffer, buffer, length);
      break;

    case 1:
      memcpy(usb_output_buffer + 512, buffer, length);
      break;

    case 2:
      memcpy(usb_output_buffer + 1024, buffer, length);
      break;

    case 3:
      memcpy(usb_output_buffer + 1024 + 512, buffer, length);
      usb_buffer_block = 0;           // reset counter
// and write the 4 usb frames to the usb in one 2k packet
      i = ozy_write(EP2_OUT_ID,usb_output_buffer,EP6_BUFFER_SIZE);
      if(i != EP6_BUFFER_SIZE)
      {
        perror("old_protocol: OzyWrite ozy failed");
      }
      break;
  }
}
#endif

static int metis_write(unsigned char ep,char* buffer,int length) {
  int i;

  // copy the buffer over
  for(i=0;i<512;i++) {
    metis_buffer[i+metis_offset]=buffer[i];
  }

  if(metis_offset==8) {
    metis_offset=520;
  } else {
    send_sequence++;
    metis_buffer[0]=0xEF;
    metis_buffer[1]=0xFE;
    metis_buffer[2]=0x01;
    metis_buffer[3]=ep;
    metis_buffer[4]=(send_sequence>>24)&0xFF;
    metis_buffer[5]=(send_sequence>>16)&0xFF;
    metis_buffer[6]=(send_sequence>>8)&0xFF;
    metis_buffer[7]=(send_sequence)&0xFF;


    // send the buffer
    metis_send_buffer(&metis_buffer[0],1032);
    metis_offset=8;

  }

  return length;
}

static void metis_restart() {
  // send commands twice
  command=0;
  do {
    ozy_send_buffer();
  } while (command!=0);

  do {
    ozy_send_buffer();
  } while (command!=0);

  // start the data flowing
  metis_start_stop(1);
}

static void metis_start_stop(int command) {
  int i;
  unsigned char buffer[64];
    
#ifdef USBOZY
  if(device!=DEVICE_OZY)
  {
#endif

  buffer[0]=0xEF;
  buffer[1]=0xFE;
  buffer[2]=0x04;    // start/stop command
  buffer[3]=command;    // send EP6 and EP4 data (0x00=stop)

  for(i=0;i<60;i++) {
    buffer[i+4]=0x00;
  }

  metis_send_buffer(buffer,sizeof(buffer));
#ifdef USBOZY
  }
#endif
}

static void metis_send_buffer(char* buffer,int length) {
  if(sendto(data_socket,buffer,length,0,(struct sockaddr*)&data_addr,data_addr_length)!=length) {
    perror("sendto socket failed for metis_send_data\n");
  }
}


