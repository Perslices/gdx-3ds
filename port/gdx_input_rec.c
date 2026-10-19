// G-Diffuser — .gdxrec input recorder / replayer. Contract and format in gdx_input_rec.h.
#define _CRT_SECURE_NO_WARNINGS 1

#include "gdx_input_rec.h"

#include "port_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#define gdx_rec_mkdir(p) _mkdir(p)
#else
#include <sys/stat.h>
#define gdx_rec_mkdir(p) mkdir((p), 0777)
#endif

// n64_gfx_bridge.cpp: arms the frame dump / oracle capture for the next presented frame.
extern void gdx_request_frame_dump(const char* label);

#define GDX_REC_LINE_CAP 160
#define GDX_REC_NAME_CAP 96
#define GDX_REC_FLUSH_EVERY 60

static GdxInputRecHeader sHdr;
static char sHdrBuild[64] = "unknown";
static char sHdrPins[128] = "";

// ---- recording ----
static FILE* sRecFile = NULL;
static char sRecPath[GDX_REC_NAME_CAP];
static unsigned long sRecLines = 0;
static int sRecLastMode = -1;
static char sShotLabel[32];

// ---- replay ----
static FILE* sRepFile = NULL;
static char sRepPath[GDX_REC_NAME_CAP];
static int sRepPending = 0;
static int sRepActive = 0;
static unsigned long sRepBase = 0;
static unsigned long sRepFrames = 0;
static unsigned long sRepLine = 0;
static int sRepMismatchLogged = 0;
static void (*sEndCb)(void) = NULL;

static void SafeCopy(char* dst, size_t cap, const char* src) {
    if (src == NULL) {
        src = "";
    }
    snprintf(dst, cap, "%s", src);
}

void gdx_input_rec_init(const GdxInputRecHeader* hdr) {
    memset(&sHdr, 0, sizeof(sHdr));
    if (hdr == NULL) {
        return;
    }
    sHdr = *hdr;
    SafeCopy(sHdrBuild, sizeof(sHdrBuild), hdr->build != NULL ? hdr->build : "unknown");
    SafeCopy(sHdrPins, sizeof(sHdrPins), hdr->pins);
    sHdr.build = sHdrBuild;
    sHdr.pins = sHdrPins;
}

int gdx_input_rec_start_record(const char* path) {
    gdx_input_rec_stop();
    if (path == NULL || path[0] == '\0') {
        return -1;
    }
    gdx_rec_mkdir("rec"); // EEXIST is fine; CWD-relative (sdmc root on 3DS)
    sRecFile = fopen(path, "w");
    if (sRecFile == NULL) {
        gdx_port_logf("[rec] record open failed: %s\n", path);
        return -1;
    }
    SafeCopy(sRecPath, sizeof(sRecPath), path);
    sRecLines = 0;
    sRecLastMode = -1;
    sShotLabel[0] = '\0';
    fprintf(sRecFile, "gdxrec 1\n");
    fprintf(sRecFile, "build %s\n", sHdrBuild[0] != '\0' ? sHdrBuild : "unknown");
    if (sHdr.seed1Have) {
        fprintf(sRecFile, "seed1 0x%08x\n", sHdr.seed1);
    } else {
        fprintf(sRecFile, "seed1 none\n");
    }
    if (sHdr.seed2Have) {
        fprintf(sRecFile, "seed2 0x%08x\n", sHdr.seed2);
    } else {
        fprintf(sRecFile, "seed2 none\n");
    }
    fprintf(sRecFile, "pins %s\n", sHdrPins);
    fprintf(sRecFile, "# tick btn stickx sticky [touch tx ty] [SHOT label]\n");
    fflush(sRecFile);
    gdx_port_logf("[rec] recording -> %s\n", path);
    return 0;
}

void gdx_input_rec_stop(void) {
    if (sRecFile != NULL) {
        fclose(sRecFile);
        sRecFile = NULL;
        gdx_port_logf("[rec] recording closed %s lines=%lu\n", sRecPath, sRecLines);
    }
}

static int IsBlankOrComment(const char* line) {
    const char* p = line;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        p++;
    }
    return *p == '\0' || *p == '#';
}

// Reads the header block; leaves the stream at the first data line. Returns 0 on a valid
// "gdxrec 1" header. Logs the header fields so a replay run's receipt names its inputs.
static int ReadHeader(FILE* f, unsigned long* outFrames) {
    char line[GDX_REC_LINE_CAP];
    long dataStart = -1;
    int sawMagic = 0;
    unsigned long frames = 0;
    while (1) {
        long pos = ftell(f);
        if (fgets(line, (int)sizeof(line), f) == NULL) {
            break;
        }
        if (IsBlankOrComment(line)) {
            continue;
        }
        if (!sawMagic) {
            int version = 0;
            if (sscanf(line, "gdxrec %d", &version) != 1 || version != 1) {
                gdx_port_logf("[rec] replay: bad magic line: %s", line);
                return -1;
            }
            sawMagic = 1;
            continue;
        }
        {
            char key[16];
            char value[GDX_REC_LINE_CAP];
            value[0] = '\0';
            if (sscanf(line, "%15s %[^\r\n]", key, value) >= 1 &&
                (strcmp(key, "build") == 0 || strcmp(key, "seed1") == 0 || strcmp(key, "seed2") == 0 ||
                 strcmp(key, "pins") == 0)) {
                gdx_port_logf("[rec] replay header %s %s\n", key, value);
                if (strcmp(key, "seed1") == 0 || strcmp(key, "seed2") == 0) {
                    const int which = key[4] - '0';
                    const int have = (which == 1) ? sHdr.seed1Have : sHdr.seed2Have;
                    const unsigned int seed = (which == 1) ? sHdr.seed1 : sHdr.seed2;
                    const int fileHave = strcmp(value, "none") != 0;
                    const unsigned int fileSeed = fileHave ? (unsigned int)strtoul(value, NULL, 0) : 0u;
                    if (fileHave != have || (fileHave && fileSeed != seed)) {
                        gdx_port_logf("[rec] WARNING %s: file=%s run=%s0x%08x (replay is not a parity run)\n", key,
                                      value, have ? "" : "none ", seed);
                    }
                }
                /* RTPARITY: `pins` is DESCRIPTIVE — a snapshot of the RECORDER's ini, written
                 * into the header and never applied on replay. Read as a configuration it is a
                 * trap: every committed replay says `renderthread=0` because the shared Azahar
                 * SD happened to carry that key when the file was recorded, and three progress
                 * docs plus an audit then reported the fork's parity gate as render-thread-off
                 * while the RetroArch lane had in fact been running it ON. So say it out loud:
                 * print the LIVE pins beside the file's whenever they differ, in the same shape
                 * as the seed warning above. */
                if (strcmp(key, "pins") == 0 && strcmp(value, sHdrPins) != 0) {
                    gdx_port_logf("[rec] WARNING pins: file='%s' run='%s' (the header is "
                                  "descriptive only — the RUN's values are what executed)\n",
                                  value, sHdrPins);
                }
                continue;
            }
        }
        dataStart = pos; // first data line
        frames = 1;
        break;
    }
    if (!sawMagic) {
        return -1;
    }
    if (dataStart < 0) {
        *outFrames = 0;
        return 0;
    }
    while (fgets(line, (int)sizeof(line), f) != NULL) {
        if (!IsBlankOrComment(line)) {
            frames++;
        }
    }
    fseek(f, dataStart, SEEK_SET);
    *outFrames = frames;
    return 0;
}

int gdx_replay_start(const char* sdmc_path) {
    FILE* f;
    unsigned long frames = 0;
    if (sdmc_path == NULL || sdmc_path[0] == '\0') {
        return -1;
    }
    f = fopen(sdmc_path, "r");
    if (f == NULL) {
        gdx_port_logf("[rec] replay open failed: %s\n", sdmc_path);
        return -1;
    }
    if (ReadHeader(f, &frames) != 0) {
        fclose(f);
        gdx_port_logf("[rec] replay rejected (not a gdxrec 1 file): %s\n", sdmc_path);
        return -2;
    }
    if (sRepFile != NULL) {
        fclose(sRepFile);
        gdx_port_logf("[rec] replay %s replaced\n", sRepPath);
    }
    sRepFile = f;
    SafeCopy(sRepPath, sizeof(sRepPath), sdmc_path);
    sRepFrames = frames;
    sRepLine = 0;
    sRepMismatchLogged = 0;
    sRepActive = 0;
    sRepPending = 1;
    gdx_port_logf("[rec] replay armed %s frames=%lu (starts at the next tick)\n", sdmc_path, frames);
    return 0;
}

int gdx_input_rec_replay_armed(void) {
    return sRepPending || sRepActive;
}

int gdx_input_rec_replay_active(void) {
    return sRepActive;
}

void gdx_input_rec_set_end_callback(void (*cb)(void)) {
    sEndCb = cb;
}

void gdx_input_rec_note_shot(const char* label) {
    if (label != NULL && label[0] != '\0') {
        SafeCopy(sShotLabel, sizeof(sShotLabel), label);
    }
}

static void ReplayEnd(unsigned long tick) {
    if (sRepFile != NULL) {
        fclose(sRepFile);
        sRepFile = NULL;
    }
    sRepActive = 0;
    sRepPending = 0;
    gdx_port_logf("[rec] end tick=%lu lines=%lu\n", tick, sRepLine);
    if (sEndCb != NULL) {
        sEndCb();
    }
}

// Parses one data line into the pad; returns 0 ok. `shot` receives the SHOT label or "".
static int ParseDataLine(const char* line, unsigned long* ftick, GdxInputPad* pad, char* shot, size_t shotCap) {
    unsigned int btn = 0;
    int sx = 0;
    int sy = 0;
    int consumed = 0;
    const char* p;
    shot[0] = '\0';
    if (sscanf(line, "%lu %4x %d %d%n", ftick, &btn, &sx, &sy, &consumed) != 4) {
        return -1;
    }
    pad->buttons = (unsigned short)btn;
    pad->stickX = (signed char)(sx < -80 ? -80 : (sx > 80 ? 80 : sx));
    pad->stickY = (signed char)(sy < -80 ? -80 : (sy > 80 ? 80 : sy));
    p = line + consumed;
    while (*p != '\0') {
        char tok[32];
        int n = 0;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '\0' || *p == '\r' || *p == '\n') {
            break;
        }
        if (sscanf(p, "%31s%n", tok, &n) != 1) {
            break;
        }
        p += n;
        if (strcmp(tok, "touch") == 0) {
            int tx, ty;
            if (sscanf(p, "%d %d%n", &tx, &ty, &n) == 2) {
                p += n; // parsed, ignored: no touch consumer in the replay path yet
            }
        } else if (strcmp(tok, "SHOT") == 0) {
            if (sscanf(p, "%31s%n", tok, &n) == 1) {
                p += n;
                snprintf(shot, shotCap, "%s", tok);
            }
        }
    }
    return 0;
}

static void ReplayApply(unsigned long tick, GdxInputPad* pad) {
    char line[GDX_REC_LINE_CAP];
    while (1) {
        unsigned long ftick = 0;
        char shot[32];
        if (fgets(line, (int)sizeof(line), sRepFile) == NULL) {
            ReplayEnd(tick);
            return;
        }
        if (IsBlankOrComment(line)) {
            continue;
        }
        if (ParseDataLine(line, &ftick, pad, shot, sizeof(shot)) != 0) {
            gdx_port_logf("[rec] replay parse error (line %lu): %s", sRepLine, line);
            continue;
        }
        sRepLine++;
        if (ftick != tick - sRepBase && !sRepMismatchLogged) {
            sRepMismatchLogged = 1;
            gdx_port_logf("[rec] tick mismatch: file=%lu expected=%lu (line %lu) — continuing line-per-tick\n", ftick,
                          tick - sRepBase, sRepLine);
        }
        if (shot[0] != '\0') {
            gdx_request_frame_dump(shot);
            gdx_input_rec_note_shot(shot);
            gdx_port_logf("[rec] SHOT '%s' at tick %lu\n", shot, tick);
        }
        return;
    }
}

/* C5TICK: does the NEXT replay line carry a SHOT? The hollow-tick lever (port/3ds/gdx3ds_c5tick.h)
 * decides one tick ahead whether the display list being built will be dropped, and a SHOT
 * forces that next tick to render. Reads ahead one data line and seeks back; replay only. */
int gdx_input_rec_peek_shot(void) {
    char line[GDX_REC_LINE_CAP];
    long pos;
    int shotNext = 0;
    if (sRepPending) {
        return 1; /* not started yet: be conservative */
    }
    if (!sRepActive || sRepFile == NULL) {
        return 0;
    }
    pos = ftell(sRepFile);
    while (fgets(line, (int)sizeof(line), sRepFile) != NULL) {
        GdxInputPad pad;
        unsigned long ftick = 0;
        char shot[32];
        if (IsBlankOrComment(line)) {
            continue;
        }
        memset(&pad, 0, sizeof(pad));
        if (ParseDataLine(line, &ftick, &pad, shot, sizeof(shot)) == 0) {
            shotNext = shot[0] != '\0';
        }
        break;
    }
    fseek(sRepFile, pos, SEEK_SET);
    return shotNext;
}

void gdx_input_rec_poll(unsigned long tick, GdxInputPad* pad, int gameMode) {
    if (sRepPending) {
        sRepPending = 0;
        sRepActive = 1;
        sRepBase = tick;
        gdx_port_logf("[rec] replay %s frames=%lu base=%lu\n", sRepPath, sRepFrames, sRepBase);
    }
    if (sRepActive) {
        ReplayApply(tick, pad);
    }
    if (sRecFile != NULL) {
        if (gameMode != sRecLastMode) {
            fprintf(sRecFile, "# mode %d tick %lu\n", gameMode, tick);
            sRecLastMode = gameMode;
        }
        fprintf(sRecFile, "%lu %04x %d %d", tick, (unsigned int)pad->buttons, (int)pad->stickX, (int)pad->stickY);
        if (sShotLabel[0] != '\0') {
            fprintf(sRecFile, " SHOT %s", sShotLabel);
        }
        fputc('\n', sRecFile);
        sRecLines++;
        if ((sRecLines % GDX_REC_FLUSH_EVERY) == 0) {
            fflush(sRecFile);
        }
    }
    sShotLabel[0] = '\0';
}
