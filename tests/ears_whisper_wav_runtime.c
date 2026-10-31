/*
 * Runtime regression for the real local Ears -> whisper.cpp audio format.
 * The two raw continuous 48 kHz stereo sources remain untouched; only the
 * completed short listening clip becomes a 16 kHz mono 16-bit RIFF WAV.
 */
#define main r2_program_main
#include "../r2.c"
#undef main
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t read_u32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t read_u16(const unsigned char *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static void write_stereo_frame(FILE *fp, int16_t left, int16_t right)
{
    unsigned char p[4] = {
        (unsigned char)((uint16_t)left & 0xffU),
        (unsigned char)(((uint16_t)left >> 8) & 0xffU),
        (unsigned char)((uint16_t)right & 0xffU),
        (unsigned char)(((uint16_t)right >> 8) & 0xffU)
    };
    assert(fwrite(p, 1, sizeof(p), fp) == sizeof(p));
}
static FILE *source_wav(uint32_t frames)
{
    FILE *fp = tmpfile();
    assert(fp);
    unsigned char blank[44] = {0};
    assert(fwrite(blank, 1, sizeof(blank), fp) == sizeof(blank));
    for (uint32_t i = 0; i < frames; ++i)
        write_stereo_frame(fp, 2000, -3000);
    assert(r2_ears_write_wav_header(fp, frames * 4U) == 0);
    assert(fflush(fp) == 0);
    return fp;
}
int main(void)
{
    FILE *raw = source_wav(48000U);
    FILE *whisper = tmpfile();
    assert(whisper);
    assert(r2_ears_write_whisper_wav(raw, whisper, 48000U * 4U) == 0);
    assert(fseek(whisper, 0, SEEK_SET) == 0);

    unsigned char header[44];
    assert(fread(header, 1, sizeof(header), whisper) == sizeof(header));
    assert(memcmp(header, "RIFF", 4) == 0);
    assert(memcmp(header + 8, "WAVEfmt ", 8) == 0);
    assert(memcmp(header + 36, "data", 4) == 0);
    assert(read_u32(header + 4) == 36U + 16000U * 2U);
    assert(read_u16(header + 20) == 1U);
    assert(read_u16(header + 22) == 1U);
    assert(read_u32(header + 24) == 16000U);
    assert(read_u32(header + 28) == 32000U);
    assert(read_u16(header + 32) == 2U);
    assert(read_u16(header + 34) == 16U);
    assert(read_u32(header + 40) == 16000U * 2U);

    unsigned char pcm[2];
    for (int i = 0; i < 16000; ++i) {
        assert(fread(pcm, 1, 2, whisper) == 2);
        int16_t sample = (int16_t)read_u16(pcm);
        assert(sample == -500);
    }
    assert(fgetc(whisper) == EOF);
    assert(fseek(raw, 0, SEEK_SET) == 0);
    assert(fread(header, 1, sizeof(header), raw) == sizeof(header));
    assert(read_u32(header + 24) == 48000U &&
           read_u16(header + 22) == 2U &&
           read_u32(header + 40) == 48000U * 4U);
    fclose(raw);
    fclose(whisper);

    /* Reject mismatched/truncated source data instead of making Whisper
       reason about an incomplete or misidentified audio interval. */
    raw = source_wav(24000U);
    whisper = tmpfile();
    assert(whisper);
    assert(r2_ears_write_whisper_wav(raw, whisper, 48000U * 4U) != 0);
    fclose(raw);
    fclose(whisper);

    /* Long real listening choices retain their entire normalized duration. */
    raw = source_wav(30U * 48000U);
    whisper = tmpfile(); assert(whisper);
    assert(r2_ears_write_whisper_wav(raw, whisper, 30U * 192000U) == 0);
    assert(fseek(whisper, 0, SEEK_SET) == 0);
    assert(fread(header, 1, sizeof(header), whisper) == sizeof(header));
    assert(read_u32(header + 40) == 30U * 32000U);
    assert(fseek(whisper, 0, SEEK_END) == 0);
    assert(ftell(whisper) == 44L + 30L * 32000L);
    fclose(raw); fclose(whisper);
    raw = tmpfile(); assert(raw);
    assert(r2_ears_write_wav_header(raw, UINT32_MAX) < 0);
    fclose(raw);

    puts("Whisper-compatible 16 kHz mono audio conversion and duration regression passed.");
    return 0;
}
