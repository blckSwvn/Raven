#include "bits/time.h"
#include "bits/types/struct_itimerspec.h"
#include "picohttpparser/picohttpparser.h"
#include "arena/arena.h"
#include "time.h"
#include <sys/timerfd.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <malloc.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/epoll.h>

#define DEBUG 1

typedef struct {
	const char *method;
	size_t method_len;
	const char *path;
	size_t path_len;
	int minor_version;
	struct phr_header headers[16];
	size_t num_headers;
} HTTPRequest;

struct l_conn {
	int fd;
	int listen;
}; 

#define MIN_BUF 8000 //8kb
#define MAX_BUF 32000 //32kb

struct conn {
	int fd;
	void *buf;
	uint16_t buf_length; //starts at MIN_BUF
	uint16_t buf_used;
	struct conn *next;
	struct conn *prev;
	uint64_t time;
};

typedef struct {
	const char *ext;
	const char *mime;
} mime_type;

static const mime_type mime[] = {
	{"html", "text/html"},
	{"htm", "text/html"},
	{"css", "text/css"},
	{"js", "application/javascript"},
	{"json", "application/json"},
	{"png", "image/png"},
	{"jpg", "image/jpeg"},
	{"jpeg", "image/jpeg"},
	{"gif", "image/gif"},
	{"svg", "image/svg+xml"},
	{"txt", "text/plain"},
	{"md", "text/markdown"},
	{NULL, "application/octet-stream"}
};

#define PORT 8080
#define MAX_EVENTS 255

long procs = 0;

long all_mem = 0;
long page_size = 0;
long avphys_pages = 0;

long get_threads(){
	procs = sysconf(_SC_NPROCESSORS_ONLN);
	return procs;
}


void get_mem(){
	page_size = sysconf(_SC_PHYS_PAGES);
	avphys_pages = sysconf(_SC_AVPHYS_PAGES);
	all_mem = page_size * avphys_pages;
}


inline int make_nonblocking(int fd) {
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags == -1) return -1;
	return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}


const char *get_mime(const char *path){
	const char *ext = strrchr(path, '.');
	if(!ext || ext == path)return "text/plain";
	ext++;
	uint8_t i = 0;
	//stops at 12 beeing {NULL, "application/octet-stream"}
	while(mime[i].ext){
		if(strcmp(ext, mime[i].ext) == 0){
			return mime[i].mime;
		}
		i++;
	}
	return mime[i].mime; //aplication/octet-stream
}


int parse_request(HTTPRequest *req, const char *buf, size_t buf_len){
	req->num_headers = 16;

	int pret = phr_parse_request(buf, buf_len,
			&req->method, &req->method_len,
			&req->path, &req->path_len,
			&req->minor_version, req->headers, &req->num_headers,
			0);

	return pret; // >0 means sucess, -1 means error, -2 = incomplete
}


void *build_path(const char *path, size_t path_len, size_t filepath_len){
	char *filepath = arena_alloc(filepath_len);

	if(path_len > 0 && path[0] == '/'){
		path++;
		path_len--;
	}

	if(path_len == 0)
		snprintf(filepath, filepath_len, "../www/index.html");
	else{
		snprintf(filepath, filepath_len, "../www/%.*s",
				(int)path_len, path);
		if(filepath[strlen(filepath) -1] == '/')
			strncat(filepath, "index.html", filepath_len - strlen(filepath) - 1);
		else if(access(filepath, F_OK) != 0){
			strncat(filepath, ".html", filepath_len - strlen(filepath) - 1);
		}
	}
	return filepath;
}


inline void handle_file_not_found(int fd){
	int f404 = open("../www/404.html", O_RDONLY);
	if(f404 < 0){
		const char *not_found =
			"HTTP/1.1 404 Not Found\r\n"
			"Content-Type: text/html\r\n"
			"Content-Length: 13\r\n"
			"Connection: close\r\n\r\n";
		send(fd, not_found, strlen(not_found), 0);
		return;
	} else {
		struct stat st;
		if (fstat(f404, &st) == 0) {
			size_t header_len = 256;
			char *header = arena_alloc(256);
			int hlen = snprintf(header, header_len,
					"HTTP/1.1 404 Not Found\r\n"
					"Content-Type: text/html\r\n"
					"Content-Length: %jd\r\n"
					"Connection: close\r\n\r\n",
					(intmax_t)st.st_size);
			send(fd, header, hlen, 0);

			size_t buf2_len = 4096;
			char *buf2 = arena_alloc(buf2_len);
			ssize_t r;
			while ((r = read(f404, buf2, buf2_len)) > 0) {
				send(fd, buf2, r, 0);
			}
		}
		close(f404);
		return;
	}
}


const char *process_client(int fd, HTTPRequest *req){
	const char *path = req->path;
	size_t path_len = req->path_len;
	size_t filepath_len = 512;

	char *filepath = build_path(path, path_len, filepath_len);

	int f = open(filepath, O_RDONLY);
	if(f < 0){
		handle_file_not_found(fd);
		return "close";
	}

	char *connection = "close";
	if(req->minor_version){
		size_t i = 2;
		while(i < req->num_headers){
			if(strncmp(req->headers[i].name, "Connection", 10) == 0){
				if(strncmp(req->headers[i].value, "close", 5) != 0){
					connection = "keep-alive";
				} else {
					connection = "close";
				}
				break;
			}
			i++;
		}
	}

	const char *mime = get_mime(filepath);
	struct stat st;
	if(fstat(f, &st) == 0){
		size_t header_len = 512;
		char *header = arena_alloc(header_len);
		int hlen = snprintf(header, header_len,
				"HTTP/1.1 200 OK\r\n"
				"Content-Type: %s\r\n"
				"Content-Length: %jd\r\n"
				"Connection: %s\r\n\r\n",
				mime, (intmax_t)st.st_size,
				connection);
		send(fd, header, hlen, 0);
		size_t buf2_len = 4096;
		char *buf2 = arena_alloc(buf2_len);
		ssize_t r;
		while((r = read(f, buf2, buf2_len)) > 0){
			send(fd, buf2, r, 0);
		}
	}
	close(f);
	return connection;
}

#ifdef DEBUG
void dump_list(void *list){
	struct conn *client = NULL;
	if(list)client = list;

	printf("list:%p\n",list);
	while(client){
		printf("%p\n", client);
		printf("next:%p\n", client->next);
		printf("prev:%p\n", client->prev);
		client = client->next;
	}
	printf("\n");
}
#endif

void rm_from_list(struct conn *client, void **list){
	printf("rm_from_list\n");
	if(client->next)client->next->prev = client->prev;
	if(client->prev)client->prev->next = client->next;
	if(*list == client)*list = client->next;

	client->next = NULL;
	client->prev = NULL;
	dump_list(*list);
}

void add_to_list(struct conn *client, void **list){
	printf("add_to_list\n");
	if(*list){
		struct conn *head = *list;
		client->next = head;
		head->prev = client;
	} else	{
		client->next = NULL;
	}

	client->prev = NULL;
	*list = client;
	dump_list(*list);
}

void update_timer(int tfd, struct conn *curr, void **active_list){
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	uint64_t now = ts.tv_sec;

	struct conn *head = *active_list;
	if(curr->time < head->time){
		rm_from_list(curr, *active_list);
		add_to_list(curr, *active_list);
		head = *active_list;
	}

	time_t soonest = head->time - ts.tv_sec;

	struct itimerspec its;
	its.it_value.tv_sec = soonest;
	its.it_value.tv_nsec = 0;
	timerfd_settime(tfd, 0, &its, NULL);
}

int main(){
	// get_threads();
	// get_mem();

	struct sockaddr_in adress;
	int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if(listen_fd < 0){
		perror("socket failed");
	} 

	adress.sin_family = AF_INET;
	adress.sin_addr.s_addr = INADDR_ANY;
	adress.sin_port = htons(PORT);

	if(bind(listen_fd,(struct sockaddr *)&adress, sizeof(adress)) < 0){
		perror("binds failed");	
		return 2;
	}

	listen(listen_fd, SOMAXCONN);

	make_nonblocking(listen_fd);

	int epoll_fd = epoll_create1(0);

#define magic_l_val -1
	struct epoll_event ev;
	struct l_conn *data = malloc(sizeof(struct l_conn));
	data->listen = magic_l_val;
	data->fd = listen_fd;

	ev.events = EPOLLIN;
	ev.data.ptr = data;
	epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev);

	struct epoll_event events[MAX_EVENTS];

#define magic_t_val -2
	int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
	struct l_conn *timer = malloc(sizeof(struct l_conn));
	timer->fd = tfd;
	timer->listen = magic_t_val;
	struct epoll_event tev;
	tev.events = EPOLLIN;
	tev.data.ptr = timer; 

	struct itimerspec its = {0};

	timerfd_settime(tfd, 0, &its, NULL);

	epoll_ctl(epoll_fd, EPOLL_CTL_ADD, tfd, &tev);

	printf("server listenning on port %i\n", PORT);

	void *inactive_list = NULL;
	void *active_list = NULL;
	arena_init(1024 * 1024); //1MB
	while(1){
		int wait = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);

		int i = 0;
		while(wait > i){
			struct l_conn *listen_data = events[i].data.ptr;
			if(listen_data->listen != magic_l_val && listen_data->listen != magic_t_val){
				struct conn *client = events[i].data.ptr;
				int client_fd = client->fd;

				int r = read(client_fd,
						client->buf + client->buf_used,
						client->buf_length - client->buf_used);
				if(r > 0){
					client->buf_used += r;

					struct timespec ts;
					clock_gettime(CLOCK_MONOTONIC, &ts);
					client->time = ts.tv_sec + 5; //resets time to current + 5 seconds
				} else if (r == 0){
					//client closed conn
				} else {
					epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, NULL);
					close(client_fd);
					free(client->buf);
					continue;
				} 

				HTTPRequest req;

				// >0 means sucess, -1 means error, -2 = incomplete
				int parsed = parse_request(&req, client->buf, client->buf_used);
				if(parsed > 0){
					const char *connection = process_client(client_fd, &req);
					if(strcmp(connection, "close") == 0){
						printf("close\n");
						epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, NULL);
						close(client_fd);
						free(client->buf);
						rm_from_list(client, &active_list);
						add_to_list(client, &inactive_list);
					} else {
						printf("keep-alive\n");
						memmove(client->buf, client->buf + parsed, client->buf_used - parsed);
						client->buf_used -= parsed;
						update_timer(tfd, client, &active_list);
					}

				} else if(parsed == -1){
					printf("close");
					epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, NULL);
					close(client_fd);
					free(client->buf);
					rm_from_list(client, &active_list);
					add_to_list(client, &inactive_list);
				}
			} else if(listen_data->listen == magic_l_val){

				//new client
				struct sockaddr_in client_addr;
				socklen_t client_len = sizeof(client_addr);
				int new_client_fd = accept(listen_fd,(struct sockaddr *)&client_addr, &client_len);

				if(new_client_fd < 0)continue;

				make_nonblocking(new_client_fd);

				struct epoll_event client_ev;
				client_ev.events = EPOLLIN;

				struct conn *new_conn;
				struct timespec ts;
				if(inactive_list){
					printf("inactive_list\n");
					new_conn = inactive_list;
					rm_from_list(new_conn, &inactive_list);
					new_conn->fd = new_client_fd;
					new_conn->buf_length = MIN_BUF;
					new_conn->buf = malloc(MIN_BUF);
					new_conn->buf_used = 0;
					new_conn->time = ts.tv_sec + 10;
				} else {
					printf("!inactive_list\n");
					new_conn = malloc(sizeof(struct conn));
					printf("new_conn:%p\n", new_conn);
					new_conn->fd = new_client_fd;
					new_conn->buf_length = MIN_BUF;
					new_conn->buf = malloc(MIN_BUF);
					new_conn->next = NULL;
					new_conn->prev = NULL;
					new_conn->buf_used = 0;
					new_conn->time = ts.tv_sec + 10;
				}
				add_to_list(new_conn, &active_list);

				client_ev.data.ptr = new_conn;
				epoll_ctl(epoll_fd, EPOLL_CTL_ADD, new_client_fd, &client_ev);
			} else if(listen_data->listen == magic_t_val){
				printf("magic_t_val\n");
				uint64_t experation;
				read(tfd, &experation, sizeof experation);

				struct timespec now;
				clock_gettime(CLOCK_MONOTONIC, &now);

				struct conn *curr = active_list;

				while(curr){
					struct conn *next = curr->next;
					if(curr->time <= now.tv_sec){
						epoll_ctl(epoll_fd, EPOLL_CTL_DEL, curr->fd, NULL);
						close(curr->fd);
						free(curr->buf);
						rm_from_list(curr, &active_list);
						add_to_list(curr, &inactive_list);
					}
					curr = next;
				}
				struct itimerspec its = {0};
				timerfd_settime(listen_data->fd, 0, &its, NULL);
			}
			i++;
		}

	}
	arena_free();
	return 0;
}

