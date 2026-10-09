#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#define RECOMP_GENERATED_CODE 1
#include "recomp_types.h"
static void *child(void *ignored) {
 (void)ignored;
 if(g_fp_control_word != 0x027f || RECOMP_FP_PC(16777217.0) != 16777217.0) return (void *)1;
 g_fp_control_word=0x007f;
 if(RECOMP_FP_PC(16777217.0) != 16777216.0) return (void *)2;
 g_fp_control_word=0x077f;
 if(recomp_frndint(1.5,g_fp_control_word) != 1.0) return (void *)3;
 return 0;
}
int main(void) {
 if(g_fp_control_word != 0x027f) return 4;
 g_fp_control_word=0x0b7f;
 pthread_t th; void *result;
 if(pthread_create(&th,0,child,0)||pthread_join(th,&result)||result) return 5;
 if(g_fp_control_word != 0x0b7f || recomp_frndint(1.5,g_fp_control_word) != 2.0) return 6;
 puts("fresh53bit/explicit24bit/independentRC passed"); return 0;
}
