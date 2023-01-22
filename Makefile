CC?=clang
CFLAGS?=-I/usr/local/include -DPW_ENABLE_DEPRECATED

all:
	$(CC) $(CFLAGS) -std=c99 -shared -O2 -o ddb_out_pw.so pw.c `pkg-config --cflags --libs libpipewire-0.3` -fPIC -Wall -march=native
debug: CFLAGS += -DDDBPW_DEBUG -g
debug: all

install: all
	cp ddb_out_pw.so ~/.local/lib64/deadbeef/
installdebug: debug install
