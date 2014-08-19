/*udpserver.c*/

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netdb.h>
#include <string.h>
#include <arpa/inet.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <iostream>
#include "RingBuffer.h"
#define DATA_NUM 11
#define PACKET_N 5
#define PACKET_L 1024
struct p_data {
	long l; // Paketlänge
	long n; // Paketanzahl
	double datarate, droprate, delay;
};
struct packet {
	long l; // Paket länge
	long n; //Paket Anzahl
	long seqnum;
	double droprate, datarate, old_delay, delay;
	long pnum;
	timespec t_send;
	timespec t_recv;
};

int sock;
int n;  // Anzahl Pakete im Train
int packet_size;
double packet_timer;
double x;
sockaddr_in caddr;
pthread_mutex_t mutex;
pthread_cond_t cond;
buffer buf;
buffer send_buf;
packet cur_packet;
p_data new_packet;
char* send_log;
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void logging(char* data, char* path) //loggen von DATA
		{
	FILE *file;

	file = fopen(path, "a+");
	fputs(data, file);
	fputs("\n", file);
	fclose(file);

}
void *send_logging(void *vdata) 	// Loggen der gesendeten Pakete (Thread)
		{
	char* current;
	char path[] = "send_logfile.txt";
	char key[] =
			"packet_size packet_number seqnum delay datarate droprate SEND_tstmp_sec SEND_tstmp_nsec RECV_tstmp_sec RECV_tstmp_nsec\n";
	timespec time;

	clock_gettime(CLOCK_REALTIME, &time); 	//Inititalsieren der Log-Datei
	logging(ctime(&time.tv_sec), path);
	logging(key, path);

	printf("Send_Logger ready.\n");
	long how_often = 0;
	struct timespec tim, tim2;
	tim.tv_sec = 0;
	tim.tv_nsec = 10000000L;
	while (1) {
		//TODO: Why the hack do you do busy waiting here?

		if (buffer_is_empty(&send_buf) == 0) {

			current = read_buffer(&send_buf);
			logging(current, path);

		} else {
			nanosleep(&tim, &tim2);
		}
	}
	return NULL;
}

void *udprecv(void * vdata) {  		//Empfangen der Pakete

	int rec;
	socklen_t slen = sizeof(caddr);		//für recvfrom
	char client_ip[INET_ADDRSTRLEN] = "";
	char msg_buffer[100];
	char time_s[12];
	timespec time;
	char * msg;
	create_buffer(&buf, 2 * n, 100); //Intitalsieren des recv_buffers  mit ausreichender Größe
	printf("Recv ready.\n");
	while (1) {
		msg_buffer[0] = '\0';
		time_s[0] = '\0';
		if (rec = recvfrom(sock, msg_buffer, (packet_size - 28), 0,
				(struct sockaddr*) &caddr, &slen) > 0) {

			clock_gettime(CLOCK_REALTIME, &time);
			sprintf(time_s, "%ld %ld ", time.tv_sec, time.tv_nsec);
			strcat(msg_buffer, time_s);

			write_buffer(&buf, msg_buffer);

		}

	}
	delete_buffer(&buf);
	return (void*) vdata;
}
void *udpsend(void * vdata) {  //Senden der Pakete

	char msg_first[70] = "";
	char msg[70] = "";
	int len = sizeof(msg);
	int i = 0;
	long seqnum = 0;
	char time_s[12];
	char pnum[12];
	timespec time;

	char client_ip[INET_ADDRSTRLEN] = "";
	create_buffer(&send_buf, 2 * n, 70);
	printf("Udpsend ready.\n");
	while (1) {
		pthread_mutex_lock(&mutex);
		pthread_cond_wait(&cond, &mutex);
		sprintf(msg_first, "%ld %ld %ld %f %.0f %.4f ", new_packet.l,
				new_packet.n, seqnum, new_packet.delay, new_packet.datarate,
				new_packet.droprate);

		for (i = 0; i < n; i++) {		 //Senden der Pakete
			msg[0] = '\0';
			time_s[0] = '\0';
			strcat(msg, msg_first);
			sprintf(pnum, "%i ", i);
			strcat(msg, pnum);
			clock_gettime(CLOCK_REALTIME, &time);
			sprintf(time_s, "%ld %ld ", time.tv_sec, time.tv_nsec);
			strcat(msg, time_s);

			if (sendto(sock, msg, (packet_size - 28 - 26 - 12), 0,
					(struct sockaddr*) &caddr, sizeof(caddr)) == -1)// Antwort senden Paket_size -IP/UDP-Header - Ethnernet-Frame -Interpaket gab
					{
				perror("send()");

			}

			write_buffer(&send_buf, msg);
		}
		pthread_mutex_unlock(&mutex);

		seqnum++;
	}
	delete_buffer(&send_buf);
	return (void*) vdata;
}
double time_diff(timespec first, timespec last) // Differenz von 2 timespec bilden.
		{
	long sec = (first.tv_sec - last.tv_sec);
	double nsec = (double) (first.tv_nsec - last.tv_nsec);
	return ((double) sec + (nsec / 1000000000));
}

void *fill_cur_packet(double *data) { //füllen der Paketstruktur
	cur_packet.l = (long) data[0];
	cur_packet.n = (long) data[1];
	cur_packet.seqnum = (long) data[2];
	cur_packet.old_delay = data[3];
	cur_packet.datarate = data[4];
	cur_packet.droprate = data[5];
	cur_packet.pnum = (long) data[6];
	cur_packet.t_send.tv_sec = (long) data[7];
	cur_packet.t_send.tv_nsec = (long) data[8];
	cur_packet.t_recv.tv_sec = (long) data[9];
	cur_packet.t_recv.tv_nsec = (long) data[10];
	cur_packet.delay = time_diff(cur_packet.t_recv, cur_packet.t_send);

}

void * print_p_data(p_data * data) {
	printf("n:%ld \n", data->n);
	printf("l:%ld \n", data->l);
	printf("drop:%f \n", data->droprate);
	printf("delay:%f \n", data->delay);

}

void *init_p_data(p_data * data) {
	data->n = 0; // Paketanzahl
	data->l = 0; // Paketlänge
	data->datarate = 0;
	data->droprate = 0;
	data->delay = 0;
}

void *rec_worker(void *vdata) {  //Bearbeiten der empfangenen Pakete
	int fpc = 0;					//Counter für x-tiemouts
	int ptfpc = 0;				//Counter für y-Timeouts
	long actseq = 0;			//aktuelle Seqnum für falsche Timeout ausgabe
	char path[] = "recv_logfile.txt";
	char* current;
	int i = 0;
	timespec recv_first;
	timespec recv_last;
	timespec send_first;
	int first_pnum = 0;
	int last_pnum = 0;
	char *pEnd;
	double data[DATA_NUM] = { 0 };
	long cur_seqnum = 0;
	long pcounter = 0;
	timespec timer;
	clock_gettime(CLOCK_REALTIME, &timer);
	timespec start = timer;

	long how_often = 0;
	struct timespec tim, tim2;
	tim.tv_sec = 0;
	tim.tv_nsec = 1000000L;

	char key[] =
			"packet_size packet_number seqnum delay datarate droprate SEND_tstmp_sec SEND_tstmp_nsec RECV_tstmp_sec RECV_tstmp_nsec\n";
	timespec time;

	clock_gettime(CLOCK_REALTIME, &time);
	logging(ctime(&time.tv_sec), path);
	logging(key, path);

	printf("Recv_Worker ready.\n");
	while (1) {

		clock_gettime(CLOCK_REALTIME, &timer);

		if (time_diff(timer, start) > packet_timer && pcounter > 0) { //Abfrage x-Timer

			pthread_mutex_lock(&mutex);
			new_packet.droprate = 1
					- ((double) pcounter / (double) new_packet.n);
			new_packet.delay = time_diff(recv_first, send_first);
			new_packet.datarate = ((double) cur_packet.l
					* (double) (last_pnum - first_pnum))
					/ (time_diff(recv_last, recv_first));
			if (new_packet.datarate != 0) {
				packet_timer = x * ((double) (new_packet.n - pcounter))
						* (new_packet.l) / (new_packet.datarate);
			} else {
				packet_timer = 2.0;
			}
			cur_seqnum++;
			pcounter = 0;
			pthread_mutex_unlock(&mutex);
			printf("please send");
			pthread_cond_signal(&cond);
			clock_gettime(CLOCK_REALTIME, &timer);
			start = timer;
		}

		if (buffer_is_empty(&buf) == 0) {
			nanosleep(&tim, &tim2);
		} else {
			while (buffer_is_empty(&buf) == 0) { //Durchgehen der Pakete

				current = read_buffer(&buf); //Auslesen der Paketdaten aus recv_buffer
				logging(current, path);
				data[0] = strtod(current, &pEnd);
				for (i = 1; i < DATA_NUM; i++) {
					data[i] = strtod(pEnd, &pEnd);
				}
				fill_cur_packet(data);

				if (cur_packet.seqnum == cur_seqnum) { //erwartete Sequenznummer
					if (pcounter == 0) {
						recv_first = cur_packet.t_recv;
						first_pnum = cur_packet.pnum;
						send_first = cur_packet.t_send;

					}

					if (cur_packet.pnum == (cur_packet.n - 1)) { //Paketnummer entspricht Anzahl von erwarteten Paketen

						pcounter++;
						recv_last = cur_packet.t_recv;
						last_pnum = cur_packet.pnum;

						pthread_mutex_lock(&mutex);
						new_packet.droprate = 1
								- ((double) pcounter / (double) cur_packet.n);
						new_packet.n = cur_packet.n;
						new_packet.l = cur_packet.l;

						new_packet.delay = time_diff(recv_first, send_first);
						if (pcounter == 1) {
							new_packet.datarate = (double) cur_packet.l
									/ (time_diff(recv_first, send_first));
						}

						else {
							new_packet.datarate =
									((double) cur_packet.l
											* ((double) (last_pnum - first_pnum))
											+ 12.0)
											/ (time_diff(recv_last, recv_first)); // +12   Letze bei Senden eingerechnete Interpacket gab existiert natürlich

						}

						pthread_mutex_unlock(&mutex);
						pthread_cond_signal(&cond);
						n = cur_packet.n;
						pcounter = 0;
						cur_seqnum++; //Train beendet, erwarte nächsten.
					}

					else if (cur_packet.pnum < (cur_packet.n - 1)) { //Paketnummer innerhalb des Packettrains

						pcounter++;
						recv_last = cur_packet.t_recv;
						last_pnum = cur_packet.pnum;
						new_packet.n = cur_packet.n;
						new_packet.l = cur_packet.l;
					}

					else {
						printf("PNUM bigger than expected"); //Paketnummer größer als erwartet, verwerfen.
					}
				} else if (cur_packet.seqnum > cur_seqnum) { // Neue Seqnum min. letztes Paket des alten Trains nicht angekommen.
					ptfpc++;
					cur_seqnum++;
					recv_first = cur_packet.t_recv;
					first_pnum = cur_packet.pnum;
					send_first = cur_packet.t_send;

					new_packet.n = cur_packet.n;
					new_packet.l = cur_packet.l;

				} else {	//Seqnum kleiner als erwartet
					if (actseq < cur_packet.seqnum) {
						fpc++;
					}
					//logging(falspos,fppath);
					actseq = cur_packet.seqnum;
				}

				clock_gettime(CLOCK_REALTIME, &timer);
				start = timer;

			}
		}
	}
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
int main(int argc, char **argv) {

	if (argc < 3) {
		printf("USE: %s<PACKET Anzahl> <PACKET size> <alpa-Wert> <beta-Wert> <Messdauer""> \n", *argv);
		exit(1);
	}

	n = atoi(argv[1]);
	packet_size = atoi(argv[2]);
	x = atof(argv[3]);
	printf("n: %i , l: %i alpha:%f \n", n, packet_size, x);

	pthread_t thread[4];
	int thr = 0;
	void * status;
	packet_timer = 2.0;
	struct sockaddr_in this_addr, client_addr; //Strukturen zum speichern und verändern der Server/Clientaddr

	sock = socket(AF_INET, SOCK_DGRAM, 0); //Socket erstellen
	if (sock == -1) {
		perror(" no Socket!!");
	}

	/* Addr , port und family für bind*/
	this_addr.sin_family = AF_INET;
	this_addr.sin_port = htons(5005);
	this_addr.sin_addr.s_addr = htonl(INADDR_ANY);

	if (bind(sock, (struct sockaddr*) &this_addr, sizeof(this_addr)) == -1) {
		perror("bind()");
	}

	pthread_mutex_init(&mutex, NULL);
	pthread_cond_init(&cond, NULL);
	//erstellen der Threads und warten.
	thr = pthread_create(&thread[1], NULL, &udpsend, NULL);
	sleep(1);
	thr = pthread_create(&thread[3], NULL, &send_logging, NULL);
	sleep(1); // damit udpsend mit dem mutexlock beginnt;
	thr = pthread_create(&thread[0], NULL, &udprecv, NULL);
	sleep(1);
	thr = pthread_create(&thread[2], NULL, &rec_worker, NULL);

	thr = pthread_join(thread[1], &status);
	thr = pthread_join(thread[0], &status);
	thr = pthread_join(thread[2], &status);
	thr = pthread_join(thread[3], &status);

	pthread_mutex_destroy(&mutex);
	pthread_cond_destroy(&cond);

}

