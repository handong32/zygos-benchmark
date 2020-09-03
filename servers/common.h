#pragma once

#include <sys/time.h>
#include <unistd.h>

void init_ix(int udp);
void init_linux(void);
void init_thread(void);
void process_request(void);
void start_ix_server(int udp);
void start_linux_server(void);

extern __thread int thread_no;
extern int nr_cpu;
//extern double dsum[16];
//extern double freqsum[16];
//extern long long workerloop[16];

static inline long mytime(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec * 1000000 + tv.tv_usec;
}
