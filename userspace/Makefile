# =============================================================================
# userspace/Makefile — сборка пользовательских программ для MyOS
#
# Использует тот же кросс-компилятор что и ядро, но другие флаги:
#   - НЕТ -ffreestanding (у нас есть libc)
#   - НЕТ -nostdlib (у нас myos_libc.h)
#   - -static (статическая линковка — нет динамического линкера)
#   - Линкер скрипт гарантирует нужные адреса
# =============================================================================

ifneq ($(shell which i686-elf-g++ 2>/dev/null),)
  CXX := i686-elf-g++
  CC  := i686-elf-gcc
  LD  := i686-elf-gcc
else ifneq ($(shell which i686-linux-gnu-g++ 2>/dev/null),)
  CXX := i686-linux-gnu-g++
  CC  := i686-linux-gnu-gcc
  LD  := i686-linux-gnu-gcc
else ifneq ($(shell which g++ 2>/dev/null),)
  CXX := g++
  CC  := gcc
  LD  := gcc
else
  CXX := gcc
  CC  := gcc
  LD  := gcc
endif

# Флаги для userspace программ
# Ключевое отличие от ядра: НЕТ -ffreestanding, есть наша libc
CFLAGS_USER := \
    -m32 \
    -fno-pic -fno-pie -no-pie \
    -fno-builtin \
    -fno-stack-protector \
    -Wall -O2 \
    -Ilibc

CXXFLAGS_USER := $(CFLAGS_USER) -std=gnu++20 -fno-rtti -fno-exceptions

# Для i686-linux-gnu-gcc нужно исключить системные заголовки
ifeq ($(CC),i686-linux-gnu-gcc)
  CFLAGS_USER += -nostdinc -isystem $(shell $(CC) -print-file-name=include 2>/dev/null)
endif

# Линкер флаги для userspace: статическая линковка без libc
LDFLAGS_USER := \
    -m32 \
    -nostdlib \
    -static \
    -no-pie \
    -Wl,--build-id=none \
    -Wl,-T,userspace.ld

# Выходная директория
OUT_DIR := ../build/userspace
SRC ?=
OUT ?= $(OUT_DIR)/$(notdir $(basename $(SRC))).elf

.PHONY: all clean install hello demo compile

ifeq ($(SRC),)
all: $(OUT_DIR)/hello.elf $(OUT_DIR)/demo.elf
else
all: $(OUT)
endif

$(OUT_DIR):
	@mkdir -p $(OUT_DIR)

# ─── hello.elf ──────────────────────────────────────────────────────────────
$(OUT_DIR)/hello.elf: hello/hello.c libc/myos_libc.h userspace.ld | $(OUT_DIR)
	@echo "[USER] Compiling hello.elf..."
	@$(CC) $(CFLAGS_USER) $(LDFLAGS_USER) -o $@ hello/hello.c
	@echo "[OK]   $@ ($$(du -sh $@ | cut -f1))"

# ─── demo.elf ───────────────────────────────────────────────────────────────
$(OUT_DIR)/demo.elf: calc/demo.c libc/myos_libc.h userspace.ld | $(OUT_DIR)
	@echo "[USER] Compiling demo.elf..."
	@$(CC) $(CFLAGS_USER) $(LDFLAGS_USER) -o $@ calc/demo.c
	@echo "[OK]   $@ ($$(du -sh $@ | cut -f1))"
# ─── Компиляция произвольного файла в .elf ──────────────────────────────────────
$(OUT): $(SRC) libc/myos_libc.h userspace.ld | $(OUT_DIR)
	@echo "[USER] Compiling $(SRC) -> $@"
	@$(if $(filter %.cpp %.CPP %.cc %.CC %.cxx %.CXX,$(SRC)),$(CXX),$(CC)) \
	  $(if $(filter %.cpp %.CPP %.cc %.CC %.cxx %.CXX,$(SRC)),$(CXXFLAGS_USER),$(CFLAGS_USER)) \
	  $(LDFLAGS_USER) -o $@ $(SRC)
	@echo "[OK]   $@ ($$(du -sh $@ | cut -f1))"

compile: all
# ─── Копировать на FAT32 диск ────────────────────────────────────────────────
DISK_IMG := ../build/disk.img

install: all
	@if [ ! -f "$(DISK_IMG)" ]; then \
	    echo "[ERR] No disk image. Run: make disk"; exit 1; fi
	@echo "[DISK] Installing programs to FAT32..."
	@mkdir -p /tmp/myos_mnt
	@sudo mount -o loop $(DISK_IMG) /tmp/myos_mnt 2>/dev/null && \
	    sudo cp $(OUT_DIR)/hello.elf /tmp/myos_mnt/ && \
	    sudo cp $(OUT_DIR)/demo.elf  /tmp/myos_mnt/ && \
	    sudo umount /tmp/myos_mnt && \
	    echo "[OK] Programs installed to disk image" || \
	    echo "[WARN] Mount failed. Try: make embed"

# ─── Встроить программы в RamFS (альтернатива disk) ─────────────────────────
# Генерирует C файл с массивом байт для встраивания в ядро
embed: all
	@echo "[EMBED] Generating embedded_progs.c..."
	@echo "/* Auto-generated — встроенные ELF программы */" > ../kernel/embedded_progs.c
	@echo "#include <stdint.h>" >> ../kernel/embedded_progs.c
	@echo "" >> ../kernel/embedded_progs.c
	@xxd -i $(OUT_DIR)/hello.elf | \
	    sed 's/build_userspace_hello/hello_elf/g' | \
	    sed 's/unsigned char/const uint8_t/g' | \
	    sed 's/unsigned int/const uint32_t/g' \
	    >> ../kernel/embedded_progs.c
	@echo "[OK] embedded_progs.c generated"
	@echo "     Add to kernel.c: extern const uint8_t hello_elf[];"
	@echo "     Then: process_exec_buf(\"hello\", hello_elf, hello_elf_len, ...);"

clean:
	@rm -rf $(OUT_DIR)
	@echo "[OK] Clean"
