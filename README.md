# Asynchronous Web Server

# Program Description

Simulates a web server that responds to HTTP Get requests sending the requested files asynchronously using `libaio`. Multiple connections are handled with `epoll`.

### HTTP Parser

An external HTTP parser was used to handle HTTP header data.

### Debugging

Logs are collected in `test.log` and `wget.log` files.

## Resources

- [sendfile](https://man7.org/linux/man-pages/man2/sendfile.2.html)

- [io_setup & friends](https://man7.org/linux/man-pages/man2/io_setup.2.html)

- [epoll](https://man7.org/linux/man-pages/man7/epoll.7.html)
