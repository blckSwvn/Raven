#include "picohttpparser.h"
#include "strings.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/eventfd.h>
#include <pthread.h>
#include <sys/stat.h>

#define PORT 8080
#define MAX_EVENTS 64
#define QUE_SIZE 255

int THREAD_COUNT = 1;

void get_threads(){
	long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
	if (nprocs > 0) THREAD_COUNT = nprocs;
	printf("detected nprocs %d\n", THREAD_COUNT);
}

typedef struct worker {
	uint8_t busy_count;
	int ev_fd;
	int ep_fd;
	pthread_t tid;
	uint8_t head;
	uint8_t tail;
	int que[QUE_SIZE];
} worker;

int make_nonblocking(int fd) {
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags == -1) return -1;
	return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}


void processClient(int fd) {
    char buf[4096]; 
    const char *method, *path;
    char filepath[512];
    int pret, minor_version;
    struct phr_header headers[16];
    size_t buf_len = 0, prevbuflen = 0, method_len, path_len; 
    size_t num_headers = sizeof(headers)/sizeof(headers[0]);

    buf_len = recv(fd, buf, sizeof(buf), 0);
    if (buf_len <= 0) return;

    pret = phr_parse_request(buf, buf_len,
                             &method, &method_len,
                             &path, &path_len,
                             &minor_version, headers, &num_headers,
                             prevbuflen);
    if (pret <= 0) return;
    
    // Build full path
    if (path_len == 1 && path[0] == '/') {
        snprintf(filepath, sizeof(filepath), "../www/index.html");
    } else if (path_len > 0 && path[path_len - 1] == '/') {
        snprintf(filepath, sizeof(filepath), "../www/%.*sindex.html",
                 (int)path_len, path);
    } else {
        snprintf(filepath, sizeof(filepath), "../www/%.*s",
                 (int)path_len, path);
    }

    int f = open(filepath, O_RDONLY);
    if (f < 0) {
        // Fallback 404 page
        int f404 = open("../www/404.html", O_RDONLY);
        if (f404 < 0) {
            const char *not_found =
                "HTTP/1.1 404 Not Found\r\n"
                "Content-Length: 13\r\n"
                "Content-Type: text/plain\r\n\r\n"
                "404 Not Found";
            send(fd, not_found, strlen(not_found), 0);
            shutdown(fd, SHUT_WR);
            return;
        } else {
	// size_t i = 0;
	// while(i > num_headers) {
	// 	if(strcpy(headers[i].name))
	// 	i++;
	// }

            struct stat st;
            if (fstat(f404, &st) == 0) {
                char header[256];
                int hlen = snprintf(header, sizeof(header),
                    "HTTP/1.1 404 Not Found\r\n"
                    "Content-Type: text/html\r\n"
                    "Content-Length: %jd\r\n\r\n",
                    (intmax_t)st.st_size);
                send(fd, header, hlen, 0);

                char buf2[4096];
                ssize_t r;
                while ((r = read(f404, buf2, sizeof(buf2))) > 0) {
                    send(fd, buf2, r, 0);
                }
            }
            close(f404);
            shutdown(fd, SHUT_WR);
            return;
        }
    }

    struct stat st;
    if (fstat(f, &st) == 0) {
        char header[256];
        int hlen = snprintf(header, sizeof(header),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html\r\n"
            "Content-Length: %jd\r\n\r\n",
            (intmax_t)st.st_size);
        send(fd, header, hlen, 0);

        char buf2[4096];
        ssize_t r;
        while ((r = read(f, buf2, sizeof(buf2))) > 0) {
            send(fd, buf2, r, 0);
        }
    }
    close(f);
    shutdown(fd, SHUT_WR);
}


void *work(void *arg) {
	worker *job = (worker *)arg;
	job->busy_count = 0;
	job->head = 0;
	job->tail = 0;
	job->ep_fd = epoll_create1(0);
	if (job->ep_fd < 0) { perror("epoll_create1 failed"); pthread_exit(NULL); }

	job->ev_fd = eventfd(0, EFD_NONBLOCK);
	if (job->ev_fd < 0) { perror("eventfd failed"); pthread_exit(NULL); }

	struct epoll_event events[MAX_EVENTS];
	struct epoll_event ev;
	ev.events = EPOLLIN;
	ev.data.fd = job->ev_fd;

	// Register eventfd **once** before the loop
	if (epoll_ctl(job->ep_fd, EPOLL_CTL_ADD, job->ev_fd, &ev) < 0) {
		perror("epoll_ctl failed");
		pthread_exit(NULL);
	}

	while (1) {
		int n = epoll_wait(job->ep_fd, events, MAX_EVENTS, -1);
		if (n < 0) { perror("epoll_wait failed"); break; }

		for (int i = 0; i < n; i++) {

			while(job->busy_count > 0) {
				int client_fd = job->que[job->head];
				job->head++;
				job->busy_count--;

				struct epoll_event client_ev;
				client_ev.events = EPOLLIN;
				client_ev.data.fd = client_fd;
				epoll_ctl(job->ep_fd, EPOLL_CTL_ADD, client_fd, &client_ev);
			}

			int fd = events[i].data.fd;
			if (fd == job->ev_fd) {
				uint64_t count;
				read(job->ev_fd, &count, sizeof(count)); // drain eventfd
			} else {
				processClient(fd);
				epoll_ctl(job->ep_fd, EPOLL_CTL_DEL, fd, NULL);
				close(fd);
			}
		}
	}
	pthread_exit(NULL);
}

int main() {
	get_threads();
	int listen_fd;
	struct sockaddr_in adress;
	listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if(listen_fd < 0) { perror("socket failed"); exit(EXIT_FAILURE); }

	int opt = 1;
    // Allow reuse of address and port
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

	adress.sin_family = AF_INET;
	adress.sin_addr.s_addr = INADDR_ANY;
	adress.sin_port = htons(PORT);

	if(bind(listen_fd, (struct sockaddr *)&adress, sizeof(adress)) < 0) {
		perror("binds failed");
		exit(EXIT_FAILURE);
	}

	// Listen
	if (listen(listen_fd, SOMAXCONN) < 0) {
		perror("listen failed");
		exit(EXIT_FAILURE);
	}

	// Make nonblocking
	make_nonblocking(listen_fd);

	int ep_fd = epoll_create1(0);
	if (ep_fd < 0) { perror("epoll_create1 failed"); exit(EXIT_FAILURE); }

	struct epoll_event ev;
	ev.events = EPOLLIN;
	ev.data.fd = listen_fd;
	if (epoll_ctl(ep_fd, EPOLL_CTL_ADD, listen_fd, &ev) < 0) {
		perror("epoll_ctl failed");
		exit(EXIT_FAILURE);
	}

	struct epoll_event events[MAX_EVENTS];

	worker thread[THREAD_COUNT];
	int i = 0;
	while(i < THREAD_COUNT) {
		pthread_create(&thread[i].tid, NULL, work, &thread[i]);
		i++;
	}


	printf("server listening on port %i\n", PORT);

	while(1) {
		int n = epoll_wait(ep_fd, events, MAX_EVENTS, -1);
		if(n < 0) {
			perror("epoll_wait failed"); exit(EXIT_FAILURE);
		}
		i = 0;
		while(i < n){
			int fd = events[i].data.fd;
			if(fd == listen_fd){
				struct sockaddr_in client_addr;
				socklen_t client_len = sizeof(client_addr);
				int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
				if(client_fd < 0)continue;
				make_nonblocking(client_fd);

				uint8_t y = 0;
				uint8_t least_index = 0;
				uint8_t min_load = 255;

				while(y < THREAD_COUNT) {
					if(thread[y].busy_count < min_load) {
						min_load = thread[y].busy_count;
						least_index = y;
					}
					y++;
				}

				// Now assign client to the least busy worker
				thread[least_index].que[thread[least_index].tail++] = client_fd;
				thread[least_index].busy_count++;

				uint64_t one = 1;
				write(thread[least_index].ev_fd, &one, sizeof(one));
			}
		}
	}
}
