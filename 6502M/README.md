## (yet another) 6502 simulator (V2)

This time in small, maybe the smallest ;-)
In C++, and not really nice readable...
Not optimized for speed, only for size.
Not even cycle counting (use the "real" cpu6502.c/h for that).
But small, less than 100 "lines", less than 5kB.
More the category "why ?"...

- Only NMOS 6502
- No illegal opcodes

### source files

#### the simulator itself

- `6502M/cpu6502M.h`

### testing

#### a little test

- `main001.cpp`
	minimal example for the minimal simulator
	a "booting" c64 in <6k source code
```
	PC=FCE2 A=00 X=00 Y=00 P=24 SP=FD
	PC=FCE4 A=00 X=FF Y=00 P=A4 SP=FD
	PC=FCE5 A=00 X=FF Y=00 P=A4 SP=FD
	PC=FCE6 A=00 X=FF Y=00 P=A4 SP=FF
	PC=FCE7 A=00 X=FF Y=00 P=A4 SP=FF
	PC=FD02 A=00 X=FF Y=00 P=A4 SP=FD
	PC=FD04 A=00 X=05 Y=00 P=24 SP=FD
	PC=FD07 A=30 X=05 Y=00 P=24 SP=FD
	PC=FD0A A=30 X=05 Y=00 P=25 SP=FD
	PC=FD0F A=30 X=05 Y=00 P=25 SP=FD
	PC=FCEA A=30 X=05 Y=00 P=25 SP=FF
	PC=FCEF A=30 X=05 Y=00 P=25 SP=FF
	PC=FCF2 A=30 X=05 Y=00 P=25 SP=FF
	PC=FDA3 A=30 X=05 Y=00 P=25 SP=FD
	PC=FDA5 A=7F X=05 Y=00 P=25 SP=FD
	PC=FDA8 A=7F X=05 Y=00 P=25 SP=FD
	PC=FDAB A=7F X=05 Y=00 P=25 SP=FD
	PC=FDAE A=7F X=05 Y=00 P=25 SP=FD
	PC=FDB0 A=08 X=05 Y=00 P=25 SP=FD
	PC=FDB3 A=08 X=05 Y=00 P=25 SP=FD
```
