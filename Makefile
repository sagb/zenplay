
all: zenindex zenplay

zenindex.o: zenindex.c settings.h
	gcc -g `pkg-config --cflags hiredis` `pkg-config --cflags librhash` -o zenindex.o -c zenindex.c

zenindex: zenindex.o
	gcc -g -o zenindex zenindex.o `pkg-config --libs hiredis` `pkg-config --libs librhash`

zenplay.o: zenplay.c settings.h
	gcc -g `pkg-config --cflags hiredis` `pkg-config --cflags mpv` `pkg-config --cflags libgpiod` -o zenplay.o -c zenplay.c

zenplay: zenplay.o
	gcc -g -o zenplay zenplay.o `pkg-config --libs hiredis` `pkg-config --libs mpv` `pkg-config --libs libgpiod`

install: zenindex zenplay zenplay.service
	install zenindex zenplay /usr/local/bin/
	cp -v zenplay.service /etc/systemd/system/

