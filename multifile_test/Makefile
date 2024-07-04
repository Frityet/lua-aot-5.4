LUAFILES=$(wildcard *.lua)
CMOD_FILES=$(LUAFILES:.lua=.c)
CFILES=$(wildcard *.c)
OBJFILES=$(CFILES:.c=.o)

CFLAGS=-O0
LDFLAGS=

%.o: %.c
	clang $(CFLAGS) -I../src -c $< -o $@

%.c: %.lua
	../src/luaot $^ -o $@

main.c: main.lua
	../src/luaot $^ -o $@ -e -i posix

main: $(OBJFILES)
	clang $^ -o $@ $(LDFLAGS) -L../src -llua


.PHONY: clean gen-lua all
gen-lua: $(CMOD_FILES)
all:
	$(MAKE) gen-lua
	$(MAKE) main

clean:
	-rm *.o *.c
