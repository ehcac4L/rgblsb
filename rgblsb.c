#define _FILE_OFFSET_BITS 64
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define die(...) do {fprintf(stderr, __VA_ARGS__); exit(1);} while(0)

int recover_byte(FILE *in, uint8_t *out)
{
    int buf;
    uint8_t rec = 0;
    for (int i = 7; i >= 0; --i) {
        buf = fgetc(in);
        if (buf == EOF) {
            *out = rec;
            return EOF;
        }
        buf &= 1;
        rec |= (buf << i);
    }
    *out = rec;
    return 0;
}

int inject_byte(FILE *in, FILE *payload, uint8_t *out, int *trail, int byte)
{
    int buf, pl;
    if (payload) {
        pl = fgetc(payload);
        if (pl == EOF)
            return 1;
    }
    else
        pl = byte;
    for (int i = 0; i < 8; ++i) {
        buf = fgetc(in);
        if (buf == EOF) {
            *trail = i;
            return 2;
        }
        buf = (buf >> 1) << 1;
        buf |= (pl >> (7 - i)) & 1;
        out[i] = buf;
    }
    return 0;
}

/* simple PPM detection, assuming LF separator between different fields
and whitespace (0x20) separator between dimensions, so 3 "lines" essentially */
void ppm_help(FILE *in, FILE *out)
{
    char sbuf[256] = { 0 };
    fgets(sbuf, 10, in);
    if (!strcmp(sbuf, "P6\n")) {
        if (out) fputs(sbuf, out);
        fgets(sbuf, 256, in);
        if (out) fputs(sbuf, out);
        fgets(sbuf, 256, in);
        if (out) fputs(sbuf, out);
    }
    else {
        rewind(in);
    }
}

void usage(void)
{
    die("usage: program <d|e> input [payload] output [-f] [-s]\n"
        "       input can be raw RGB file or PPM file\n"
        "       payload is required when in e mode\n"
        "       -f for overwrite the existing output file\n"
        "       -s for prepend payload size in message or stop there in d mode\n"
        "       files are required to be in the exact place and order\n"
        "       flags can be in different order\n");
}

int got_flag(char *flag, int argc, char *argv[])
{
    for (int i = 0; i < argc; ++i) {
        if (!strcmp(argv[i], flag))
            return 1;
    }
    return 0;
}
#define gotflag(x) got_flag(x, argc, argv)

/* enough size, it's unlikely you need huge payload right? */
uint32_t get_file_size(char *path)
{
    FILE *f = fopen(path, "rb");
    fseek(f, 0, SEEK_END);
    off_t s = ftell(f);
    fclose(f);
    if (s > 0 && s < (uint32_t)-1)
        return (uint32_t)s;
    else
        return 0;
}

void write_byte_count(uint8_t *buf, FILE *out, int count)
{
    for (int i = 0; i < count; ++i)
        fputc(buf[i], out);
}

int main(int argc, char *argv[])
{
    if (argc < 2)
        usage();
    if (*argv[1] == 'd') {
        if (argc < 4)
            die("error: not enough args\n");
        FILE *input, *output;
        input = fopen(argv[2], "rb");
        if (!input)
            die("error: can't open input file\n");
        if ((output = fopen(argv[3], "rb"))) {
            if (gotflag("-f"))
                fclose(output);
            else
                die("error: output file exists\n");
        }
        output = fopen(argv[3], "wb");
        ppm_help(input, NULL);
        uint8_t buf = 0;
        uint32_t target_bytes = 0, written = 0;
        if (gotflag("-s")) {
            for (int i = 0; i < 4; ++i) {
                recover_byte(input, &buf);
                target_bytes |= buf << (8 * i);
            }
        }
        while (!recover_byte(input, &buf)) {
            fputc(buf, output);
            ++written;
            if (target_bytes && written >= target_bytes)
                break;
        }
        fclose(input);
        fclose(output);
    }
    else if (*argv[1] == 'e') {
        if (argc < 5)
            die("error: not enough args\n");
        FILE *input, *output, *payload;
        input = fopen(argv[2], "rb");
        if (!input)
            die("error: can't open input file\n");
        payload = fopen(argv[3], "rb");
        if (!payload)
            die("error: can't open payload file\n");
        if ((output = fopen(argv[4], "rb"))) {
            if (gotflag("-f"))
                fclose(output);
            else
                die("error: output file exists\n");
        }
        output = fopen(argv[4], "wb");
        ppm_help(input, output);
        uint8_t buf[8] = { 0 };
        int ret, trail = 0;
        if (gotflag("-s")) {
            uint32_t fs = get_file_size(argv[3]);
            if (!fs)
                die("error: payload size problem or something\n");
            for (int i = 0; i < 4; ++i) {
                inject_byte(input, NULL, buf, &trail, fs >> (8 * i));
                write_byte_count(buf, output, 8);
            }
        }
        while (!(ret = inject_byte(input, payload, buf, &trail, 0))) {
            write_byte_count(buf, output, 8);
        }
        if (ret == 1) {
            #define CHUNK_SIZE 32768
            char *chunk = malloc(CHUNK_SIZE);
            size_t read;
            while ((read = fread(chunk, 1, CHUNK_SIZE, input))) {
                if (!gotflag("-s")) {
                    for (size_t i = 0; i < read; ++i)
                        chunk[i] = (chunk[i] >> 1) << 1;
                }
                fwrite(chunk, 1, read, output);
            }
        }
        else if (ret == 2) {
            write_byte_count(buf, output, trail);
            if (!feof(payload))
                fprintf(stderr, "warning: input capacity smaller than payload\n");
        }
        fclose(input);
        fclose(payload);
        fclose(output);
    }
    else {
        usage();
    }
    return 0;
}
