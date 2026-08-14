PROG1 = steerx
OBJS1 = steerx.c

PKGS = gio-2.0 glib-2.0
CFLAGS += -W -Wall -std=c99 -D_POSIX_C_SOURCE=200809L \
          $(shell pkg-config --cflags $(PKGS))
LDLIBS += $(shell pkg-config --libs $(PKGS)) -lcurl -lm

all: $(PROG1)

$(PROG1): $(OBJS1)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

clean:
	rm -f $(PROG1) *.o *.eap *_LICENSE.txt package.conf* param.conf
