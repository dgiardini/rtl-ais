CFLAGS?=-O2 -g -Wall -W 
CFLAGS+= -I./aisdecoder -I ./aisdecoder/lib -I./tcp_listener
LDFLAGS+=-lpthread -lm  -L /usr/lib/arm-linux-gnueabihf/ 

ifeq ($(PREFIX),)
    PREFIX := /usr/local
endif
UNAME := $(shell uname)
ifeq ($(UNAME),Linux)
#Conditional for Linux
CFLAGS+= $(shell pkg-config --cflags librtlsdr)
LDFLAGS+=$(shell pkg-config --libs librtlsdr)

else
#
#ADD THE CORRECT PATH FOR LIBUSB AND RTLSDR
#TODO:
#    CMAKE will be much better or create a conditional pkg-config


# RTLSDR
RTLSDR_INCLUDE=/tmp/rtl-sdr/include
RTLSDR_LIB=/tmp/rtl-sdr/build/src

# LIBUSB
LIBUSB_INCLUDE=/tmp/libusb/include/libusb-1.0
LIBUSB_LIB=/tmp/libusb/lib

ifeq ($(UNAME),Darwin)
#Conditional for OSX
CFLAGS+= -I/usr/local/include/ -I$(LIBUSB_INCLUDE) -I$(RTLSDR_INCLUDE)
LDFLAGS+= -L/usr/local/lib -L$(LIBUSB_LIB) -L$(RTLSDR_LIB) -lrtlsdr -lusb-1.0 
else
#Conditional for Windows
CFLAGS+=-I $(LIBUSB_INCLUDE) -I $(RTLSDR_INCLUDE)
LDFLAGS+=-L$(LIBUSB_INCLUDE) -L$(RTLSDR_LIB) -L/usr/lib -lusb-1.0 -lrtlsdr -lWs2_32
endif


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

