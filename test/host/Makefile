CORE = ../../components/bridge_core
CFLAGS = -I$(CORE) -Wall -Wextra -std=c11
TESTS = test_ftms_parse test_rsc_encode test_ftms_devlist test_ifit_parse

all: $(TESTS)
	@for t in $(TESTS); do ./$$t || exit 1; done

test_ftms_parse: test_ftms_parse.c $(CORE)/ftms_parse.c
	$(CC) $(CFLAGS) -o $@ $^ -lm

test_rsc_encode: test_rsc_encode.c $(CORE)/rsc_encode.c
	$(CC) $(CFLAGS) -o $@ $^ -lm

test_ftms_devlist: test_ftms_devlist.c $(CORE)/ftms_devlist.c
	$(CC) $(CFLAGS) -o $@ $^ -lm

test_ifit_parse: test_ifit_parse.c $(CORE)/ifit_parse.c
	$(CC) $(CFLAGS) -o $@ $^ -lm

clean:
	rm -f $(TESTS)
