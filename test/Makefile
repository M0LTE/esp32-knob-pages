# Host tests for the device's parsers: make && ./test_parsers && ./test_energy && ./test_rooms && ./test_adsb && ./test_ws && ./test_rxa
CFLAGS = -O1 -g -Wall -Wextra -Wno-unused-parameter -I../ui -I../main -fsanitize=address,undefined
CJSON = $(HOME)/esp/esp-idf-v5.5.1/components/json/cJSON

all: test_parsers test_energy test_rooms test_adsb test_ws test_rxa

test_rxa: test_rxa.c ../main/rx_audio.c
	$(CC) $(CFLAGS) -o $@ $^ -lm

test_ws: test_ws.c ../main/ws_parse.c ../main/wf_fold.c
	$(CC) $(CFLAGS) -o $@ $^ -lm

test_adsb: test_adsb.c ../main/adsb_parse.c $(CJSON)/cJSON.c
	$(CC) $(CFLAGS) -I$(CJSON) -o $@ $^ -lm

test_rooms: test_rooms.c ../main/rooms_parse.c $(CJSON)/cJSON.c
	$(CC) $(CFLAGS) -I$(CJSON) -o $@ $^ -lm

test_energy: test_energy.c ../main/energy_parse.c
	$(CC) $(CFLAGS) -o $@ $^ -lm

test_parsers: test_parsers.c ../main/kiss_parse.c ../main/metrics_parse.c
	$(CC) $(CFLAGS) -o $@ $^ -lm
clean:
	rm -f test_parsers test_energy test_rooms test_adsb test_ws test_rxa
