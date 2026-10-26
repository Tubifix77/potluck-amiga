/* Copper + Denise (perimetro Fase 0) -> dump PPM dell'ultimo frame.
 * Semplificazioni dichiarate (da rimuovere in fasi successive):
 *   - Copper eseguito con granularita di linea (WAIT valutato solo in verticale;
 *     la posizione orizzontale del WAIT è ignorata)
 *   - Fetch bitplane standard lores 320px (DDFSTRT/STOP non interpretati:
 *     assunta finestra standard 0x38/0xD0 come quella del KS)
 *   - Niente sprite, niente HAM, niente dual playfield, niente hires
 * Sufficiente e corretto per la schermata insert-disk del KS 1.3.
 */
#include "a500.h"
#include <string.h>
#ifdef ARDUINO
#include "esp_heap_caps.h"
#endif

/* stato copper */
static uint32_t cop_pc;
static int      cop_stopped;      /* WAIT mai soddisfatto (fine lista) */
static int      cop_wait_v = -1;  /* linea attesa (-1 = nessuna)       */
static int      coplog = 0;       /* log delle prime istruzioni        */

/* framebuffer 320 x 312 (finestra PAL completa), 12 bit -> 24 bit in uscita */
#ifdef ARDUINO
#endif
#define FB_W 640                  /* larghezza hires; in lores i pixel si raddoppiano */
#define FB_H 312
#ifdef ARDUINO
bool g_row_onscreen[FB_H] = {false};  // riga confermata sul display dal dirty-check
#endif
#ifdef ARDUINO
// niente framebuffer intero (400KB non stanno in DRAM): una riga alla volta.
static uint16_t fb_line[FB_W];
// callback fornito dallo sketch: riceve (riga_video, pixel RGB565, larghezza)
void (*denise_line_cb)(int vpos, const uint16_t *pixels, int w) = nullptr;
// converte Amiga 12-bit (0x0RGB) in RGB565
static inline uint16_t amiga_to_565(uint16_t c) {
    uint8_t r = ((c >> 8) & 0xF) * 17, g = ((c >> 4) & 0xF) * 17, b = (c & 0xF) * 17;
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
#else
static uint16_t fb[FB_H][FB_W];   /* colore RGB 4:4:4 */
#endif

/*
 * Palette Amiga OCS/ECS:
 * - COLOR00..COLOR31 sono 12-bit RGB (0x0RGB).
 * - con 5 bitplane il pixel puo' usare indici 0..31.
 * - con 6 bitplane, in modalita' EHB, gli indici 32..63 sono half-bright
 *   di COLOR00..COLOR31.
 *
 * La vecchia versione usava array da 16 colori: con Holiday Lemmings
 * (schermata a 5 bitplane / 32 colori) idx poteva superare 15 e leggere
 * fuori dalla palette locale, producendo forme corrette ma colori sbagliati.
 */
static inline uint16_t half_bright_12(uint16_t c)
{
    uint16_t r = (c >> 8) & 0x0F;
    uint16_t g = (c >> 4) & 0x0F;
    uint16_t b = (c >> 0) & 0x0F;
    r >>= 1; g >>= 1; b >>= 1;
    return (uint16_t)((r << 8) | (g << 4) | b);
}

static inline uint16_t palette_entry_12(int idx)
{
    idx &= 63;
    if (idx < 32)
        return (uint16_t)(custom_get(0x180 + (uint32_t)idx * 2) & 0x0FFF);
    return half_bright_12(custom_get(0x180 + (uint32_t)(idx - 32) * 2));
}

#ifdef ARDUINO
static uint8_t  color_diag_max_idx = 0;
static uint16_t color_diag_last_bplcon0 = 0;
static int      color_diag_last_nplanes = 0;
#endif

void video_color_diag_dump(void)
{
#ifdef ARDUINO
    uint16_t bplcon0 = custom_get(0x100);
    int nplanes = (bplcon0 >> 12) & 7;
    logmsg("[COLOR-DIAG] BPLCON0=%04X nplanes=%d lastBPLCON0=%04X lastN=%d max_color_index=%u\n",
           bplcon0, nplanes, color_diag_last_bplcon0, color_diag_last_nplanes, color_diag_max_idx);
    for (int i = 0; i < 32; i += 8) {
        logmsg("[COLOR-DIAG] COLOR%02d-%02d: %03X %03X %03X %03X %03X %03X %03X %03X\n",
               i, i + 7,
               custom_get(0x180 + (uint32_t)(i + 0) * 2) & 0x0FFF,
               custom_get(0x180 + (uint32_t)(i + 1) * 2) & 0x0FFF,
               custom_get(0x180 + (uint32_t)(i + 2) * 2) & 0x0FFF,
               custom_get(0x180 + (uint32_t)(i + 3) * 2) & 0x0FFF,
               custom_get(0x180 + (uint32_t)(i + 4) * 2) & 0x0FFF,
               custom_get(0x180 + (uint32_t)(i + 5) * 2) & 0x0FFF,
               custom_get(0x180 + (uint32_t)(i + 6) * 2) & 0x0FFF,
               custom_get(0x180 + (uint32_t)(i + 7) * 2) & 0x0FFF);
    }
#endif
}

static inline uint32_t chip_r16(uint32_t a)
{
    a &= 0x7FFFE;
    return (uint32_t)chip_ram[a] << 8 | chip_ram[a + 1];
}

void copper_vblank(void)
{
    cop_pc = ((uint32_t)custom_get(0x080) << 16 | custom_get(0x082)) & 0x7FFFE;
    cop_stopped = 0;
    cop_wait_v = -1;
}

void copper_jmp(int which)   /* strobe COPJMP1/2 */
{
    uint32_t off = which == 1 ? 0x080 : 0x084;
    cop_pc = ((uint32_t)custom_get(off) << 16 | custom_get(off + 2)) & 0x7FFFE;
    cop_stopped = 0;
    cop_wait_v = -1;
}

int copper_writing = 0;   /* attribuzione log: 1 durante i MOVE del copper */

/* esegue il copper per la linea corrente (vpos globale) */
void copper_run_line(void)
{
    if (cop_stopped) return;
    if (!((dmacon & 0x0200) && (dmacon & 0x0080))) return;  /* DMAEN+COPEN */
    if (cop_wait_v >= 0) {
        if (vpos < cop_wait_v) return;
        cop_wait_v = -1;
    }
    for (int guard = 0; guard < 512; guard++) {   /* anti-loop */
        uint16_t ir1 = (uint16_t)chip_r16(cop_pc);
        uint16_t ir2 = (uint16_t)chip_r16(cop_pc + 2);
        if (!(ir1 & 1)) {                          /* MOVE */
            uint32_t reg = ir1 & 0x1FE;
            /* CDANG (hardware reale): MOVE sotto 0x080 senza COPCON bit0
               e' illegale e FERMA il copper fino al prossimo vblank */
            if (reg < 0x080 && !(custom_get(0x02E) & 1)) {
                if (coplog < 24) { logmsg("[COP] %06X MOVE ILLEGALE %03X (CDANG): copper fermato\n", cop_pc, reg); coplog++; }
                cop_stopped = 1;
                return;
            }
            cop_pc = (cop_pc + 4) & 0x7FFFE;
            if (coplog < 24) { logmsg("[COP] %06X MOVE %03X,%04X\n", cop_pc - 4, reg, ir2); coplog++; }
            copper_writing = 1;
            custom_write(reg, ir2);
            copper_writing = 0;
        } else if (!(ir2 & 1)) {                   /* WAIT */
            int vp = (ir1 >> 8) & 0xFF;
            int ve = ((ir2 >> 8) & 0x7F) | 0x80;
            if (coplog < 24) { logmsg("[COP] %06X WAIT v=%02X ve=%02X\n", cop_pc, vp, ve); coplog++; }
            if (ir1 == 0xFFFF && ir2 == 0xFFFE) { cop_stopped = 1; return; } /* fine lista */
            if ((vpos & ve) >= (vp & ve)) { cop_pc = (cop_pc + 4) & 0x7FFFE; continue; }
            cop_wait_v = vp & ve;                  /* approssimazione per-linea */
            return;
        } else {                                   /* SKIP */
            int vp = (ir1 >> 8) & 0xFF;
            int ve = ((ir2 >> 8) & 0x7F) | 0x80;
            cop_pc = (cop_pc + 4) & 0x7FFFE;
            if ((vpos & ve) >= (vp & ve)) cop_pc = (cop_pc + 4) & 0x7FFFE;
        }
    }
    cop_stopped = 1;  /* guard scattato: lista degenere, fermo il copper */
}

/* rendering di una linea nel framebuffer */
/* ============ SPRITE HARDWARE (8 canali DMA) ============
 * Lista in Chip RAM per sprite: POS,CTL poi coppie DATA,DATB per riga,
 * chiusa da 0,0 (o nuova POS,CTL per riuso). Il write di SPRxPTL arma
 * il canale. Colori: sprite 0-1 -> COLOR17-19, 2-3 -> 21-23, ecc.
 * Priorita': per ora sprite sopra il playfield (BPLCON2 in futuro). */
enum { SPR_OFF, SPR_FETCH, SPR_WAIT, SPR_ACTIVE };
static struct {
    uint32_t pc;              /* puntatore DMA corrente */
    int state;
    int vstart, vstop, hstart;
    uint16_t data, datb;      /* parole della riga corrente */
} spr[8];

static uint16_t spr_rd16(uint32_t a) {
    a &= 0x7FFFE;
    return ((uint16_t)chip_ram[a] << 8) | chip_ram[a + 1];
}

void sprite_arm(int n)   /* chiamata al write di SPRxPTL */
{
    uint32_t ph = custom_get(0x120 + n * 4), pl = custom_get(0x122 + n * 4);
    spr[n].pc = (((uint32_t)ph << 16) | pl) & 0x7FFFE;
    spr[n].state = SPR_FETCH;
}

void sprite_vblank(void)  /* inizio frame */
{
    for (int i = 0; i < 8; i++)
        if (spr[i].state == SPR_ACTIVE)
            spr[i].state = SPR_OFF;   /* sprite lasciato a meta': senza riarmo resta spento */
        /* SPR_WAIT resta WAIT: il fetch e' fatto, aspetta il vstart del NUOVO frame.
           E' il caso del riarmo a fine frame (riga ~256): il fetch avviene subito,
           lo sprite parte alla riga giusta del frame successivo. */
}

/* aggiorna il DMA sprite per la riga corrente (vpos). Chiamata PRIMA del render. */
static void sprite_dma_line(void)
{
    if (!((dmacon & 0x0200) && (dmacon & 0x0020))) return;   /* DMAEN+SPREN */
    for (int i = 0; i < 8; i++) {
        if (spr[i].state == SPR_FETCH) {
            uint16_t pos = spr_rd16(spr[i].pc), ctl = spr_rd16(spr[i].pc + 2);
            spr[i].pc += 4;
            if (pos == 0 && ctl == 0) { spr[i].state = SPR_OFF; continue; }
            spr[i].vstart = (pos >> 8) | ((ctl & 0x04) << 6);          /* +E8 come bit 8 */
            spr[i].vstop  = (ctl >> 8) | ((ctl & 0x02) << 7);
            spr[i].hstart = ((pos & 0xFF) << 1) | (ctl & 0x01);
            spr[i].state = SPR_WAIT;
        }
        if (spr[i].state == SPR_WAIT && vpos == spr[i].vstart)
            spr[i].state = SPR_ACTIVE;
        if (spr[i].state == SPR_ACTIVE) {
            if (vpos == spr[i].vstop) {
                spr[i].state = SPR_FETCH;      /* fine: rifetch POS/CTL (riuso) */
                /* riprocesso subito il fetch per questa stessa riga */
                uint16_t pos = spr_rd16(spr[i].pc), ctl = spr_rd16(spr[i].pc + 2);
                spr[i].pc += 4;
                if (pos == 0 && ctl == 0) { spr[i].state = SPR_OFF; continue; }
                spr[i].vstart = (pos >> 8) | ((ctl & 0x04) << 6);
                spr[i].vstop  = (ctl >> 8) | ((ctl & 0x02) << 7);
                spr[i].hstart = ((pos & 0xFF) << 1) | (ctl & 0x01);
                spr[i].state = (vpos == spr[i].vstart) ? SPR_ACTIVE : SPR_WAIT;
                if (spr[i].state != SPR_ACTIVE) continue;
            }
            /* riga attiva: fetcha le parole dati */
            spr[i].data = spr_rd16(spr[i].pc);
            spr[i].datb = spr_rd16(spr[i].pc + 2);
            spr[i].pc += 4;
        }
    }
}

/* sovrappone gli sprite attivi alla riga corrente nel buffer row (Amiga 12-bit).
 * width_mode: 0 = buffer 640 con pixel lores raddoppiati (PC e hires),
 *             1 = buffer 320 lores netto (Arduino lores). */
static void sprite_overlay(uint16_t *row, int lores_narrow)
{
    if (!((dmacon & 0x0200) && (dmacon & 0x0020))) return;
    for (int i = 7; i >= 0; i--) {          /* sprite 0 = priorita' massima: disegnato ultimo */
        if (spr[i].state != SPR_ACTIVE) continue;
        int x0 = spr[i].hstart - 0x81;      /* coordinate lores relative al bordo sinistro */
        for (int b = 0; b < 16; b++) {
            int pix = (((spr[i].datb >> (15 - b)) & 1) << 1) | ((spr[i].data >> (15 - b)) & 1);
            if (!pix) continue;             /* 0 = trasparente */
            uint16_t col = custom_get(0x180 + (16 + (i >> 1) * 4 + pix) * 2);
            int xl = x0 + b;
            if (lores_narrow) {
                if (xl >= 0 && xl < 320) row[xl] = col;
            } else {
                int xf = xl * 2;
                if (xf >= 0 && xf < FB_W - 1) { row[xf] = col; row[xf + 1] = col; }
            }
        }
    }
}


void denise_render_line(void)
{
    if (vpos >= FB_H) return;
    sprite_dma_line();   /* DMA sprite: avanza SEMPRE, anche se il playfield e' saltato */
    uint16_t bg = custom_get(0x180);              /* COLOR00 */
#ifdef ARDUINO
    uint16_t *row = fb_line;
#else
    uint16_t *row = fb[vpos];
#endif
    for (int x = 0; x < FB_W; x++) row[x] = bg;

#ifdef ARDUINO
#define BG_FLUSH() do { if (denise_line_cb) { \
    for (int _x=0;_x<FB_W;_x++) fb_line[_x]=bg; \
    denise_line_cb(vpos, fb_line, 640); } } while(0)
#else
#define BG_FLUSH() do {} while(0)
#endif

    if (!((dmacon & 0x0200) && (dmacon & 0x0100))) { BG_FLUSH(); return; }

    uint16_t diwstrt = custom_get(0x08E), diwstop = custom_get(0x090);
    int vstart = diwstrt >> 8;
    int vstop  = diwstop >> 8; if (!(vstop & 0x80)) vstop += 256;
    if (vpos < vstart || vpos >= vstop) { BG_FLUSH(); return; }

    uint16_t bplcon0 = custom_get(0x100);
    int hires   = (bplcon0 >> 15) & 1;
    int nplanes = (bplcon0 >> 12) & 7;
    if (nplanes == 0) {
        /* zero bitplane: la schermata e' fatta di solo copper-color + sprite
           (es. cracktro/starfield). Riempio con lo sfondo e disegno gli sprite. */
#ifdef ARDUINO
        if (denise_line_cb) {
            for (int _x = 0; _x < FB_W; _x++) fb_line[_x] = bg;
            sprite_overlay(fb_line, 0);       /* stelle/testo della cracktro */
            denise_line_cb(vpos, fb_line, 640);
        }
#endif
        return;
    }
    if (hires && nplanes > 4) nplanes = 4;    /* hires: max 4 bitplane */
    if (nplanes > 6) { BG_FLUSH(); return; }

    int bytes_per_row = hires ? 80 : 40;      /* word DMA per riga di bitplane */
    int pixels        = hires ? 640 : 320;

    uint8_t line[6][80];
    for (int p = 0; p < nplanes; p++) {
        uint32_t pt = bpl_pt[p] & 0x7FFFF;
        /* lettura a BLOCCO invece che byte-per-byte: la PSRAM e' molto piu'
           veloce con memcpy che con tanti accessi singoli. Gestisco il wrap. */
        if (pt + (uint32_t)bytes_per_row <= 0x80000) {
            memcpy(line[p], &chip_ram[pt], bytes_per_row);
        } else {
            for (int b = 0; b < bytes_per_row; b++)
                line[p][b] = chip_ram[(pt + (uint32_t)b) & 0x7FFFF];
        }
        bpl_pt[p] = pt + (uint32_t)bytes_per_row + (uint32_t)(int16_t)custom_get((p & 1) ? 0x10A : 0x108);
    }   /* piani 1,3,5 (indice pari) -> BPL1MOD; 2,4,6 -> BPL2MOD */

#ifdef ARDUINO
    /* SKIP sui DATI (non sui puntatori: quelli cambiano ogni frame nel Workbench).
       Confronto i bitplane grezzi + palette con il frame precedente. Se identici,
       i pixel saranno identici -> salto calcolo e disegno. */
    // buffer snapshot in PSRAM (156KB: troppi per la DRAM interna)
    static uint8_t  (*prev_line)[6][80] = nullptr;
    static uint16_t (*prev_pal)[64] = nullptr;
    static uint8_t  prev_np[FB_H];
    static bool     prev_valid[FB_H] = {false};
    extern bool g_row_onscreen[FB_H];
    if (!prev_line) {
        prev_line = (uint8_t(*)[6][80]) heap_caps_malloc(FB_H*6*80, MALLOC_CAP_SPIRAM);
        prev_pal  = (uint16_t(*)[64])   heap_caps_malloc(FB_H*64*2, MALLOC_CAP_SPIRAM);
        if (!prev_line || !prev_pal) { return; }  // fallback: se PSRAM esaurita, non skippo
    }

    int sk_ncol = 1 << nplanes;
    if (sk_ncol > 64) sk_ncol = 64;
    bool data_same = prev_valid[vpos] && prev_np[vpos] == nplanes && g_row_onscreen[vpos];
    /* uno sprite attivo su questa riga si muove: NON skippare, va ridisegnata */
    if (data_same)
        for (int i = 0; i < 8; i++)
            if (spr[i].state == SPR_ACTIVE) { data_same = false; break; }
    if (data_same)
        for (int p = 0; p < nplanes; p++)
            if (memcmp(prev_line[vpos][p], line[p], bytes_per_row) != 0) { data_same = false; break; }
    if (data_same)
        for (int i = 0; i < sk_ncol; i++)
            if (prev_pal[vpos][i] != palette_entry_12(i)) { data_same = false; break; }

    if (data_same) return;   // riga identica e gia' sul display: niente da fare

    // dati cambiati: salvo lo snapshot per il prossimo frame e procedo a disegnare
    prev_valid[vpos] = true;
    prev_np[vpos] = nplanes;
    for (int p = 0; p < nplanes; p++) memcpy(prev_line[vpos][p], line[p], bytes_per_row);
    for (int i = 0; i < sk_ncol; i++) prev_pal[vpos][i] = palette_entry_12(i);
    g_row_onscreen[vpos] = false;   // sara' ri-confermata dal dirty-check nel callback
#endif

    /* palette precalcolata fuori dal loop: le tinte non cambiano pixel-per-pixel.
       Un accesso array invece di custom_get() 640 volte. */
    uint16_t pal[64];
    int ncol = 1 << nplanes;
    if (ncol > 64) ncol = 64;
    for (int i = 0; i < ncol; i++) pal[i] = palette_entry_12(i);
#ifdef ARDUINO
    color_diag_last_bplcon0 = bplcon0;
    color_diag_last_nplanes = nplanes;
#endif

    /* BPLCON1: scroll orizzontale fine (PF1 dispari, PF2 pari) */
    uint16_t bplcon1 = custom_get(0x102);
    int scroll_pf1 = bplcon1 & 0x0F;
    int scroll_pf2 = (bplcon1 >> 4) & 0x0F;

#if defined(ARDUINO)
    /* Arduino: in lores calcolo 320 pixel UNA volta (niente raddoppio: denise_cb
       decima comunque). In hires i 640 pixel servono tutti. Meta' del lavoro. */
    int outpx = hires ? 640 : 320;
    for (int x = 0; x < outpx; x++) {
        int idx = 0;
        for (int p = 0; p < nplanes; p++) {
            int sc = (p & 1) ? scroll_pf2 : scroll_pf1;
            int sx = x - sc;
            if (sx < 0) continue;
            idx |= ((line[p][sx >> 3] >> (7 - (sx & 7))) & 1) << p;
        }
#ifdef ARDUINO
        if ((uint8_t)idx > color_diag_max_idx) color_diag_max_idx = (uint8_t)idx;
#endif
        row[x] = pal[idx & 63];
    }
    sprite_overlay(row, !hires);              /* sprite sopra il playfield */
    if (denise_line_cb) denise_line_cb(vpos, row, outpx);
    return;
#else
    for (int x = 0; x < pixels; x++) {
        int idx = 0;
        for (int p = 0; p < nplanes; p++)
            idx |= ((line[p][x >> 3] >> (7 - (x & 7))) & 1) << p;
        uint16_t col = pal[idx & 63];
        if (hires) {
            row[x] = col;
        } else {                              /* lores: ogni pixel occupa 2 colonne */
            row[x * 2]     = col;
            row[x * 2 + 1] = col;
        }
    }
#endif
}

#ifndef ARDUINO
int video_write_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "P6\n%d %d\n255\n", FB_W, FB_H);
    for (int y = 0; y < FB_H; y++)
        for (int x = 0; x < FB_W; x++) {
            uint16_t c = fb[y][x];
            uint8_t rgb[3] = { (uint8_t)(((c >> 8) & 15) * 17),
                               (uint8_t)(((c >> 4) & 15) * 17),
                               (uint8_t)((c & 15) * 17) };
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
    return 0;
}
#endif
