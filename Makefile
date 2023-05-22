CFLAGS?=-O2 -g -Wall -W 
CFLAGS+= -I./aisdecoder -I ./aisdecoder/lib -I./tcp_listener
LDFLAGS+=-lpthread -lm  -L /usr/lib/arm-linux-gnueabihf/ 

ifeq ($(PREFIX),)
    PREFIX := /usr/local
endif

UNAME := $(shell uname)
ifeq ($(UNAME),Linux)
	CFLAGS += $(shell pkg-config --cflags librtlsdr libusb-1.0)
	LDFLAGS +=$(shell pkg-config --libs librtlsdr libusb-1.0)
else ifeq ($(UNAME),Darwin)
	CFLAGS += $(shell pkg-config --cflags librtlsdr libusb-1.0)
	LDFLAGS += $(shell pkg-config --libs librtlsdr libusb-1.0)
else
	#ADD THE CORRECT PATH FOR LIBUSB AND RTLSDR
	#TODO:
	#    CMAKE will be much better or create a conditional pkg-config

	# RTLSDR
	RTLSDR_INCLUDE=/tmp/rtl-sdr/include
	RTLSDR_LIB=/tmp/rtl-sdr/build/src

	# LIBUSB
	LIBUSB_INCLUDE=/opt/homebrew/Cellar/libusb/1.0.24/include
	LIBUSB_LIB=/opt/homebrew/Cellar/libusb/1.0.24/lib

	#Conditional for Windows
	CFLAGS+=-I $(LIBUSB_INCLUDE) -I $(RTLSDR_INCLUDE)
	LDFLAGS+=-L$(LIBUSB_INCLUDE) -L$(RTLSDR_LIB) -L/usr/lib -lusb-1.0 -lrtlsdr -lWs2_32
endif

CC?=gcc
SOURCES= \
	main.c rtl_ais.c convenience.c \
	./aisdecoder/aisdecoder.c \
	./aisdecoder/sounddecoder.c \
	./aisdecoder/lib/receiver.c \
	./aisdecoder/lib/protodec.c \
	./aisdecoder/lib/hmalloc.c \
	./aisdecoder/lib/filter.c \
	./tcp_listener/tcp_listener.c

OBJECTS=$(SOURCES:.c=.o)
EXECUTABLE=rtl_ais

all: $(SOURCES) $(EXECUTABLE)
    
$(EXECUTABLE): $(OBJECTS) 
	$(CC) $(OBJECTS) -o $@ $(LDFLAGS)

.c.o:
	$(CC) -c $< -o $@ $(CFLAGS)

clean:
	rm -f $(OBJECTS) $(EXECUTABLE) $(EXECUTABLE).exe

install:
	install -d -m 755 $(DESTDIR)/$(PREFIX)/bin
	install -m 755 $(EXECUTABLE) "$(DESTDIR)/$(PREFIX)/bin/"

