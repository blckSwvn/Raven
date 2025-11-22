# mini http server

IN PROGRESS
small http server in pure C with minimal external libraries
may be reused for a mail server

---

### TODO

replace read with sendfile
worker threads
keep alive connections(working on currently)
use useragent to determine http version
then decide on Connection: like keep alive or close imediately
also need timeout to prevent ppl holding onto sockets forever
pool struct conn add them to a linked list rather than freeing

logging
UDP for video(maybe)
caching pages like html, css, js, SVGs except vidoes and images to avoid IO

