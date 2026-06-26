DIRS = 6502 6502M 6502T 6502X 65816 m68k z80

all: $(DIRS)

6502 6502M 6502T 6502X 65816 m68k z80:
	$(MAKE) -C $@

clean:
	$(foreach dir,$(DIRS),$(MAKE) -C $(dir) clean;)

test: $(DIRS)
	$(foreach dir,$(DIRS),$(MAKE) -C $(dir) test;)

.PHONY: all clean test 6502 6502M 6502T 6502X 65816 m68k z80