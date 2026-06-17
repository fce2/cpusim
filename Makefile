DIRS = 6502 6502M 65816 z80

all: $(DIRS)

6502 6502M 65816 z80:
	$(MAKE) -C $@

clean:
	$(foreach dir,$(DIRS),$(MAKE) -C $(dir) clean;)

test: $(DIRS)
	$(foreach dir,$(DIRS),$(MAKE) -C $(dir) test;)

.PHONY: all clean test $(DIRS)