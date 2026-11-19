PREFIX ?= /usr
UNITDIR ?= $(PREFIX)/lib/systemd/system
CFLAGS ?= -O2 -g
CFLAGS += -Wall -Wextra -std=gnu11

PROGRAMS = aurora-kbd-light aurora-display-light

all: $(PROGRAMS)

%: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< -lm

install: $(PROGRAMS)
	for p in $(PROGRAMS); do \
		install -Dm0755 $$p $(DESTDIR)$(PREFIX)/bin/$$p && \
		install -Dm0644 $$p.service $(DESTDIR)$(UNITDIR)/$$p.service || exit 1; \
	done

clean:
	rm -f $(PROGRAMS)

.PHONY: all install clean
