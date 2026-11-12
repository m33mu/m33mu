CC=arm-none-eabi-gcc
OBJCOPY ?= arm-none-eabi-objcopy

CFLAGS := -mcpu=cortex-m33 -mthumb -mcmse -Os -ffreestanding -fdata-sections -ffunction-sections -g -ggdb
LDFLAGS := -nostdlib -T target.ld -Wl,-gc-sections
LDLIBS := -Wl,--start-group -lc -lm -lgcc -lnosys -Wl,--end-group

PARTS := nrf54l15 nrf54lm20
COMMON_OBJS := ivt.o syscalls.o

all: $(addprefix app-,$(addsuffix .bin,$(PARTS)))

main-nrf54l15.o: main.c
	$(CC) $(CFLAGS) -c $< -o $@

main-nrf54lm20.o: main.c
	$(CC) $(CFLAGS) -DNRF54LM20 -c $< -o $@

app-%.elf: main-%.o $(COMMON_OBJS) target.ld
	$(CC) $(CFLAGS) $< $(COMMON_OBJS) $(LDFLAGS) $(LDLIBS) -o $@

app-%.bin: app-%.elf
	$(OBJCOPY) -O binary $< $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

.PRECIOUS: app-%.elf main-%.o

clean:
	rm -f *.o *.elf *.bin

.PHONY: all clean
