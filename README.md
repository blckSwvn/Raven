# Raven v1.0

a lightweight high-performance HTTP/1.1 server written in pure C,
Designed for speed, simplicity and minimal dependencies, for static fileserving.

---

### Features

- Fully event-driven using edge-triggered `epoll` (linux)
- Supports keep-alive `connections`
- Fully multithreaded with `SO_REUSEPORT`
- Zero-copy with sendfile for larger files like images
- Userland caching for `.html, .css, .js` to avoid IO

---

### Configuration

Add your html, css, js etc files to `../www/`
By default uses port 8080 can easily be changed by changing `#define PORT`
Async logging with ring buffer by adding `write_log("foo");` wherever you want

---

### Performance

tested on my laptop AMD RYZEN 5 pro (6 cores, 12 threads) with `wrk` serving a small index.html ~1KB.
First requests is always the slowest due to more syscalls instead of pulling from inactive_list etc.

```bash
     ~/proj/http_server/server main ! 10s
    > wrk -c 500 -t 12 -d 10s --latency http://localhost:8080/
    Running 10s test @ http://localhost:8080/
      12 threads and 500 connections
      Thread Stats   Avg      Stdev     Max   +/- Stdev
        Latency     6.99ms    8.51ms  71.04ms   86.68%
        Req/Sec     6.42k     1.10k   13.72k    67.93%
      Latency Distribution
         50%    3.64ms
         75%   10.00ms
         90%   18.16ms
         99%   39.08ms
      768128 requests in 10.10s, 773.57MB read
    Requests/sec:  76078.98
    Transfer/sec:     76.62MB

     ~/proj/http_server/server main ! 10s
    > wrk -c 500 -t 12 -d 10s --latency http://localhost:8080/
    Running 10s test @ http://localhost:8080/
      12 threads and 500 connections
      Thread Stats   Avg      Stdev     Max   +/- Stdev
        Latency     6.90ms    8.85ms 105.06ms   87.57%
        Req/Sec     6.52k     1.12k   11.02k    67.45%
      Latency Distribution
         50%    3.54ms
         75%    9.78ms
         90%   17.68ms
         99%   38.96ms
      782122 requests in 10.10s, 787.66MB read
    Requests/sec:  77441.16
    Transfer/sec:     77.99MB

     ~/proj/http_server/server main ! 10s
    > wrk -c 500 -t 12 -d 10s --latency http://localhost:8080/
    Running 10s test @ http://localhost:8080/
      12 threads and 500 connections
      Thread Stats   Avg      Stdev     Max   +/- Stdev
        Latency     6.62ms    8.11ms  93.66ms   87.09%
        Req/Sec     6.56k     1.13k   10.63k    68.34%
      Latency Distribution
         50%    3.55ms
         75%    9.38ms
         90%   16.87ms
         99%   37.60ms
      786622 requests in 10.10s, 792.19MB read
    Requests/sec:  77906.24
    Transfer/sec:     78.46MB
```

