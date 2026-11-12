CC      := arm-none-eabi-gcc
OBJCOPY := arm-none-eabi-objcopy

CFLAGS := -mcpu=cortex-m33 -mthumb -mcmse -Os -g -ggdb \
          -ffreestanding -fdata-sections -ffunction-sections \
          -Wall -Wextra -I.

LDFLAGS := -nostdlib -T target.ld -Wl,-gc-sections
LDLIBS  := -Wl,--start-group -lc -lm -lgcc -lnosys -Wl,--end-group

COMMON_SRCS := ivt.c syscalls.c emu_board.c
COMMON_OBJS := $(COMMON_SRCS:.c=.o)

all: test_uart.bin test_els.bin test_pka.bin test_flexspi.bin

test_uart.elf: $(COMMON_OBJS) test_uart.o target.ld
	$(CC) $(CFLAGS) $(COMMON_OBJS) test_uart.o $(LDFLAGS) $(LDLIBS) -o $@
test_uart.bin: test_uart.elf
	$(OBJCOPY) -O binary $< $@

test_els.elf: $(COMMON_OBJS) test_els.o target.ld
	$(CC) $(CFLAGS) $(COMMON_OBJS) test_els.o $(LDFLAGS) $(LDLIBS) -o $@
test_els.bin: test_els.elf
	$(OBJCOPY) -O binary $< $@

test_pka.elf: $(COMMON_OBJS) test_pka.o target.ld
	$(CC) $(CFLAGS) $(COMMON_OBJS) test_pka.o $(LDFLAGS) $(LDLIBS) -o $@
test_pka.bin: test_pka.elf
	$(OBJCOPY) -O binary $< $@

test_flexspi.elf: $(COMMON_OBJS) test_flexspi.o target.ld
	$(CC) $(CFLAGS) $(COMMON_OBJS) test_flexspi.o $(LDFLAGS) $(LDLIBS) -o $@
test_flexspi.bin: test_flexspi.elf
	$(OBJCOPY) -O binary $< $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o *.elf *.bin

.PHONY: all clean
