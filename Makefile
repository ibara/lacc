DIRS := ${shell find src -type d -print}
SOURCES := $(foreach sdir,$(DIRS),$(wildcard $(sdir)/*.c))

INSTALL_LIB_PATH := /usr/lib/lacc/
INSTALL_BIN_PATH := /usr/bin/
CSMITH_HOME_PATH := ~/Code/csmith

CFLAGS := -Wall -pedantic -std=c89 -g -I include/ -Wno-missing-braces -Wno-psabi
LACCFLAGS := -I include/

all: bin/lacc

bin/lacc: $(SOURCES)
	@mkdir -p $(dir $@)
	cc $(CFLAGS) $^ -o $@

bin/bootstrap: $(patsubst src/%.c,bin/%-bootstrap.o,$(SOURCES))
	cc $^ -o $@

bin/%-bootstrap.o: src/%.c bin/lacc
	@mkdir -p $(dir $@)
	bin/lacc $(LACCFLAGS) -c $< -o $@

bin/selfhost: $(patsubst src/%.c,bin/%-selfhost.o,$(SOURCES))
	cc $^ -o $@

bin/%-selfhost.o: src/%.c bin/bootstrap
	@mkdir -p $(dir $@)
	bin/bootstrap $(LACCFLAGS) -c $< -o $@

test-%: bin/%
	@$(foreach file,$(wildcard test/*.c),./check.sh $< $(file);)

test: test-lacc

install: bin/lacc
	mkdir -p $(INSTALL_LIB_PATH)
	mkdir -p $(INSTALL_LIB_PATH)/include
	cp include/stdlib/*.h $(INSTALL_LIB_PATH)/include/
	cp $< $(INSTALL_BIN_PATH)

uninstall:
	rm -rf $(INSTALL_LIB_PATH)
	rm $(INSTALL_BIN_PATH)/lacc

csmith-test: bin/lacc
	@mkdir -p csmith
	./csmith.sh $(CSMITH_HOME_PATH)

creduce-prepare-%: csmith/%.c bin/lacc
	@mkdir -p creduce
	bin/lacc -std=c99 -I $(CSMITH_HOME_PATH)/runtime -w -E $< -o creduce/reduce.c
	bin/lacc -std=c99 -c -I $(CSMITH_HOME_PATH)/runtime $< -o creduce/reduce.o
	cc creduce/reduce.o -o creduce/reduce -lm
	cc -std=c99 -I $(CSMITH_HOME_PATH)/runtime $< -o creduce/reduce-cc
	cp creduce.sh creduce/
	creduce/reduce 1 > creduce/lacc.out && creduce/reduce-cc 1 > creduce/cc.out
	diff --side-by-side --suppress-common-lines creduce/lacc.out creduce/cc.out | head -n 1

creduce-check: bin/lacc
	./check.sh "bin/lacc -std=c99" creduce/reduce.c "cc -std=c99"

clean:
	rm -rf bin
	rm -f test/*.out test/*.txt test/*.s

.PHONY: all test test-% install uninstall \
	csmith-test creduce-prepare-% creduce-check clean
