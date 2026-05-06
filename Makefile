# =============================================================================
# Makefile
# =============================================================================

ifneq ($(shell which i686-elf-gcc 2>/dev/null),)
  CC            := i686-elf-gcc
  LD            := i686-elf-gcc
  COMPILER_TYPE := elf
else ifneq ($(shell which i686-linux-gnu-gcc 2>/dev/null),)
  CC            := i686-linux-gnu-gcc
  LD            := i686-linux-gnu-gcc
  COMPILER_TYPE := linux-gnu
else
  CC            := gcc
  LD            := gcc
  COMPILER_TYPE := native-m32
endif

ASM  := nasm
GRUB := grub-mkrescue
$(info [CC] $(CC) [$(COMPILER_TYPE)])

CFLAGS := -m32 -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
          -no-pie -fno-builtin -fno-common \
          -Wall -Wno-switch -Wno-unused-function -Wno-unused-variable \
          -Wno-unused-label \
          -O2 -std=gnu11 \
          -I. -Ikernel -Ikernel/syscall -Ikernel/process \
          -Idrivers -Idrivers/net -Ilib -Imemory -Igui -Inet \
          -Ifilesystem -Ifilesystem/fat32 -Ifilesystem/pe \
          -Iprograms -Iprograms/shell -Iprograms/editor \
          -Iprograms/games -Iprograms/filemanager -Iprograms/browser \
          -Iprograms/wordproc -Iprograms/mediaplayer/video \
          -Iprograms/mediaplayer/audio -Iprograms/codeeditor

ifeq ($(COMPILER_TYPE),linux-gnu)
  CFLAGS += -nostdinc -isystem $(shell $(CC) -print-file-name=include 2>/dev/null)
endif

ASM_FLAGS := -f elf32

ifeq ($(COMPILER_TYPE),elf)
  LDFLAGS := -m32 -nostdlib -ffreestanding -no-pie -T linker.ld
else
  LDFLAGS := -m32 -nostdlib -ffreestanding -no-pie -static \
             -T linker.ld -Wl,--build-id=none -Wl,-z,notext
endif

SRCS_ASM := boot/multiboot.asm \
            kernel/gdt_flush.asm \
            kernel/isr.asm \
            kernel/syscall/syscall_entry.asm

SRCS_C := \
  kernel/kernel.c kernel/gdt.c kernel/idt.c \
  kernel/syscall/syscall.c kernel/process/process.c \
  drivers/vga/vga_text.c drivers/keyboard/keyboard.c \
  drivers/timer/pit.c drivers/framebuffer/framebuffer.c \
  drivers/mouse/mouse.c drivers/ata/ata.c \
  drivers/net/pci.c drivers/net/e1000.c drivers/net/wifi.c \
  drivers/usb/usb.c drivers/usb/usbmass.c \
  memory/pmm.c memory/vmm.c memory/heap.c \
  filesystem/vfs.c filesystem/ramfs.c filesystem/elf.c \
  filesystem/fat32/fat32.c filesystem/pe/pe.c \
  net/net.c net/net_core.c net/net_arp.c net/net_ip.c net/net_icmp.c \
  net/net_udp.c net/net_tcp.c net/net_dns.c net/net_http.c net/net_tls.c \
  gui/gui.c gui/apps.c \
  programs/shell/shell.c programs/editor/editor.c \
  programs/games/snake.c programs/games/tetris.c programs/games/pong.c \
  programs/filemanager/filemanager.c \
  programs/browser/browser.c \
  programs/wordproc/wordproc.c \
  programs/mediaplayer/video/mediaplayer_video.c \
  programs/mediaplayer/audio/mediaplayer_audio.c \
  programs/codeeditor/codeeditor.c \
  lib/string.c lib/divdi3.c

OBJS_ASM := $(patsubst %.asm,build/%.o,$(SRCS_ASM))
OBJS_C   := $(patsubst %.c,  build/%.o,$(SRCS_C))
ALL_OBJS := $(OBJS_ASM) $(OBJS_C)
KERNEL_BIN := build/myos.kernel
ISO_IMAGE  := build/myos.iso
DISK_IMG   := build/disk.img

.PHONY: all clean run run-fb run-disk run-net run-nox debug disk flash userspace dirs

all: $(ISO_IMAGE)

dirs:
	@mkdir -p build/boot/grub build/kernel \
	  build/kernel/syscall build/kernel/process \
	  build/drivers/vga build/drivers/keyboard build/drivers/timer \
	  build/drivers/framebuffer build/drivers/mouse \
	  build/drivers/ata build/drivers/net \
	  build/memory \
	  build/filesystem build/filesystem/fat32 build/filesystem/pe \
	  build/net build/gui \
	  build/programs/shell build/programs/editor build/programs/games \
	  build/programs/filemanager build/programs/browser \
	  build/programs/wordproc \
	  build/programs/mediaplayer/video build/programs/mediaplayer/audio \
	  build/programs/codeeditor \
	  build/lib build/iso/boot/grub build/userspace

$(ISO_IMAGE): $(KERNEL_BIN) | dirs
	@echo "[ISO] Building..."
	@cp $(KERNEL_BIN) build/iso/boot/myos.kernel
	@cp boot/grub/grub.cfg build/iso/boot/grub/grub.cfg
	@$(GRUB) -o $(ISO_IMAGE) build/iso 2>/dev/null
	@echo "[OK]  $(ISO_IMAGE)"

$(KERNEL_BIN): $(ALL_OBJS) | dirs
	@echo "[LD]  Linking kernel..."
	@$(LD) $(LDFLAGS) -o $@ $^
	@echo "[OK]  $(KERNEL_BIN)"

build/%.o: %.asm | dirs
	@mkdir -p $(dir $@)
	@echo "[ASM] $<"
	@$(ASM) $(ASM_FLAGS) $< -o $@

build/%.o: %.c | dirs
	@mkdir -p $(dir $@)
	@echo "[CC]  $<"
	@$(CC) $(CFLAGS) -c $< -o $@

userspace:
	@$(MAKE) -C userspace CC=$(CC)

disk: $(DISK_IMG)
$(DISK_IMG): | dirs
	@echo "[DISK] Creating 32MB FAT32 disk..."
	@dd if=/dev/zero of=$(DISK_IMG) bs=1M count=32 2>/dev/null
	@mkfs.fat -F 32 -n "MYOSDISK" $(DISK_IMG) 2>/dev/null || true
	@echo "[OK]  $(DISK_IMG)"

#ISO
flash: $(ISO_IMAGE)
	@if [ -z "$(DEVICE)" ]; then \
	    echo "Usage: make flash DEVICE=/dev/sdX"; exit 1; fi
	@echo "Writing to $(DEVICE)..."
	@sudo dd if=$(ISO_IMAGE) of=$(DEVICE) bs=4M status=progress
	@sudo sync
	@echo "[OK] Flash complete. Eject and boot!"

# ─── QEMU ────────────────────────────────────────────────────────────────────
run: $(ISO_IMAGE) disk
	qemu-system-i386 \
	  -cdrom $(ISO_IMAGE) \
	  -hda $(DISK_IMG) \
	  -m 256M \
	  -device VGA,vgamem_mb=16 \
	  -display sdl \
	  -nic user,model=e1000 \
	  -no-reboot

run-nox: $(ISO_IMAGE)
	qemu-system-i386 -cdrom $(ISO_IMAGE) -m 256M -nographic -no-reboot

debug: $(ISO_IMAGE)
	qemu-system-i386 -cdrom $(ISO_IMAGE) -m 256M -vga std \
	  -display sdl -s -S -no-reboot &
	@echo "GDB: target remote :1234 / symbol-file $(KERNEL_BIN)"

clean:
	@rm -rf build/ && echo "[OK] Clean"