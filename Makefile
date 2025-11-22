# Makefile for SDPrep (GTK3 + json-c)

CC          = gcc
PKG_CFLAGS  := $(shell pkg-config --cflags gtk+-3.0 json-c)
PKG_LIBS    := $(shell pkg-config --libs gtk+-3.0 json-c)

CFLAGS      := -O2 -Wall $(PKG_CFLAGS)
LDFLAGS     := $(PKG_LIBS)

all: sdprep

sdprep: sdprep.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f sdprep *.o

