#include <stdio.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <pthread.h>
#include <strings.h>

#define MAX_FRM_SIZE	256

int fd;
uint32_t rtot, stot, err;
char std_buf[MAX_FRM_SIZE];
int std_len;

void* recv_thread(void *arg)
{
	int i, len;
	char buf[MAX_FRM_SIZE];
	int is_err;

	while (1) {
		bzero(buf, MAX_FRM_SIZE);
		len = read(fd, buf, MAX_FRM_SIZE);
		is_err = 0;
		for (i=0; i<std_len; ++i) {
			if (buf[i] != std_buf[i]) {
				printf("mismatch!\n");
				err++;
				is_err = 1;
				break;
			}
		}
		if (!is_err)
			rtot += len;
	}

	return NULL;
}

void* send_thread(void *arg)
{
	int i, len;
	char buf[MAX_FRM_SIZE];

	for (i=0; i<std_len; ++i)
		buf[i] = std_buf[i];
	while (1) {
		len = write(fd, buf, std_len);
		if (len != std_len) {
			printf("send err, ret=%d\n", len);
			continue;
		}
		stot += len;
	}

	return NULL;
}

int main(void)
{
	pthread_t tid;
	int i;

	fd = open("/dev/scc1", O_RDWR);

	if (fd < 0) {
		printf("open file err\n");
		exit(-1);
	}

#if 0
{
	char buf[MAX_FRM_SIZE];
	int len;

	write(fd, "hello!", 7);
	while (1) {
		len = read(fd, buf, 1024);
		printf("len=%d\n", len);
		printf("buf=%s\n", buf);
	}
}
#else
	for (i=0; i<MAX_FRM_SIZE; ++i)
		std_buf[i] = i;
	std_len = MAX_FRM_SIZE;

	pthread_create(&tid, NULL, recv_thread, NULL);
	pthread_create(&tid, NULL, send_thread, NULL);

	while (1) {
		sleep(1);
		printf("stot=%d KB\n", stot/1000);
		printf("rtot=%d KB\n", rtot/1000);
		printf("err=%d\n", err);
	}
#endif

	return 0;
}
