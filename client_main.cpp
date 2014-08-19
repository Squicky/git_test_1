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

static int sock;
static int n;  // Anzahl Pakete im Train
static int packet_size;
static double packet_timer;
static double pt_timer;
static double x;		//Toleranzparameter
static double y;
static timespec y_start, y_end; // y- Timer
static timespec pt_start;
static int end;		// Abbruch Bedingung für Messunf
static double times; 		// Eingegebene Dauer der Messung
static sockaddr_in addr;
static pthread_mutex_t mutex;
static pthread_cond_t cond;
static buffer buf;
static buffer send_buf;
static packet cur_packet;
static p_data new_packet;
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void logging(char* data, char* path) //loggen von data
		{
	FILE *file;

	file = fopen(path, "a+");
	fputs(data, file);
	fputs("\n", file);
	fclose(file);

}

double time_diff(timespec first, timespec last) //Berechnet aus zwei timespec de Zeitdifferenz
		{
	double result = 0;
	long sec = (first.tv_sec - last.tv_sec);
	long nnsec = first.tv_nsec - last.tv_nsec;
	double nsec = (double) (first.tv_nsec - last.tv_nsec);

	result = ((double) sec + (nsec / 1000000000));
	return (result);
}

void *fill_cur_packet(double *data) {   //füllt Daten in Paketstruktur
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
	cur_packet.delay = time_diff(cur_packet.t_recv, cur_packet.t_send);	//Latenz des akutellen Pakets

}

void * print_p_data(p_data * data) {
	printf("n:%ld \n", data->n);
	printf("l:%ld \n", data->l);
	//printf("seq :%ld \n",data->seqnum); 
	//data->datarate; 
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

void *send_logging(void *vdata)	//Methode für das loggen der gesendeten Pakete (Thread)
		{
	char* current;
	char path[1024] = "";
	char key[] =
			"# packet_size packet_number seqnum delay datarate droprate timestpamp_sec timestpamp_nsec\n";
	timespec time;

	clock_gettime(CLOCK_REALTIME, &time);

	sprintf(path, "%dBytes_%d_x%f_y%f_%fs_send.txt", packet_size, n, x, y,
			times);
	logging(ctime(&time.tv_sec), path);
	logging(key, path);
	printf("Send_Logger ready.\n");
	long how_often = 0;
	struct timespec tim, tim2;
	tim.tv_sec = 0;
	tim.tv_nsec = 10000000L;
	while (end == 0) {
		if (buffer_is_empty(&send_buf) == 0) {

			current = read_buffer(&send_buf);	//entnahme aus dem Ringbuffer
			logging(current, path);

		} else {
			//TODO: Why the hack do you do busy waiting here?
			nanosleep(&tim, &tim2);
		}
	}
	return NULL;
}

void *udprecv(void * vdata)		//Empfangen von Paketen (Thread)
		{

	//int i =0;
	int rec;
	socklen_t slen = sizeof(addr);		//für recvfrom
	char client_ip[INET_ADDRSTRLEN] = "";
	char msg_buffer[100];			//Stringbuffer für zuempfangene Nachricht
	char time_s[12];			//Buffer für Zeitstempel
	timespec time;
	//char * msg;
	//printf("n: %d\n",n);
	create_buffer(&buf, 2 * n, 100);//Erzeugen des recv_buffers mit ausreichender Größe
	printf("Recv ready.\n");
	while (end == 0) {

		msg_buffer[0] = '\0';
		time_s[0] = '\0';
		if (rec = recvfrom(sock, msg_buffer, sizeof(msg_buffer), 0,
				(struct sockaddr*) &addr, &slen) > 0) //benötigt socklen_t
				{

			//Recv_Timestamp hinzufügen.

			clock_gettime(CLOCK_REALTIME, &time);

			sprintf(time_s, "%ld %ld ", time.tv_sec, time.tv_nsec);
			strcat(msg_buffer, time_s);
			write_buffer(&buf, msg_buffer);

		}

	}
	delete_buffer(&buf);
	return (void*) vdata;
}
void *udpsend(void * vdata) {				//Senden der Pakete (Thread)

	char msg_first[70] = "";//für den Teil der Pakete, der für alle im Packettrain identisch ist
	char msg[70] = "";				//für pnum etc
	int i = 0;
	long seqnum = 0;
	char time_s[12];				//für Sendezeitstempel			
	timespec time;

	char client_ip[INET_ADDRSTRLEN] = "";
	create_buffer(&send_buf, 2 * n, 70);
	printf("Udpsend ready.\n");
	while (end == 0) {
		pthread_mutex_lock(&mutex);
		pthread_cond_wait(&cond, &mutex);//Warten auf Sendesignal von RECV_WORKER
		sprintf(msg_first, "%ld %ld %ld %f %.0f %f ", new_packet.l,
				new_packet.n, seqnum, new_packet.delay, new_packet.datarate,
				new_packet.droprate); //Erzeugen des ersten Teils

		if ((time_diff(y_end, y_start)) > 0) {
			pt_timer = y * time_diff(y_end, y_start); //Berechnung des Packettrain-Timers
		}
		clock_gettime(CLOCK_REALTIME, &y_start); // Packettrain Timer Startwert.

		for (i = 0; i < n; i++) {			//Senden
			msg[0] = '\0';
			time_s[0] = '\0';
			strcat(msg, msg_first);
			clock_gettime(CLOCK_REALTIME, &time);
			sprintf(time_s, "%i %ld %ld ", i, time.tv_sec, time.tv_nsec);
			strcat(msg, time_s);

			if (sendto(sock, msg, (packet_size - 28 - 26 - 12), 0,
					(struct sockaddr*) &addr, sizeof(addr)) == -1)// Antwort senden (Paketgröße + Ehternetframe und Interpacket gap)
					{
				perror("send()");

			}
			if (seqnum != 0) {
				write_buffer(&send_buf, msg);
			}

			pthread_mutex_unlock(&mutex);
		}
		seqnum++;
	}

	sleep(5); //TODO:Not clever to free the buffer before the logger can finish it's work
	delete_buffer(&send_buf);

	return (void*) vdata;
}

void *measurment_time(void *vdata) { 	//Methode für die Dauer der Messung
	timespec m_t, m_start;
	clock_gettime(CLOCK_REALTIME, &m_start);
	while (end == 0) {
		clock_gettime(CLOCK_REALTIME, &m_t);
		if ((time_diff(m_t, m_start)) > times) {
			end = 1;				//Setze Abbruch Bedingung
		}
	}

}
void *rec_worker(void *vdata) {		//Bearbeiten der Emofangenen Pakete
	int fpc = 0;			//Counter für falsche x-Timeouts
	int ptfpc = 0;			//Counter für falsche y-Timeouts
	long actseq = 0;		//aktuelle Sequenznummer für Timeout Counter
	char path[1024] = "";
	char falspos[1024] = "";
	char fppath[] = "falsepos.txt";
	char* current;
	int i = 0;
	timespec recv_first;
	int first_pnum = 0;
	int last_pnum = 0;
	timespec recv_last;
	timespec send_first;
	char *pEnd;
	double data[DATA_NUM] = { 0 };
	long cur_seqnum = 0;
	long pcounter = 0;	//Paket Counter
	int tcounter = 0;		//Counter für Anzahl der x-Timeouts
	int pt_count = 0;		//Counter für Anzahl der y-Timeouts
	timespec timer;

	long how_often = 0;
	struct timespec tim, tim2;
	tim.tv_sec = 0;
	tim.tv_nsec = 100000L;

	clock_gettime(CLOCK_REALTIME, &timer);
	timespec start = timer;	//Startpunkt für x-Timer
	timespec m_t, m_start;	//Zeiten für Messdauer
	char key[] =
			"#packet_size packet_number seqnum delay datarate droprate SEND_tstmp_sec SEND_tstmp_nsec RECV_tstmp_sec RECV_tstmp_nsec\n";
	timespec time;

	logging(ctime(&time.tv_sec), fppath);	//Initialsieren der Recv-Log-Datei
	clock_gettime(CLOCK_REALTIME, &time);
	sprintf(path, "%dBytes_%d_x%f_y%f_%fs_recv.txt", packet_size, n, x, y,
			times);
	logging(ctime(&time.tv_sec), path);
	logging(key, path);

	printf("Recv_Worker ready.\n");
	pthread_cond_signal(&cond);
	while (end == 0) {

		clock_gettime(CLOCK_REALTIME, &timer);
		if (time_diff(timer, y_start) > pt_timer && pcounter == 0) //Abfrage y-Timer
				{
			pthread_cond_signal(&cond);
			pt_count++;
			clock_gettime(CLOCK_REALTIME, &y_start);
		}
		if (time_diff(timer, start) > packet_timer && pcounter > 0) { //Abfrage x-Timer
			tcounter++;

			pthread_mutex_lock(&mutex);
			new_packet.droprate = 1
					- ((double) pcounter / (double) new_packet.n);
			new_packet.delay = time_diff(recv_first, send_first);
			new_packet.datarate = ((double) cur_packet.l
					* (double) (last_pnum - first_pnum))
					/ (time_diff(recv_last, recv_first));

			if (new_packet.datarate != 0) { // Wenn Datenrate =0 => Fehler der Berechnung
				packet_timer = x * ((double) (new_packet.n - pcounter))
						* (new_packet.l) / (new_packet.datarate);
			} //Berechnung des x-TImers
			cur_seqnum++;
			pcounter = 0;

			pthread_mutex_unlock(&mutex);
			pthread_cond_signal(&cond);
			clock_gettime(CLOCK_REALTIME, &timer);
			start = timer;

		}

		if (buffer_is_empty(&buf) == 0) {
			nanosleep(&tim, &tim2);
		} else {
			while (buffer_is_empty(&buf) == 0) {//Abrufen der empfangen Pakete

				current = read_buffer(&buf);
				logging(current, path);
				data[0] = strtod(current, &pEnd);
				for (i = 1; i < DATA_NUM; i++) {//Füllen der Paketdatenstruktur
					data[i] = strtod(pEnd, &pEnd);
				}
				fill_cur_packet(data);

				if (cur_packet.seqnum == cur_seqnum) {//erwartete Sequenznummer
					if (pcounter == 0) {//Bei erstem Paket, Pnum und Time merken
						recv_first = cur_packet.t_recv;
						first_pnum = cur_packet.pnum;
						send_first = cur_packet.t_send;

					}

					if (cur_packet.pnum == (cur_packet.n - 1)) { // Letztes Paket des Trains

						clock_gettime(CLOCK_REALTIME, &y_end); // packettrain timer
						pcounter++;
						recv_last = cur_packet.t_recv;
						last_pnum = cur_packet.pnum;

						pthread_mutex_lock(&mutex);
						new_packet.droprate = 1
								- ((double) pcounter / (double) cur_packet.n); //Füllen der Datenstruktur für neues Paket
						new_packet.n = cur_packet.n;
						new_packet.l = cur_packet.l;
						new_packet.delay = time_diff(recv_first, send_first);
						if (pcounter == 1) {

							new_packet.datarate = (double) cur_packet.l
									/ (time_diff(recv_first, send_first));
						} else {

							new_packet.datarate = ((double) cur_packet.l
									* (double) (last_pnum - first_pnum) + 12)
									/ (time_diff(recv_last, recv_first));

						}

						pthread_mutex_unlock(&mutex);
						pthread_cond_signal(&cond);
						n = cur_packet.n;
						pcounter = 0;
						cur_seqnum++; //Train beendet, erwarte nächsten.
					}

					else if (cur_packet.pnum < (cur_packet.n - 1)) { // Pakete innerhalb des Packettrains

						pcounter++;
						recv_last = cur_packet.t_recv;
						last_pnum = cur_packet.pnum;
						new_packet.n = cur_packet.n;
						new_packet.l = cur_packet.l;
					}

					else {
						printf("PNUM %ld bigger than expected",
								cur_packet.pnum); //Paketnummer größer als erwartet, verwerfen.
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
	printf(
			"Packet-Timer- falsche Timeouts: %d  von %d \n Packettrain-Timer - falsche Timeouts: %d von %d\n",
			fpc, tcounter, ptfpc, pt_count);

}

/////////////////////////////////////////
int main(int argc, char **argv) {

	if (argc < 5) {
		printf("USE: %s <IP> <Paket Anzahl> <Paket size> <alpha-Wert> <beta-Wert> <Messdauer> \n",*argv);
		exit(0);
	}
	char *ip = argv[1];
	n = atoi(argv[2]);
	packet_size = atoi(argv[3]);
	x = atof(argv[4]);
	y = atof(argv[5]);
	times = atof(argv[6]);
	printf("IP: %s, n: %i , l: %i alpha: %f beta: %f Anzahl: %f \n", ip, n,
			packet_size, x, y, times);

	pt_timer = 2.0;
	packet_timer = 2.0;
	pthread_t thread[5];
	int thr = 0;
	end = 0;
	void * status;

	struct sockaddr_in this_addr, client_addr;//Strukturen zum speichern und verändern der Server/Clientaddr

	sock = socket(AF_INET, SOCK_DGRAM, 0);	//Socket erstellen
	if (sock == -1) {
		perror(" no Socket!!");
	}
	addr.sin_family = AF_INET;
	addr.sin_port = htons(5005);
	inet_pton(AF_INET, ip, &addr.sin_addr.s_addr);
	//Addr , port und family für bind
	this_addr.sin_family = AF_INET;
	this_addr.sin_port = htons(5000);
	this_addr.sin_addr.s_addr = htonl(INADDR_ANY);

	if (bind(sock, (struct sockaddr*) &this_addr, sizeof(this_addr)) == -1) {
		perror("bind()");
	}

	new_packet.n = n;
	new_packet.l = packet_size;
	new_packet.datarate = 0;
	new_packet.droprate = 0;
	new_packet.delay = 0;
	packet_timer = 2.0;
	pthread_mutex_init(&mutex, NULL);
	pthread_cond_init(&cond, NULL);
	// Erstellen der Threads , und warten, damit sie sich nicht gegenseitig behindern/ zufrüh beginnen.
	thr = pthread_create(&thread[1], NULL, &udpsend, NULL);
	sleep(1);
	thr = pthread_create(&thread[3], NULL, &send_logging, NULL);
	sleep(1);
	thr = pthread_create(&thread[0], NULL, &udprecv, NULL);
	sleep(1);
	thr = pthread_create(&thread[2], NULL, &rec_worker, NULL);
	sleep(1);
	thr = pthread_create(&thread[4], NULL, &measurment_time, NULL);

	thr = pthread_join(thread[1], &status);
	printf("1");
	thr = pthread_join(thread[0], &status);
	printf("2");
	thr = pthread_join(thread[2], &status);
	printf("3");
	thr = pthread_join(thread[3], &status);
	printf("4");
	thr = pthread_join(thread[4], &status);
	printf("5");

	pthread_mutex_destroy(&mutex);
	pthread_cond_destroy(&cond);
//	buffer grml;
//
//	create_buffer(&grml, 356, 100);
//	delete_buffer(&grml);
}

