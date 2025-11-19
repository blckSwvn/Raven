#include "picohttpparser/picohttpparser.h"
#include "arena/arena.h"
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

typedef struct {
	const char *method;
	size_t method_len;
	const char *path;
	size_t path_len;
	int minor_version;
	struct phr_header headers[16];
	size_t num_headers;
} HTTPRequest;


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


inline void *build_path(const char *path, size_t path_len, size_t filepath_len){
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
			"Content-Length: 13\r\n\r\n";
		send(fd, not_found, strlen(not_found), 0);
		shutdown(fd, SHUT_WR);
		return;
	} else {
		struct stat st;
		if (fstat(f404, &st) == 0) {
			size_t header_len = 256;
			char *header = arena_alloc(256);
			int hlen = snprintf(header, header_len,
					"HTTP/1.1 404 Not Found\r\n"
					"Content-Type: text/html\r\n"
					"Content-Length: %jd\r\n\r\n",
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
		shutdown(fd, SHUT_WR);
		return;
	}
}


void *process_client(int fd, HTTPRequest *req){
	const char *path = req->path;
	size_t path_len = req->path_len;
	size_t filepath_len = 512;

	char *filepath = build_path(path, path_len, filepath_len);

	int f = open(filepath, O_RDONLY);
	if(f < 0)
		handle_file_not_found(fd);

	const char *mime = get_mime(filepath);
	struct stat st;
	if(fstat(f, &st) == 0){
		size_t header_len = 512;
		char *header = arena_alloc(header_len);
		int hlen = snprintf(header, header_len,
				"HTTP/1.1 200 OK\r\n"
				"Content-Type: %s\r\n"
				"Content-Length: %jd\r\n\r\n",
				mime, (intmax_t)st.st_size);
		send(fd, header, hlen, 0);
		size_t buf2_len = 4096;
		char *buf2 = arena_alloc(buf2_len);
		ssize_t r;
		while((r = read(f, buf2, buf2_len)) > 0){
			send(fd, buf2, r, 0);
		}
	}
	close(f);
	shutdown(fd, SHUT_WR);
	return NULL;
}


int main(){
	get_threads();
	get_mem();


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

	struct epoll_event ev;
	ev.events = EPOLLIN;
	ev.data.fd = listen_fd;
	epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev);

	struct epoll_event events[MAX_EVENTS];

	printf("server listenning on port %i\n", PORT);

	arena_init(1024 * 1024); //1MB
	while(1){
		int wait = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);

		int i = 0;
		while(wait > i){
			int fd = events[i].data.fd;
			//register new client
			struct sockaddr_in client_addr;
			socklen_t client_len = sizeof(client_addr);
			if(fd == listen_fd){
				int new_client_fd = accept(listen_fd,(struct sockaddr *)&client_addr, &client_len);

				if(new_client_fd < 0)continue;

				make_nonblocking(new_client_fd);

				struct epoll_event client_ev;
				client_ev.events = EPOLLIN;
				client_ev.data.fd = new_client_fd;
				epoll_ctl(epoll_fd, EPOLL_CTL_ADD, new_client_fd, &client_ev);
				//handle old connections
			} else {
				int client_fd = fd;

				size_t buf_len = 4096;
				char *buf = arena_alloc(buf_len);
				int r = read(client_fd, buf, buf_len);
				if(r <= 0){
					close(client_fd);
					continue;
				}

				HTTPRequest req;
				int parsed = parse_request(&req, buf, r);
				process_client(client_fd, &req);
				epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, NULL);
				close(client_fd);

				arena_reset();
			}


			i++;
		}

	}
	arena_free();
	return 0;
}
