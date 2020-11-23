CC?=clang
CFLAGS?=-I/usr/local/include

all:
	$(CC) $(CFLAGS) -std=c99 -shared -O2 -o ddb_output_pw.so pw.c `pkg-config --cflags --libs libpipewire-0.3` -fPIC -Wall -march=native
debug: CFLAGS += -DDDBPW_DEBUG -g
debug: all

install: all
	cp ddb_output_pw.so ~/.local/lib64/deadbeef/
installdebug: debug install
