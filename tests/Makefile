CC ?= cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Werror -pedantic
LDLIBS = -lm

all: host_detect test_detector host_payload test_payload test_report test_protocol test_capture
host_detect: host_detect.c ../app/detector.c ../app/detector.h
	$(CC) $(CFLAGS) host_detect.c ../app/detector.c $(LDLIBS) -o $@
test_detector: test_detector.c ../app/detector.c ../app/detector.h
	$(CC) $(CFLAGS) test_detector.c ../app/detector.c $(LDLIBS) -o $@
host_payload: ../app/protocol.c ../app/mc_public.c host_payload.c ../app/payload.c ../app/payload.h ../app/detector.c ../app/detector.h
	$(CC) $(CFLAGS) host_payload.c ../app/payload.c ../app/protocol.c ../app/mc_public.c ../app/detector.c $(LDLIBS) -o $@
test_payload: ../app/protocol.c ../app/mc_public.c test_payload.c ../app/payload.c ../app/payload.h
	$(CC) $(CFLAGS) test_payload.c ../app/payload.c ../app/protocol.c ../app/mc_public.c $(LDLIBS) -o $@
test_report: test_report.c ../app/report.c ../app/report.h ../app/payload.h
	$(CC) $(CFLAGS) test_report.c ../app/report.c -o $@
test_protocol: test_protocol.c public_chat_vectors.h ../app/protocol.c ../app/mc_public.c ../app/payload.c
	$(CC) $(CFLAGS) test_protocol.c ../app/protocol.c ../app/mc_public.c ../app/payload.c $(LDLIBS) -o $@
test_capture: test_capture.c ../app/capture_codec.h
	$(CC) $(CFLAGS) test_capture.c -o $@
check: all
	./test_capture
	./test_detector
	./test_payload
	./test_report
	./test_protocol
	python3 check_captures.py
	python3 check_payload.py
	python3 check_transmit.py
clean:
	rm -f host_detect test_detector host_payload test_payload test_report test_protocol test_capture
.PHONY: all check clean
