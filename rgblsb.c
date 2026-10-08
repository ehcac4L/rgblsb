#define _FILE_OFFSET_BITS 64
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <errno.h>

#define die(...) do { fprintf(stderr, __VA_ARGS__); exit(1); } while(0)

#ifndef BUF_SIZE
#define BUF_SIZE (1<<20)
#endif

static size_t ppm_data_offset(FILE *in)
{
    long start_pos = ftell(in);
    if (start_pos == -1L) start_pos = 0;

    char head[64];
    size_t got = fread(head, 1, sizeof(head), in);
    if (got < 2) {
        fseek(in, start_pos, SEEK_SET);
        return 0;
    }
    if (!(head[0] == 'P' && head[1] == '6')) {
        fseek(in, start_pos, SEEK_SET);
        return 0;
    }

    size_t i = 2, tokens = 0;
    while (tokens < 3 && i < got) {
        while (i < got && (head[i] == ' ' || head[i] == '\t' || head[i] == '\r' || head[i] == '\n')) i++;
        if (i < got && head[i] == '#') {
            while (i < got && head[i] != '\n') i++;
            continue;
        }
        size_t start = i;
        while (i < got && head[i] != ' ' && head[i] != '\t' && head[i] != '\r' && head[i] != '\n') i++;
        if (i == start) break;
        tokens++;
    }
    if (tokens >= 3) {
        size_t j = i;
        while (j < got && (head[j] == ' ' || head[j] == '\t' || head[j] == '\r' || head[j] == '\n')) j++;
        if (j < got) {
            fseek(in, start_pos + (long)j, SEEK_SET);
            return (size_t)(start_pos + (long)j);
        }
    }

    fseek(in, start_pos, SEEK_SET);
    int c;
    c = fgetc(in); if (c == EOF) return 0;
    c = fgetc(in); if (c == EOF) return 0;
    tokens = 0;
    while (tokens < 3) {
        while ((c = fgetc(in)) != EOF && (c == ' ' || c == '\t' || c == '\r' || c == '\n'));
        if (c == EOF) { fseek(in, start_pos, SEEK_SET); return 0; }
        if (c == '#') {
            while ((c = fgetc(in)) != EOF && c != '\n');
            continue;
        }
        while (c != EOF && c != ' ' && c != '\t' && c != '\r' && c != '\n') c = fgetc(in);
        tokens++;
        if (c == EOF) break;
    }
    while ((c = fgetc(in)) != EOF && (c == ' ' || c == '\t' || c == '\r' || c == '\n'));
    if (c == EOF) { fseek(in, start_pos, SEEK_SET); return 0; }
    long off = ftell(in);
    if (off == -1L) off = 0;
    fseek(in, start_pos, SEEK_SET);
    return (size_t)off;
}

static void write_bytes(FILE *out, const uint8_t *buf, size_t count)
{
    if (count == 0) return;
    if (fwrite(buf, 1, count, out) != count) {
        die("error: write failed: %s\n", strerror(errno));
    }
}

static void decode_lsb_stream(FILE *in, FILE *out, size_t data_offset, uint32_t target_bytes)
{
    uint8_t *buf = malloc(BUF_SIZE);
    if (!buf) die("error: out of memory\n");

    if (fseeko(in, (off_t)data_offset, SEEK_SET) != 0) die("error: seek\n");

    size_t out_written = 0;
    int have_partial = 0;
    uint8_t partial = 0;
    size_t in_bytes;
    while ((in_bytes = fread(buf, 1, BUF_SIZE, in)) > 0) {
        for (size_t i = 0; i < in_bytes; ++i) {
            uint8_t bit = buf[i] & 1u;
            partial = (uint8_t)((partial << 1) | bit);
            have_partial++;
            if (have_partial == 8) {
                if (fputc(partial, out) == EOF) {
                    free(buf);
                    die("error: write failed\n");
                }
                out_written++;
                if (target_bytes && out_written >= target_bytes) {
                    free(buf);
                    return;
                }
                have_partial = 0;
                partial = 0;
            }
        }
    }
    free(buf);
}

static void encode_lsb_stream(FILE *in, FILE *payload, FILE *out, size_t data_offset, int prepend_size)
{
    uint8_t *cbuf = malloc(BUF_SIZE);
    if (!cbuf) die("error: out of memory\n");

    if (fseeko(in, (off_t)data_offset, SEEK_SET) != 0) die("error: seek\n");

    uint32_t payload_size = 0;
    int staged_bytes[4];
    int staged_count = 0;
    int staged_index = 0;

    if (prepend_size) {
        if (fseeko(payload, 0, SEEK_END) != 0) die("error: payload seek\n");
        long ppos = ftell(payload);
        if (ppos < 0) die("error: payload tell\n");
        payload_size = (uint32_t)ppos;
        rewind(payload);
        staged_bytes[0] = (int)(payload_size & 0xFFu);
        staged_bytes[1] = (int)((payload_size >> 8) & 0xFFu);
        staged_bytes[2] = (int)((payload_size >> 16) & 0xFFu);
        staged_bytes[3] = (int)((payload_size >> 24) & 0xFFu);
        staged_count = 4;
        staged_index = 0;
    }

    int end_of_payload = 0;
    size_t in_bytes;
    while ((in_bytes = fread(cbuf, 1, BUF_SIZE, in)) > 0) {
        size_t pos = 0;
        while (pos + 8 <= in_bytes) {
            int payload_byte = -1;
            if (staged_index < staged_count) {
                payload_byte = staged_bytes[staged_index++];
            } else if (!end_of_payload) {
                int pb = fgetc(payload);
                if (pb == EOF) {
                    end_of_payload = 1;
                    payload_byte = -1;
                } else {
                    payload_byte = pb;
                }
            }

            if (payload_byte == -1) {
                for (size_t i = pos; i < in_bytes; ++i) {
                    cbuf[i] = (uint8_t)((cbuf[i] >> 1) << 1);
                }
                write_bytes(out, cbuf, in_bytes);
                size_t copy_buf_sz;
                while ((copy_buf_sz = fread(cbuf, 1, BUF_SIZE, in)) > 0) {
                    write_bytes(out, cbuf, copy_buf_sz);
                }
                free(cbuf);
                return;
            }

            uint8_t pb = (uint8_t)payload_byte;
            cbuf[pos + 0] = (uint8_t)((cbuf[pos + 0] & 0xFEu) | ((pb >> 7) & 1u));
            cbuf[pos + 1] = (uint8_t)((cbuf[pos + 1] & 0xFEu) | ((pb >> 6) & 1u));
            cbuf[pos + 2] = (uint8_t)((cbuf[pos + 2] & 0xFEu) | ((pb >> 5) & 1u));
            cbuf[pos + 3] = (uint8_t)((cbuf[pos + 3] & 0xFEu) | ((pb >> 4) & 1u));
            cbuf[pos + 4] = (uint8_t)((cbuf[pos + 4] & 0xFEu) | ((pb >> 3) & 1u));
            cbuf[pos + 5] = (uint8_t)((cbuf[pos + 5] & 0xFEu) | ((pb >> 2) & 1u));
            cbuf[pos + 6] = (uint8_t)((cbuf[pos + 6] & 0xFEu) | ((pb >> 1) & 1u));
            cbuf[pos + 7] = (uint8_t)((cbuf[pos + 7] & 0xFEu) | ((pb >> 0) & 1u));
            pos += 8;
        }
        write_bytes(out, cbuf, in_bytes);
    }

    if (!end_of_payload) {
        int pb = fgetc(payload);
        if (pb != EOF) {
            fprintf(stderr, "warning: input capacity smaller than payload\n");
        }
    }
    free(cbuf);
}

static int got_flag(int argc, char *argv[], const char *flag)
{
    for (int i = 0; i < argc; ++i) {
        if (!strcmp(argv[i], flag)) return 1;
    }
    return 0;
}

static uint32_t get_file_size(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (uint32_t)st.st_size;
}

static void usage(void)
{
    die("usage: program <d|e> input [payload] output [-f] [-s]\n"
        "       input can be raw RGB file or PPM file\n"
        "       payload is required when in e mode\n"
        "       -f for overwrite the existing output file\n"
        "       -s for prepend payload size in message or stop there in d mode\n"
        "       files are required to be in the exact place and order\n"
        "       flags can be in different order\n");
}

int main(int argc, char *argv[])
{
    if (argc < 2) usage();

    if (argv[1][0] == 'd') {
        if (argc < 4) die("error: not enough args\n");
        const char *input_path = argv[2];
        const char *output_path = argv[3];
        int force = got_flag(argc, argv, "-f");
        int with_size = got_flag(argc, argv, "-s");

        FILE *in = fopen(input_path, "rb");
        if (!in) die("error: can't open input file\n");
        if (!force) {
            FILE *t = fopen(output_path, "rb");
            if (t) { fclose(t); fclose(in); die("error: output file exists\n"); }
        }
        FILE *out = fopen(output_path, "wb");
        if (!out) { fclose(in); die("error: can't open output file\n"); }

        size_t data_offset = ppm_data_offset(in);
        uint32_t target_bytes = 0;
        if (with_size) {
            uint8_t tmp[4] = {0};
            uint8_t *buf = malloc(BUF_SIZE);
            if (!buf) die("error: out of memory\n");
            if (fseeko(in, (off_t)data_offset, SEEK_SET) != 0) die("error: seek\n");

            size_t bits = 0, acc = 0;
            size_t read_count;
            size_t idx = 0;
            while (idx < 4 && (read_count = fread(buf, 1, BUF_SIZE, in)) > 0) {
                for (size_t i = 0; i < read_count && idx < 4; ++i) {
                    uint8_t bit = buf[i] & 1u;
                    acc = (acc << 1) | bit;
                    bits++;
                    if (bits == 8) {
                        tmp[idx++] = acc;
                        acc = 0;
                        bits = 0;
                    }
                }
            }
            free(buf);
            if (idx < 4) {
                fclose(in);
                fclose(out);
                die("error: carrier too small for size header\n");
            }
            target_bytes = (uint32_t)tmp[0] | ((uint32_t)tmp[1] << 8) | ((uint32_t)tmp[2] << 16) | ((uint32_t)tmp[3] << 24);
            data_offset += 32;
        }

        decode_lsb_stream(in, out, data_offset, target_bytes);
        fclose(in);
        fclose(out);
    }
    else if (argv[1][0] == 'e') {
        if (argc < 5) die("error: not enough args\n");
        const char *input_path = argv[2];
        const char *payload_path = argv[3];
        const char *output_path = argv[4];
        int force = got_flag(argc, argv, "-f");
        int with_size = got_flag(argc, argv, "-s");

        FILE *in = fopen(input_path, "rb");
        if (!in) die("error: can't open input file\n");
        FILE *payload = fopen(payload_path, "rb");
        if (!payload) { fclose(in); die("error: can't open payload file\n"); }

        if (!force) {
            FILE *t = fopen(output_path, "rb");
            if (t) { fclose(t); fclose(in); fclose(payload); die("error: output file exists\n"); }
        }
        FILE *out = fopen(output_path, "wb");
        if (!out) { fclose(in); fclose(payload); die("error: can't open output file\n"); }

        size_t data_offset = ppm_data_offset(in);
        if (data_offset == 0) {
            if (fseeko(in, 0, SEEK_SET) != 0) die("error: seek\n");
        } else {
            if (fseeko(in, 0, SEEK_SET) != 0) die("error: seek\n");
            uint8_t *hbuf = malloc(data_offset);
            if (!hbuf) die("error: out of memory\n");
            if (fread(hbuf, 1, data_offset, in) != data_offset) { free(hbuf); die("error: read header\n"); }
            if (fwrite(hbuf, 1, data_offset, out) != data_offset) { free(hbuf); die("error: write header\n"); }
            free(hbuf);
        }

        encode_lsb_stream(in, payload, out, data_offset, with_size);

        fclose(in);
        fclose(payload);
        fclose(out);
    }
    else {
        usage();
    }

    return 0;
}
