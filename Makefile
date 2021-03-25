CC?=clang
CFLAGS?=-I/usr/local/include -DDDBPW_VERSION_MAJOR=0 -DDDBPW_VERSION_MINOR=1

all:
	$(CC) $(CFLAGS) -std=c99 -shared -O2 -o ddb_out_pw.so pw.c `pkg-config --cflags --libs libpipewire-0.3` -fPIC -Wall -march=native
debug: CFLAGS += -DDDBPW_DEBUG -g
debug: all

install: all
	cp ddb_out_pw.so ~/.local/lib64/deadbeef/
installdebug: debug install
