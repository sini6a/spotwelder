# Datakonsulten Spot Welder - ATtiny13 firmware build
#
# Usage:
#   make            build spotwelder.hex
#   make flash      build and flash over ISP
#   make fuses      program factory-default fuses (9.6MHz int. RC / 8 = 1.2MHz)
#   make fuses-fast program fuses with CKDIV8 disabled (full 9.6MHz)
#                   -> if you use this, also change F_CPU in main.c to 9600000UL
#   make clean      remove build output

MCU        = attiny13
F_CPU      = 1200000UL
TARGET     = spotwelder

PROGRAMMER = usbasp
PORT       = usb

CC         = avr-gcc
OBJCOPY    = avr-objcopy
SIZE       = avr-size
AVRDUDE    = avrdude

CFLAGS     = -Wall -Wextra -Os -std=gnu11 -mmcu=$(MCU) -DF_CPU=$(F_CPU)

.PHONY: all flash fuses fuses-fast clean size

all: $(TARGET).hex

$(TARGET).elf: main.c
	$(CC) $(CFLAGS) -o $@ $^

$(TARGET).hex: $(TARGET).elf
	$(OBJCOPY) -O ihex -R .eeprom $< $@

size: $(TARGET).elf
	$(SIZE) --format=avr --mcu=$(MCU) $<

flash: $(TARGET).hex
	$(AVRDUDE) -c $(PROGRAMMER) -p t13 -P $(PORT) -U flash:w:$(TARGET).hex:i

# NOTE: double check these fuse values against the current ATtiny13/13A datasheet
# before programming - incorrect fuse settings (especially RSTDISBL/clock select)
# can require high-voltage programming to recover from.
fuses:
	$(AVRDUDE) -c $(PROGRAMMER) -p t13 -P $(PORT) -U lfuse:w:0x6a:m -U hfuse:w:0xff:m

fuses-fast:
	$(AVRDUDE) -c $(PROGRAMMER) -p t13 -P $(PORT) -U lfuse:w:0x7a:m -U hfuse:w:0xff:m

clean:
	rm -f $(TARGET).elf $(TARGET).hex
