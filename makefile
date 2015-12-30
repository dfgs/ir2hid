EXEC=ir2hid

CC=gcc
LD=ld
CFLAGS=-I/usr/local/include -I/usr/include/logger -I/usr/local/include -std=c99
LDFLAGS=-L/usr/local/lib -L/usr/lib/logger -L/usr/local/lib -lavahi-client -lavahi-common -llogger -lela -lrudp -lfoils_hid


all:$(EXEC)

$(EXEC):main.o browser.o remote.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

install:$(EXEC)
	cp $(EXEC) /usr/bin/$(EXEC)
	mkdir -p /etc/ir2hid
	cp mapping.csv /etc/ir2hid/mapping.csv
	
uninstall:
	rm -f /usr/bin/$(EXEC)
	rm -f -r /etc/ir2hid
	rm -f /var/log/ir2hid.log
clean :
	rm -f *.o $(EXEC)


