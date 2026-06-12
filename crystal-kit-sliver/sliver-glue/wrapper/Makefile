CC_64 := x86_64-w64-mingw32-gcc
CFLAGS := -Wall -Os -DBUILD_DLL -ffunction-sections -fdata-sections
LDFLAGS := -shared -Wl,--subsystem,windows -s -Wl,--gc-sections

TARGET := crystal-loader.x64.dll
SRCS := crystal-loader.c beacon_compatibility.c
OUTDIR := ../

.PHONY: all clean smoketest

all: $(OUTDIR)$(TARGET)

$(OUTDIR)$(TARGET): $(SRCS)
	$(CC_64) $(CFLAGS) $(SRCS) -o $(OUTDIR)$(TARGET) $(LDFLAGS)

smoketest: $(OUTDIR)build/smoketest.bin

$(OUTDIR)build/smoketest.bin: smoketest.asm
	mkdir -p $(OUTDIR)build
	nasm -f bin smoketest.asm -o $(OUTDIR)build/smoketest.bin

clean:
	rm -f $(OUTDIR)$(TARGET) $(OUTDIR)build/smoketest.bin
