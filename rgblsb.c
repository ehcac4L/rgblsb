#define _FILE_OFFSET_BITS 64
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define die(...) do {fprintf(stderr, __VA_ARGS__); exit(1);} while(0)
#define CHUNK_SIZE_IN_PAYLOAD_BYTES (128*1024)

void recover_bytes(uint8_t *in, uint8_t *out, size_t pl_len, int word_width, int big_endian)
{
    int endian_offs = big_endian ? word_width - 1 : 0;
    for (int i = 0; i < pl_len; ++i) {
        for (int j = 0; j < 8; ++j) {
            out[i] |= (in[(i*8+j)*word_width+endian_offs] & 0x1) << (7-j);
        }
    }
}

/* now the in and out pointer passed in calling is the same, but it's ok to keep it */
void inject_bytes(uint8_t *in, uint8_t *payload, uint8_t *out, size_t pl_len, int word_width, int big_endian)
{
    int endian_offs = big_endian ? word_width - 1 : 0;
    for (int i = 0; i < pl_len; ++i) {
        for (int j = 0; j < 8; ++j) {
            out[(i*8+j)*word_width+endian_offs] = (in[(i*8+j)*word_width+endian_offs] & 0xFE) | ((payload[i] >> (7-j)) & 0x1);
        }
    }
}

/* simple PPM detection, assuming LF separator between different fields
and whitespace (0x20) separator between dimensions, so 3 "lines" essentially */
int ppm_help(FILE *in, FILE *out)
{
    char sbuf[256] = { 0 };
    fgets(sbuf, 10, in);
    if (!strcmp(sbuf, "P6\n")) {
        if (out) fputs(sbuf, out);
        fgets(sbuf, 256, in);
        if (out) fputs(sbuf, out);
        fgets(sbuf, 256, in);
        if (out) fputs(sbuf, out);
        return strtol(sbuf, NULL, 10);
    }
    else {
        rewind(in);
        return 0;
    }
}

void usage(void)
{
    die("usage: program <d|e> input [payload] output [-f] [-s] [-wn] [-le|be]\n"
        "       input can be raw RGB file or PPM file\n"
        "       payload is required when in e mode\n"
        "       -f for overwrite the existing output file\n"
        "       -s for prepend payload size in message or stop there in d mode, default off\n"
        "       -w folllowed by number without space for word width, multiple of 8, default 1\n"
        "       -le or -be for endian, default le (no effect for 8bit)\n"
        "       files are required to be in the exact place and order\n"
        "       flags can be in different order\n");
}

int get_flag(char *flag, int n, int argc, char *argv[])
{
    for (int i = 1; i < argc; ++i) {
        if (n) {
            if (!strncmp(argv[i], flag, n))
                return i;
        }
        else {
        if (!strcmp(argv[i], flag))
            return i;
        }
    }
    return 0;
}
#define getflag(x, y) get_flag(x, y, argc, argv)

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

int main(int argc, char *argv[])
{
    if (argc < 2)
        usage();

    int word_width = 1, big_endian = 0;
    int tmp, tmp2;
    if ((tmp = getflag("-w", 2))) {
        tmp2 = strlen(argv[tmp]);
        if (tmp2 > 2 && tmp2-2 <= 4) {
            word_width = strtol(argv[tmp]+2, NULL, 10);
            word_width = word_width > 1 ? word_width : 1;
        }
        else
            die("error: invalid word width specified\n");
    }
    if (getflag("-be", 0)) {
        big_endian = 1;
    }
    if (getflag("-le", 0)) {
        big_endian = 0;
    }

    uint32_t lbufsize = CHUNK_SIZE_IN_PAYLOAD_BYTES * 8 * word_width;
    uint32_t pbufsize = CHUNK_SIZE_IN_PAYLOAD_BYTES;
    uint8_t *lbuf = calloc(lbufsize, 1);
    uint8_t *pbuf = calloc(pbufsize, 1);

    if (*argv[1] == 'd') {
        /* chore */
        if (argc < 4)
            die("error: not enough args\n");
        FILE *input, *output;
        input = fopen(argv[2], "rb");
        if (!input)
            die("error: can't open input file\n");
        if ((output = fopen(argv[3], "rb"))) {
            if (getflag("-f", 0))
                fclose(output);
            else
                die("error: output file exists\n");
        }
        output = fopen(argv[3], "wb");
        if (ppm_help(input, NULL) == 65535) {
            if (!getflag("-w", 2) && !getflag("-le", 0) && !getflag("-be", 0)) {
                word_width = 2;
                big_endian = 1;
                lbufsize = CHUNK_SIZE_IN_PAYLOAD_BYTES * 8 * word_width;
                free(lbuf);
                lbuf = calloc(lbufsize, 1);
            }
        }

        /* real deal */
        uint32_t target_bytes = 0, written = 0;
        size_t read = 0;
        if (getflag("-s", 0)) {
            fread(lbuf, 1, 4*(8*word_width), input);
            recover_bytes(lbuf, pbuf, 4, word_width, big_endian);
            for (int i = 0; i < 4; ++i) {
                target_bytes |= pbuf[i] << (8 * i);
            }
        }
        for (;;) {
            memset(pbuf, 0, pbufsize);
            read = fread(lbuf, 1, lbufsize, input);
            if (target_bytes && written + read / (8*word_width) >= target_bytes)
                read = (target_bytes - written) * (8*word_width);
            recover_bytes(lbuf, pbuf, read / (8*word_width), word_width, big_endian);
            written += fwrite(pbuf, 1, read / (8*word_width), output);
            if (read != lbufsize)
                break;
            if (target_bytes && written >= target_bytes)
                break;
        }
        if (written < target_bytes)
            fprintf(stderr, "warning: payload incomplete\n");
        fclose(input);
        fclose(output);
    }


    else if (*argv[1] == 'e') {
        /* chore */
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
            if (getflag("-f", 0))
                fclose(output);
            else
                die("error: output file exists\n");
        }
        output = fopen(argv[4], "wb");
        if (ppm_help(input, output) == 65535) {
            if (!getflag("-w", 2) && !getflag("-le", 0) && !getflag("-be", 0)) {
                word_width = 2;
                big_endian = 1;
                lbufsize = CHUNK_SIZE_IN_PAYLOAD_BYTES * 8 * word_width;
                free(lbuf);
                lbuf = calloc(lbufsize, 1);
            }
        }

        /* real deal */
        size_t orig_read = 0, pl_read = 0;
        int ret = 0, s_flag = getflag("-s", 0);
        if (s_flag) {
            uint32_t fs = get_file_size(argv[3]);
            if (!fs)
                die("error: payload size problem or something\n");
            for (int i = 0; i < 4; ++i) {
                pbuf[i] = (fs >> 8 * i) & 0xFF;
            }
            fread(lbuf, 1, 4*(8*word_width), input);
            inject_bytes(lbuf, pbuf, lbuf, 4, word_width, big_endian);
            fwrite(lbuf, 1, 4*(8*word_width), output);
        }
        for (;;) {
            orig_read = fread(lbuf, 1, lbufsize, input);
            pl_read = fread(pbuf, 1, pbufsize, payload);
            if (orig_read < pl_read * (8*word_width)) {
                pl_read = orig_read / (8*word_width);
                fprintf(stderr, "warning: input capacity smaller than payload\n");
            }
            inject_bytes(lbuf, pbuf, lbuf, pl_read, word_width, big_endian);
            fwrite(lbuf, 1, orig_read, output);
            if ((pl_read != pbufsize) ||
            (orig_read != lbufsize)) {
                if (pl_read < orig_read / (8*word_width))
                    ret = 1;
                break;
            }
        }
        if (ret == 1) {
            while ((orig_read = fread(lbuf, 1, lbufsize, input))) {
                if (!s_flag) {
                    for (size_t i = 0; i < orig_read; ++i)
                        lbuf[i] = lbuf[i] & 0xFE;
                }
                fwrite(lbuf, 1, orig_read, output);
            }
        }
        fclose(input);
        fclose(payload);
        fclose(output);
    }
    else {
        usage();
    }
    free(lbuf);
    free(pbuf);
    return 0;
}
