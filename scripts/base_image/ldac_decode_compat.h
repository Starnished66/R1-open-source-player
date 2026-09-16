/* Declarations for the firmware's existing LDAC decoder wrapper, which the
 * encoder-only AOSP ldacBT.h does not declare. Signatures match
 * anonymix007/libldacdec c90094b15e25aef0e47c6d775fa94aceb36cabbc,
 * libldacBT_dec.h. Do not import that header's private handle layout. */
#ifndef HIBY_LDAC_DECODE_COMPAT_H
#define HIBY_LDAC_DECODE_COMPAT_H
#include <ldacBT.h>
int ldacBT_init_handle_decode(HANDLE_LDAC_BT handle, int channel_mode,
                             int sample_rate, int shift, int var1, int var2);
int ldacBT_decode(HANDLE_LDAC_BT handle, unsigned char * stream, void * pcm,
                  LDACBT_SMPL_FMT_T format, int stream_size,
                  int * used_bytes, int * pcm_size);
#endif
