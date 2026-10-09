/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Standalone xemu-source reference; synthetic CPU/kernel backing, no VM. */
#include "dsp_dma.h"
#include "interp/dsp_cpu.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static DSPDMAState dma;
static uint8_t scratch[0x6d000];
static unsigned interrupts = 2, halt, phase, fifo_writes;
static void putword(unsigned o, uint32_t v) {
  for (unsigned i = 0; i < 4; i++)
    scratch[o + i] = (uint8_t)(v >> (8 * i));
}
static uint32_t memread(void *c, int s, uint32_t a) {
  return dsp56k_read_memory(c, s, a);
}
static void memwrite(void *c, int s, uint32_t a, uint32_t v) {
  dsp56k_write_memory(c, s, a, v);
}
static void scratchio(void *c, uint8_t *p, uint32_t a, size_t n, bool wr) {
  (void)c;
  if (a > 0x6c000 || n > 0x6c000 - a) {
    fprintf(stderr,
            "named unavailable scratch backing address=%x bytes=%zu write=%u "
            "phase=%u\n",
            a, n, wr, phase);
    exit(6);
  }
  if (wr)
    memcpy(scratch + a, p, n);
  else
    memcpy(p, scratch + a, n);
}
static void fifoio(void *c, uint8_t *p, unsigned a, size_t n, bool wr) {
  (void)c;
  (void)p;
  if (wr) {
    fifo_writes++;
    fprintf(stderr, "actual FIFO output index=%u size=%zu\n", a, n);
    return;
  }
  fprintf(stderr, "named unsupported FIFO input index=%u size=%zu\n", a, n);
  exit(3);
}
static uint32_t peripheralread(dsp_core_t *c, uint32_t a) {
  (void)c;
  switch (a) {
  case 0xffffb3:
    return 0;
  case 0xffffc5:
    return interrupts | (dma.eol ? 128 : 0);
  case 0xffffd4:
    return dsp_dma_read(&dma, DMA_NEXT_BLOCK);
  case 0xffffd5:
    return dsp_dma_read(&dma, DMA_START_BLOCK);
  case 0xffffd6:
    return dsp_dma_read(&dma, DMA_CONTROL);
  case 0xffffd7:
    return dsp_dma_read(&dma, DMA_CONFIGURATION);
  default:
    fprintf(stderr, "unsupported peripheral read %x\n", a);
    exit(4);
  }
}
static void peripheralwrite(dsp_core_t *c, uint32_t a, uint32_t v) {
  (void)c;
  switch (a) {
  case 0xffffc4:
    halt |= v & 1;
    break;
  case 0xffffc5:
    interrupts &= ~v;
    if (v & 128)
      dma.eol = false;
    break;
  case 0xffffd4:
    dsp_dma_write(&dma, DMA_NEXT_BLOCK, v);
    break;
  case 0xffffd5:
    dsp_dma_write(&dma, DMA_START_BLOCK, v);
    break;
  case 0xffffd6:
    dsp_dma_write(&dma, DMA_CONTROL, v);
    break;
  case 0xffffd7:
    dsp_dma_write(&dma, DMA_CONFIGURATION, v);
    break;
  default:
    fprintf(stderr, "xemu-source unmodelled peripheral write ignored %x=%x\n",
            a, v);
    break;
  }
}
static uint32_t word(unsigned o) {
  return (uint32_t)scratch[o] | (uint32_t)scratch[o + 1] << 8 |
         (uint32_t)scratch[o + 2] << 16 | (uint32_t)scratch[o + 3] << 24;
}
int main(int argc, char **argv) {
  assert(argc == 3 || argc == 5);
  const bool timeline = argc == 3 && strcmp(argv[2], "timeline") == 0;
  unsigned frames = 0;
  const bool gain_probe = argc == 5 && strcmp(argv[2], "gain") == 0;
  const unsigned index = (gain_probe || timeline) ? 0 : (unsigned)strtoul(argv[2], NULL, 0);
  assert(index < 0x7e6);
  FILE *f = fopen(argv[1], "rb");
  assert(f && fread(scratch, 1, sizeof(scratch), f) == sizeof(scratch));
  fclose(f);
  dsp_core_t *d = calloc(1, sizeof(*d));
  assert(d);
  d->is_gp = true;
  dsp56k_reset_cpu(d);
  d->read_peripheral = peripheralread;
  d->write_peripheral = peripheralwrite;
  dma.mem_opaque = d;
  dma.mem_read = memread;
  dma.mem_write = memwrite;
  dma.scratch_rw = scratchio;
  dma.fifo_rw = fifoio;
  for (unsigned i = 0; i < 0x800; i++)
    dsp56k_write_memory(d, DSP_SPACE_P, i,
                        i * 4 < 0x5cc ? word(i * 4) & 0xffffff : 0);
  for (unsigned i = 0; i < 1000000; i++) {
    if (halt) {
      if (timeline && phase) {
        if (phase == 2) {
          printf("timeline output=%x\n",dsp56k_read_memory(d,DSP_SPACE_X,0xf60));
          if (++frames == 6) {free(d);return 0;}
        }
        phase=2;
        for(unsigned k=0;k<1024;k++)d->mixbuffer[k]=0;
        d->mixbuffer[27*32]=0x1000;
        dsp56k_write_memory(d,DSP_SPACE_X,0x392,0x400000);
      }
      if (gain_probe && phase) {
        printf("real gain output=%x fifo=%u\n",
               dsp56k_read_memory(d, DSP_SPACE_X, 0xf60), fifo_writes);
        assert(fifo_writes == 0);
        free(d);
        return 0;
      }
      printf("frame halt instruction=%u pc=%x command=%u\n", i, d->pc,
             word(0x810));
      halt = 0;
      interrupts |= 2;
      d->is_idle = false;
    }
    dsp56k_execute_instruction(d);
    if (word(0x810) == 0) {
      if (!phase) {
        unsigned mismatch = 0;
        for (unsigned k = 0; k < 0x7e6; k++)
          if (dsp56k_read_memory(d, DSP_SPACE_X, 0x80 + k) !=
              word(0x27a8 + k * 4))
            mismatch++;
        printf("real command3 instruction=%u state mismatches=%u\n", i + 1,
               mismatch);
        assert(!mismatch);
        if (timeline) {phase=1;continue;}
        if (gain_probe) {
          dsp56k_write_memory(d, DSP_SPACE_X, 0x1760,
                              (unsigned)strtoul(argv[3], NULL, 0));
          dsp56k_write_memory(d, DSP_SPACE_X, 0x391, 3);
          dsp56k_write_memory(d, DSP_SPACE_X, 0x392,
                              (unsigned)strtoul(argv[4], NULL, 0));
          phase = 1;
          continue;
        }
        putword(0x27a8 + index * 4, 0x123456);
        putword(0x800, index);
        putword(0x808, 0x27a8 + index * 4);
        putword(0x80c, 1);
        putword(0x810, 2);
        phase = 1;
      } else if (!gain_probe && !timeline) {
        printf("real command2 instruction=%u stateword=%x fifo=%u\n", i + 1,
               dsp56k_read_memory(d, DSP_SPACE_X, 0x80 + index), fifo_writes);
        assert(dsp56k_read_memory(d, DSP_SPACE_X, 0x80 + index) == 0x123456);
        assert(fifo_writes == 0);
        free(d);
        return 0;
      }
    }
  }
  printf("bounded no completion pc=%x command=%u\n", d->pc, word(0x810));
  return 1;
}
