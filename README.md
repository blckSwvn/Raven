# Raven WORK IN PROGRESS

a small HTTP server in pure C with minimal external libraries

---

### TODO in order
EPOLLET
replace read with sendfile
replace malloc with other arena local allocator that doesnt return memory to the OS and doesnt split/merge nodes
worker threads/worker pools
logging
caching pages like html, css, js, SVGs except vidoes and images to avoid IO with mmap and hashmaps for lookup
