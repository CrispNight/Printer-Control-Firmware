#include "job_store.h"

#include <Arduino.h>
#include <SD.h>
#include <string.h>

namespace job {
namespace {

/* Two names, and the rename between them is the commit. A half-written upload
 * is only ever JOB_TMP, which nothing will print. */
const char* const JOB_FILE = "job.moi";
const char* const JOB_TMP  = "job.tmp";

bool     g_card = false;
bool     g_have = false;
uint32_t g_job_id = 0;
uint16_t g_layers = 0;
uint32_t g_bytes = 0;

/* Upload in progress. */
File     g_out;
bool     g_uploading = false;
uint32_t g_expect_chunk = 0;
uint32_t g_written = 0;      /* bytes received, INCLUDING the file's header */
uint32_t g_want_total = 0;   /* header + body: what the file will be */
uint32_t g_want_bytes = 0;   /* body only, matching job_file_header_t */
uint16_t g_want_crc = 0;
uint16_t g_want_layers = 0;
uint32_t g_want_job = 0;

void closeUpload()
{
    if (g_out) g_out.close();
    g_uploading = false;
}

/* Give up on the transfer entirely: close the handle and delete the partial
 * file. A rejected chunk means the sender and this board disagree about what
 * has arrived, and there is no way to reconcile that mid-stream -- so the
 * transfer ends here and the sender has to start again from BEGIN. Leaving it
 * open would strand a file handle and report an upload in progress forever. */
void abortUpload()
{
    closeUpload();
    if (SD.exists(JOB_TMP)) SD.remove(JOB_TMP);
}

/* Read the header of whatever is on the card and decide whether it is a job we
 * would be willing to print. Called at boot, so a power cut between prints
 * costs nothing. */
void adoptExisting()
{
    g_have = false;
    if (!g_card || !SD.exists(JOB_FILE)) return;

    File f = SD.open(JOB_FILE, FILE_READ);
    if (!f) return;

    job_file_header_t hdr;
    if (f.read(&hdr, sizeof(hdr)) != (int)sizeof(hdr)) { f.close(); return; }
    f.close();

    if (memcmp(hdr.magic, "MOIRENJB", 8) != 0) return;
    if (hdr.format_version != 1) return;

    g_job_id = hdr.job_id;
    g_layers = hdr.layer_count;
    g_bytes  = hdr.total_bytes;
    g_have   = true;
    /* The whole-file CRC is NOT re-checked here. It would mean reading tens of
     * megabytes at every boot, and it is the per-layer CRC checked at read time
     * that protects a print anyway. */
}

}  // namespace

void begin()
{
    g_card = SD.begin(BUILTIN_SDCARD);
    if (!g_card) {
        Serial.println(F("SD: no card (job upload and printing unavailable)"));
        return;
    }
    /* A leftover temporary means the last upload was interrupted. It is not a
     * job and never will be, so it goes. */
    if (SD.exists(JOB_TMP)) SD.remove(JOB_TMP);
    adoptExisting();

    Serial.print(F("SD: card ready, "));
    if (g_have) {
        Serial.print(F("job 0x")); Serial.print(g_job_id, HEX);
        Serial.print(F(" with ")); Serial.print(g_layers);
        Serial.println(F(" layers"));
    } else {
        Serial.println(F("no job"));
    }
}

bool card_ready()   { return g_card; }
bool have_job()     { return g_have; }
uint32_t job_id()   { return g_job_id; }
uint16_t layer_count() { return g_layers; }
uint32_t job_bytes()   { return g_bytes; }

uint8_t upload_begin(const job_upload_begin_t& hdr)
{
    if (!g_card) return ACK_REFUSED;
    if (hdr.total_bytes == 0 || hdr.layer_count == 0) return ACK_BAD_PARAM;

    closeUpload();
    if (SD.exists(JOB_TMP)) SD.remove(JOB_TMP);

    g_out = SD.open(JOB_TMP, FILE_WRITE);
    if (!g_out) return ACK_REFUSED;

    g_uploading    = true;
    g_expect_chunk = 0;
    g_written      = 0;
    g_want_bytes   = hdr.total_bytes;
    g_want_total   = (uint32_t)sizeof(job_file_header_t) + hdr.total_bytes;
    g_want_crc     = hdr.file_crc;
    g_want_layers  = hdr.layer_count;
    g_want_job     = hdr.job_id;
    return ACK_OK;
}

uint8_t upload_data(const job_upload_data_t& hdr, const uint8_t* bytes, uint8_t len)
{
    if (!g_uploading) return ACK_BAD_STATE;

    /* In order, and never past the declared size. A gap that went unnoticed
     * would shift everything after it, and the file would still look like a
     * job. */
    if (hdr.chunk_index != g_expect_chunk) { abortUpload(); return ACK_BAD_PARAM; }
    if (hdr.byte_count == 0 || hdr.byte_count > len) { abortUpload(); return ACK_BAD_LENGTH; }
    if (g_written + hdr.byte_count > g_want_total) { abortUpload(); return ACK_BAD_PARAM; }

    if (g_out.write(bytes, hdr.byte_count) != (size_t)hdr.byte_count) {
        abortUpload();
        return ACK_REFUSED;      /* card full, or it went away mid-write */
    }

    g_written += hdr.byte_count;
    g_expect_chunk++;
    return ACK_OK;
}

uint8_t upload_end(const job_upload_end_t& hdr)
{
    if (!g_uploading) return ACK_BAD_STATE;

    if (hdr.job_id != g_want_job)  { abortUpload(); return ACK_BAD_PARAM; }
    if (g_written != g_want_total) { abortUpload(); return ACK_BAD_LENGTH; }

    g_out.close();
    g_uploading = false;

    /* Read the whole thing back off the card and check it. Reading rather than
     * trusting what we just wrote is the point: this catches a card that
     * accepted the bytes and stored something else. */
    File f = SD.open(JOB_TMP, FILE_READ);
    if (!f) return ACK_REFUSED;

    if (f.size() != (uint64_t)g_want_total) {
        f.close();
        SD.remove(JOB_TMP);
        return ACK_BAD_LENGTH;
    }

    f.seek(sizeof(job_file_header_t));
    uint16_t crc = 0xFFFF;
    uint8_t buf[512];
    uint32_t left = g_want_bytes;
    while (left) {
        const uint16_t want = (left > sizeof(buf)) ? sizeof(buf) : (uint16_t)left;
        const int got = f.read(buf, want);
        if (got != (int)want) { f.close(); SD.remove(JOB_TMP); return ACK_REFUSED; }
        for (uint16_t i = 0; i < want; i++) {
            crc ^= (uint16_t)buf[i] << 8;
            for (uint8_t bit = 0; bit < 8; bit++) {
                crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
                                      : (uint16_t)(crc << 1);
            }
        }
        left -= want;
    }
    f.close();

    if (crc != g_want_crc) {
        SD.remove(JOB_TMP);
        return ACK_BAD_CRC;
    }

    /* Commit. Up to this line the old job is still the printable one. */
    if (SD.exists(JOB_FILE)) SD.remove(JOB_FILE);
    if (!SD.rename(JOB_TMP, JOB_FILE)) {
        SD.remove(JOB_TMP);
        return ACK_REFUSED;
    }

    g_job_id = g_want_job;
    g_layers = g_want_layers;
    g_bytes  = g_want_bytes;
    g_have   = true;
    return ACK_OK;
}

void cmd_status()
{
    Serial.print(F("SD card: "));
    Serial.println(g_card ? F("ready") : F("ABSENT"));
    if (!g_card) return;

    if (g_uploading) {
        Serial.print(F("  upload  : "));
        Serial.print(g_written); Serial.print('/'); Serial.print(g_want_total);
        Serial.print(F(" bytes, chunk ")); Serial.println(g_expect_chunk);
    }

    if (!g_have) {
        Serial.println(F("  job     : none"));
        return;
    }
    Serial.print(F("  job id  : 0x")); Serial.println(g_job_id, HEX);
    Serial.print(F("  layers  : ")); Serial.println(g_layers);
    Serial.print(F("  bytes   : ")); Serial.println(g_bytes);
}

}  // namespace job
