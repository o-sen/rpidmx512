PREFIX ?=

CC	= $(PREFIX)gcc
CPP	= $(PREFIX)g++
AS	= $(CC)
LD	= $(PREFIX)ld
AR	= $(PREFIX)ar

ROOT = ./../..

LIB := -L$(ROOT)/lib-display/lib_linux -L$(ROOT)/lib-i2c/lib_linux
LDLIBS := -ldisplay -li2c
LIBDEP := $(ROOT)/lib-display/lib_linux/libdisplay.a $(ROOT)/lib-i2c/lib_linux/libi2c.a

INCLUDES := -I$(ROOT)/lib-display/include -I$(ROOT)/lib-i2c/include

COPS := -Wall -Werror -O3 -fno-rtti -std=c++11 -DRASPPI -DNDEBUG

BCM2835 = $(ROOT)/lib-bcm2835_raspbian

ifneq "$(wildcard $(BCM2835) )" ""
	LIB += -L$(BCM2835)/lib_linux
	LDLIBS += -lbcm2835_raspbian
	INCLUDES += -I$(BCM2835)/include
else
	LDLIBS += -lbcm2835
endif

all : detect oled128x32 ssd1306 lcd1602 lcd2004

clean :
	rm -f *.o
	rm -f detect oled128x32
	cd $(ROOT)/lib-display && make -f Makefile.Linux clean
	cd $(ROOT)/lib-i2c && make -f Makefile.Linux clean
	
$(ROOT)/lib-display/lib_linux/libdisplay.a :
	cd $(ROOT)/lib-display && make -f Makefile.Linux	

$(ROOT)/lib-i2c/lib_linux/libi2c.a :
	cd $(ROOT)/lib-i2c && make -f Makefile.Linux
	
mircos.o	: Makefile micros.c
	$(CC) -c micros.c
	
detect : Makefile detect.cpp $(ROOT)/lib-display/lib_linux/libdisplay.a $(ROOT)/lib-i2c/lib_linux/libi2c.a mircos.o
	$(CPP) detect.cpp micros.o $(INCLUDES) $(COPS) -o detect $(LIB) $(LDLIBS) 
	
oled128x32 : Makefile detect.cpp $(ROOT)/lib-display/lib_linux/libdisplay.a $(ROOT)/lib-i2c/lib_linux/libi2c.a mircos.o
	$(CPP) oled128x32.cpp micros.o $(INCLUDES) $(COPS) -o oled128x32 $(LIB) $(LDLIBS) 
	
ssd1306 : Makefile ssd1306.cpp $(ROOT)/lib-display/lib_linux/libdisplay.a $(ROOT)/lib-i2c/lib_linux/libi2c.a mircos.o
	$(CPP) ssd1306.cpp micros.o $(INCLUDES) $(COPS) -o ssd1306 $(LIB) $(LDLIBS)

lcd1602 : Makefile lcd1602.cpp $(ROOT)/lib-display/lib_linux/libdisplay.a $(ROOT)/lib-i2c/lib_linux/libi2c.a mircos.o
	$(CPP) lcd1602.cpp micros.o $(INCLUDES) $(COPS) -o lcd1602 $(LIB) $(LDLIBS)
	
lcd2004 : Makefile lcd2004.cpp $(ROOT)/lib-display/lib_linux/libdisplay.a $(ROOT)/lib-i2c/lib_linux/libi2c.a mircos.o
	$(CPP) lcd2004.cpp micros.o $(INCLUDES) $(COPS) -o lcd2004 $(LIB) $(LDLIBS)
