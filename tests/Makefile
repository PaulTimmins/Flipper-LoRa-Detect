CC ?= cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Werror -pedantic
LDLIBS = -lm

all: host_detect test_detector host_payload test_payload test_report
host_detect: host_detect.c ../app/detector.c ../app/detector.h
	$(CC) $(CFLAGS) host_detect.c ../app/detector.c $(LDLIBS) -o $@
test_detector: test_detector.c ../app/detector.c ../app/detector.h
	$(CC) $(CFLAGS) test_detector.c ../app/detector.c $(LDLIBS) -o $@
host_payload: host_payload.c ../app/payload.c ../app/payload.h ../app/detector.c ../app/detector.h
	$(CC) $(CFLAGS) host_payload.c ../app/payload.c ../app/detector.c $(LDLIBS) -o $@
test_payload: test_payload.c ../app/payload.c ../app/payload.h
	$(CC) $(CFLAGS) test_payload.c ../app/payload.c $(LDLIBS) -o $@
test_report: test_report.c ../app/report.c ../app/report.h ../app/payload.h
	$(CC) $(CFLAGS) test_report.c ../app/report.c -o $@
check: all
	./test_detector
	./test_payload
	./test_report
	python3 check_captures.py
	python3 check_payload.py
clean:
	rm -f host_detect test_detector host_payload test_payload test_report
.PHONY: all check clean
