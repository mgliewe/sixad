# You know, there are pre-compile DEBs of this...

CXX ?= g++
CXXFLAGS ?= -O2 -Wall
LDFLAGS += -Wl,-Bsymbolic-functions

GASIA_GAMEPAD_HACKS = false # Set to 'true' to enable hacks

ifeq ($(GASIA_GAMEPAD_HACKS),true)
CXXFLAGS += -DGASIA_GAMEPAD_HACKS
endif

BINS = bins/sixad-bin bins/sixad-sixaxis bins/sixad-remote bins/sixad-raw bins/sixad-usbd bins/sixpair

all: bins $(BINS)

bins:
	mkdir -p bins

bins/sixad-bin: sixad-bin.o bluetooth.o shared.o textfile.o
	$(CXX) $(LDFLAGS) sixad-bin.o bluetooth.o shared.o textfile.o -o bins/sixad-bin `pkg-config --libs bluez` -lpthread

bins/sixad-sixaxis: sixad-sixaxis.o sixaxis.o shared.o uinput.cpp textfile.o
	$(CXX) $(CXXFLAGS) $(LDFLAGS) sixad-sixaxis.o sixaxis.o shared.o uinput.cpp textfile.o -o bins/sixad-sixaxis -lpthread -lrt

bins/sixad-remote: sixad-remote.o remote.o shared.o uinput.o textfile.o
	$(CXX) $(CXXFLAGS) $(LDFLAGS) sixad-remote.o remote.o shared.o uinput.o textfile.o -o bins/sixad-remote -lrt

bins/sixad-raw: sixad-raw.o sixaxis.o shared.o uinput.o textfile.o
	$(CXX) $(CXXFLAGS) $(LDFLAGS) sixad-raw.o sixaxis.o shared.o uinput.o textfile.o -o bins/sixad-raw

bins/sixad-usbd: sixad-usbd.o sixaxis.o shared.o uinput.o textfile.o
	$(CXX) $(CXXFLAGS) $(LDFLAGS) sixad-usbd.o sixaxis.o shared.o uinput.o textfile.o -o bins/sixad-usbd -lpthread

bins/sixpair: sixpair.o
	$(CC) $(CFLAGS) $(LDFLAGS) sixpair.c -o bins/sixpair `pkg-config --cflags --libs libusb`


# avoid warnings for certain source files

sixad-bin.o: sixad-bin.cpp
	$(CXX) -c $(CXXFLAGS) -fpermissive sixad-bin.cpp

sixad-usbd.o: sixad-usbd.cpp
	$(CXX) -c $(CXXFLAGS) -Wno-stringop-truncation -Wno-unused-function sixad-usbd.cpp

sixad-sixaxis.o: sixad-sixaxis.cpp
	$(CXX) -c $(CXXFLAGS) -Wno-unused-result sixad-sixaxis.cpp



clean:
	rm -Rf *.o *~ bins

install: all
	install -d $(DESTDIR)/etc/default/
	install -d $(DESTDIR)/etc/init.d/
	install -d $(DESTDIR)/etc/logrotate.d/
	install -d $(DESTDIR)/usr/bin/
	install -d $(DESTDIR)/usr/sbin/
	install -d $(DESTDIR)/var/lib/sixad/
	install -d $(DESTDIR)/var/lib/sixad/profiles/
	
	install -m 644 sixad.default $(DESTDIR)/etc/default/sixad
	install -m 755 sixad.init $(DESTDIR)/etc/init.d/sixad
	install -m 644 sixad.log $(DESTDIR)/etc/logrotate.d/sixad
	install -m 755 99-sixad-usb.rules $(DESTDIR)/etc/udev/rules.d/

	install -m 755 sixad $(DESTDIR)/usr/bin/
	install -m 755 bins/sixad-bin $(DESTDIR)/usr/sbin/
	install -m 755 bins/sixad-sixaxis $(DESTDIR)/usr/sbin/
	install -m 755 bins/sixad-remote $(DESTDIR)/usr/sbin/
	install -m 755 bins/sixad-raw $(DESTDIR)/usr/sbin/
	install -m 755 bins/sixpair $(DESTDIR)/usr/sbin/
	install -m 755 bins/sixad-usbd $(DESTDIR)/usr/sbin/
	
	install -m 777 hidraw.profile $(DESTDIR)/var/lib/sixad/profiles/hidraw
	install -m 777 retroarch-Joystick.profile $(DESTDIR)/var/lib/sixad/profiles/retroarch-Joystick
	install -m 777 retroarch-Joystick2.profile $(DESTDIR)/var/lib/sixad/profiles/retroarch-Joystick2

	@chmod 777 -R $(DESTDIR)/var/lib/sixad/
	@echo "Installation is Complete!"

uninstall:
	rm -f $(DESTDIR)/etc/default/sixad
	rm -f $(DESTDIR)/etc/init.d/sixad
	rm -f $(DESTDIR)/etc/init.d/sixad-usbd
	rm -f $(DESTDIR)/etc/logrotate.d/sixad
	rm -f $(DESTDIR)/etc/udev/rules.d/ 99-sixad-usb.rules

	rm -f $(DESTDIR)/usr/bin/sixad
	rm -f $(DESTDIR)/usr/sbin/sixad-bin
	rm -f $(DESTDIR)/usr/sbin/sixad-sixaxis
	rm -f $(DESTDIR)/usr/sbin/sixad-remote
	rm -f $(DESTDIR)/usr/sbin/sixad-raw
	rm -f $(DESTDIR)/usr/sbin/sixad-usbd
	rm -f $(DESTDIR)/usr/sbin/sixpair

	rm -rf $(DESTDIR)/var/lib/sixad/
