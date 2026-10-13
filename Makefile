CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
CFLAGS  += $(shell pkg-config --cflags libcurl libzip)
LDLIBS  += $(shell pkg-config --libs libcurl libzip)

SRC = src/main.c src/ics.c src/ods.c
OBJ = $(SRC:.c=.o)

emargement: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

src/%.o: src/%.c src/ics.h src/ods.h
	$(CC) $(CFLAGS) -c -o $@ $<

# Test hors ligne avec l'emploi du temps d'octobre 2026
test: emargement
	./emargement -f test/octobre_2026.ics -m 2026-10 -n "Victor CHARMES" -o test/emargement_2026-10.ods

clean:
	rm -f $(OBJ) emargement

.PHONY: test clean
