#include <bits/time.h>
#include <errno.h>
#include <signal.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <sys/sendfile.h>
#include <sys/errno.h>
#include "picohttpparser/picohttpparser.h"
#include "arena/arena.h"
#include <sys/mman.h>
#include <strings.h>
#include <dirent.h>
#include "time.h"
#include <sys/timerfd.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <threads.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/epoll.h>

#define LOG_DEBUG 1

typedef struct {
	const char *method;
	size_t method_len;
	const char *path;
	size_t path_len;
	int minor_version;
	struct phr_header headers[16];
	size_t num_headers;
} HTTPRequest;

alignas(16) struct l_conn {
	int fd;
	int listen;
}; 

#define MIN_BUF 8000 //8kb
#define MAX_BUF 32000 //32kb

struct cached_file{
	const char *path; //key
	const char *data; //restrict with mmap for file
	size_t size;
	struct cached_file *next;
};

typedef enum {
	OUT_NONE = 0,
	OUT_FD,
	OUT_CACHED
}file_out;

alignas(16) struct conn {
	int fd;
	void *buf;
	uint16_t buf_length; //starts at MIN_BUF
	uint16_t buf_used;
	file_out out;
	union {
		int fd;
		struct cached_file *cached;
	}file_u;
	off_t offset;
	size_t file_size;
	struct conn *next;
	struct conn *prev;
	uint64_t time;
};


typedef struct {
	char msg[64];
}log_entry;

struct ring_buffer{
	log_entry buf[256];
	alignas(64) uint8_t head;
	alignas(64) _Atomic uint8_t tail;
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
	{"txt", "text/plain"},
	{"md", "text/markdown"},
	{"svg", "image/svg+xml"},
	{"png", "image/png"},
	{"jpg", "image/jpeg"},
	{"jpeg", "image/jpeg"},
	{"gif", "image/gif"},
	{NULL, "application/octet-stream"}
};

#define PORT 8080
#define MAX_EVENTS 255

long procs = 0;

long all_mem = 0;
long page_size = 0;
long avphys_pages = 0;

static struct ring_buffer rb;

inline int write_log(const char *msg){
	uint8_t tail = atomic_fetch_add_explicit(&rb.tail, 1, memory_order_relaxed);
	uint8_t head = rb.head;
	if(tail+1 == head)
		return -1;

	strncpy(rb.buf[tail].msg, msg, 64);
	rb.buf[tail].msg[63] = '\0';
	atomic_thread_fence(memory_order_release);
	return 0;
}


inline int read_log(char *out){
	uint8_t tail = atomic_load_explicit(&rb.tail, memory_order_acquire);
	uint8_t head = rb.head;
	if(head == tail)return -1;
	strncpy(out, rb.buf[head].msg, 64);
	rb.head = head +1;
	return 0;
}

#define NUM_BUCKETS 1024

static inline uint32_t hash(const char *str){
	uint32_t hash = 5381;
	int c;
	while((c = *str++))
		hash = ((hash << 5) + hash) + c;
	return hash % NUM_BUCKETS;
}

static struct cached_file *hash_table[NUM_BUCKETS];

void insert_file(const char *path, int file, size_t size){
	uint32_t index = hash(path);
	struct cached_file *entry = mmap(NULL, sizeof(struct cached_file), PROT_READ | PROT_WRITE,
				  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if(!entry){
		write_log("insert_file entry mmap = NULL");
		return;
	}

	entry->data = mmap(NULL, size, PROT_READ, MAP_PRIVATE, file, 0);
	if(!entry->data){
		write_log("insert_file entry->data mmap = NULL");
		return;
	}

	entry->path = strdup(path);
	entry->size = size;
	entry->next = hash_table[index];
	hash_table[index] = entry;
}


static inline struct cached_file *get_file(const char *path){
	struct cached_file *entry = hash_table[hash(path)];
	while(entry){
		if(strcmp(entry->path, path) == 0)
			return entry;
		entry = entry->next;
	}
	return NULL;
}


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

static const char *not_found =  "HTTP/1.1 404 Not Found\r\n"
				"Content-Type: text/html\r\n"
				"Content-Length: 13\r\n"
				"Connection: close\r\n\r\n";

static inline char *handle_file_not_found(struct conn *client){
	int f404 = open("../www/404.html", O_RDONLY);
	if(f404 < 0){
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
			client->offset = 0;
			ssize_t n = sendfile(client->fd, f404, &client->offset, client->file_size);
				if(n >= st.st_size){
				}
				if(errno == EAGAIN || errno == EWOULDBLOCK){
					client->file_u.fd = f404;
					client->file_size = st.st_size;
					return "EAGAIN";
				}else{
					close(f404);
					return "close";
				}
			}
			close(f404);
			return "close";
	}
}


const char *process_client(struct conn *client, HTTPRequest *req){
	const char *path = req->path;
	size_t path_len = req->path_len;
	size_t filepath_len = 512;

	char *filepath = build_path(path, path_len, filepath_len);
	struct cached_file *file = get_file(filepath + strlen("../www"));
	if(file){
		char *connection = "close";
		if(req->minor_version){
			size_t i = 2;
			while(i < req->num_headers){
				if(strncmp(req->headers[i].name, "Connection", 10) == 0){
					if(strncmp(req->headers[i].value, "close", 5) != 0)
						connection = "keep-alive";
					else connection = "close";
					break;
				}
				i++;
			}
		} 

		const char *mime = get_mime(filepath);
		size_t header_len = 512;
		char *header = arena_alloc(header_len);
		int hlen = snprintf(header, header_len,
		      "HTTP/1.1 200 OK\r\n"
		      "Content-Type: %s\r\n"
		      "Content-Length: %jd\r\n"
		      "Connection: %s\r\n\r\n",
		      mime, (intmax_t)file->size,
		      connection);
		send(client->fd, header, hlen, 0);
		client->file_size = file->size;
		client->offset = 0;
		size_t n = write(client->fd, file->data + client->offset, file->size - client->offset);
		if(n > 0) client->offset += n;
		if(n < (file->size - client->offset)){
			if(errno == EAGAIN || errno == EWOULDBLOCK){
				client->out = OUT_CACHED;
				client->file_u.cached = file;
				return "EAGAIN";
			} else return "close";
		}
		return connection;
	}

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
		client->offset = 0;
		n = sendfile(client->fd, f, &client->offset, client->file_size - client->offset);
		if(n < 0){
			if(errno == EAGAIN || errno == EWOULDBLOCK){
				client->out = OUT_FD;
				client->file_u.fd = f;
				return "EAGAIN";
			} else {
				close(f);
				return "close";
			}
		}
	}
	close(f);
	return connection;
}


thread_local void *inactive_list = NULL;
thread_local void *active_head = NULL;
thread_local void *active_tail = NULL;

void update_timer(int tfd){
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	uint64_t now = ts.tv_sec;

	struct conn *head = active_head;

	time_t soonest = head->time - ts.tv_sec;

	struct itimerspec its = {0};
	its.it_value.tv_sec = soonest;
	timerfd_settime(tfd, 0, &its, NULL);
}


static inline void append_active(struct conn *client){
	client->next = NULL;
	client->prev = active_tail;
	if(active_tail){
		struct conn *last = active_tail;
		last->next = client;
	} else {
		active_head = client;
	}
	active_tail = client;

}


static inline void rm_active(struct conn *client){
	if(active_head == client)
		active_head = client->next;
	if(active_tail == client)
		active_tail = client->prev;
	if(client->next)client->next->prev = client->prev;
	if(client->prev)client->prev->next = client->next;
	client->next = NULL;
	client->prev = NULL;
}


static inline void insert_inactive(struct conn *client){
	client->next = inactive_list;
	inactive_list = client;
}


volatile sig_atomic_t stop = 0;

void signal_handler(int sig){
	printf("\nserver shutdown\n");
	stop = 1;
}


void *work(){
	struct sockaddr_in adress;
	int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if(listen_fd < 0){
		write_log("socket failed");
		return 0;
	} 

	adress.sin_family = AF_INET;
	adress.sin_addr.s_addr = INADDR_ANY;
	adress.sin_port = htons(PORT);
	int one = 1;
	setsockopt(listen_fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);

	if(bind(listen_fd,(struct sockaddr *)&adress, sizeof(adress)) < 0){
		write_log("bind failed");
		return 0;
	}

	listen(listen_fd, SOMAXCONN);

	make_nonblocking(listen_fd);

	write_log("listening on Port: 8080");

	int epoll_fd = epoll_create1(0);

#define magic_l_val -1
	struct epoll_event ev;
	struct l_conn *data = mmap(NULL, sizeof(struct l_conn),
			    PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	data->listen = magic_l_val;
	data->fd = listen_fd;

	ev.events = EPOLLIN | EPOLLET;
	ev.data.ptr = data;
	epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev);

	struct epoll_event events[MAX_EVENTS];

#define magic_t_val -2
	int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
	struct l_conn *timer = mmap(NULL, sizeof(struct l_conn),
			     PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	timer->fd = tfd;
	timer->listen = magic_t_val;
	struct epoll_event tev;
	tev.events = EPOLLIN;
	tev.data.ptr = timer; 

	struct itimerspec its = {0};

	timerfd_settime(tfd, 0, &its, NULL);

	epoll_ctl(epoll_fd, EPOLL_CTL_ADD, tfd, &tev);


	arena_init(1024 * 1024); //1MB
	while(!stop){
		int wait = epoll_wait(epoll_fd, events, MAX_EVENTS, 1000);

		int i = 0;
		while(wait > i){
			if(events[i].events & EPOLLOUT){
				struct conn *client = events[i].data.ptr;
				if(client->out == OUT_CACHED){
					ssize_t n = write(client->fd, client->file_u.cached->data, client->file_size - client->offset);
					client->offset += n;
					if(n < 0){
						if(errno == EAGAIN || errno == EWOULDBLOCK){
							i++;
							continue;
						} else {
							epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client->fd, NULL);
							close(client->fd);
							rm_active(client);
							insert_inactive(client);
							continue;
						}
					}
					if(client->offset >= client->file_size){
						struct epoll_event ev;
						ev.data.ptr = client;
						ev.events = EPOLLIN | EPOLLET;
						epoll_ctl(epoll_fd, EPOLL_CTL_MOD, client->fd, &ev);
					}
				}

				else{
					ssize_t n = sendfile(client->fd, client->file_u.fd, &client->offset, client->file_size - client->offset);
					if(n < 0){
						if(errno == EAGAIN || errno == EWOULDBLOCK){
							i++;
							continue;
						} else {
							epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client->fd, NULL);
							close(client->file_u.fd);
							close(client->fd);
							rm_active(client);
							insert_inactive(client);
							continue;
						}
						if(client->offset >= client->file_size){
							close(client->file_u.fd);
							ev.data.ptr = client;
							ev.events = EPOLLIN | EPOLLET;
							epoll_ctl(epoll_fd, EPOLL_CTL_MOD, client->fd, &ev);
						}
					}
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
						epoll_ctl(epoll_fd, EPOLL_CTL_DEL,client->fd, NULL);
						close(client->fd);
						rm_active(client);
						insert_inactive(client);
					} else {
						if(errno == EAGAIN || errno == EWOULDBLOCK){
							break;
						}
						else{
							epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client->fd, NULL);
							close(client->fd);
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
						memmove(client->buf, client->buf + parsed, client->buf_used - parsed);
						client->buf_used -= parsed;
						update_timer(tfd);
					} else if(strcmp(connection, "EAGAIN") == 0){
						struct epoll_event ev;
						ev.events = EPOLLOUT | EPOLLET;
						ev.data.ptr = client;
						epoll_ctl(epoll_fd, EPOLL_CTL_MOD, client->fd, &ev);
					} else {
						epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client->fd, NULL);
						close(client->fd);
						rm_active(client);
						insert_inactive(client);
					}

				} else if(parsed == -1){
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

				make_nonblocking(new_client_fd);

				struct epoll_event client_ev;
				client_ev.events = EPOLLIN | EPOLLET;

				struct conn *new_conn;
				struct timespec ts = {0};
				clock_gettime(CLOCK_MONOTONIC, &ts);
				if(inactive_list){
					new_conn = inactive_list;
					inactive_list = new_conn->next;
					new_conn->next = NULL;
					new_conn->prev = NULL;
					// new_conn->file_u.fd = -1;
					//new_conn->out is lazily initlized
					// new_conn->offset = 0; laziliy initilized
					// new_conn->file_size = 0; laziliy initlized
					// new_conn->buf_used = 0; should inherit previous size
					// new_conn->buf_length = MIN_BUF should inherit previous
					new_conn->fd = new_client_fd;
					new_conn->time = ts.tv_sec + 10;
				} else {
					new_conn = mmap(NULL, sizeof(struct conn), 
		     					PROT_WRITE | PROT_READ, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
					new_conn->next = NULL;
					new_conn->prev = NULL;
					// new_conn->file_u.fd = -1;
					//new_conn->out is lazily initlized
					// new_conn->offset = 0; laziliy initlized
					// new_conn->file_size = 0; laziliy initlized
					new_conn->buf_used = 0;
					new_conn->fd = new_client_fd;
					new_conn->buf_length = MIN_BUF;
					new_conn->buf = mmap(NULL, MIN_BUF,
			  				PROT_WRITE | PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
					new_conn->time = ts.tv_sec + 10;
				}
				append_active(new_conn);

				client_ev.data.ptr = new_conn;
				epoll_ctl(epoll_fd, EPOLL_CTL_ADD, new_client_fd, &client_ev);

			} else if(listen_data->listen == magic_t_val){
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
				#ifdef LOG_DEBUG
				write_log("LOG_DEBUG magic_t_val");
				#endif
			}
			i++;
		}
		arena_reset();

	}
	write_log("closing worker thread");
	arena_free();
	struct conn *curr = active_head;
	while(curr){
		struct conn *next = curr->next;
		munmap(curr->buf, curr->buf_length);	
		if(curr->file_u.fd >= 0)close(curr->file_u.fd);
		close(curr->fd);
		munmap(curr, sizeof(struct conn));
		curr = next;
	}
	curr = inactive_list;
	while(curr){
		struct conn *next = curr->next;
		munmap(curr->buf, curr->buf_length);	
		munmap(curr, sizeof(struct conn));
		curr = next;
	}

	close(epoll_fd);
	close(tfd);
	close(listen_fd);
	munmap(timer, sizeof(struct l_conn));
	munmap(data, sizeof(struct l_conn));
	return 0;
}


void init_cache(const char *dirpath){
	struct dirent *entry;
	struct stat info;
	DIR *dp = opendir(dirpath);

	if (!dp){
		perror("opendir");
		return;
	}

	while ((entry = readdir(dp)) != NULL) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue; // skip current and parent directories

		char fullpath[512];
		snprintf(fullpath, sizeof(fullpath), "%s/%s", dirpath, entry->d_name);

		if (stat(fullpath, &info) != 0) {
			perror("stat");
			continue;
		}

		if(S_ISREG(info.st_mode)){ //is file
			const char *ext = strrchr(fullpath, '.');
			if(!ext || ext == fullpath)continue;
			ext++;
			uint8_t i = 0;
			//mime[9].ext and beyond are things we dont want to cache like png, jpg, jpeg
			while(mime[i].ext && i < 8){
				if(strcmp(ext, mime[i].ext) == 0){
					int fd = open(fullpath, O_RDONLY);
					struct stat st;
					fstat(fd, &st);
					const char *key = fullpath + strlen("../www");
					printf("key:%s\n", key);
					insert_file(key, fd, st.st_size);
					close(fd);
				} 
				i++;
			}
		} else if(S_ISDIR(info.st_mode))
			init_cache(fullpath);
	}
	closedir(dp);
}


int main(){
	rb.head = 0;
	atomic_init(&rb.tail, 0);

	init_cache("../www");

	uint16_t n = sysconf(_SC_NPROCESSORS_CONF);
	uint8_t i = 0;
	pthread_t tid[n];
	printf("threads detected:%d\n", n);
	while(i < n){
		pthread_create(&tid[i], NULL, work, NULL);
		i++;
	}

	signal(SIGINT, signal_handler);

	char msg[64];
	while(!stop){
		while(read_log(msg) == 0){
			struct timespec ts;
			clock_gettime(CLOCK_MONOTONIC, &ts);
			printf("%ld %s\n", ts.tv_sec, msg);
		}
		struct timespec ts = {0, 10000};
		nanosleep(&ts, NULL);
	}

	uint16_t z = 0;
	while(hash_table[z]){
		struct cached_file *curr = hash_table[z];
		while(curr){
			struct cached_file *next = curr->next;
			free((void*)curr->path);
			munmap((void *)curr->data, curr->size);
			munmap(curr, sizeof(struct cached_file));
			curr = next;
		}
		hash_table[z] = NULL;
		z++;
	}

	for(uint16_t i = 0; i < n; i++) {
		printf("closing thread %d\n", i);
		pthread_join(tid[i], NULL);
	}
}
