#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define _FILE_OFFSET_BITS 64

#define die(...) {fprintf(stderr, __VA_ARGS__); exit(1);}

int recover_byte(FILE *in, char *out)
{
    int buf;
    char rec = 0;
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

int inject_byte(FILE *in, FILE *payload, char *out, int *trail)
{
    int buf, pl;
    pl = fgetc(payload);
    if (pl == EOF)
        return 1;
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

/* simple PPM detection, assuming LF separator */
void ppm_help(FILE *in, FILE *out)
{
    char sbuf[256] = { 0 };
    fgets(sbuf, 10, in);
    if (strcmp(sbuf, "P6")) {
        if (out) fputs(sbuf, out);
        fgets(sbuf, 256, in);
        if (out) fputs(sbuf, out);
        fgets(sbuf, 256, in);
        if (out) fputs(sbuf, out);
    }
}

void usage(void)
{
    die("usage: program <d|e> <input> [payload] <output> [-f]\n"
        "       input can be raw RGB file or PPM file\n"
        "       payload is required when in e mode\n"
        "       -f for overwrite the existing output file\n"
        "       args are required to be in the exact order\n");
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
            if (argc == 5 && !strcmp(argv[4], "-f"))
                fclose(output);
            else
                die("error: output file exists\n");
        }
        output = fopen(argv[3], "wb");
        ppm_help(input, NULL);
        char buf = 0;
        while (!recover_byte(input, &buf))
            fputc(buf, output);
        fclose(input);
        fclose(output);
    }
    else if (*argv[1] == 'e') {
        if (argc < 5)
            die("error: not enough args\n")
        FILE *input, *output, *payload;
        input = fopen(argv[2], "rb");
        if (!input)
            die("error: can't open input file\n");
        payload = fopen(argv[3], "rb");
        if (!payload)
            die("error: can't open payload file\n");
        if ((output = fopen(argv[4], "rb"))) {
            if (argc == 6 && !strcmp(argv[5], "-f"))
                fclose(output);
            else
                die("error: output file exists\n");
        }
        output = fopen(argv[4], "wb");
        ppm_help(input, output);
        char buf[8] = { 0 };
        int ret, trail = 0;
        while (!(ret = inject_byte(input, payload, buf, &trail))) {
            for (int i = 0; i < 8; ++i)
                fputc(buf[i], output);
        }
        if (ret == 1) {
            #define CHUNK_SIZE 32768
            char *chunk = malloc(CHUNK_SIZE);
            size_t read;
            while ((read = fread(chunk, 1, CHUNK_SIZE, input))) {
                for (size_t i = 0; i < read; ++i)
                    chunk[i] = (chunk[i] >> 1) << 1;
                fwrite(chunk, 1, read, output);
            }
        }
        else if (ret == 2) {
            for (int i = 0; i < trail; ++i)
                fputc(buf[i], output);
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
