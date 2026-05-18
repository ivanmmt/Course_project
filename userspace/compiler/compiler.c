#include "myos_libc.h"

#define ELF_BASE       0x08048000u
#define ELF_ENTRY_OFF  0x54u
#define ELF_PHOFF      0x34u
#define ELF_HDR_SIZE   0x34u
#define ELF_PH_SIZE    0x20u

static int my_strlen(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static int my_strncmp(const char* a, const char* b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i] || a[i] == '\0' || b[i] == '\0') return (unsigned char)a[i] - (unsigned char)b[i];
    }
    return 0;
}

static char* my_strncat(char* dest, const char* src, int n) {
    int len = my_strlen(dest);
    for (int i = 0; i < n && src[i]; i++) {
        dest[len++] = src[i];
    }
    dest[len] = '\0';
    return dest;
}

static const char* default_output(const char* path) {
    static char out[256];
    int len = 0;
    while (path[len] && len < 240) {
        out[len] = path[len];
        len++;
    }
    out[len] = '\0';

    int dot = -1;
    for (int i = len - 1; i >= 0; i--) {
        if (out[i] == '/' || out[i] == '\\') break;
        if (out[i] == '.') { dot = i; break; }
    }
    if (dot >= 0) {
        out[dot] = '\0';
    }
    my_strncat(out, ".elf", 5);
    return out;
}

static int parse_integer(const char* s) {
    int value = 0;
    int sign = 1;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { sign = -1; s++; }
    while (*s >= '0' && *s <= '9') {
        value = value * 10 + (*s - '0');
        s++;
    }
    return sign * value;
}

static bool parse_string_literal(const char* src, char* dst, int* out_len) {
    const char* p = src;
    while (*p && *p != '"') p++;
    if (*p != '"') return false;
    p++;
    int idx = 0;
    while (*p && *p != '"' && idx < 240) {
        if (*p == '\\' && p[1]) {
            p++;
            if (*p == 'n') dst[idx++] = '\n';
            else if (*p == 't') dst[idx++] = '\t';
            else dst[idx++] = *p;
        } else {
            dst[idx++] = *p;
        }
        p++;
    }
    if (*p != '"') return false;
    dst[idx] = '\0';
    *out_len = idx;
    return true;
}

static bool compile_source(const char* source, uint8_t** out_buf, int* out_size) {
    const char* p = source;
    bool in_main = false;
    int return_code = 0;
    int string_count = 0;
    static char strings[8][256];
    static int string_len[8];

    while (*p) {
        if (!in_main) {
            if (my_strncmp(p, "int main", 8) == 0) {
                in_main = true;
            }
            p++;
            continue;
        }

        if (my_strncmp(p, "printf", 6) == 0 || my_strncmp(p, "puts", 4) == 0) {
            if (string_count >= 8) return false;
            char literal[256];
            int lit_len = 0;
            if (!parse_string_literal(p, literal, &lit_len)) return false;
            if (lit_len <= 0) return false;
            if (my_strncmp(p, "puts", 4) == 0) {
                if (lit_len + 1 >= 256) return false;
                literal[lit_len++] = '\n';
            }
            memcpy(strings[string_count], literal, lit_len);
            string_len[string_count] = lit_len;
            string_count++;
        } else if (my_strncmp(p, "return", 6) == 0) {
            p += 6;
            return_code = parse_integer(p);
        }
        p++;
    }

    if (!in_main) return false;

    uint8_t* buffer = (uint8_t*)malloc(8192);
    if (!buffer) return false;
    memset(buffer, 0, 8192);

    /* ELF header */
    buffer[0] = 0x7F; buffer[1] = 'E'; buffer[2] = 'L'; buffer[3] = 'F';
    buffer[4] = 1;  /* 32-bit */
    buffer[5] = 1;  /* little-endian */
    buffer[6] = 1;  /* version */
    buffer[7] = 0;  /* ABI */
    buffer[8] = 0;  /* ABI version */

    buffer[0x10] = 2;  /* ET_EXEC */
    buffer[0x12] = 3;  /* EM_386 */
    buffer[0x14] = 1;  /* version */
    uint32_t entry = ELF_BASE + ELF_ENTRY_OFF;
    memcpy(buffer + 0x18, &entry, 4);
    uint32_t phoff = ELF_PHOFF;
    memcpy(buffer + 0x1C, &phoff, 4);
    uint16_t ehsize = ELF_HDR_SIZE;
    memcpy(buffer + 0x28, &ehsize, 2);
    uint16_t phentsize = ELF_PH_SIZE;
    memcpy(buffer + 0x2A, &phentsize, 2);
    uint16_t phnum = 1;
    memcpy(buffer + 0x2C, &phnum, 2);

    /* Program header */
    uint32_t ph = ELF_PHOFF;
    uint32_t p_type = 1;
    uint32_t p_flags = 5; /* R + X */
    uint32_t p_offset = 0;
    uint32_t p_vaddr = ELF_BASE;
    uint32_t p_paddr = ELF_BASE;
    uint32_t p_align = 0x1000;

    memcpy(buffer + ph + 0x00, &p_type, 4);
    memcpy(buffer + ph + 0x04, &p_offset, 4);
    memcpy(buffer + ph + 0x08, &p_vaddr, 4);
    memcpy(buffer + ph + 0x0C, &p_paddr, 4);

    int cursor = ELF_ENTRY_OFF;
    int total_strings = string_count;

    for (int i = 0; i < total_strings; i++) {
        /* mov eax, 4 */
        buffer[cursor++] = 0xB8;
        buffer[cursor++] = 4;
        buffer[cursor++] = 0;
        buffer[cursor++] = 0;
        buffer[cursor++] = 0;
        /* mov ebx, 1 */
        buffer[cursor++] = 0xBB;
        buffer[cursor++] = 1;
        buffer[cursor++] = 0;
        buffer[cursor++] = 0;
        buffer[cursor++] = 0;
        /* mov ecx, <addr> */
        buffer[cursor++] = 0xB9;
        uint32_t string_addr = ELF_BASE + cursor + 4 + 5 + 2;
        memcpy(buffer + cursor, &string_addr, 4);
        cursor += 4;
        /* mov edx, len */
        buffer[cursor++] = 0xBA;
        memcpy(buffer + cursor, &string_len[i], 4);
        cursor += 4;
        /* int 0x80 */
        buffer[cursor++] = 0xCD;
        buffer[cursor++] = 0x80;
    }

    /* mov eax, 1 */
    buffer[cursor++] = 0xB8;
    buffer[cursor++] = 1;
    buffer[cursor++] = 0;
    buffer[cursor++] = 0;
    buffer[cursor++] = 0;
    /* mov ebx, return_code */
    buffer[cursor++] = 0xBB;
    memcpy(buffer + cursor, &return_code, 4);
    cursor += 4;
    /* int 0x80 */
    buffer[cursor++] = 0xCD;
    buffer[cursor++] = 0x80;

    int data_start = cursor;

    for (int i = 0; i < 8; i++) {
        if (string_len[i] == 0) break;
        memcpy(buffer + cursor, strings[i], string_len[i]);
        cursor += string_len[i];
    }

    uint32_t filesz = cursor;
    memcpy(buffer + ph + 0x10, &filesz, 4);
    memcpy(buffer + ph + 0x14, &filesz, 4);
    memcpy(buffer + ph + 0x18, &p_flags, 4);
    memcpy(buffer + ph + 0x1C, &p_align, 4);

    *out_buf = buffer;
    *out_size = filesz;
    return true;
}

static void print_usage(void) {
    printf("Usage: compile <source.c|source.cpp> [output.elf]\n");
}

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    const char* src_path = argv[1];
    const char* out_path = (argc >= 3 && argv[2] && argv[2][0]) ? argv[2] : default_output(src_path);

    int fd = open(src_path, O_RDONLY);
    if (fd < 0) {
        printf("compile: cannot open source file '%s'\n", src_path);
        return 2;
    }

    char src_buf[8192];
    int read_len = read(fd, src_buf, sizeof(src_buf) - 1);
    close(fd);
    if (read_len <= 0) {
        printf("compile: source read failed\n");
        return 3;
    }
    src_buf[read_len] = '\0';

    uint8_t* elf_buf = 0;
    int elf_size = 0;
    if (!compile_source(src_buf, &elf_buf, &elf_size)) {
        printf("compile: unsupported source format. Only simple printf/puts + return constant supported.\n");
        return 4;
    }

    int out_fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC);
    if (out_fd < 0) {
        printf("compile: cannot open output file '%s'\n", out_path);
        return 5;
    }

    int written = write(out_fd, elf_buf, elf_size);
    close(out_fd);
    if (written != elf_size) {
        printf("compile: write failed\n");
        return 6;
    }

    printf("Compiled %s -> %s (%d bytes)\n", src_path, out_path, elf_size);
    return 0;
}
