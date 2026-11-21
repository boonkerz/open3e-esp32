# Host-side tests. These build the same sources the firmware uses, without
# ESP-IDF, so the codec port can be diffed against open3e on a workstation.
CC      ?= gcc
CFLAGS  ?= -std=gnu99 -O1 -g -Wall -Wextra -Wno-unused-parameter
LDLIBS  := -lcjson -lm
SRC     := ../main/o3e_json.c ../main/o3e_db.c ../main/o3e_codec.c
FLAT    := ../main/o3e_flatten.c

.PHONY: all clean codec encode flatten isotp uds em380 collect hadisco contact knx topic updatepkg sanitize
all: codec encode flatten em380 isotp uds collect hadisco contact knx topic updatepkg

build:
	@mkdir -p build

build/test_codec: build test_codec.c $(SRC)
	$(CC) $(CFLAGS) -o $@ test_codec.c $(SRC) $(LDLIBS)

build/test_hadisco: build test_hadisco.c ../main/ha_disco.c ../main/contact.c $(SRC)
	$(CC) $(CFLAGS) -I stubs -I ../main -o $@ test_hadisco.c ../main/ha_disco.c ../main/contact.c $(SRC) $(LDLIBS)

hadisco: build/test_hadisco
	cd .. && test/build/test_hadisco data/o3edb.bin

build/test_collect: build test_collect.c ../main/collect_parse.c
	$(CC) $(CFLAGS) -o $@ test_collect.c ../main/collect_parse.c

collect: build/test_collect
	cd .. && test/build/test_collect test/collect_frames.txt

build/test_updatepkg: build test_updatepkg.c ../main/update_pkg.c
	$(CC) $(CFLAGS) -o $@ test_updatepkg.c ../main/update_pkg.c

# Two stand-in images (gen_update_stubs.py), packed by the real tool.
updatepkg: build/test_updatepkg
	python3 -I gen_update_stubs.py build
	python3 -I ../tools/make_update.py --firmware build/fw_test.bin --storage build/st_test.bin --version 9.8.7+test -o build/update_test.bin
	./build/test_updatepkg build/update_test.bin

build/test_contact: build test_contact.c ../main/contact.c
	$(CC) $(CFLAGS) -o $@ test_contact.c ../main/contact.c

contact: build/test_contact
	./build/test_contact

topic:
	@if command -v node >/dev/null; then node test_topic.js; \
	else echo "topic: node fehlt, übersprungen"; fi

build/test_knx: build test_knx.c ../main/knx_frame.c
	$(CC) $(CFLAGS) -o $@ test_knx.c ../main/knx_frame.c

knx: build/test_knx
	./build/test_knx

build/test_isotp: build test_isotp.c ../main/isotp.c
	$(CC) $(CFLAGS) -o $@ test_isotp.c ../main/isotp.c -lpthread

build/test_flatten: build test_flatten.c $(SRC) $(FLAT)
	$(CC) $(CFLAGS) -o $@ test_flatten.c $(SRC) $(FLAT) $(LDLIBS)

flatten: build/test_flatten
	cd .. && test/build/test_flatten data/o3edb.bin test/flat_fixtures.json

build/test_encode: build test_encode.c $(SRC)
	$(CC) $(CFLAGS) -o $@ test_encode.c $(SRC) $(LDLIBS)

encode: build/test_encode
	cd .. && test/build/test_encode data/o3edb.bin test/encode_fixtures.json

build/test_em380: build test_em380.c $(SRC)
	$(CC) $(CFLAGS) -o $@ test_em380.c $(SRC) $(LDLIBS)

em380: build/test_em380
	cd .. && test/build/test_em380 data/o3edb.bin test/em_fixtures.json

codec: build/test_codec
	cd .. && test/build/test_codec data/o3edb.bin test/fixtures.json

isotp: build/test_isotp
	./build/test_isotp

# The codec walks payloads by computed offsets and allocates per datapoint, so
# the suite is worth running once under the sanitizers after any change to it.
SANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1

sanitize: build
	$(CC) -std=gnu99 $(SANFLAGS) -o build/san_codec test_codec.c $(SRC) $(FLAT) $(LDLIBS)
	$(CC) -std=gnu99 $(SANFLAGS) -o build/san_encode test_encode.c $(SRC) $(FLAT) $(LDLIBS)
	$(CC) -std=gnu99 $(SANFLAGS) -o build/san_flatten test_flatten.c $(SRC) $(FLAT) $(LDLIBS)
	$(CC) -std=gnu99 $(SANFLAGS) -o build/san_isotp test_isotp.c ../main/isotp.c -lpthread
	$(CC) -std=gnu99 $(SANFLAGS) -o build/san_uds test_uds.c ../main/isotp.c ../main/uds.c -lpthread
	$(CC) -std=gnu99 $(SANFLAGS) -o build/san_em380 test_em380.c $(SRC) $(FLAT) $(LDLIBS)
	cd .. && ASAN_OPTIONS=detect_leaks=1 test/build/san_codec
	cd .. && ASAN_OPTIONS=detect_leaks=1 test/build/san_encode
	cd .. && ASAN_OPTIONS=detect_leaks=1 test/build/san_flatten
	ASAN_OPTIONS=detect_leaks=1 ./build/san_isotp
	ASAN_OPTIONS=detect_leaks=1 ./build/san_uds
	cd .. && ASAN_OPTIONS=detect_leaks=1 test/build/san_em380

build/test_uds: build test_uds.c ../main/isotp.c ../main/uds.c
	$(CC) $(CFLAGS) -o $@ test_uds.c ../main/isotp.c ../main/uds.c -lpthread

uds: build/test_uds
	./build/test_uds

clean:
	rm -rf build
