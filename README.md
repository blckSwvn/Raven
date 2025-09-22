# http server

## name: idk yet

im trying to make a high performance http server so
mult threaded with worker threads using my own allocator `salloc`
all in C btw including `salloc`

for some design choices
    epoll

it will be 86x-64x only due to allocator but may be subject to change.
also it will be using pthreads so posix aka not windows so linux.

order of progression
TCP echo server socket(), bind(), listen(), accept(), read(), write()

epoll basics
so non blocking sockets

multi threading basics
thread pool

http basics
parsing using picohttparser

static files
Map /index.html to ./www/index.html.
Use fopen() + fread() to send file contents as the response body.
Add minimal headers like Content-Type and Content-Length.

keep alive + concurrency
Add support for multiple requests on one connection.
Either via threads or epoll-based loop.

Polish & safety
MIME types, 404/500 errors, path traversal prevention.
Logging + clean shutdown.
