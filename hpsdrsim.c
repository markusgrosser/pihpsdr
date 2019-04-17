/*
 * HPSDR simulator, (C) Christoph van Wuellen, April 2019
 *
 * This program simulates a HPSDR board.
 * If an SDR program such as phipsdr "connects" with this program, it
 * writes to stdout what goes on. This is great for debugging.
 *
 * In addition, I have built in the following features:
 *
 * This device has four "RF sources"
 *
 * RF1: ADC noise (16-bit ADC) plus a  800 Hz signal at -100dBm
 * RF2: ADC noise (16-bit ADC) plus a 2000 Hz signal at - 80dBm
 * RF3: (upon TX with sample rate=48000): TX feedback signal with some distortion. This signal is modulated
 *      according to the "TX drive" and "TX ATT" settings.
 * RF4: (upon TX with sample rate=48000): TX signal with a peak value of 0.400
 *
 * RF1 and RF2 are attenuated according to the preamp/attenuator settings.
 * RF3 respects the "TX drive" and "TX ATT" settings
 * RF4 is the TX signal multiplied with 0.4 (ignores TX_DRIVE and TX_ATT).
 *
 * We support 4 receivers, they see
 * RX1:  RF1
 * RX2:  RF2 while receiving, RF3 while transmitting/48000
 * RX3:  RF2 while receiving, RF3 while transmitting/48000
 * RX4:  RF3 
 *
 * RX5 to RX8: no signal
 * 
 * This is the setting for PURESIGNAL with HERMES boards.
 * To simulate Orion2 boards (Anan7000 etc), we should connect RX4 with RF2 and RX5 with RF3.
 *
 */
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <stdbool.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <termios.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#ifndef __APPLE__
// using clock_nanosleep of librt
extern int clock_nanosleep(clockid_t __clock_id, int __flags,
      __const struct timespec *__req,
      struct timespec *__rem);
#endif


static int sock_TCP_Server = -1;
static int sock_TCP_Client = -1;

/*
 * These variables store the state of the SDR.
 * Whenevery they are changed, this is reported.
 */

int		AlexTXrel = -1;
int		alexRXout = -1;
int		alexRXant = -1;
int		MicTS = -1;
int		duplex = -1;
int		receivers = -1;
int		rate = -1;
int             preamp = -1;
int		LTdither = -1;
int		LTrandom = -1;
int		AlexRXant = -1;
int		AlexRXout = -1;
int             ref10 = -1;
int		src122 = -1;
int		PMconfig = -1;
int		MicSrc = -1;
int		txdrive = 0;
int		txatt = 0;
int		sidetone_volume = -1;
int		cw_internal = -1;
int		rx_att[2] = {-1,-1};
int		rx1_attE = -1;
int             rx_preamp[4] = {-1,-1,-1,-1};
int		MerTxATT0 = -1;
int		MerTxATT1 = -1;
int		MetisDB9 = -1;
int		PeneSel = -1;
int		PureSignal = -1;
int		LineGain = -1;
int		MicPTT = -1;
int		tip_ring = -1;
int		MicBias = -1;
int		ptt=-1;
int		att=-1;
int		TX_class_E = -1;
int		OpenCollectorOutputs=-1;
long		tx_freq=-1;
long		rx_freq[7] = {-1,-1,-1,-1,-1,-1,-1};
int		hermes_config=-1;
int		alex_lpf=-1;
int		alex_hpf=-1;
int		c25_ext_board_i2c_data=-1;
int		rx_adc[7]={0,1,1,2,-1,-1,-1};
int		cw_hang = -1;
int		cw_reversed = -1;
int		cw_speed = -1;
int		cw_mode = -1;
int		cw_weight = -1;
int		cw_spacing = -1;
int		cw_delay = -1;
int		CommonMercuryFreq = -1;
int             freq=-1;


// floating-point represeners of TX att, RX att, and RX preamp settings

double txdrv_dbl = 1.0;
double txatt_dbl = 1.0;
double rxatt_dbl[4] = {1.0, 1.0, 1.0, 1.0};   // this reflects both ATT and PREAMP

int sock_ep2;

struct sockaddr_in addr_ep6;

/*
 * These two variables monitor whether the TX thread is active
 */
int enable_thread = 0;
int active_thread = 0;

void process_ep2(uint8_t *frame);
void *handler_ep6(void *arg);


/*
 * The TX data ring buffer
 */

// 63 * 130,  RTXLEN must be an even multiple of 63!
#define RTXLEN 8190
double  isample[RTXLEN];
double  qsample[RTXLEN];
int  txptr=0;
int  rxptr=0;

int main(int argc, char *argv[])
{
	int j, size;
	struct sched_param param;
	pthread_attr_t attr;
	pthread_t thread;

	uint8_t reply[11] = { 0xef, 0xfe, 2, 0, 0, 0, 0, 0, 0, 32, 1 };

	uint8_t id[4] = { 0xef, 0xfe, 1, 6 };
	uint32_t code;
        int16_t  sample;

	struct sockaddr_in addr_ep2, addr_from;
	uint8_t buffer[1032];
	struct iovec iovec;
	struct msghdr msghdr;
	struct timeval tv;
	struct timespec ts;
	int yes = 1;
        uint8_t *bp;

	uint32_t last_seqnum = 0xffffffff, seqnum;  // sequence number of received packet

	int udp_retries=0;
	int bytes_read, bytes_left;
	uint32_t *code0 = (uint32_t *) buffer;  // fast access to code of first buffer

	if ((sock_ep2 = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
	{
		perror("socket");
		return EXIT_FAILURE;
	}

	// Fake MAC address
	reply[3]=0xAA;
	reply[4]=0xBB;
	reply[5]=0xCC;
	reply[6]=0xDD;
	reply[7]=0xEE;
	reply[8]=0xFF;

	setsockopt(sock_ep2, SOL_SOCKET, SO_REUSEADDR, (void *)&yes, sizeof(yes));

	tv.tv_sec = 0;
	tv.tv_usec = 1000;
	setsockopt(sock_ep2, SOL_SOCKET, SO_RCVTIMEO, (void *)&tv, sizeof(tv));

	memset(&addr_ep2, 0, sizeof(addr_ep2));
	addr_ep2.sin_family = AF_INET;
	addr_ep2.sin_addr.s_addr = htonl(INADDR_ANY);
	addr_ep2.sin_port = htons(1024);

	if (bind(sock_ep2, (struct sockaddr *)&addr_ep2, sizeof(addr_ep2)) < 0)
	{
		perror("bind");
		return EXIT_FAILURE;
	}

	if ((sock_TCP_Server = socket(AF_INET, SOCK_STREAM, 0)) < 0)
	{
		perror("socket tcp");
		return EXIT_FAILURE;
	}

	fprintf(stderr, "DEBUG_TCP: RP <--> PC: sock_TCP_Server: %d\n", sock_TCP_Server);

	setsockopt(sock_TCP_Server, SOL_SOCKET, SO_REUSEADDR, (void *)&yes, sizeof(yes));

	int tcpmaxseg = 1032;
	setsockopt(sock_TCP_Server, IPPROTO_TCP, TCP_MAXSEG, (const char *)&tcpmaxseg, sizeof(int));

	int sndbufsize = 65535;
	int rcvbufsize = 65535;
	setsockopt(sock_TCP_Server, SOL_SOCKET, SO_SNDBUF, (const char *)&sndbufsize, sizeof(int));
	setsockopt(sock_TCP_Server, SOL_SOCKET, SO_RCVBUF, (const char *)&rcvbufsize, sizeof(int));
	tv.tv_sec = 0;
	tv.tv_usec = 1000;
	setsockopt(sock_TCP_Server, SOL_SOCKET, SO_RCVTIMEO, (void *)&tv, sizeof(tv));

	if (bind(sock_TCP_Server, (struct sockaddr *)&addr_ep2, sizeof(addr_ep2)) < 0)
	{
		perror("bind tcp");
		return EXIT_FAILURE;
	}

	listen(sock_TCP_Server, 1024);
	fprintf(stderr, "DEBUG_TCP: RP <--> PC: Listening for TCP client connection request\n");

        int flags = fcntl(sock_TCP_Server, F_GETFL, 0);
        fcntl(sock_TCP_Server, F_SETFL, flags | O_NONBLOCK);

	while (1)
	{
		memset(&iovec, 0, sizeof(iovec));
		memset(&msghdr, 0, sizeof(msghdr));

		memcpy(buffer, id, 4);
		iovec.iov_base = buffer;
		iovec.iov_len = 1032;
		msghdr.msg_iov = &iovec;
		msghdr.msg_iovlen = 1;
		msghdr.msg_name = &addr_from;
		msghdr.msg_namelen = sizeof(addr_from);

		ts.tv_sec = 0;
		ts.tv_nsec = 1000000;

		if (sock_TCP_Client > -1)
		{
			// Using recvmmsg with a time-out should be used for a byte-stream protocol like TCP
			// (Each "packet" in the datagram may be incomplete). This is especially true if the
			// socket has a receive time-out, but this problem also occurs if the is no such
			// receive time-out.
			// Therefore we read a complete packet here (1032 bytes). Our TCP-extension to the
			// HPSDR protocol ensures that only 1032-byte packets may arrive here.
			bytes_read = 0;
			bytes_left = 1032;
			while (bytes_left > 0)
			{
				size = recvfrom(sock_TCP_Client, buffer + bytes_read, (size_t)bytes_left, 0, NULL, 0);
				if (size < 0 && errno == EAGAIN) continue;
				if (size < 0) break;
				bytes_read += size;
				bytes_left -= size;

			}

			bytes_read=size;
			if (size >= 0)
			{
				// 1032 bytes have successfully been read by TCP.
				// Let the downstream code know that there is a single packet, and its size
				bytes_read=1032;

				// In the case of a METIS-discovery packet, change the size to 63
				if (*code0 == 0x0002feef)
				{
					bytes_read = 63;
				}

				// In principle, we should check on (*code0 & 0x00ffffff) == 0x0004feef,
				// then we cover all kinds of start and stop packets.

				// In the case of a METIS-stop packet, change the size to 64
				if (*code0 == 0x0004feef)
				{
					bytes_read = 64;
				}

				// In the case of a METIS-start TCP packet, change the size to 64
				// The special start code 0x11 has no function any longer, but we shall still support it.
				if (*code0 == 0x1104feef || *code0 == 0x0104feef)
				{
					bytes_read = 64;
				}
			}
		}
		else
		{
			bytes_read = recvmsg(sock_ep2, &msghdr, 0);
			if (bytes_read > 0)
			{
				udp_retries=0;
			}
			else
			{
				udp_retries++;
			}

		}

		if (bytes_read < 0 && errno != EAGAIN)
		{
			perror("recvfrom");
			return EXIT_FAILURE;
		}

		// If nothing has arrived via UDP for some time, try to open TCP connection.
		// "for some time" means 10 subsequent un-successful UDP rcvmmsg() calls
                if (sock_TCP_Client < 0 && udp_retries > 10)
                {
                        if((sock_TCP_Client = accept(sock_TCP_Server, NULL, NULL)) > -1)
                        {
                                fprintf(stderr, "DEBUG_TCP: RP <--> PC: sock_TCP_Client: %d connected to sock_TCP_Server: %d\n", sock_TCP_Client, sock_TCP_Server);
                        }
			// This avoids firing accept() too often if it constantly fails
			udp_retries=0;
		}
		if (bytes_read <= 0) continue;
			memcpy(&code, buffer, 4);

			switch (code)
			{
				// PC to Red Pitaya transmission via process_ep2
				case 0x0201feef:

				// processing an invalid packet is too dangerous -- skip it!
				if (bytes_read != 1032)
				{
					fprintf(stderr,"DEBUG_PROT: RvcMsg Code=0x%08x Len=%d\n", code, (int)bytes_read);
					break;
				}

				// sequence number check
				seqnum = ((buffer[4]&0xFF)<<24) + ((buffer[5]&0xFF)<<16) + ((buffer[6]&0xFF)<<8) + (buffer[7]&0xFF);

				if (seqnum != last_seqnum + 1)
				{
					fprintf(stderr,"DEBUG_SEQ: SEQ ERROR: last %ld, recvd %ld\n", (long)last_seqnum, (long)seqnum);
				}

				last_seqnum = seqnum;

                                // Put TX IQ samples into the ring buffer
				// In the old protocol, samples come in groups of 8 bytes L1 L0 R1 R0 I1 I0 Q1 Q0
				// Here, L1/L0 and R1/R0 are audio samples, and I1/I0 and Q1/Q0 are the TX iq samples
				// I1 contains bits 8-15 and I0 bits 0-7 of a signed 16-bit integer. We convert this
				// here to double
                                if (ptt) {
                                   bp=buffer+16;  // skip 8 header and 8 SYNC/C&C bytes
                                   for (j=0; j<63; j++) {
					bp += 4;  // skip microphone samples
					sample  = (int)((signed char) *bp++)<<8;
					sample |= (int) ((signed char) *bp++ & 0xFF);
                                        isample[txptr]=(double) sample/ 32768.0;
					sample  = (int)((signed char) *bp++)<<8;
					sample |= (int) ((signed char) *bp++ & 0xFF);
					qsample[txptr]=(double) sample/32768.0;
					txptr++;
                                   }
                                   bp+=8; // skip 8  SYNC/C&C bytes of second 512-byte-block
                                   for (j=0; j<63; j++) {
					bp += 4;  // skip microphone samples
					sample  = (int)((signed char) *bp++)<<8;
					sample |= (int)((signed char) *bp++ & 0xFF);
                                        isample[txptr]=(double) sample/32768.0;
					sample  = (int)((signed char) *bp++)<<8;
					sample |= (int) ((signed char) *bp++ & 0xFF);
					qsample[txptr]=(double) sample/32768.0;
					txptr++;
                                   }
                                } else {
				   // put silence into TX buffer
				   for (j=0; j<126; j++) {
					isample[txptr]=0.0;
					qsample[txptr++]=0.0;
				   }	
				}
				// wrap-around of ring buffer
                                if (txptr >= RTXLEN) txptr=0;

				process_ep2(buffer + 11);
				process_ep2(buffer + 523);
				break;

				// respond to an incoming Metis detection request
				case 0x0002feef:

				fprintf(stderr, "DEBUG_PROT: RP -> PC: respond to an incoming Metis detection request / code: 0x%08x\n", code);

				// processing an invalid packet is too dangerous -- skip it!
				if (bytes_read != 63)
				{
					fprintf(stderr,"DEBUG_PROT: RvcMsg Code=0x%08x Len=%d\n", code, (int)bytes_read);
					break;
				}
				reply[2] = 2 + active_thread;
				memset(buffer, 0, 60);
				memcpy(buffer, reply, 11);

				// ab-use some of the "unused" bytes in the reply block to indicate we can do TCP
				// This information can be used by SDR programs
				buffer[11]='T';
				buffer[12]='C';
				buffer[13]='P';
				if (sock_TCP_Client > -1)
				{
					// We will get into trouble if we respond via TCP while the radio is
					// running with TCP.
					// We simply suppress the response in this (very unlikely) case.
					if (!active_thread)
					{
					    if (send(sock_TCP_Client, buffer, 60, 0) < 0)
					    {
						fprintf(stderr, "DEBUG_TCP: RP -> PC: TCP send error occurred when responding to an incoming Metis detection request!\n");
					    }
					    // close the TCP socket which was only used for the detection
					    close(sock_TCP_Client);
					    sock_TCP_Client = -1;
					}
				}
				else
				{
					sendto(sock_ep2, buffer, 60, 0, (struct sockaddr *)&addr_from, sizeof(addr_from));
				}

				break;

				// stop the Red Pitaya to PC transmission via handler_ep6
				case 0x0004feef:

				fprintf(stderr, "DEBUG_PROT: RP -> PC: stop the transmission via handler_ep6 / code: 0x%08x\n", code);

				// processing an invalid packet is too dangerous -- skip it!
				if (bytes_read != 64)
				{
					fprintf(stderr,"DEBUG_PROT: RvcMsg Code=0x%08x Len=%d\n", code, bytes_read);
					break;
				}

				enable_thread = 0;
				while (active_thread) usleep(1000);

				if (sock_TCP_Client > -1)
				{
					close(sock_TCP_Client);
					sock_TCP_Client = -1;
				}
				break;

				// start the Red Pitaya to PC transmission via handler_ep6
				case 0x1104feef:
				//// This special code 0x11 is no longer needed, is does exactly the same thing
				//// as the other start codes 0x01, 0x02, 0x03

				fprintf(stderr, "DEBUG_TCP: PC -> RP: TCP METIS-start message received / code: 0x%08x\n", code);

				/* FALLTHROUGH */

				case 0x0104feef:
				case 0x0204feef:
				case 0x0304feef:

				fprintf(stderr, "DEBUG_PROT: RP <--> PC: start the handler_ep6 thread / code: 0x%08x\n", code);

				// processing an invalid packet is too dangerous -- skip it!
				if (bytes_read != 64)
				{
					fprintf(stderr,"DEBUG_PROT: RvcMsg Code=0x%08x Len=%d\n", code, bytes_read);
					break;
				}

				enable_thread = 0;
				while (active_thread) usleep(1000);
				memset(&addr_ep6, 0, sizeof(addr_ep6));
				addr_ep6.sin_family = AF_INET;
				addr_ep6.sin_addr.s_addr = addr_from.sin_addr.s_addr;
				addr_ep6.sin_port = addr_from.sin_port;

                                txptr=3150;  // must be even multiple of 63
                                rxptr=0;
				memset(isample, 0, RTXLEN*sizeof(double));
				memset(qsample, 0, RTXLEN*sizeof(double));
				enable_thread = 1;
				active_thread = 1;
				CommonMercuryFreq = 0;
				if (pthread_create(&thread, NULL, handler_ep6, NULL) < 0)
				{
					perror("pthread_create");
					return EXIT_FAILURE;
				}
				pthread_detach(thread);
				break;

				default:
				// Possibly a discovery packet of the New protocol, otherwise a severe error
				if (bytes_read == 60 && code == 0 && buffer[4] == 0x02)
				{
					fprintf(stderr,"DEBUG_PROT: PC -> RP: NewProtocol discovery packet received (no response)\n");
				}
				else
				{
					fprintf(stderr,"DEBUG_PROT: PC -> RP: invalid code: 0x%08x (Len=%d)\n", code, bytes_read);
				}
				break;
			}
	}

	close(sock_ep2);

	if (sock_TCP_Client > -1)
	{
		close(sock_TCP_Client);
	}

	if (sock_TCP_Server > -1)
	{
		close(sock_TCP_Server);
	}

	return EXIT_SUCCESS;
}

#define chk_data(a,b,c) if ((a) != b) { b = (a); printf("%20s= %08lx (%10ld)\n", c, (long) b, (long) b ); }

void process_ep2(uint8_t *frame)
{

	uint16_t data;

        chk_data(frame[0] & 1, ptt, "PTT");
	switch (frame[0])
	{
	case 0:
	case 1:
	  chk_data((frame[1] & 0x03) >> 0, rate, "SampleRate");
	  chk_data((frame[1] & 0x0C) >> 3, ref10, "Ref10MHz");
  	  chk_data((frame[1] & 0x10) >> 4, src122, "Source122MHz");
  	  chk_data((frame[1] & 0x60) >> 5, PMconfig, "Pen/Mer config");
  	  chk_data((frame[1] & 0x80) >> 7, MicSrc, "MicSource");

          chk_data(frame[2] & 1, TX_class_E, "TX CLASS-E");
          chk_data((frame[2] & 0xfe) >> 1, OpenCollectorOutputs,"OpenCollector");

          chk_data((frame[3] & 0x03) >> 0, att, "ALEX Attenuator");
          chk_data((frame[3] & 0x04) >> 2, preamp, "ALEX preamp");
          chk_data((frame[3] & 0x08) >> 2, LTdither, "LT2208 Dither");
          chk_data((frame[3] & 0x10) >> 2, LTrandom, "LT2208 Random");
          chk_data((frame[3] & 0x60) >> 5, alexRXant, "ALEX RX ant");
          chk_data((frame[3] & 0x80) >> 7, alexRXout, "ALEX RX out");

          chk_data(((frame[4] >> 0) & 3), AlexTXrel, "ALEX TX relay");
          chk_data(((frame[4] >> 2) & 1), duplex,    "DUPLEX");
          chk_data(((frame[4] >> 3) & 7) + 1, receivers, "RECEIVERS");
          chk_data(((frame[4] >> 6) & 1), MicTS, "TimeStampMic");
          chk_data(((frame[4] >> 7) & 1), CommonMercuryFreq,"Common Mercury Freq");
	  break;

        case 2:
        case 3:
	   chk_data(frame[4] | (frame[3] << 8) | (frame[2] << 16) | (frame[1] << 24), tx_freq,"TX FREQ");
	   break;

        case 4:
        case 5:
	   chk_data(frame[4] | (frame[3] << 8) | (frame[2] << 16) | (frame[1] << 24), rx_freq[0],"RX FREQ1");
	   break;

        case 6:
        case 7:
	   chk_data(frame[4] | (frame[3] << 8) | (frame[2] << 16) | (frame[1] << 24), rx_freq[1],"RX FREQ2");
	   break;

        case 8:
        case 9:
	   chk_data(frame[4] | (frame[3] << 8) | (frame[2] << 16) | (frame[1] << 24), rx_freq[2],"RX FREQ3");
	   break;

        case 10:
        case 11:
	   chk_data(frame[4] | (frame[3] << 8) | (frame[2] << 16) | (frame[1] << 24), rx_freq[3],"RX FREQ4");
	   break;

        case 12:
        case 13:
	   chk_data(frame[4] | (frame[3] << 8) | (frame[2] << 16) | (frame[1] << 24), rx_freq[4],"RX FREQ5");
	   break;

        case 14:
        case 15:
	   chk_data(frame[4] | (frame[3] << 8) | (frame[2] << 16) | (frame[1] << 24), rx_freq[5],"RX FREQ6");
	   break;

        case 16:
        case 17:
	   chk_data(frame[4] | (frame[3] << 8) | (frame[2] << 16) | (frame[1] << 24), rx_freq[6],"RX FREQ7");
	   break;

	case 18:
	case 19:
	   chk_data(frame[1],txdrive,"TX DRIVE");
	   chk_data(frame[2],hermes_config,"HERMES CONFIG");
	   chk_data(frame[3],alex_hpf,"ALEX HPF");
	   chk_data(frame[4],alex_lpf,"ALEX LPF");
           // reset TX level
	   txdrv_dbl=(double) txdrive / 255.0;
	   break;

	case 20:
	case 21:
	   chk_data((frame[1] & 0x01) >> 0, rx_preamp[0], "ADC1 preamp");
	   chk_data((frame[1] & 0x02) >> 1, rx_preamp[1], "ADC2 preamp");
	   chk_data((frame[1] & 0x04) >> 2, rx_preamp[2], "ADC3 preamp");
	   chk_data((frame[1] & 0x08) >> 3, rx_preamp[3], "ADC4 preamp");
	   chk_data((frame[1] & 0x10) >> 4, tip_ring  , "TIP/Ring");
	   chk_data((frame[1] & 0x20) >> 5, MicBias   , "MicBias");
	   chk_data((frame[1] & 0x40) >> 6, MicPTT    , "MicPTT");

   	   chk_data((frame[2] & 0x1F) >> 0, LineGain  , "LineGain");
   	   chk_data((frame[2] & 0x20) >> 5, MerTxATT0 , "Mercury Att on TX/0");
   	   chk_data((frame[2] & 0x40) >> 6, PureSignal, "PureSignal");
   	   chk_data((frame[2] & 0x80) >> 7, PeneSel   , "PenelopeSelect");

   	   chk_data((frame[3] & 0x0F) >> 0, MetisDB9  , "MetisDB9");
   	   chk_data((frame[3] & 0x10) >> 4, MerTxATT1 , "Mercury Att on TX/1");

   	   chk_data((frame[4] & 0x1F) >> 0, rx_att[0], "RX1 ATT");
   	   chk_data((frame[4] & 0x20) >> 5, rx1_attE, "RX1 ATT enable");

	   // Set RX amplification factors. Assume 20 dB preamps
           rxatt_dbl[0]=pow(10.0, -0.05*(rx_att[0]-20*rx_preamp[0]));
           rxatt_dbl[1]=pow(10.0, -0.05*(rx_att[1]-20*rx_preamp[1]));
           rxatt_dbl[2]=pow(10.0, (double) rx_preamp[2]);
           rxatt_dbl[3]=pow(10.0, (double) rx_preamp[3]);
	   break;

	case 22:
	case 23:
           chk_data(frame[1] & 0x1f, rx_att[1],"RX2 ATT");
           chk_data((frame[2] >> 6) & 1, cw_reversed, "CW REV");
           chk_data(frame[3] & 63, cw_speed, "CW SPEED");
           chk_data((frame[3] >> 6) & 3, cw_mode, "CW MODE");
           chk_data(frame[4] & 127, cw_weight,"CW WEIGHT");
           chk_data((frame[4] >> 7) & 1, cw_spacing, "CW SPACING");

	   // Set RX amplification factors. Assume 20 dB preamps
           rxatt_dbl[1]=pow(10.0, -0.05*(rx_att[1]-20*rx_preamp[1]));
	   break;

	case 24:
	case 25:
           data = frame[1];
           data |= frame[2] << 8;
           chk_data((frame[2] << 8) | frame[1], c25_ext_board_i2c_data, "C25 EXT BOARD DATA");
           break;

	case 28:
	case 29:
            chk_data((frame[1] & 0x03) >> 0, rx_adc[0], "RX1 ADC");
            chk_data((frame[1] & 0x0C) >> 2, rx_adc[1], "RX2 ADC");
            chk_data((frame[1] & 0x30) >> 4, rx_adc[2], "RX3 ADC");
            chk_data((frame[1] & 0xC0) >> 6, rx_adc[3], "RX4 ADC");
            chk_data((frame[2] & 0x03) >> 0, rx_adc[4], "RX5 ADC");
            chk_data((frame[2] & 0x0C) >> 2, rx_adc[5], "RX6 ADC");
            chk_data((frame[2] & 0x30) >> 4, rx_adc[6], "RX7 ADC");
	    chk_data((frame[3] & 0x1f), txatt, "TX ATT");
	    txatt_dbl=pow(10.0, -0.05*(double) txatt);
	    break;

	case 30:
	case 31:
            chk_data(frame[1] & 1,cw_internal,"CW INT");
            chk_data(frame[2], sidetone_volume,"SIDE TONE VOLUME");
            chk_data(frame[3], cw_delay,"CW DELAY");
	    cw_delay = frame[3];
	    break;

	case 32:
	case 33:
            chk_data((frame[1] << 2) | (frame[2] & 3), cw_hang, "CW HANG");
            chk_data((frame[3] << 4) | (frame[4] & 255), freq, "SIDE TONE FREQ");
	    break;
	}
}

//
// The "RX" signal. This is some noise, and some frequencies
//
#define LENNOISE 48000
#define NOISEDIV (RAND_MAX / 24000)

static double noiseItab[LENNOISE];
static double noiseQtab[LENNOISE];

static double T0800Itab[480];
static double T0800Qtab[480];
static double T2000Itab[192];
static double T2000Qtab[192];

void *handler_ep6(void *arg)
{
	int i, j, k, n, size;
	int data_offset, header_offset;
	uint32_t counter;
	uint16_t audio[512];
	uint8_t buffer[1032];
	uint8_t *pointer;
	struct iovec iovec;
	struct msghdr msghdr;
	uint8_t id[4] = { 0xef, 0xfe, 1, 6 };
	uint8_t header[40] =
	{
		127, 127, 127, 0, 0, 33, 17, 21,
		127, 127, 127, 8, 0, 0, 0, 0,
		127, 127, 127, 16, 0, 0, 0, 0,
		127, 127, 127, 24, 0, 0, 0, 0,
		127, 127, 127, 32, 66, 66, 66, 66
	};
        int32_t sample;
        struct timespec delay;
#ifdef __APPLE__
	struct timespec now;
#endif
        int wait;
        int noiseIQpt;
	int len2000,pt2000;
	int len0800,pt0800;
        double run,inc;
        double i1,q1,fac;

	memset(audio, 0, sizeof(audio));
	memset(&iovec, 0, sizeof(iovec));
	memset(&msghdr, 0, sizeof(msghdr));

	memcpy(buffer + i * 1032, id, 4);
	iovec.iov_base = buffer;
	iovec.iov_len = 1032;
	msghdr.msg_iov = &iovec;
	msghdr.msg_iovlen = 1;
	msghdr.msg_name = &addr_ep6;
	msghdr.msg_namelen = sizeof(addr_ep6);

	header_offset = 0;
	counter = 0;

        //
        // Produce RX data
        //
        // a) noise from a 16-bit ADC
        //
        k=RAND_MAX / 2;
        for (i=0; i<LENNOISE; i++) {
	  noiseItab[i]= ((double) rand() / k - 1.0) * 0.00003;
	  noiseQtab[i]= ((double) rand() / k - 1.0) * 0.00003;
        }
	noiseIQpt=0;
	//
	// b) some tones in the upper side band (one wave)
	//
        len2000=24 << rate;
	len0800=60 << rate;

	inc = 6.283185307179586476925287 / (double) len2000;
	run = 0.0;
        for (i=0; i<len2000; i++) {
	  T2000Qtab[i]=cos(run);
	  T2000Itab[i]=sin(run);
	  run += inc;
        }
	inc = 6.283185307179586476925287 / (double) len0800;
	run = 0.0;
        for (i=0; i<len0800; i++) {
	  T0800Qtab[i]=cos(run);
	  T0800Itab[i]=sin(run);
	  run += inc;
        }

        pt2000=0;
        pt0800=0;
	  
        
        clock_gettime(CLOCK_MONOTONIC, &delay);
	while (1)
	{
		if (!enable_thread) break;

		size = receivers * 6 + 2;
		n = 504 / size;  // number of samples per 512-byte-block
                // Time (in nanosecs) to "collect" the samples sent in one sendmsg
                wait = (2*n*1000000L) / (48 << rate);

                // plug in sequence numbers
		data_offset = 0;
		*(uint32_t *)(buffer + 4) = htonl(counter);
		++counter;

//
//		This defines the distortion as well as the amplification
//
#define IM3a  0.70
#define IM3b  0.15

		for (i = 0; i < 2; ++i)
		{
		    pointer = buffer + i * 516 - i % 2 * 4 + 8;
		    memcpy(pointer, header + header_offset, 8);

		    header_offset = header_offset >= 32 ? 0 : header_offset + 8;

		    pointer += 8;
		    memset(pointer, 0, 504);
		    for (j=0; j<n; j++) {
			for (k=0; k< receivers; k++) {
			    switch (k) {
				case 0: // RX1 sees RF1
				    //
				    // RF1: noise + weak 800 Hz tone
				    //
				    sample= noiseItab[noiseIQpt] * 8388607.0;
			  	    sample += T0800Itab[pt0800] * 83.886070 *rxatt_dbl[0];  // 100 dB below peak
			   	    *pointer++ = (sample >> 16) & 0xFF;
				    *pointer++ = (sample >>  8) & 0xFF;
				    *pointer++ = (sample >>  0) & 0xFF;
				    sample=noiseQtab[noiseIQpt] * 8388607.0;
			  	    sample += T0800Qtab[pt0800] * 83.886070 *rxatt_dbl[0];  // 100 dB below peak
				    *pointer++ = (sample >> 16) & 0xFF;
				    *pointer++ = (sample >>  8) & 0xFF;
				    *pointer++ = (sample >>  0) & 0xFF;
				    break;
			  	case 1: // RX2 and RX3 see RF2 upon receiving, RF3 upon transmitting
				case 2:
                                    if (rate == 0 && ptt) {
                                    	//
					// RF3:
                                    	// Distorted (feed-back) TX signal
                                    	// Note we first add distortion, then adjust level
                                    	// Therefore we first multiply with txdrv_dbl, then
                                    	// distort, and then attenuate with txatt_dbl.
                                   	//
                                        i1=isample[rxptr]*txdrv_dbl;
                                        q1=qsample[rxptr]*txdrv_dbl;
                                        fac=IM3a+IM3b*(i1*i1+q1*q1);
                                        sample= txatt_dbl*i1*fac * 8388607.0;
                                        *pointer++ = (sample >> 16) & 0xFF;
                                        *pointer++ = (sample >>  8) & 0xFF;
                                        *pointer++ = (sample >>  0) & 0xFF;
                                        sample= txatt_dbl*q1*fac * 8388607.0;
                                        *pointer++ = (sample >> 16) & 0xFF;
                                        *pointer++ = (sample >>  8) & 0xFF;
                                        *pointer++ = (sample >>  0) & 0xFF;
                                    } else {
				    	//
				    	// RF2: noise + weak 2000 Hz tone
				    	//
				    	sample= noiseItab[noiseIQpt] * 8388607.0;
			  	    	sample += T2000Itab[pt2000] * 838.86070 * rxatt_dbl[1];  // 80 dB below peak
				    	*pointer++ = (sample >> 16) & 0xFF;
				    	*pointer++ = (sample >>  8) & 0xFF;
				    	*pointer++ = (sample >>  0) & 0xFF;
				    	sample=noiseQtab[noiseIQpt] * 8388607.0;
			  	    	sample += T2000Qtab[pt2000] * 838.86070 * rxatt_dbl[1];  // 80 dB below peak
				    	*pointer++ = (sample >> 16) & 0xFF;
				    	*pointer++ = (sample >>  8) & 0xFF;
				    	*pointer++ = (sample >>  0) & 0xFF;
				    }
				    break;
				case 3: // RX4 sees TX outgoing signal (no distortion, no attenuation)
				    //
				    // RF4: TX signal with HWPeak = 0.4
				    //
                        	    if (rate == 0 && ptt) {
                                  	sample= isample[rxptr] * 0.400 * 8388607.0;
                                  	*pointer++ = (sample >> 16) & 0xFF;
                                  	*pointer++ = (sample >>  8) & 0xFF;
                                  	*pointer++ = (sample >>  0) & 0xFF;
                                  	sample= qsample[rxptr] * 0.400 * 8388607.0;
                                  	*pointer++ = (sample >> 16) & 0xFF;
                                  	*pointer++ = (sample >>  8) & 0xFF;
                                  	*pointer++ = (sample >>  0) & 0xFF;
                               	    } else {
				  	pointer +=6;
				    }
				    break;
				default:
				    pointer +=6;
			    }
		        }
			// Microphone samples: silence
			pointer += 2;
			rxptr++;     if (rxptr >= RTXLEN) rxptr=0;
			noiseIQpt++; if (noiseIQpt == LENNOISE) noiseIQpt=rand() / NOISEDIV;
			pt2000++;    if (pt2000 == len2000) pt2000=0;
			pt0800++;    if (pt0800 == len0800) pt0800=0;
		    }
		}
		//
		// Wait until the time has passed for all these samples
		//
                delay.tv_nsec += wait;
                while (delay.tv_nsec >= 1000000000) {
                  delay.tv_nsec -= 1000000000;
                  delay.tv_sec++;
                }
#ifdef __APPLE__
		//
		// The (so-called) operating system for Mac does not have clock_nanosleep(),
		// but is has clock_gettime as well as nanosleep.
		// So, to circumvent this problem, we look at the watch and determine
		// how long we should sleep now.
		//
		clock_gettime(CLOCK_MONOTONIC, &now);
		now.tv_sec =delay.tv_sec  - now.tv_sec;
		now.tv_nsec=delay.tv_nsec - now.tv_nsec;
		while (now.tv_nsec < 0) {
		    now.tv_nsec += 1000000000;
		    now.tv_sec--;
		}
		nanosleep(&now, NULL);
#else
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &delay, NULL);
#endif

		if (sock_TCP_Client > -1)
		{
			if (sendmsg(sock_TCP_Client, &msghdr, 0) < 0)
			{
				fprintf(stderr, "DEBUG_TCP: RP -> PC: TCP sendmsg error occurred at sequence number: %u !\n", counter);
			}
		}
		else
		{
			sendmsg(sock_ep2, &msghdr, 0);
		}

	}
	active_thread = 0;
	return NULL;
}
