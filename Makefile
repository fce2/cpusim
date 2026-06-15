DIRS = 6502 6502M z80

all: $(DIRS)

6502 6502M z80:
	$(MAKE) -C $@

clean:
	$(foreach dir,$(DIRS),$(MAKE) -C $(dir) clean;)

.PHONY: all clean $(DIRS)
