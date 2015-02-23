# epoll-server
A sample (and simple) epoll TCP server implementation written in C.

By the way, this is my first GitHub project so please have mercy.

## Compiling
A simple Makefile is included. If you have gcc installed, just run the supplied
Makefile:

```
cd path/to/epoll-server
make
```

Specify `DEBUG=1` to use the debug configuration, see Makefile for details.

## Usage
The binary will be located inside the `build` directory. Passing the argument
`-h` prints the integrated help text. If no argument is given, the server
will be started on port 5033. Once the server is started, you can use a tool
like `nc` to open a connection and send data to the server. Note that the
server doesn't process any received data or sends a reply to the clients.
Maybe I'll add some simple message processing in a future version.
