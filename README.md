# mini http server

IN PROGRESS
small http server in pure C with minimal external libraries
may be reused for a mail server

---

### TODO in order

timeout using timerfd
EPOLLET
replace read with sendfile
replace malloc with other arena local allocator that doesnt return memory to the OS and doesnt split/merge nodes
worker threads/worker pools

logging
caching pages like html, css, js, SVGs except vidoes and images to avoid IO

