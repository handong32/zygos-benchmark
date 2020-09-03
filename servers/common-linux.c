#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <signal.h>

#include "config.h"
#include "common.h"
#include "memcached.h"

//#define BUFSIZE 2048
#define BUFSIZE (0x1 << 17)

struct conn {
#if CONFIG_REGISTER_FD_TO_ALL_EPOLLS
	volatile int lock;
#endif
	int fd;
	enum conn_state state;
	binary_header_t header;
	int buf_head;
	int buf_tail;
	unsigned char buf[BUFSIZE];
};

#define BACKLOG 8192
#define MAX_THREADS 64
#define EPOLLEXCLUSIVE (1 << 28)

static int epollfd[MAX_THREADS];
__thread int thread_no;
int nr_cpu;
//double dsum[16];
//double freqsum[16];
//long long workerloop[16];

inline unsigned long long rdtsc()
{
  unsigned long long tsc;
  asm volatile("rdtsc;"
               "shl $32,%%rdx;"
	       "or %%rdx,%%rax"
               : "=a"(tsc)
               :
               : "%rcx", "%rdx");
  return tsc;
}

struct conn_stats {
    int sfd;
    int id;
    unsigned long long start;
    unsigned long long end;
};
#define MAX_CONN_STATS 4096
struct conn_stats cat_conn_stats[MAX_CONN_STATS];
int conn_stats_start_count = 0;
int conn_stats_end_count = 0;
pthread_mutex_t connlock; 

static int avail_bytes(struct conn *conn)
{
	return conn->buf_tail - conn->buf_head;
}

static int recv_exactly(struct conn *conn, void *buf, size_t size)
{
	ssize_t ret;
	if (avail_bytes(conn) < size) {
		if (conn->buf_head) {
			memmove(conn->buf, &conn->buf[conn->buf_head], conn->buf_head);
			conn->buf_tail -= conn->buf_head;
			conn->buf_head = 0;
		}
		while (conn->buf_tail < size) {
			assert(BUFSIZE - conn->buf_tail > 0);
			ret = recv(conn->fd, &conn->buf[conn->buf_tail], BUFSIZE - conn->buf_tail, 0);
			if (ret <= 0)
				return ret;
			conn->buf_tail += ret;
		}
	}
	memcpy(buf, &conn->buf[conn->buf_head], size);
	conn->buf_head += size;

	return 1;
}

static int send_exactly(struct conn *conn, void *buf, size_t size)
{
	ssize_t ret;
	char *cbuf = (char *) buf;
	size_t partial = 0;

	while (partial < size) {
		ret = send(conn->fd, &cbuf[partial], size - partial, MSG_NOSIGNAL);
		if (ret <= 0)
			return ret;
		partial += ret;
	}

	return 1;
}

static int drain_exactly(struct conn *conn, size_t size)
{
	ssize_t ret;
	char buf[BUFSIZE];
	size_t left;

	if (avail_bytes(conn) < size) {
		left = size;
		left -= avail_bytes(conn);
		conn->buf_head = 0;
		conn->buf_tail = 0;
		//printf("drain_exactly left = %d sizeof(buf)=%d\n", (int)left, (int)sizeof(buf));
		assert(left < sizeof(buf));
		while (left) {
			assert(left > 0);
			ret = recv(conn->fd, buf, left, 0);
			if (ret <= 0)
				return ret;
			left -= ret;
		}
	} else {
		conn->buf_head += size;
	}

	return 1;
}

static int handle_ret(struct conn *conn, ssize_t ret, int line)
{
	if (ret == 0) {
		close(conn->fd);
		/* TODO: should also free conn */
		return 1;
	} else if (ret == -1) {
		switch (errno) {
		case EAGAIN:
		case EBADF:
			return 1;
		case EPIPE:
		case ECONNRESET:
			close(conn->fd);
			/* TODO: should also free conn */
			return 1;
		default:
			fprintf(stderr, "Unexpected errno %d at line %d\n", errno, line);
		}
	}

	assert(ret == 1);

	return 0;
}

static void drive_machine(struct conn *conn)
{
	ssize_t ret;

	switch (conn->state) {
	case STATE_HEADER:
next_request:
		ret = recv_exactly(conn, &conn->header, sizeof(conn->header));
		if (handle_ret(conn, ret, __LINE__))
			return;
		assert(conn->header.magic == 0x80);
		conn->state = STATE_EXTRA;
		/* fallthrough */
	case STATE_EXTRA:
		ret = drain_exactly(conn, conn->header.extra_len);
		if (handle_ret(conn, ret, __LINE__))
			return;
		conn->state = STATE_KEY;
		/* fallthrough */
	case STATE_KEY:
		ret = drain_exactly(conn, __builtin_bswap16(conn->header.key_len));
		if (handle_ret(conn, ret, __LINE__))
			return;
		conn->state = STATE_VALUE;
		/* fallthrough */
	case STATE_VALUE:
		if (conn->header.opcode == CMD_SET) {
			ret = drain_exactly(conn, __builtin_bswap32(conn->header.body_len) - __builtin_bswap16(conn->header.key_len) - conn->header.extra_len);
			if (handle_ret(conn, ret, __LINE__))
				return;
		} else {
			assert(conn->header.opcode == CMD_GET);
		}
		conn->state = STATE_PROC;
		/* fallthrough */
	case STATE_PROC:
	        process_request();
		conn->state = STATE_RESPONSE;
		conn->header.magic = 0x81;
		conn->header.status = __builtin_bswap16(1); /* Key not found */
		conn->header.body_len = 0;
		/* fallthrough */
	case STATE_RESPONSE:
		ret = send_exactly(conn, &conn->header, sizeof(conn->header));
		if (handle_ret(conn, ret, __LINE__))
			return;
		conn->state = STATE_HEADER;
		if (avail_bytes(conn) >= sizeof(conn->header))
			goto next_request;
		break;
	default:
		assert(0);
	}
}

static void epoll_ctl_add(int fd, void *arg)
{
	struct epoll_event ev;

	ev.events = EPOLLIN | EPOLLERR;
#if CONFIG_USE_EPOLLEXCLUSIVE
	ev.events |= EPOLLEXCLUSIVE;
#endif
	ev.data.fd = fd;
	ev.data.ptr = arg;
#if CONFIG_REGISTER_FD_TO_ALL_EPOLLS
	for (int i = 0; i < nr_cpu; i++) {
		if (epoll_ctl(epollfd[i], EPOLL_CTL_ADD, fd, &ev) == -1) {
			perror("epoll_ctl: EPOLL_CTL_ADD");
			exit(EXIT_FAILURE);
		}
	}
#else
	if (epoll_ctl(epollfd[thread_no], EPOLL_CTL_ADD, fd, &ev) == -1) {
		perror("epoll_ctl: EPOLL_CTL_ADD");
		exit(EXIT_FAILURE);
	}
#endif
}

static void setnonblocking(int fd)
{
	int flags;

	flags = fcntl(fd, F_GETFL, 0);
	assert(flags >= 0);
	flags = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	assert(flags >= 0);
}

static int try_lock(struct conn *conn)
{
#if CONFIG_REGISTER_FD_TO_ALL_EPOLLS
	asm volatile("" : : : "memory");
	int ret = __sync_bool_compare_and_swap(&conn->lock, 0, 1);
	asm volatile("" : : : "memory");
	return ret;
#else
	return 1;
#endif
}

static void unlock(struct conn *conn)
{
#if CONFIG_REGISTER_FD_TO_ALL_EPOLLS
	asm volatile("" : : : "memory");
	conn->lock = 0;
	asm volatile("" : : : "memory");
#endif
}

static void *tcp_thread_main(void *arg)
{
	struct sockaddr_in sin;
	int sock;
	int one;
	int ret, i, nfds, conn_sock;
	struct epoll_event ev, events[CONFIG_MAX_EVENTS];
	struct conn *conn;

	sock = socket(AF_INET, SOCK_STREAM, 0);
	if (!sock) {
		perror("socket");
		exit(1);
	}

	setnonblocking(sock);

	one = 1;
	if (setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, (void *) &one, sizeof(one))) {
		perror("setsockopt(SO_REUSEPORT)");
		exit(1);
	}

	one = 1;
	if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (void *) &one, sizeof(one))) {
		perror("setsockopt(SO_REUSEADDR)");
		exit(1);
	}

	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = htonl(0);
	sin.sin_port = htons(11211);

	if (bind(sock, (struct sockaddr*)&sin, sizeof(sin))) {
		perror("bind");
		exit(1);
	}

	if (listen(sock, BACKLOG)) {
		perror("listen");
		exit(1);
	}

	thread_no = (long) arg;

	init_thread();

	epollfd[thread_no] = epoll_create1(0);
	ev.events = EPOLLIN;
	ev.data.u32 = 0;
	ret = epoll_ctl(epollfd[thread_no], EPOLL_CTL_ADD, sock, &ev);
	assert(!ret);

	while (1) {
		nfds = epoll_wait(epollfd[thread_no], events, CONFIG_MAX_EVENTS, -1);
		//assert(nfds > 0);
		for (i = 0; i < nfds; i++) {
			if (events[i].data.u32 == 0) {
				conn_sock = accept(sock, NULL, NULL);
				if (conn_sock == -1) {
					perror("accept");
					exit(EXIT_FAILURE);
				}
				setnonblocking(conn_sock);
				if (setsockopt(conn_sock, IPPROTO_TCP, TCP_NODELAY, (void *) &one, sizeof(one))) {
					perror("setsockopt(TCP_NODELAY)");
					exit(1);
				}
				conn = malloc(sizeof *conn);
#if CONFIG_REGISTER_FD_TO_ALL_EPOLLS
				conn->lock = 0;
#endif
				conn->fd = conn_sock;
				conn->state = STATE_HEADER;
				conn->buf_head = 0;
				conn->buf_tail = 0;
				
				pthread_mutex_lock(&connlock);
				
				cat_conn_stats[conn_stats_start_count].start = rdtsc();
				//printf("conn_stats_start %d %lld\n", conn_stats_start_count, 	cat_conn_stats[conn_stats_start_count].start);
				cat_conn_stats[conn_stats_start_count].sfd = conn_sock;
				cat_conn_stats[conn_stats_start_count].id = conn_stats_start_count;
				conn_stats_start_count ++;
				pthread_mutex_unlock(&connlock);
				
				epoll_ctl_add(conn_sock, conn);
			} else {
				conn = events[i].data.ptr;
				if (events[i].events & (EPOLLHUP | EPOLLERR)) {
					close(conn->fd);
					/* TODO: should also free conn */
					pthread_mutex_lock(&connlock);
					cat_conn_stats[conn_stats_end_count].end = rdtsc();
					//printf("conn_stats_end %d %lld\n", conn_stats_end_count, cat_conn_stats[conn_stats_end_count].end);
					conn_stats_end_count ++;
					pthread_mutex_unlock(&connlock);
				} else {
					if (try_lock(conn)) {
						drive_machine(conn);
						unlock(conn);
					}
				}
			}
		}
	}

	return NULL;
}

void init_linux(void)
{
	srand48(mytime());

	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	pthread_getaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
	nr_cpu = CPU_COUNT(&cpuset);
	nr_cpu -= 1;
	printf("%s: nr_cpu = %d\n", __FUNCTION__, nr_cpu);
	//nr_cpu = 1;
}

static void sig_usr2handler(const int sig) {
    int i;
    FILE *fptr;
    fptr = fopen("/tmp/mcdsilo.rdtsc", "w");
    if(fptr == NULL) {
        perror("Error creating file mcdsilo.rdtsc\n");      
    }
    
    for(i=0;i<conn_stats_start_count;i++) {     
      fprintf(fptr, "%d %d %llu %llu\n",
	      cat_conn_stats[i].id, cat_conn_stats[i].sfd,
	      cat_conn_stats[i].start, cat_conn_stats[i].end);
    }

    //    double tdsum = 0.0;
    //double tfreqsum = 0.0;
    //long long tworkerloop = 0;

    //for(i=0;i<16;i++) {
    //  tdsum += dsum[i];
    //  tfreqsum += freqsum[i];
    //  tworkerloop += workerloop[i];
    //}
    
    //fprintf(fptr, "tdsum = %lf\n", tdsum);
    //fprintf(fptr, "tfreqsum = %lf\n", tfreqsum);
    //fprintf(fptr, "tworkerloop = %lld\n", tworkerloop);

    //memset(dsum, 0x0, sizeof(dsum));
    //memset(freqsum, 0x0, sizeof(freqsum));
    //memset(workerloop, 0x0, sizeof(workerloop));
  
    fclose(fptr);
    memset(cat_conn_stats, 0, sizeof(cat_conn_stats));
    conn_stats_start_count = 0;
    conn_stats_end_count = 0;
}

void start_linux_server(void)
{
	int i;
	pthread_t tid;
	//printf("start_linux_server\n");
	signal(SIGUSR2, sig_usr2handler);

	if (pthread_mutex_init(&connlock, NULL) != 0) {
	  printf("\n mutex init has failed\n");
	  return;
	}
	
	for (i = 1; i < nr_cpu; i++) {
		if (pthread_create(&tid, NULL, tcp_thread_main, (void *) (long) i)) {
			fprintf(stderr, "failed to spawn thread %d\n", i);
			exit(-1);
		}
		printf("Spawn thread %d\n", i);
	}

	tcp_thread_main(0);
}
