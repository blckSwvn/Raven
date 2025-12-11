#include "bits/time.h"
#include <sys/sendfile.h>
#include <sys/errno.h>
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
	int file;
	off_t offset;
	size_t file_size;
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


static inline char *handle_file_not_found(struct conn *client){
	int f404 = open("../www/404.html", O_RDONLY);
	if(f404 < 0){
		const char *not_found =
			"HTTP/1.1 404 Not Found\r\n"
			"Content-Type: text/html\r\n"
			"Content-Length: 13\r\n"
			"Connection: close\r\n\r\n";
		send(client->fd, not_found, strlen(not_found), 0);
		return "close";
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
			send(client->fd, header, hlen, 0);

			client->file_size = st.st_size;
			// client->offset = 0;
			ssize_t n = sendfile(client->fd, f404, &client->offset, client->file_size);
				if(n >= st.st_size){
					printf("n >= st.st_size\n");
				}
				if(errno == EAGAIN || errno == EWOULDBLOCK){
					client->file = f404;
					client->file_size = st.st_size;
					return "EAGAIN";
				}else{
					close(f404);
					return "close";
				}
			}
			printf("n >= st.st_size\n");
			close(f404);
			return "close";
	}
}


const char *process_client(struct conn *client, HTTPRequest *req){
	const char *path = req->path;
	size_t path_len = req->path_len;
	size_t filepath_len = 512;

	char *filepath = build_path(path, path_len, filepath_len);

	int f = open(filepath, O_RDONLY);
	if(f < 0){
		char *connection = handle_file_not_found(client);
		return connection;
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
		send(client->fd, header, hlen, 0);
		ssize_t n = 0;
		client->file_size = st.st_size;
		// client->offset = 0;
		n = sendfile(client->fd, f, &client->offset, client->file_size - client->offset);
		if(n < 0){
			if(errno == EAGAIN || errno == EWOULDBLOCK){
				client->file = f;
				return "EAGAIN";
			} else {
				close(f);
				return "close";
			}
		}
	}
	printf("n ?!?\n");
	close(f);
	return connection;
}


void *inactive_list = NULL;

void *active_head = NULL;
void *active_tail = NULL;

void update_timer(int tfd){
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	uint64_t now = ts.tv_sec;

	struct conn *head = active_head;
	printf("active_head:%p\n", active_head);
	printf("active_tail:%p\n", active_tail);
	printf("inactive_list:%p\n", inactive_list);

	time_t soonest = head->time - ts.tv_sec;

	struct itimerspec its = {0};
	its.it_value.tv_sec = soonest;
	timerfd_settime(tfd, 0, &its, NULL);
}

#ifdef DEBUG
void dump_list(void *list){
	struct conn *client = list;

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


static inline void append_active(struct conn *client){
#ifdef DEBUG
	printf("appeand_active\n");
#endif
	client->next = NULL;
	client->prev = active_tail;
	if(active_tail){
		struct conn *last = active_tail;
		last->next = client;
	} else {
		active_head = client;
	}
	active_tail = client;

	dump_list(active_head);
}


static inline void rm_active(struct conn *client){
#ifdef DEBUG
	printf("rm_active\n");
#endif
	if(active_head == client)
		active_head = client->next;
	if(active_tail == client)
		active_tail = client->prev;
	if(client->next)client->next->prev = client->prev;
	if(client->prev)client->prev->next = client->next;
	client->next = NULL;
	client->prev = NULL;
	dump_list(active_head);
}


static inline void insert_inactive(struct conn *client){
#ifdef DEBUG
	printf("insert_inactive\n");
#endif
	client->next = inactive_list;
	inactive_list = client;
	dump_list(inactive_list);
}


int main(){
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

	ev.events = EPOLLIN | EPOLLET;
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

	arena_init(1024 * 1024); //1MB
	while(1){
		int wait = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);

		int i = 0;
		while(wait > i){
			if(events[i].events & EPOLLOUT){
				printf("EPOLLOUT\n");
				struct conn *client = events[i].data.ptr;
				ssize_t n = sendfile(client->fd, client->file, &client->offset, client->file_size - client->offset);
				if(n < 0){
					if(errno == EAGAIN || errno == EWOULDBLOCK){
						i++;
						continue;
					} else {
					epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client->fd, NULL);
					close(client->file);
					close(client->fd);
					client->file = -1;
					client->offset = 0;
					rm_active(client);
					insert_inactive(client);
					continue;
					}
				}
				if(client->offset >= client->file_size){
					close(client->file);
					client->file = -1;
					client->offset = 0;
					struct epoll_event ev;
					ev.data.ptr = client;
					ev.events = EPOLLIN | EPOLLET;
					epoll_ctl(epoll_fd, EPOLL_CTL_MOD, client->fd, &ev);
				}
			}
			struct l_conn *listen_data = events[i].data.ptr;
			if(listen_data->listen != magic_l_val && listen_data->listen != magic_t_val){
				struct conn *client = events[i].data.ptr;

				while(1){
					int r = read(client->fd,
							client->buf + client->buf_used,
							client->buf_length - client->buf_used);
					if(r > 0){
						client->buf_used += r;
					} else if (r == 0){
						//client closed conn
						printf("client closed conn\n");
						epoll_ctl(epoll_fd, EPOLL_CTL_DEL,client->fd, NULL);
						close(client->fd);
						client->fd = -1;
						rm_active(client);
						insert_inactive(client);
					} else {
						if(errno == EAGAIN || errno == EWOULDBLOCK){
							break;
						}
						else{
							printf("fatal error\n");
							epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client->fd, NULL);
							close(client->fd);
							client->fd = -1;
							rm_active(client);
							insert_inactive(client);
						}
					} 
				}

				HTTPRequest req;

				// >0 means sucess, -1 means error, -2 = incomplete
				int parsed;
				if(client->fd){
					parsed = parse_request(&req, client->buf, client->buf_used);
				}
				if(parsed > 0){
					const char *connection = process_client(client, &req);
					if(strcmp(connection, "keep-alive") == 0){
						printf("keep-alive\n");
						memmove(client->buf, client->buf + parsed, client->buf_used - parsed);
						client->buf_used -= parsed;
						dump_list(active_head);
						update_timer(tfd);
					} else if(strcmp(connection, "EAGAIN") == 0){
						printf("EAGAIN\n");
						struct epoll_event ev;
						ev.events = EPOLLOUT | EPOLLET;
						ev.data.ptr = client;
						epoll_ctl(epoll_fd, EPOLL_CTL_MOD, client->fd, &ev);
					} else {
						printf("close\n");
						printf("client->file_size, %zu\n", client->file_size);
						printf("client->file, %d\n", client->file);
						epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client->fd, NULL);
						close(client->fd);
						rm_active(client);
						insert_inactive(client);
					}

				} else if(parsed == -1){
					printf("close");
					epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client->fd, NULL);
					close(client->fd);
					rm_active(client);
					insert_inactive(client);
				}
			} else if(listen_data->listen == magic_l_val){

				//new client
				struct sockaddr_in client_addr;
				socklen_t client_len = sizeof(client_addr);
				int new_client_fd = accept(listen_fd,(struct sockaddr *)&client_addr, &client_len);
				if(new_client_fd < 0)continue;
				printf("accepted new\n");

				make_nonblocking(new_client_fd);

				struct epoll_event client_ev;
				client_ev.events = EPOLLIN | EPOLLET;

				struct conn *new_conn;
				struct timespec ts = {0};
				clock_gettime(CLOCK_MONOTONIC, &ts);
				if(inactive_list){
					printf("inactive_list\n");
					new_conn = inactive_list;
					inactive_list = new_conn->next;
					new_conn->next = NULL;
					new_conn->prev = NULL;
					new_conn->buf_used = 0;
					new_conn->buf_length = MIN_BUF;
					new_conn->fd = new_client_fd;
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
				append_active(new_conn);

				client_ev.data.ptr = new_conn;
				epoll_ctl(epoll_fd, EPOLL_CTL_ADD, new_client_fd, &client_ev);

			} else if(listen_data->listen == magic_t_val){
				printf("magic_t_val\n");
				uint64_t experation;
				read(tfd, &experation, sizeof experation);

				struct timespec now;
				clock_gettime(CLOCK_MONOTONIC, &now);

				struct conn *curr = active_head;

				while(curr){
					struct conn *next = curr->next;
					if(curr->time <= now.tv_sec){
						epoll_ctl(epoll_fd, EPOLL_CTL_DEL, curr->fd, NULL);
						close(curr->fd);
						rm_active(curr);
						insert_inactive(curr);
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
