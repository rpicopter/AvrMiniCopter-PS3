#include <unistd.h>
#include <stdio.h>
#include <math.h>
#include <time.h>
#include <pthread.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netdb.h>
#include <stdlib.h>
#include <time.h>
#include <sched.h>
#include <sys/mman.h>
#include <getopt.h>

#include "ps3config.h"
#include "ps3dev.h"

#include "routines.h"

#define CFG_PATH "/etc/avrminicopter/"

struct ps3_config ps3config;
int verbose; 

struct s_rec js[2];

int ret;
int err = 0;
int stop = 0;

int avr_s[256];

int trim[3] = {0,0,0};//in degrees * 1000
int mode = 0;
int rec_setting = 0;

int alt_hold = 0;
int throttle_hold = 0;
int throttle_target = 0;

int sock = 0,len;
uint8_t sock_type = 0;
char sock_path[256] = "/dev/avrspi";
struct sockaddr_un address;

int nocontroller = 0;
int cam_seq = 0;

int flight_threshold;

int sendMsg(int t, int v) {
        static unsigned char buf[4];
        static struct local_msg m;
        m.c = 0;
        m.t = t;
        m.v = v;
        pack_lm(buf,&m);
	if (write(sock, buf, 4) < 0) {
		perror("writing");
		return -1;
	}
/*
        ret = sendto(sock,buf,LOCAL_MSG_SIZE,0,(struct sockaddr *)&address,sizeof(address));
        if (ret<=0) {
                perror("AVRBARO: writing");
        }
*/
        return 0;
}


void processMsg(struct local_msg *m) {
	//printf("Recieved t: %u v: %i\n",m->t,m->v);

	if (m->c == 1) {
		printf("Disconnect request.\n");
		stop = 1;
	}
}

void recvMsgs() {
	static int i=0,ret=0;
	static unsigned char buf[256];
	static int buf_c = 0;
	static struct local_msg msg;
	static int count = 0;


	if (ioctl(sock, TIOCINQ, &count)!=0) {
		printf("Ioctl failed.\n");
		stop = 1;
	} else if (count) {
		ret = read(sock,buf+buf_c,256-buf_c);
		buf_c += ret;

		int msg_no = buf_c / LOCAL_MSG_SIZE;
		int reminder = buf_c % LOCAL_MSG_SIZE;
		for (i=0;i<msg_no;i++) {
			unpack_lm(buf+i*LOCAL_MSG_SIZE,&msg);
			processMsg(&msg);
		}

		if (msg_no) {
			for (i=0;i<reminder;i++)
				buf[i] = buf[msg_no*LOCAL_MSG_SIZE+i];
				buf_c = reminder;
		}

	}

}

void reset_avr() {
	//ensure AVR is properly rebooted
	while (avr_s[255]!=1) { //once rebooted AVR will report status = 1;
		avr_s[255] = -1;
		sendMsg(255,255);
		mssleep(1500);
		recvMsgs();
	}
}

void do_adjustments_secondary(struct s_rec *js) {
	static int adj4 = 25; //for altitude (cm)
	if (js->aux<0) return;
	switch (js->aux) {
		case 11: //R1
			if (alt_hold) {
				sendMsg(16,adj4);
			}
			break;
		case 9: //R2
			if (alt_hold) {
				sendMsg(16,-adj4);
			}
			break;
		case 12:
			if (rec_setting) rec_setting = 0;
			else rec_setting = 1;
			rec_config(js,ps3config.rec_ypr[rec_setting],ps3config.throttle);
			break;
	}

	js->aux = -1;
}

void do_adjustments(struct s_rec *js) {
	if (js->aux<0) return;

	static int adj3 = 1; //for trim
	static int adj4 = 25; //for altitude (cm)

	static char str[128];

	switch (js->aux) {
		case 8: //L2
			memset(str, '\0', 128);
			sprintf(str, "/usr/local/bin/ps3vidsnap.sh %05d ", cam_seq++);
			ret=system(str);
			break;
		case 10: //L1
			memset(str, '\0', 128);
			sprintf(str, "/usr/local/bin/ps3picsnap.sh %05d ", cam_seq++);
			ret=system(str);
			break;
		case 11: //R1
			if (alt_hold) {
				sendMsg(16,adj4);
			}
			break;
		case 9: //R2
			if (alt_hold) {
				sendMsg(16,-adj4);
			}
			break;
		case 0:
			if (js[0].yprt[3]<flight_threshold) sendMsg(0,4); 
			break;
		case 3: 
			stop=1;
			break;
		case 12:
			if (rec_setting) rec_setting = 0;
			else rec_setting = 1;
			rec_config(js,ps3config.rec_ypr[rec_setting],ps3config.throttle);
			break;
		case 1:
			alt_hold = 0;
			throttle_hold = 0;
			sendMsg(15,alt_hold);
			break;
		case 13:
			/*
			   if (throttle_hold) throttle_hold=0;
			   else throttle_hold = 1;
			   throttle_target = js->yprt[3];
			 */
			break;
		case 14:
			if (alt_hold) alt_hold=0;
			else alt_hold = 1;
			sendMsg(15,alt_hold);
			break;
		case 4:
			trim[1]+=adj3;
			sendMsg(21,trim[1]);
			break;
		case 6:
			trim[1]-=adj3;
			sendMsg(21,trim[1]);
			break;
		case 7:
			trim[2]+=adj3;
			sendMsg(22,trim[2]);
			break;
		case 5:
			trim[2]-=adj3;
			sendMsg(22,trim[2]);
			break;
		case 16: //mode change
			mode++;
			if (mode==2) mode=0;
			sendMsg(3,mode);
			break;
		default:
			printf("Unknown command %i\n",js->aux);
	}
	if ((verbose) && (js->aux!=-1)) printf("Button: %i\n",js->aux);
	js->aux=-1; //reset receiver command

} 

void catch_signal(int sig)
{
	printf("signal: %i\n",sig);
	stop = 1;
}

long dt_ms = 0;
static struct timespec ts,t1,t2,*dt;

unsigned long k = 0;

void loop() {
	clock_gettime(CLOCK_REALTIME,&t2);                                           
	ts = t1 = t2;
	if (verbose) printf("Starting main loop...\n");
	int yprt[4] = {0,0,0,0};
	while (1 && !err && !stop) {
		if (!nocontroller) {
			ret = rec_update(&js[0]); 
			// 0 - no update but read ok
			// 1 - update
			if (ret < 0) {
				err = 1;
				printf("Receiver reading error: [%s]\n",strerror(ret));
				return;
			}
			do_adjustments(&js[0]);
			memcpy(yprt,js[0].yprt,sizeof(int)*4);

			if (js[1].fd) {
				ret = rec_update(&js[1]);
				if (ret<0) {
					printf("Secondary receiver error: [%s]\n",strerror(ret));
					js[1].fd = 0;
				}
				do_adjustments_secondary(&js[1]);
				//js0 is the master control so only use js1 if js0 has no value
				if (abs(yprt[0]) < 5) yprt[0] = js[1].yprt[0]; 
				if (yprt[1] == 0) yprt[1] = js[1].yprt[1];
				if (yprt[2] == 0) yprt[2] = js[1].yprt[2];
			}
		} else {
			ret = 0;
			js[0].aux = -1;
			yprt[0] = js[0].yprt[0] = 0;
			yprt[1] = js[0].yprt[1] = 0;
			yprt[2] = js[0].yprt[2] = 0;
			yprt[2] = js[0].yprt[3] = 1000;
		}

		if (alt_hold && (yprt[3] > (ps3config.throttle[1]-50) || yprt[3] < ps3config.throttle[0]-50)) {
			alt_hold = 0;
			throttle_hold = 0;
			sendMsg(15,alt_hold);
		}



		clock_gettime(CLOCK_REALTIME,&t2);                                           
		dt = TimeSpecDiff(&t2,&t1);
		dt_ms = dt->tv_sec*1000 + dt->tv_nsec/1000000;

		if (dt_ms<50) {
			mssleep(50-dt_ms);
			continue; //do not flood AVR with data - will cause only problems; each loop sends 4 msg; 50ms should be enough for AVR to consume them
		}
		t1 = t2;

		if (throttle_hold) {
			yprt[3] = throttle_target;
		}

		sendMsg(10,yprt[0]+trim[0]);
		sendMsg(11,yprt[1]+trim[1]);
		sendMsg(12,yprt[2]+trim[2]);
		sendMsg(13,yprt[3]);
		recvMsgs();
	}

	sendMsg(13,ps3config.throttle[0]);
	sendMsg(15,0);
}

void print_usage() {
	printf("-v [level] - verbose mode\n");
	printf("-u [SOCKET] - socket to connect to (defaults to %s)\n",sock_path);
	printf("-f - do not initialize joystic\n");
}

int main(int argc, char **argv) {

	signal(SIGTERM, catch_signal);
	signal(SIGINT, catch_signal);

	int option;
	verbose = 0;
	while ((option = getopt(argc, argv,"v:u::f")) != -1) {
		switch (option)  {
			case 'v': verbose=atoi(optarg); break;
			case 'f': nocontroller = 1; break;
			case 'u': strcpy(sock_path,optarg); break;
			default:
				  print_usage();
				  return -1;
		}
	}

	for (int i=0;i<256;i++)
		avr_s[i] = 0;

	if (verbose) printf("Opening socket...\n");

	/* Create socket on which to send. */
	sock = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sock < 0) {
		perror("opening socket");
		exit(1);
	}
	bzero((char *) &address, sizeof(address));
	address.sun_family = AF_UNIX;
	strcpy(address.sun_path, sock_path);
	len = strlen(address.sun_path) + sizeof(address.sun_family);

	if (connect(sock, (struct sockaddr *) &address, len) < 0) {
		perror("connecting socket");
		close(sock);
		exit(1);
	}

	if (write(sock,&sock_type,1)!=1) {
		perror("writing to socket");
		close(sock);
		exit(1);
	}
	

	/* set non-blocking
	   ret = fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK);
	   if (ret == -1){
	   perror("calling fcntl");
	   return -1;
	   }
	 */
	if (verbose) printf("Connected to avrspi\n");



	ret=ps3config_open(&ps3config,CFG_PATH);
	if (ret<0) {
		printf("Failed to initiate config! [%s]\n", strerror(ret));	
		return -1;
	}
	flight_threshold = ps3config.throttle[0]+50;

	if (!nocontroller) {
		ret=rec_open("/dev/input/js0",&js[0]);
		if (ret<0) {
			printf("Failed to initiate receiver! [%s]\n", strerror(ret));	
			return -1;
		}
		rec_config(&js[0],ps3config.rec_ypr[0],ps3config.throttle);

		ret=rec_open("/dev/input/js1",&js[1]);
		if (ret<0) {
			printf("Using single receiver\n");	
		} else {
			printf("Using two receivers\n");	
			rec_config(&js[1],ps3config.rec_ypr[0],ps3config.throttle);
		}
	}

	loop();
	close(sock);
	if (!nocontroller) {
		if (js[0].fd) rec_close(&js[0]);
		if (js[1].fd) rec_close(&js[1]);
	}
	if (verbose) printf("Closing.\n");
	return 0;
}

