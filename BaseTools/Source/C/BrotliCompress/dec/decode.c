/* Copyright 2013 Google Inc. All Rights Reserved.

   Distributed under MIT license.
   See file LICENSE for detail or copy at https://opensource.org/licenses/MIT
*/

#include <brotli/decode.h>

#if defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

#include <stdlib.h>  /* free, malloc */
#include <string.h>  /* memcpy, memset */

#include "../common/constants.h"
#include "../common/context.h"
#include "../common/dictionary.h"
#include "../common/platform.h"
#include "../common/transform.h"
#include "../common/version.h"
#include "./bit_reader.h"
#include "./huffman.h"
#include "./prefix.h"
#include "./state.h"

#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

#define BROTLI_FAILURE(CODE) (BROTLI_DUMP(), CODE)

#define BROTLI_LOG_UINT(name)                                       \
  BROTLI_LOG(("[%s] %s = %lu\n", __func__, #name, (unsigned long)(name)))
#define BROTLI_LOG_ARRAY_INDEX(array_name, idx)                     \
  BROTLI_LOG(("[%s] %s[%lu] = %lu\n", __func__, #array_name,        \
         (unsigned long)(idx), (unsigned long)array_name[idx]))

#define HUFFMAN_TABLE_BITS 8U
#define HUFFMAN_TABLE_MASK 0xFF

/* We need the slack region for the following reasons:
    - doing up to two 16-byte copies for fast backward copying
    - inserting transformed dictionary word (5 prefix + 24 base + 8 suffix) */
static const uint32_t kRingBufferWriteAheadSlack = 42;

static const uint8_t kCodeLengthCodeOrder[BROTLI_CODE_LENGTH_CODES] = {
  1, 2, 3, 4, 0, 5, 17, 6, 16, 7, 8, 9, 10, 11, 12, 13, 14, 15,
};

/* Static prefix code for the complex code length code lengths. */
static const uint8_t kCodeLengthPrefixLength[16] = {
  2, 2, 2, 3, 2, 2, 2, 4, 2, 2, 2, 3, 2, 2, 2, 4,
};

static const uint8_t kCodeLengthPrefixValue[16] = {
  0, 4, 3, 2, 0, 4, 3, 1, 0, 4, 3, 2, 0, 4, 3, 5,
};

BROTLI_BOOL BrotliDecoderSetParameter(
    BrotliDecoderState* state, BrotliDecoderParameter p, uint32_t value) {
  if (state->state != BROTLI_STATE_UNINITED) return BROTLI_FALSE;
  switch (p) {
    case BROTLI_DECODER_PARAM_DISABLE_RING_BUFFER_REALLOCATION:
      state->canny_ringbuffer_allocation = !!value ? 0 : 1;
      return BROTLI_TRUE;

    case BROTLI_DECODER_PARAM_LARGE_WINDOW:
      state->large_window = TO_BROTLI_BOOL(!!value);
      return BROTLI_TRUE;

    default: return BROTLI_FALSE;
  }
}

BrotliDecoderState* BrotliDecoderCreateInstance(
    brotli_alloc_func alloc_func, brotli_free_func free_func, void* opaque) {
  BrotliDecoderState* state = 0;
  if (!alloc_func && !free_func) {
    state = (BrotliDecoderState*)malloc(sizeof(BrotliDecoderState));
  } else if (alloc_func && free_func) {
    state = (BrotliDecoderState*)alloc_func(opaque, sizeof(BrotliDecoderState));
  }
  if (state == 0) {
    BROTLI_DUMP();
    return 0;
  }
  if (!BrotliDecoderStateInit(state, alloc_func, free_func, opaque)) {
    BROTLI_DUMP();
    if (!alloc_func && !free_func) {
      free(state);
    } else if (alloc_func && free_func) {
      free_func(opaque, state);
    }
    return 0;
  }
  return state;
}

/* Deinitializes and frees BrotliDecoderState instance. */
void BrotliDecoderDestroyInstance(BrotliDecoderState* state) {
  if (!state) {
    return;
  } else {
    brotli_free_func free_func = state->free_func;
    void* opaque = state->memory_manager_opaque;
    BrotliDecoderStateCleanup(state);
    free_func(opaque, state);
  }
}

/* Saves error code and converts it to BrotliDecoderResult. */
static BROTLI_NOINLINE BrotliDecoderResult SaveErrorCode(
    BrotliDecoderState* s, BrotliDecoderErrorCode e) {
  s->error_code = (int)e;
  switch (e) {
    case BROTLI_DECODER_SUCCESS:
      return BROTLI_DECODER_RESULT_SUCCESS;

    case BROTLI_DECODER_NEEDS_MORE_INPUT:
      return BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT;

    case BROTLI_DECODER_NEEDS_MORE_OUTPUT:
      return BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT;

    default:
      return BROTLI_DECODER_RESULT_ERROR;
  }
}

/* Decodes WBITS by reading 1 - 7 bits, or 0x11 for "Large Window Brotli".
   Precondition: bit-reader accumulator has at least 8 bits. */
static BrotliDecoderErrorCode DecodeWindowBits(BrotliDecoderState* s,
                                               BrotliBitReader* br) {
  uint32_t n;
  BROTLI_BOOL large_window = s->large_window;
  s->large_window = BROTLI_FALSE;
  BrotliTakeBits(br, 1, &n);
  if (n == 0) {
    s->window_bits = 16;
    return BROTLI_DECODER_SUCCESS;
  }
  BrotliTakeBits(br, 3, &n);
  if (n != 0) {
    s->window_bits = 17 + n;
    return BROTLI_DECODER_SUCCESS;
  }
  BrotliTakeBits(br, 3, &n);
  if (n == 1) {
    if (large_window) {
      BrotliTakeBits(br, 1, &n);
      if (n == 1) {
        return BROTLI_FAILURE(BROTLI_DECODER_ERROR_FORMAT_WINDOW_BITS);
      }
      s->large_window = BROTLI_TRUE;
      return BROTLI_DECODER_SUCCESS;
    } else {
      return BROTLI_FAILURE(BROTLI_DECODER_ERROR_FORMAT_WINDOW_BITS);
    }
  }
  if (n != 0) {
    s->window_bits = 8 + n;
    return BROTLI_DECODER_SUCCESS;
  }
  s->window_bits = 17;
  return BROTLI_DECODER_SUCCESS;
}

static BROTLI_INLINE void memmove16(uint8_t* dst, uint8_t* src) {
#if defined(__ARM_NEON__)
  vst1q_u8(dst, vld1q_u8(src));
#else
  uint32_t buffer[4];
  memcpy(buffer, src, 16);
  memcpy(dst, buffer, 16);
#endif
}

/* Decodes a number in the range [0..255], by reading 1 - 11 bits. */
static BROTLI_NOINLINE BrotliDecoderErrorCode DecodeVarLenUint8(
    BrotliDecoderState* s, BrotliBitReader* br, uint32_t* value) {
  uint32_t bits;
  switch (s->substate_decode_uint8) {
    case BROTLI_STATE_DECODE_UINT8_NONE:
      if (BROTLI_PREDICT_FALSE(!BrotliSafeReadBits(br, 1, &bits))) {
        return BROTLI_DECODER_NEEDS_MORE_INPUT;
      }
      if (bits == 0) {
        *value = 0;
        return BROTLI_DECODER_SUCCESS;
      }
    /* Fall through. */

    case BROTLI_STATE_DECODE_UINT8_SHORT:
      if (BROTLI_PREDICT_FALSE(!BrotliSafeReadBits(br, 3, &bits))) {
        s->substate_decode_uint8 = BROTLI_STATE_DECODE_UINT8_SHORT;
        return BROTLI_DECODER_NEEDS_MORE_INPUT;
      }
      if (bits == 0) {
        *value = 1;
        s->substate_decode_uint8 = BROTLI_STATE_DECODE_UINT8_NONE;
        return BROTLI_DECODER_SUCCESS;
      }
      /* Use output value as a temporary storage. It MUST be persisted. */
      *value = bits;
    /* Fall through. */

    case BROTLI_STATE_DECODE_UINT8_LONG:
      if (BROTLI_PREDICT_FALSE(!BrotliSafeReadBits(br, *value, &bits))) {
        s->substate_decode_uint8 = BROTLI_STATE_DECODE_UINT8_LONG;
        return BROTLI_DECODER_NEEDS_MORE_INPUT;
      }
      *value = (1U << *value) + bits;
      s->substate_decode_uint8 = BROTLI_STATE_DECODE_UINT8_NONE;
      return BROTLI_DECODER_SUCCESS;

    default:
      return
          BROTLI_FAILURE(BROTLI_DECODER_ERROR_UNREACHABLE);
  }
}

/* Decodes a metablock length and flags by reading 2 - 31 bits. */
static BrotliDecoderErrorCode BROTLI_NOINLINE DecodeMetaBlockLength(
    BrotliDecoderState* s, BrotliBitReader* br) {
  uint32_t bits;
  int i;
  for (;;) {
    switch (s->substate_metablock_header) {
      case BROTLI_STATE_METABLOCK_HEADER_NONE:
        if (!BrotliSafeReadBits(br, 1, &bits)) {
          return BROTLI_DECODER_NEEDS_MORE_INPUT;
        }
        s->is_last_metablock = bits ? 1 : 0;
        s->meta_block_remaining_len = 0;
        s->is_uncompressed = 0;
        s->is_metadata = 0;
        if (!s->is_last_metablock) {
          s->substate_metablock_header = BROTLI_STATE_METABLOCK_HEADER_NIBBLES;
          break;
        }
        s->substate_metablock_header = BROTLI_STATE_METABLOCK_HEADER_EMPTY;
      /* Fall through. */

      case BROTLI_STATE_METABLOCK_HEADER_EMPTY:
        if (!BrotliSafeReadBits(br, 1, &bits)) {
          return BROTLI_DECODER_NEEDS_MORE_INPUT;
        }
        if (bits) {
          s->substate_metablock_header = BROTLI_STATE_METABLOCK_HEADER_NONE;
          return BROTLI_DECODER_SUCCESS;
        }
        s->substate_metablock_header = BROTLI_STATE_METABLOCK_HEADER_NIBBLES;
      /* Fall through. */

      case BROTLI_STATE_METABLOCK_HEADER_NIBBLES:
        if (!BrotliSafeReadBits(br, 2, &bits)) {
          return BROTLI_DECODER_NEEDS_MORE_INPUT;
        }
        s->size_nibbles = (uint8_t)(bits + 4);
        s->loop_counter = 0;
        if (bits == 3) {
          s->is_metadata = 1;
          s->substate_metablock_header = BROTLI_STATE_METABLOCK_HEADER_RESERVED;
          break;
        }
        s->substate_metablock_header = BROTLI_STATE_METABLOCK_HEADER_SIZE;
      /* Fall through. */

      case BROTLI_STATE_METABLOCK_HEADER_SIZE:
        i = s->loop_counter;
        for (; i < (int)s->size_nibbles; ++i) {
          if (!BrotliSafeReadBits(br, 4, &bits)) {
            s->loop_counter = i;
            return BROTLI_DECODER_NEEDS_MORE_INPUT;
          }
          if (i + 1 == s->size_nibbles && s->size_nibbles > 4 && bits == 0) {
            return BROTLI_FAILURE(BROTLI_DECODER_ERROR_FORMAT_EXUBERANT_NIBBLE);
          }
          s->meta_block_remaining_len |= (int)(bits << (i * 4));
        }
        s->substate_metablock_header =
            BROTLI_STATE_METABLOCK_HEADER_UNCOMPRESSED;
      /* Fall through. */

      case BROTLI_STATE_METABLOCK_HEADER_UNCOMPRESSED:
        if (!s->is_last_metablock) {
          if (!BrotliSafeReadBits(br, 1, &bits)) {
            return BROTLI_DECODER_NEEDS_MORE_INPUT;
          }
          s->is_uncompressed = bits ? 1 : 0;
        }
        ++s->meta_block_remaining_len;
        s->substate_metablock_header = BROTLI_STATE_METABLOCK_HEADER_NONE;
        return BROTLI_DECODER_SUCCESS;

      case BROTLI_STATE_METABLOCK_HEADER_RESERVED:
        if (!BrotliSafeReadBits(br, 1, &bits)) {
          return BROTLI_DECODER_NEEDS_MORE_INPUT;
        }
        if (bits != 0) {
          return BROTLI_FAILURE(BROTLI_DECODER_ERROR_FORMAT_RESERVED);
        }
        s->substate_metablock_header = BROTLI_STATE_METABLOCK_HEADER_BYTES;
      /* Fall through. */

      case BROTLI_STATE_METABLOCK_HEADER_BYTES:
        if (!BrotliSafeReadBits(br, 2, &bits)) {
          return BROTLI_DECODER_NEEDS_MORE_INPUT;
        }
        if (bits == 0) {
          s->substate_metablock_header = BROTLI_STATE_METABLOCK_HEADER_NONE;
          return BROTLI_DECODER_SUCCESS;
        }
        s->size_nibbles = (uint8_t)bits;
        s->substate_metablock_header = BROTLI_STATE_METABLOCK_HEADER_METADATA;
      /* Fall through. */

      case BROTLI_STATE_METABLOCK_HEADER_METADATA:
        i = s->loop_counter;
        for (; i < (int)s->size_nibbles; ++i) {
          if (!BrotliSafeReadBits(br, 8, &bits)) {
            s->loop_counter = i;
            return BROTLI_DECODER_NEEDS_MORE_INPUT;
          }
          if (i + 1 == s->size_nibbles && s->size_nibbles > 1 && bits == 0) {
            return BROTLI_FAILURE(
                BROTLI_DECODER_ERROR_FORMAT_EXUBERANT_META_NIBBLE);
          }
          s->meta_block_remaining_len |= (int)(bits << (i * 8));
        }
        ++s->meta_block_remaining_len;
        s->substate_metablock_header = BROTLI_STATE_METABLOCK_HEADER_NONE;
        return BROTLI_DECODER_SUCCESS;

      default:
        return
            BROTLI_FAILURE(BROTLI_DECODER_ERROR_UNREACHABLE);
    }
  }
}

/* Decodes the Huffman code.
   This method doesn't read data from the bit reader, BUT drops the amount of
   bits that correspond to the decoded symbol.
   bits MUST contain at least 15 (BROTLI_HUFFMAN_MAX_CODE_LENGTH) valid bits. */
static BROTLI_INLINE uint32_t DecodeSymbol(uint32_t bits,
                                           const HuffmanCode* table,
                                           BrotliBitReader* br) {
  table += bits & HUFFMAN_TABLE_MASK;
  if (table->bits > HUFFMAN_TABLE_BITS) {
    uint32_t nbits = table->bits - HUFFMAN_TABLE_BITS;
    BrotliDropBits(br, HUFFMAN_TABLE_BITS);
    table += table->value;
    table += (bits >> HUFFMAN_TABLE_BITS) & BitMask(nbits);
  }
  BrotliDropBits(br, table->bits);
  return table->value;
}

/* Reads and decodes the next Huffman code from bit-stream.
   This method peeks 16 bits of input and drops 0 - 15 of them. */
static BROTLI_INLINE uint32_t ReadSymbol(const HuffmanCode* table,
                                         BrotliBitReader* br) {
  return DecodeSymbol(BrotliGet16BitsUnmasked(br), table, br);
}

/* Same as DecodeSymbol, but it is known that there is less than 15 bits of
   input are currently available. */
static BROTLI_NOINLINE BROTLI_BOOL SafeDecodeSymbol(
    const HuffmanCode* table, BrotliBitReader* br, uint32_t* result) {
  uint32_t val;
  uint32_t available_bits = BrotliGetAvailableBits(br);
  if (available_bits == 0) {
    if (table->bits == 0) {
      *result = table->value;
      return BROTLI_TRUE;
    }
    return BROTLI_FALSE;  /* No valid bits at all. */
  }
  val = (uint32_t)BrotliGetBitsUnmasked(br);
  table += val & HUFFMAN_TABLE_MASK;
  if (table->bits <= HUFFMAN_TABLE_BITS) {
    if (table->bits <= available_bits) {
      BrotliDropBits(br, table->bits);
      *result = table->value;
      return BROTLI_TRUE;
    } else {
      return BROTLI_FALSE;  /* Not enough bits for the first level. */
    }
  }
  if (available_bits <= HUFFMAN_TABLE_BITS) {
    return BROTLI_FALSE;  /* Not enough bits to move to the second level. */
  }

  /* Speculatively drop HUFFMAN_TABLE_BITS. */
  val = (val & BitMask(table->bits)) >> HUFFMAN_TABLE_BITS;
  available_bits -= HUFFMAN_TABLE_BITS;
  table += table->value + val;
  if (available_bits < table->bits) {
    return BROTLI_FALSE;  /* Not enough bits for the second level. */
  }

  BrotliDropBits(br, HUFFMAN_TABLE_BITS + table->bits);
  *result = table->value;
  return BROTLI_TRUE;
}

static BROTLI_INLINE BROTLI_BOOL SafeReadSymbol(
    const HuffmanCode* table, BrotliBitReader* br, uint32_t* result) {
  uint32_t val;
  if (BROTLI_PREDICT_TRUE(BrotliSafeGetBits(br, 15, &val))) {
    *result = DecodeSymbol(val, table, br);
    return BROTLI_TRUE;
  }
  return SafeDecodeSymbol(table, br, result);
}

/* Makes a look-up in first level Huffman table. Peeks 8 bits. */
static BROTLI_INLINE void PreloadSymbol(int safe,
                                        const HuffmanCode* table,
                                        BrotliBitReader* br,
                                        uint32_t* bits,
                                        uint32_t* value) {
  if (safe) {
    return;
  }
  table += BrotliGetBits(br, HUFFMAN_TABLE_BITS);
  *bits = table->bits;
  *value = table->value;
}

/* Decodes the next Huffman code using data prepared by PreloadSymbol.
   Reads 0 - 15 bits. Also peeks 8 following bits. */
static BROTLI_INLINE uint32_t ReadPreloadedSymbol(const HuffmanCode* table,
                                                  BrotliBitReader* br,
                                                  uint32_t* bits,
                                                  uint32_t* value) {
  uint32_t result = *value;
  if (BROTLI_PREDICT_FALSE(*bits > HUFFMAN_TABLE_BITS)) {
    uint32_t val = BrotliGet16BitsUnmasked(br);
    const HuffmanCode* ext = table + (val & HUFFMAN_TABLE_MASK) + *value;
    uint32_t mask = BitMask((*bits - HUFFMAN_TABLE_BITS));
    BrotliDropBits(br, HUFFMAN_TABLE_BITS);
    ext += (val >> HUFFMAN_TABLE_BITS) & mask;
    BrotliDropBits(br, ext->bits);
    result = ext->value;
  } else {
    BrotliDropBits(br, *bits);
  }
  PreloadSymbol(0, table, br, bits, value);
  return result;
}

static BROTLI_INLINE uint32_t Log2Floor(uint32_t x) {
  uint32_t result = 0;
  while (x) {
    x >>= 1;
    ++result;
  }
  return result;
}

/* Reads (s->symbol + 1) symbols.
   Totally 1..4 symbols are read, 1..11 bits each.
   The list of symbols MUST NOT contain duplicates. */
static BrotliDecoderErrorCode ReadSimpleHuffmanSymbols(
    uint32_t alphabet_size, uint32_t max_symbol, BrotliDecoderState* s) {
  /* max_bits == 1..11; symbol == 0..3; 1..44 bits will be read. */
  BrotliBitReader* br = &s->br;
  uint32_t max_bits = Log2Floor(alphabet_size - 1);
  uint32_t i = s->sub_loop_counter;
  uint32_t num_symbols = s->symbol;
  while (i <= num_symbols) {
    uint32_t v;
    if (BROTLI_PREDICT_FALSE(!BrotliSafeReadBits(br, max_bits, &v))) {
      s->sub_loop_counter = i;
      s->substate_huffman = BROTLI_STATE_HUFFMAN_SIMPLE_READ;
      return BROTLI_DECODER_NEEDS_MORE_INPUT;
    }
    if (v >= max_symbol) {
      return
          BROTLI_FAILURE(BROTLI_DECODER_ERROR_FORMAT_SIMPLE_HUFFMAN_ALPHABET);
    }
    s->symbols_lists_array[i] = (uint16_t)v;
    BROTLI_LOG_UINT(s->symbols_lists_array[i]);
    ++i;
  }

  for (i = 0; i < num_symbols; ++i) {
    uint32_t k = i + 1;
    for (; k <= num_symbols; ++k) {
      if (s->symbols_lists_array[i] == s->symbols_lists_array[k]) {
        return BROTLI_FAILURE(BROTLI_DECODER_ERROR_FORMAT_SIMPLE_HUFFMAN_SAME);
      }
    }
  }

  return BROTLI_DECODER_SUCCESS;
}

/* Process single decoded symbol code length:
    A) reset the repeat variable
    B) remember code length (if it is not 0)
    C) extend corresponding index-chain
    D) reduce the Huffman space
    E) update the histogram */
static BROTLI_INLINE void ProcessSingleCodeLength(uint32_t code_len,
    uint32_t* symbol, uint32_t* repeat, uint32_t* space,
    uint32_t* prev_code_len, uint16_t* symbol_lists,
    uint16_t* code_length_histo, int* next_symbol) {
  *repeat = 0;
  if (code_len != 0) {  /* code_len == 1..15 */
    symbol_lists[next_symbol[code_len]] = (uint16_t)(*symbol);
    next_symbol[code_len] = (int)(*symbol);
    *prev_code_len = code_len;
    *space -= 32768U >> code_len;
    code_length_histo[code_len]++;
    BROTLI_LOG(("[ReadHuffmanCode] code_length[%d] = %d\n",
        (int)*symbol, (int)code_len));
  }
  (*symbol)++;
}

/* Process repeated symbol code length.
    A) Check if it is the extension of previous repeat sequence; if the decoded
       value is not BROTLI_REPEAT_PREVIOUS_CODE_LENGTH, then it is a new
       symbol-skip
    B) Update repeat variable
    C) Check if operation is feasible (fits alphabet)
    D) For each symbol do the same operations as in ProcessSingleCodeLength

   PRECONDITION: code_len == BROTLI_REPEAT_PREVIOUS_CODE_LENGTH or
                 code_len == BROTLI_REPEAT_ZERO_CODE_LENGTH */
static BROTLI_INLINE void ProcessRepeatedCodeLength(uint32_t code_len,
    uint32_t repeat_delta, uint32_t alphabet_size, uint32_t* symbol,
    uint32_t* repeat, uint32_t* space, uint32_t* prev_code_len,
    uint32_t* repeat_code_len, uint16_t* symbol_lists,
    uint16_t* code_length_histo, int* next_symbol) {
  uint32_t old_repeat;
  uint32_t extra_bits = 3;  /* for BROTLI_REPEAT_ZERO_CODE_LENGTH */
  uint32_t new_len = 0;  /* for BROTLI_REPEAT_ZERO_CODE_LENGTH */
  if (code_len == BROTLI_REPEAT_PREVIOUS_CODE_LENGTH) {
    new_len = *prev_code_len;
    extra_bits = 2;
  }
  if (*repeat_code_len != new_len) {
    *repeat = 0;
    *repeat_code_len = new_len;
  }
  old_repeat = *repeat;
  if (*repeat > 0) {
    *repeat -= 2;
    *repeat <<= extra_bits;
  }
  *repeat += repeat_delta + 3U;
  repeat_delta = *repeat - old_repeat;
  if (*symbol + repeat_delta > alphabet_size) {
    BROTLI_DUMP();
    *symbol = alphabet_size;
    *space = 0xFFFFF;
    return;
  }
  BROTLI_LOG(("[ReadHuffmanCode] code_length[%d..%d] = %d\n",
      (int)*symbol, (int)(*symbol + repeat_delta - 1), (int)*repeat_code_len));
  if (*repeat_code_len != 0) {
    unsigned last = *symbol + repeat_delta;
    int next = next_symbol[*repeat_code_len];
    do {
      symbol_lists[next] = (uint16_t)*symbol;
      next = (int)*symbol;
    } while (++(*symbol) != last);
    next_symbol[*repeat_code_len] = next;
    *space -= repeat_delta << (15 - *repeat_code_len);
    code_length_histo[*repeat_code_len] =
        (uint16_t)(code_length_histo[*repeat_code_len] + repeat_delta);
  } else {
    *symbol += repeat_delta;
  }
}

/* Reads and decodes symbol codelengths. */
static BrotliDecoderErrorCode ReadSymbolCodeLengths(
    uint32_t alphabet_size, BrotliDecoderState* s) {
  BrotliBitReader* br = &s->br;
  uint32_t symbol = s->symbol;
  uint32_t repeat = s->repeat;
  uint32_t space = s->space;
  uint32_t prev_code_len = s->prev_code_len;
  uint32_t repeat_code_len = s->repeat_code_len;
  uint16_t* symbol_lists = s->symbol_lists;
  uint16_t* code_length_histo = s->code_length_histo;
  int* next_symbol = s->next_symbol;
  if (!BrotliWarmupBitReader(br)) {
    return BROTLI_DECODER_NEEDS_MORE_INPUT;
  }
  while (symbol < alphabet_size && space > 0) {
    const HuffmanCode* p = s->table;
    uint32_t code_len;
    if (!BrotliCheckInputAmount(br, BROTLI_SHORT_FILL_BIT_WINDOW_READ)) {
      s->symbol = symbol;
      s->repeat = repeat;
      s->prev_code_len = prev_code_len;
      s->repeat_code_len = repeat_code_len;
      s->space = space;
      return BROTLI_DECODER_NEEDS_MORE_INPUT;
    }
    BrotliFillBitWindow16(br);
    p += BrotliGetBitsUnmasked(br) &
        BitMask(BROTLI_HUFFMAN_MAX_CODE_LENGTH_CODE_LENGTH);
    BrotliDropBits(br, p->bits);  /* Use 1..5 bits. */
    code_len = p->value;  /* code_len == 0..17 */
    if (code_len < BROTLI_REPEAT_PREVIOUS_CODE_LENGTH) {
      ProcessSingleCodeLength(code_len, &symbol, &repeat, &space,
          &prev_code_len, symbol_lists, code_length_histo, next_symbol);
    } else {  /* code_len == 16..17, extra_bits == 2..3 */
      uint32_t extra_bits =
          (code_len == BROTLI_REPEAT_PREVIOUS_CODE_LENGTH) ? 2 : 3;
      uint32_t repeat_delta =
          (uint32_t)BrotliGetBitsUnmasked(br) & BitMask(extra_bits);
      BrotliDropBits(br, extra_bits);
      ProcessRepeatedCodeLength(code_len, repeat_delta, alphabet_size,
          &symbol, &repeat, &space, &prev_code_len, &repeat_code_len,
          symbol_lists, code_length_histo, next_symbol);
    }
  }
  s->space = space;
  return BROTLI_DECODER_SUCCESS;
}

static BrotliDecoderErrorCode SafeReadSymbolCodeLengths(
    uint32_t alphabet_size, BrotliDecoderState* s) {
  BrotliBitReader* br = &s->br;
  BROTLI_BOOL get_byte = BROTLI_FALSE;
  while (s->symbol < alphabet_size && s->space > 0) {
    const HuffmanCode* p = s->table;
    uint32_t code_len;
    uint32_t available_bits;
    uint32_t bits = 0;
    if (get_byte && !BrotliPullByte(br)) return BROTLI_DECODER_NEEDS_MORE_INPUT;
    get_byte = BROTLI_FALSE;
    available_bits = BrotliGetAvailableBits(br);
    if (available_bits != 0) {
      bits = (uint32_t)BrotliGetBitsUnmasked(br);
    }
    p += bits & BitMask(BROTLI_HUFFMAN_MAX_CODE_LENGTH_CODE_LENGTH);
    if (p->bits > available_bits) {
      get_byte = BROTLI_TRUE;
      continue;
    }
    code_len = p->value;  /* code_len == 0..17 */
    if (code_len < BROTLI_REPEAT_PREVIOUS_CODE_LENGTH) {
      BrotliDropBits(br, p->bits);
      ProcessSingleCodeLength(code_len, &s->symbol, &s->repeat, &s->space,
          &s->prev_code_len, s->symbol_lists, s->code_length_histo,
          s->next_symbol);
    } else {  /* code_len == 16..17, extra_bits == 2..3 */
      uint32_t extra_bits = code_len - 14U;
      uint32_t repeat_delta = (bits >> p->bits) & BitMask(extra_bits);
      if (available_bits < p->bits + extra_bits) {
        get_byte = BROTLI_TRUE;
        continue;
      }
      BrotliDropBits(br, p->bits + extra_bits);
      ProcessRepeatedCodeLength(code_len, repeat_delta, alphabet_size,
          &s->symbol, &s->repeat, &s->space, &s->prev_code_len,
          &s->repeat_code_len, s->symbol_lists, s->code_length_histo,
          s->next_symbol);
    }
  }
  return BROTLI_DECODER_SUCCESS;
}

/* Reads and decodes 15..18 codes using static prefix code.
   Each code is 2..4 bits long. In total 30..72 bits are used. */
static BrotliDecoderErrorCode ReadCodeLengthCodeLengths(BrotliDecoderState* s) {
  BrotliBitReader* br = &s->br;
  uint32_t num_codes = s->repeat;
  unsigned space = s->space;
  uint32_t i = s->sub_loop_counter;
  for (; i < BROTLI_CODE_LENGTH_CODES; ++i) {
    const uint8_t code_len_idx = kCodeLengthCodeOrder[i];
    uint32_t ix;
    uint32_t v;
    if (BROTLI_PREDICT_FALSE(!BrotliSafeGetBits(br, 4, &ix))) {
      uint32_t available_bits = BrotliGetAvailableBits(br);
      if (available_bits != 0) {
        ix = BrotliGetBitsUnmasked(br) & 0xF;
      } else {
        ix = 0;
      }
      if (kCodeLengthPrefixLength[ix] > available_bits) {
        s->sub_loop_counter = i;
        s->repeat = num_codes;
        s->space = space;
        s->substate_huffman = BROTLI_STATE_HUFFMAN_COMPLEX;
        return BROTLI_DECODER_NEEDS_MORE_INPUT;
      }
    }
    v = kCodeLengthPrefixValue[ix];
    BrotliDropBits(br, kCodeLengthPrefixLength[ix]);
    s->code_length_code_lengths[code_len_idx] = (uint8_t)v;
    BROTLI_LOG_ARRAY_INDEX(s->code_length_code_lengths, code_len_idx);
    if (v != 0) {
      space = space - (32U >> v);
      ++num_codes;
      ++s->code_length_histo[v];
      if (space - 1U >= 32U) {
        /* space is 0 or wrapped around. */
        break;
      }
    }
  }
  if (!(num_codes == 1 || space == 0)) {
    return BROTLI_FAILURE(BROTLI_DECODER_ERROR_FORMAT_CL_SPACE);
  }
  return BROTLI_DECODER_SUCCESS;
}

/* Decodes the Huffman tables.
   There are 2 scenarios:
    A) Huffman code contains only few symbols (1..4). Those symbols are read
       directly; their code lengths are defined by the number of symbols.
       For this scenario 4 - 49 bits will be read.

    B) 2-phase decoding:
    B.1) Small Huffman table is decoded; it is specified with code lengths
         encoded with predefined entropy code. 32 - 74 bits are used.
    B.2) Decoded table is used to decode code lengths of symbols in resulting
         Huffman table. In worst case 3520 bits are read. */
static BrotliDecoderErrorCode ReadHuffmanCode(uint32_t alphabet_size,
                                              uint32_t max_symbol,
                                              HuffmanCode* table,
                                              uint32_t* opt_table_size,
                                              BrotliDecoderState* s) {
  BrotliBitReader* br = &s->br;
  /* Unnecessary masking, but might be good for safety. */
  alphabet_size &= 0x7FF;
  /* State machine. */
  for (;;) {
    switch (s->substate_huffman) {
      case BROTLI_STATE_HUFFMAN_NONE:
        if (!BrotliSafeReadBits(br, 2, &s->sub_loop_counter)) {
          return BROTLI_DECODER_NEEDS_MORE_INPUT;
        }
        BROTLI_LOG_UINT(s->sub_loop_counter);
        /* The value is used as follows:
           1 for simple code;
           0 for no skipping, 2 skips 2 code lengths, 3 skips 3 code lengths */
        if (s->sub_loop_counter != 1) {
          s->space = 32;
          s->repeat = 0;  /* num_codes */
          memset(&s->code_length_histo[0], 0, sizeof(s->code_length_histo[0]) *
              (BROTLI_HUFFMAN_MAX_CODE_LENGTH_CODE_LENGTH + 1));
          memset(&s->code_length_code_lengths[0], 0,
              sizeof(s->code_length_code_lengths));
          s->substate_huffman = BROTLI_STATE_HUFFMAN_COMPLEX;
          continue;
        }
      /* Fall through. */

      case BROTLI_STATE_HUFFMAN_SIMPLE_SIZE:
        /* Read symbols, codes & code lengths directly. */
        if (!BrotliSafeReadBits(br, 2, &s->symbol)) {  /* num_symbols */
          s->substate_huffman = BROTLI_STATE_HUFFMAN_SIMPLE_SIZE;
          return BROTLI_DECODER_NEEDS_MORE_INPUT;
        }
        s->sub_loop_counter = 0;
      /* Fall through. */

      case BROTLI_STATE_HUFFMAN_SIMPLE_READ: {
        BrotliDecoderErrorCode result =
            ReadSimpleHuffmanSymbols(alphabet_size, max_symbol, s);
        if (result != BROTLI_DECODER_SUCCESS) {
          return result;
        }
      }
      /* Fall through. */

      case BROTLI_STATE_HUFFMAN_SIMPLE_BUILD: {
        uint32_t table_size;
        if (s->symbol == 3) {
          uint32_t bits;
          if (!BrotliSafeReadBits(br, 1, &bits)) {
            s->substate_huffman = BROTLI_STATE_HUFFMAN_SIMPLE_BUILD;
            return BROTLI_DECODER_NEEDS_MORE_INPUT;
          }
          s->symbol += bits;
        }
        BROTLI_LOG_UINT(s->symbol);
        table_size = BrotliBuildSimpleHuffmanTable(
            table, HUFFMAN_TABLE_BITS, s->symbols_lists_array, s->symbol);
        if (opt_table_size) {
          *opt_table_size = table_size;
        }
        s->substate_huffman = BROTLI_STATE_HUFFMAN_NONE;
        return BROTLI_DECODER_SUCCESS;
      }

      /* Decode Huffman-coded code lengths. */
      case BROTLI_STATE_HUFFMAN_COMPLEX: {
        uint32_t i;
        BrotliDecoderErrorCode result = ReadCodeLengthCodeLengths(s);
        if (result != BROTLI_DECODER_SUCCESS) {
          return result;
        }
        BrotliBuildCodeLengthsHuffmanTable(s->table,
                                           s->code_length_code_lengths,
                                           s->code_length_histo);
        memset(&s->code_length_histo[0], 0, sizeof(s->code_length_histo));
        for (i = 0; i <= BROTLI_HUFFMAN_MAX_CODE_LENGTH; ++i) {
          s->next_symbol[i] = (int)i - (BROTLI_HUFFMAN_MAX_CODE_LENGTH + 1);
          s->symbol_lists[s->next_symbol[i]] = 0xFFFF;
        }

        s->symbol = 0;
        s->prev_code_len = BROTLI_INITIAL_REPEATED_CODE_LENGTH;
        s->repeat = 0;
        s->repeat_code_len = 0;
        s->space = 32768;
        s->substate_huffman = BROTLI_STATE_HUFFMAN_LENGTH_SYMBOLS;
      }
      /* Fall through. */

      case BROTLI_STATE_HUFFMAN_LENGTH_SYMBOLS: {
        uint32_t table_size;
        BrotliDecoderErrorCode result = ReadSymbolCodeLengths(max_symbol, s);
        if (result == BROTLI_DECODER_NEEDS_MORE_INPUT) {
          result = SafeReadSymbolCodeLengths(max_symbol, s);
        }
        if (result != BROTLI_DECODER_SUCCESS) {
          return result;
        }

        if (s->space != 0) {
          BROTLI_LOG(("[ReadHuffmanCode] space = %d\n", (int)s->space));
          return BROTLI_FAILURE(BROTLI_DECODER_ERROR_FORMAT_HUFFMAN_SPACE);
        }
        table_size = BrotliBuildHuffmanTable(
            table, HUFFMAN_TABLE_BITS, s->symbol_lists, s->code_length_histo);
        if (opt_table_size) {
          *opt_table_size = table_size;
        }
        s->substate_huffman = BROTLI_STATE_HUFFMAN_NONE;
        return BROTLI_DECODER_SUCCESS;
      }

      default:
        return
            BROTLI_FAILURE(BROTLI_DECODER_ERROR_UNREACHABLE);
    }
  }
}

/* Decodes a block length by reading 3..39 bits. */
static BROTLI_INLINE uint32_t ReadBlockLength(const HuffmanCode* table,
                                              BrotliBitReader* br) {
  uint32_t code;
  uint32_t nbits;
  code = ReadSymbol(table, br);
  nbits = kBlockLengthPrefixCode[code].nbits;  /* nbits == 2..24 */
  return kBlockLengthPrefixCode[code].offset + BrotliReadBits(br, nbits);
}

/* WARNING: if state is not BROTLI_STATE_READ_BLOCK_LENGTH_NONE, then
   reading can't be continued with ReadBlockLength. */
static BROTLI_INLINE BROTLI_BOOL SafeReadBlockLength(
    BrotliDecoderState* s, uint32_t* result, const HuffmanCode* table,
    BrotliBitReader* br) {
  uint32_t index;
  if (s->substate_read_block_length == BROTLI_STATE_READ_BLOCK_LENGTH_NONE) {
    if (!SafeReadSymbol(table, br, &index)) {
      return BROTLI_FALSE;
    }
  } else {
    index = s->block_length_index;
  }
  {
    uint32_t bits;
    uint32_t nbits = kBlockLengthPrefixCode[index].nbits;  /* nbits == 2..24 */
    if (!BrotliSafeReadBits(br, nbits, &bits)) {
      s->block_length_index = index;
      s->substate_read_block_length = BROTLI_STATE_READ_BLOCK_LENGTH_SUFFIX;
      return BROTLI_FALSE;
    }
    *result = kBlockLengthPrefixCode[index].offset + bits;
    s->substate_read_block_length = BROTLI_STATE_READ_BLOCK_LENGTH_NONE;
    return BROTLI_TRUE;
  }
}

/* Transform:
    1) initialize list L with values 0, 1,... 255
    2) For each input element X:
    2.1) let Y = L[X]
    2.2) remove X-th element from L
    2.3) prepend Y to L
    2.4) append Y to output

   In most cases max(Y) <= 7, so most of L remains intact.
   To reduce the cost of initialization, we reuse L, remember the upper bound
   of Y values, and reinitialize only first elements in L.

   Most of input values are 0 and 1. To reduce number of branches, we replace
   inner for loop with do-while. */
static BROTLI_NOINLINE void InverseMoveToFrontTransform(
    uint8_t* v, uint32_t v_len, BrotliDecoderState* state) {
  /* Reinitialize elements that could have been changed. */
  uint32_t i = 1;
  uint32_t upper_bound = state->mtf_upper_bound;
  uint32_t* mtf = &state->mtf[1];  /* Make mtf[-1] addressable. */
  uint8_t* mtf_u8 = (uint8_t*)mtf;
  /* Load endian-aware constant. */
  const uint8_t b0123[4] = {0, 1, 2, 3};
  uint32_t pattern;
  memcpy(&pattern, &b0123, 4);

  /* Initialize list using 4 consequent values pattern. */
  mtf[0] = pattern;
  do {
    pattern += 0x04040404;  /* Advance all 4 values by 4. */
    mtf[i] = pattern;
    i++;
  } while (i <= upper_bound);

  /* Transform the input. */
  upper_bound = 0;
  for (i = 0; i < v_len; ++i) {
    int index = v[i];
    uint8_t value = mtf_u8[index];
    upper_bound |= v[i];
    v[i] = value;
    mtf_u8[-1] = value;
    do {
      index--;
      mtf_u8[index + 1] = mtf_u8[index];
    } while (index >= 0);
  }
  /* Remember amount of elements to be reinitialized. */
  state->mtf_upper_bound = upper_bound >> 2;
}

/* Decodes a series of Huffman table using ReadHuffmanCode function. */
static BrotliDecoderErrorCode HuffmanTreeGroupDecode(
    HuffmanTreeGroup* group, BrotliDecoderState* s) {
  if (s->substate_tree_group != BROTLI_STATE_TREE_GROUP_LOOP) {
    s->next = group->codes;
    s->htree_index = 0;
    s->substate_tree_group = BROTLI_STATE_TREE_GROUP_LOOP;
  }
  while (s->htree_index < group->num_htrees) {
    uint32_t table_size;
    BrotliDecoderErrorCode result =
        ReadHuffmanCode(group->alphabet_size, group->max_symbol,
                        s->next, &table_size, s);
    if (result != BROTLI_DECODER_SUCCESS) return result;
    group->htrees[s->htree_index] = s->next;
    s->next += table_size;
    ++s->htree_index;
  }
  s->substate_tree_group = BROTLI_STATE_TREE_GROUP_NONE;
  return BROTLI_DECODER_SUCCESS;
}

/* Decodes a context map.
   Decoding is done in 4 phases:
    1) Read auxiliary information (6..16 bits) and allocate memory.
       In case of trivial context map, decoding is finished at this phase.
    2) Decode Huffman table using ReadHuffmanCode function.
       This table will be used for reading context map items.
    3) Read context map items; "0" values could be run-length encoded.
    4) Optionally, apply InverseMoveToFront transform to the resulting map. */
static BrotliDecoderErrorCode DecodeContextMap(uint32_t context_map_size,
                                               uint32_t* num_htrees,
                                               uint8_t** context_map_arg,
                                               BrotliDecoderState* s) {
  BrotliBitReader* br = &s->br;
  BrotliDecoderErrorCode result = BROTLI_DECODER_SUCCESS;

  switch ((int)s->substate_context_map) {
    case BROTLI_STATE_CONTEXT_MAP_NONE:
      result = DecodeVarLenUint8(s, br, num_htrees);
      if (result != BROTLI_DECODER_SUCCESS) {
        return result;
      }
      (*num_htrees)++;
      s->context_index = 0;
      BROTLI_LOG_UINT(context_map_size);
      BROTLI_LOG_UINT(*num_htrees);
      *context_map_arg =
          (uint8_t*)BROTLI_DECODER_ALLOC(s, (size_t)context_map_size);
      if (*context_map_arg == 0) {
        return BROTLI_FAILURE(BROTLI_DECODER_ERROR_ALLOC_CONTEXT_MAP);
      }
      if (*num_htrees <= 1) {
        memset(*context_map_arg, 0, (size_t)context_map_size);
        return BROTLI_DECODER_SUCCESS;
      }
      s->substate_context_map = BROTLI_STATE_CONTEXT_MAP_READ_PREFIX;
    /* Fall through. */

    case BROTLI_STATE_CONTEXT_MAP_READ_PREFIX: {
      uint32_t bits;
      /* In next stage ReadHuffmanCode uses at least 4 bits, so it is safe
         to peek 4 bits ahead. */
      if (!BrotliSafeGetBits(br, 5, &bits)) {
        return BROTLI_DECODER_NEEDS_MORE_INPUT;
      }
      if ((bits & 1) != 0) { /* Use RLE for zeros. */
        s->max_run_length_prefix = (bits >> 1) + 1;
        BrotliDropBits(br, 5);
      } else {
        s->max_run_length_prefix = 0;
        BrotliDropBits(br, 1);
      }
      BROTLI_LOG_UINT(s->max_run_length_prefix);
      s->substate_context_map = BROTLI_STATE_CONTEXT_MAP_HUFFMAN;
    }
    /* Fall through. */

    case BROTLI_STATE_CONTEXT_MAP_HUFFMAN: {
      uint32_t alphabet_size = *num_htrees + s->max_run_length_prefix;
      result = ReadHuffmanCode(alphabet_size, alphabet_size,
                               s->context_map_table, NULL, s);
      if (result != BROTLI_DECODER_SUCCESS) return result;
      s->code = 0xFFFF;
      s->substate_context_map = BROTLI_STATE_CONTEXT_MAP_DECODE;
    }
    /* Fall through. */

    case BROTLI_STATE_CONTEXT_MAP_DECODE: {
      uint32_t context_index = s->context_index;
      uint32_t max_run_length_prefix = s->max_run_length_prefix;
      uint8_t* context_map = *context_map_arg;
      uint32_t code = s->code;
      BROTLI_BOOL skip_preamble = (code != 0xFFFF);
      while (context_index < context_map_size || skip_preamble) {
        if (!skip_preamble) {
          if (!SafeReadSymbol(s->context_map_table, br, &code)) {
            s->code = 0xFFFF;
            s->context_index = context_index;
            return BROTLI_DECODER_NEEDS_MORE_INPUT;
          }
          BROTLI_LOG_UINT(code);

          if (code == 0) {
            context_map[context_index++] = 0;
            continue;
          }
          if (code > max_run_length_prefix) {
            context_map[context_index++] =
                (uint8_t)(code - max_run_length_prefix);
            continue;
          }
        } else {
          skip_preamble = BROTLI_FALSE;
        }
        /* RLE sub-stage. */
        {
          uint32_t reps;
          if (!BrotliSafeReadBits(br, code, &reps)) {
            s->code = code;
            s->context_index = context_index;
            return BROTLI_DECODER_NEEDS_MORE_INPUT;
          }
          reps += 1U << code;
          BROTLI_LOG_UINT(reps);
          if (context_index + reps > context_map_size) {
            return
                BROTLI_FAILURE(BROTLI_DECODER_ERROR_FORMAT_CONTEXT_MAP_REPEAT);
          }
          do {
            context_map[context_index++] = 0;
          } while (--reps);
        }
      }
    }
    /* Fall through. */

    case BROTLI_STATE_CONTEXT_MAP_TRANSFORM: {
      uint32_t bits;
      if (!BrotliSafeReadBits(br, 1, &bits)) {
        s->substate_context_map = BROTLI_STATE_CONTEXT_MAP_TRANSFORM;
        return BROTLI_DECODER_NEEDS_MORE_INPUT;
      }
      if (bits != 0) {
        InverseMoveToFrontTransform(*context_map_arg, context_map_size, s);
      }
      s->substate_context_map = BROTLI_STATE_CONTEXT_MAP_NONE;
      return BROTLI_DECODER_SUCCESS;
    }

    default:
      return
          BROTLI_FAILURE(BROTLI_DECODER_ERROR_UNREACHABLE);
  }
}

/* Decodes a command or literal and updates block type ring-buffer.
   Reads 3..54 bits. */
static BROTLI_INLINE BROTLI_BOOL DecodeBlockTypeAndLength(
    int safe, BrotliDecoderState* s, int tree_type) {
  uint32_t max_block_type = s->num_block_types[tree_type];
  const HuffmanCode* type_tree = &s->block_type_trees[
      tree_type * BROTLI_HUFFMAN_MAX_SIZE_258];
  const HuffmanCode* len_tree = &s->block_len_trees[
      tree_type * BROTLI_HUFFMAN_MAX_SIZE_26];
  BrotliBitReader* br = &s->br;
  uint32_t* ringbuffer = &s->block_type_rb[tree_type * 2];
  uint32_t block_type;
  if (max_block_type <= 1) {
    return BROTLI_FALSE;
  }

  /* Read 0..15 + 3..39 bits. */
  if (!safe) {
    block_type = ReadSymbol(type_tree, br);
    s->block_length[tree_type] = ReadBlockLength(len_tree, br);
  } else {
    BrotliBitReaderState memento;
    BrotliBitReaderSaveState(br, &memento);
    if (!SafeReadSymbol(type_tree, br, &block_type)) return BROTLI_FALSE;
    if (!SafeReadBlockLength(s, &s->block_length[tree_type], len_tree, br)) {
      s->substate_read_block_length = BROTLI_STATE_READ_BLOCK_LENGTH_NONE;
      BrotliBitReaderRestoreState(br, &memento);
      return BROTLI_FALSE;
    }
  }

  if (block_type == 1) {
    block_type = ringbuffer[1] + 1;
  } else if (block_type == 0) {
    block_type = ringbuffer[0];
  } else {
    block_type -= 2;
  }
  if (block_type >= max_block_type) {
    block_type -= max_block_type;
  }
  ringbuffer[0] = ringbuffer[1];
  ringbuffer[1] = block_type;
  return BROTLI_TRUE;
}

static BROTLI_INLINE void DetectTrivialLiteralBlockTypes(
    BrotliDecoderState* s) {
  size_t i;
  for (i = 0; i < 8; ++i) s->trivial_literal_contexts[i] = 0;
  for (i = 0; i < s->num_block_types[0]; i++) {
    size_t offset = i << BROTLI_LITERAL_CONTEXT_BITS;
    size_t error = 0;
    size_t sample = s->context_map[offset];
    size_t j;
    for (j = 0; j < (1u << BROTLI_LITERAL_CONTEXT_BITS);) {
      BROTLI_REPEAT(4, error |= s->context_map[offset + j++] ^ sample;)
    }
    if (error == 0) {
      s->trivial_literal_contexts[i >> 5] |= 1u << (i & 31);
    }
  }
}

static BROTLI_INLINE void PrepareLiteralDecoding(BrotliDecoderState* s) {
  uint8_t context_mode;
  size_t trivial;
  uint32_t block_type = s->block_type_rb[1];
  uint32_t context_offset = block_type << BROTLI_LITERAL_CONTEXT_BITS;
  s->context_map_slice = s->context_map + context_offset;
  trivial = s->trivial_literal_contexts[block_type >> 5];
  s->trivial_literal_context = (trivial >> (block_type & 31)) & 1;
  s->literal_htree = s->literal_hgroup.htrees[s->context_map_slice[0]];
  context_mode = s->context_modes[block_type] & 3;
  s->context_lookup = BROTLI_CONTEXT_LUT(context_mode);
}

/* Decodes the block type and updates the state for literal context.
   Reads 3..54 bits. */
static BROTLI_INLINE BROTLI_BOOL DecodeLiteralBlockSwitchInternal(
    int safe, BrotliDecoderState* s) {
  if (!DecodeBlockTypeAndLength(safe, s, 0)) {
    return BROTLI_FALSE;
  }
  PrepareLiteralDecoding(s);
  return BROTLI_TRUE;
}

static void BROTLI_NOINLINE DecodeLiteralBlockSwitch(BrotliDecoderState* s) {
  DecodeLiteralBlockSwitchInternal(0, s);
}

static BROTLI_BOOL BROTLI_NOINLINE SafeDecodeLiteralBlockSwitch(
    BrotliDecoderState* s) {
  return DecodeLiteralBlockSwitchInternal(1, s);
}

/* Block switch for insert/copy length.
   Reads 3..54 bits. */
static BROTLI_INLINE BROTLI_BOOL DecodeCommandBlockSwitchInternal(
    int safe, BrotliDecoderState* s) {
  if (!DecodeBlockTypeAndLength(safe, s, 1)) {
    return BROTLI_FALSE;
  }
  s->htree_command = s->insert_copy_hgroup.htrees[s->block_type_rb[3]];
  return BROTLI_TRUE;
}

static void BROTLI_NOINLINE DecodeCommandBlockSwitch(BrotliDecoderState* s) {
  DecodeCommandBlockSwitchInternal(0, s);
}

static BROTLI_BOOL BROTLI_NOINLINE SafeDecodeCommandBlockSwitch(
    BrotliDecoderState* s) {
  return DecodeCommandBlockSwitchInternal(1, s);
}

/* Block switch for distance codes.
   Reads 3..54 bits. */
static BROTLI_INLINE BROTLI_BOOL DecodeDistanceBlockSwitchInternal(
    int safe, BrotliDecoderState* s) {
  if (!DecodeBlockTypeAndLength(safe, s, 2)) {
    return BROTLI_FALSE;
  }
  s->dist_context_map_slice = s->dist_context_map +
      (s->block_type_rb[5] << BROTLI_DISTANCE_CONTEXT_BITS);
  s->dist_htree_index = s->dist_context_map_slice[s->distance_context];
  return BROTLI_TRUE;
}

static void BROTLI_NOINLINE DecodeDistanceBlockSwitch(BrotliDecoderState* s) {
  DecodeDistanceBlockSwitchInternal(0, s);
}

static BROTLI_BOOL BROTLI_NOINLINE SafeDecodeDistanceBlockSwitch(
    BrotliDecoderState* s) {
  return DecodeDistanceBlockSwitchInternal(1, s);
}

static size_t UnwrittenBytes(const BrotliDecoderState* s, BROTLI_BOOL wrap) {
  size_t pos = wrap && s->pos > s->ringbuffer_size ?
      (size_t)s->ringbuffer_size : (size_t)(s->pos);
  size_t partial_pos_rb = (s->rb_roundtrips * (size_t)s->ringbuffer_size) + pos;
  return partial_pos_rb - s->partial_pos_out;
}

/* Dumps output.
   Returns BROTLI_DECODER_NEEDS_MORE_OUTPUT only if there is more output to push
   and either ring-buffer is as big as window size, or |force| is true. */
static BrotliDecoderErrorCode BROTLI_NOINLINE WriteRingBuffer(
    BrotliDecoderState* s, size_t* available_out, uint8_t** next_out,
    size_t* total_out, BROTLI_BOOL force) {
  uint8_t* start =
      s->ringbuffer + (s->partial_pos_out & (size_t)s->ringbuffer_mask);
  size_t to_write = UnwrittenBytes(s, BROTLI_TRUE);
  size_t num_written = *available_out;
  if (num_written > to_write) {
    num_written = to_write;
  }
  if (s->meta_block_remaining_len < 0) {
    return BROTLI_FAILURE(BROTLI_DECODER_ERROR_FORMAT_BLOCK_LENGTH_1);
  }
  if (next_out && !*next_out) {
    *next_out = start;
  } else {
    if (next_out) {
      memcpy(*next_out, start, num_written);
      *next_out += num_written;
    }
  }
  *available_out -= num_written;
  BROTLI_LOG_UINT(to_write);
  BROTLI_LOG_UINT(num_written);
  s->partial_pos_out += num_written;
  if (total_out) {
    *total_out = s->partial_pos_out;
  }
  if (num_written < to_write) {
    if (s->ringbuffer_size == (1 << s->window_bits) || force) {
      return BROTLI_DECODER_NEEDS_MORE_OUTPUT;
    } else {
      return BROTLI_DECODER_SUCCESS;
    }
  }
  /* Wrap ring buffer only if it has reached its maximal size. */
  if (s->ringbuffer_size == (1 << s->window_bits) &&
      s->pos >= s->ringbuffer_size) {
    s->pos -= s->ringbuffer_size;
    s->rb_roundtrips++;
    s->should_wrap_ringbuffer = (size_t)s->pos != 0 ? 1 : 0;
  }
  return BROTLI_DECODER_SUCCESS;
}

static void BROTLI_NOINLINE WrapRingBuffer(BrotliDecoderState* s) {
  if (s->should_wrap_ringbuffer) {
    memcpy(s->ringbuffer, s->ringbuffer_end, (size_t)s->pos);
    s->should_wrap_ringbuffer = 0;
  }
}

/* Allocates ring-buffer.

   s->ringbuffer_size MUST be updated by BrotliCalculateRingBufferSize before
   this function is called.

   Last two bytes of ring-buffer are initialized to 0, so context calculation
   could be done uniformly for the first two and all other positions. */
static BROTLI_BOOL BROTLI_NOINLINE BrotliEnsureRingBuffer(
    BrotliDecoderState* s) {
  uint8_t* old_ringbuffer = s->ringbuffer;
  if (s->ringbuffer_size == s->new_ringbuffer_size) {
    return BROTLI_TRUE;
  }

  s->ringbuffer = (uint8_t*)BROTLI_DECODER_ALLOC(s,
      (size_t)(s->new_ringbuffer_size) + kRingBufferWriteAheadSlack);
  if (s->ringbuffer == 0) {
    /* Restore previous value. */
    s->ringbuffer = old_ringbuffer;
    return BROTLI_FALSE;
  }
  s->ringbuffer[s->new_ringbuffer_size - 2] = 0;
  s->ringbuffer[s->new_ringbuffer_size - 1] = 0;

  if (!!old_ringbuffer) {
    memcpy(s->ringbuffer, old_ringbuffer, (size_t)s->pos);
    BROTLI_DECODER_FREE(s, old_ringbuffer);
  }

  s->ringbuffer_size = s->new_ringbuffer_size;
  s->ringbuffer_mask = s->new_ringbuffer_size - 1;
  s->ringbuffer_end = s->ringbuffer + s->ringbuffer_size;

  return BROTLI_TRUE;
}

static BrotliDecoderErrorCode BROTLI_NOINLINE CopyUncompressedBlockToOutput(
    size_t* available_out, uint8_t** next_out, size_t* total_out,
    BrotliDecoderState* s) {
  /* TODO: avoid allocation for single uncompressed block. */
  if (!BrotliEnsureRingBuffer(s)) {
    return BROTLI_FAILURE(BROTLI_DECODER_ERROR_ALLOC_RING_BUFFER_1);
  }

  /* State machine */
  for (;;) {
    switch (s->substate_uncompressed) {
      case BROTLI_STATE_UNCOMPRESSED_NONE: {
        int nbytes = (int)BrotliGetRemainingBytes(&s->br);
        if (nbytes > s->meta_block_remaining_len) {
          nbytes = s->meta_block_remaining_len;
        }
        if (s->pos + nbytes > s->ringbuffer_size) {
          nbytes = s->ringbuffer_size - s->pos;
        }
        /* Copy remaining bytes from s->br.buf_ to ring-buffer. */
        BrotliCopyBytes(&s->ringbuffer[s->pos], &s->br, (size_t)nbytes);
        s->pos += nbytes;
        s->meta_block_remaining_len -= nbytes;
        if (s->pos < 1 << s->window_bits) {
          if (s->meta_block_remaining_len == 0) {
            return BROTLI_DECODER_SUCCESS;
          }
          return BROTLI_DECODER_NEEDS_MORE_INPUT;
        }
        s->substate_uncompressed = BROTLI_STATE_UNCOMPRESSED_WRITE;
      }
      /* Fall through. */

      case BROTLI_STATE_UNCOMPRESSED_WRITE: {
        BrotliDecoderErrorCode result;
        result = WriteRingBuffer(
            s, available_out, next_out, total_out, BROTLI_FALSE);
        if (result != BROTLI_DECODER_SUCCESS) {
          return result;
        }
        if (s->ringbuffer_size == 1 << s->window_bits) {
          s->max_distance = s->max_backward_distance;
        }
        s->substate_uncompressed = BROTLI_STATE_UNCOMPRESSED_NONE;
        break;
      }
    }
  }
  BROTLI_DCHECK(0);  /* Unreachable */
}

/* Calculates the smallest feasible ring buffer.

   If we know the data size is small, do not allocate more ring buffer
   size than needed to reduce memory usage.

   When this method is called, metablock size and flags MUST be decoded. */
static void BROTLI_NOINLINE BrotliCalculateRingBufferSize(
    BrotliDecoderState* s) {
  int window_size = 1 << s->window_bits;
  int new_ringbuffer_size = window_size;
  /* We need at least 2 bytes of ring buffer size to get the last two
     bytes for context from there */
  int min_size = s->ringbuffer_size ? s->ringbuffer_size : 1024;
  int output_size;

  /* If maximum is already reached, no further extension is retired. */
  if (s->ringbuffer_size == window_size) {
    return;
  }

  /* Metadata blocks does not touch ring buffer. */
  if (s->is_metadata) {
    return;
  }

  if (!s->ringbuffer) {
    output_size = 0;
  } else {
    output_size = s->pos;
  }
  output_size += s->meta_block_remaining_len;
  min_size = min_size < output_size ? output_size : min_size;

  if (!!s->canny_ringbuffer_allocation) {
    /* Reduce ring buffer size to save memory when server is unscrupulous.
       In worst case memory usage might be 1.5x bigger for a short period of
       ring buffer reallocation. */
    while ((new_ringbuffer_size >> 1) >= min_size) {
      new_ringbuffer_size >>= 1;
    }
  }

  s->new_ringbuffer_size = new_ringbuffer_size;
}

/* Reads 1..256 2-bit context modes. */
static BrotliDecoderErrorCode ReadContextModes(BrotliDecoderState* s) {
  BrotliBitReader* br = &s->br;
  int i = s->loop_counter;

  while (i < (int)s->num_block_types[0]) {
    uint32_t bits;
    if (!BrotliSafeReadBits(br, 2, &bits)) {
      s->loop_counter = i;
      return BROTLI_DECODER_NEEDS_MORE_INPUT;
    }
    s->context_modes[i] = (uint8_t)bits;
    BROTLI_LOG_ARRAY_INDEX(s->context_modes, i);
    i++;
  }
  return BROTLI_DECODER_SUCCESS;
}

static BROTLI_INLINE void TakeDistanceFromRingBuffer(BrotliDecoderState* s) {
  if (s->distance_code == 0) {
    --s->dist_rb_idx;
    s->distance_code = s->dist_rb[s->dist_rb_idx & 3];
    /* Compensate double distance-ring-buffer roll for dictionary items. */
    s->distance_context = 1;
  } else {
    int distance_code = s->distance_code << 1;
    /* kDistanceShortCodeIndexOffset has 2-bit values from LSB:
        3, 2, 1, 0, 3, 3, 3, 3, 3, 3, 2, 2, 2, 2, 2, 2 */
    const uint32_t kDistanceShortCodeIndexOffset = 0xAAAFFF1B;
    /* kDistanceShortCodeValueOffset has 2-bit values from LSB:
       -0, 0,-0, 0,-1, 1,-2, 2,-3, 3,-1, 1,-2, 2,-3, 3 */
    const uint32_t kDistanceShortCodeValueOffset = 0xFA5FA500;
    int v = (s->dist_rb_idx +
        (int)(kDistanceShortCodeIndexOffset >> distance_code)) & 0x3;
    s->distance_code = s->dist_rb[v];
    v = (int)(kDistanceShortCodeValueOffset >> distance_code) & 0x3;
    if ((distance_code & 0x3) != 0) {
      s->distance_code += v;
    } else {
      s->distance_code -= v;
      if (s->distance_code <= 0) {
        /* A huge distance will cause a BROTLI_FAILURE() soon.
           This is a little faster than failing here. */
        s->distance_code = 0x7FFFFFFF;
      }
    }
  }
}

static BROTLI_INLINE BROTLI_BOOL SafeReadBits(
    BrotliBitReader* const br, uint32_t n_bits, uint32_t* val) {
  if (n_bits != 0) {
    return BrotliSafeReadBits(br, n_bits, val);
  } else {
    *val = 0;
    return BROTLI_TRUE;
  }
}

/* Precondition: s->distance_code < 0. */
static BROTLI_INLINE BROTLI_BOOL ReadDistanceInternal(
    int safe, BrotliDecoderState* s, BrotliBitReader* br) {
  int distval;
  BrotliBitReaderState memento;
  HuffmanCode* distance_tree = s->distance_hgroup.htrees[s->dist_htree_index];
  if (!safe) {
    s->distance_code = (int)ReadSymbol(distance_tree, br);
  } else {
    uint32_t code;
    BrotliBitReaderSaveState(br, &memento);
    if (!SafeReadSymbol(distance_tree, br, &code)) {
      return BROTLI_FALSE;
    }
    s->distance_code = (int)code;
  }
  /* Convert the distance code to the actual distance by possibly
     looking up past distances from the s->ringbuffer. */
  s->distance_context = 0;
  if ((s->distance_code & ~0xF) == 0) {
    TakeDistanceFromRingBuffer(s);
    --s->block_length[2];
    return BROTLI_TRUE;
  }
  distval = s->distance_code - (int)s->num_direct_distance_codes;
  if (distval >= 0) {
    uint32_t nbits;
    int postfix;
    int offset;
    if (!safe && (s->distance_postfix_bits == 0)) {
      nbits = ((uint32_t)distval >> 1) + 1;
      offset = ((2 + (distval & 1)) << nbits) - 4;
      s->distance_code = (int)s->num_direct_distance_codes + offset +
                         (int)BrotliReadBits(br, nbits);
    } else {
      /* This branch also works well when s->distance_postfix_bits == 0. */
      uint32_t bits;
      postfix = distval & s->distance_postfix_mask;
      distval >>= s->distance_postfix_bits;
      nbits = ((uint32_t)distval >> 1) + 1;
      if (safe) {
        if (!SafeReadBits(br, nbits, &bits)) {
          s->distance_code = -1;  /* Restore precondition. */
          BrotliBitReaderRestoreState(br, &memento);
          return BROTLI_FALSE;
        }
      } else {
        bits = BrotliReadBits(br, nbits);
      }
      offset = ((2 + (distval & 1)) << nbits) - 4;
      s->distance_code = (int)s->num_direct_distance_codes +
          ((offset + (int)bits) << s->distance_postfix_bits) + postfix;
    }
  }
  s->distance_code = s->distance_code - BROTLI_NUM_DISTANCE_SHORT_CODES + 1;
  --s->block_length[2];
  return BROTLI_TRUE;
}

static BROTLI_INLINE void ReadDistance(
    BrotliDecoderState* s, BrotliBitReader* br) {
  ReadDistanceInternal(0, s, br);
}

static BROTLI_INLINE BROTLI_BOOL SafeReadDistance(
    BrotliDecoderState* s, BrotliBitReader* br) {
  return ReadDistanceInternal(1, s, br);
}

static BROTLI_INLINE BROTLI_BOOL ReadCommandInternal(
    int safe, BrotliDecoderState* s, BrotliBitReader* br, int* insert_length) {
  uint32_t cmd_code;
  uint32_t insert_len_extra = 0;
  uint32_t copy_length;
  CmdLutElement v;
  BrotliBitReaderState memento;
  if (!safe) {
    cmd_code = ReadSymbol(s->htree_command, br);
  } else {
    BrotliBitReaderSaveState(br, &memento);
    if (!SafeReadSymbol(s->htree_command, br, &cmd_code)) {
      return BROTLI_FALSE;
    }
  }
  v = kCmdLut[cmd_code];
  s->distance_code = v.distance_code;
  s->distance_context = v.context;
  s->dist_htree_index = s->dist_context_map_slice[s->distance_context];
  *insert_length = v.insert_len_offset;
  if (!safe) {
    if (BROTLI_PREDICT_FALSE(v.insert_len_extra_bits != 0)) {
      insert_len_extra = BrotliReadBits(br, v.insert_len_extra_bits);
    }
    copy_length = BrotliReadBits(br, v.copy_len_extra_bits);
  } else {
    if (!SafeReadBits(br, v.insert_len_extra_bits, &insert_len_extra) ||
        !SafeReadBits(br, v.copy_len_extra_bits, &copy_length)) {
      BrotliBitReaderRestoreState(br, &memento);
      return BROTLI_FALSE;
    }
  }
  s->copy_length = (int)copy_length + v.copy_len_offset;
  --s->block_length[1];
  *insert_length += (int)insert_len_extra;
  return BROTLI_TRUE;
}

static BROTLI_INLINE void ReadCommand(
    BrotliDecoderState* s, BrotliBitReader* br, int* insert_length) {
  ReadCommandInternal(0, s, br, insert_length);
}

static BROTLI_INLINE BROTLI_BOOL SafeReadCommand(
    BrotliDecoderState* s, BrotliBitReader* br, int* insert_length) {
  return ReadCommandInternal(1, s, br, insert_length);
}

static BROTLI_INLINE BROTLI_BOOL CheckInputAmount(
    int safe, BrotliBitReader* const br, size_t num) {
  if (safe) {
    return BROTLI_TRUE;
  }
  return BrotliCheckInputAmount(br, num);
}

#define BROTLI_SAFE(METHOD)                       \
  {                                               \
    if (safe) {                                   \
      if (!Safe##METHOD) {                        \
        result = BROTLI_DECODER_NEEDS_MORE_INPUT; \
        goto saveStateAndReturn;                  \
      }                                           \
    } else {                                      \
      METHOD;                                     \
    }                                             \
  }

static BROTLI_INLINE BrotliDecoderErrorCode ProcessCommandsInternal(
    int safe, BrotliDecoderState* s) {
  int pos = s->pos;
  int i = s->loop_counter;
  BrotliDecoderErrorCode result = BROTLI_DECODER_SUCCESS;
  BrotliBitReader* br = &s->br;

  if (!CheckInputAmount(safe, br, 28)) {
    result = BROTLI_DECODER_NEEDS_MORE_INPUT;
    goto saveStateAndReturn;
  }
  if (!safe) {
    BROTLI_UNUSED(BrotliWarmupBitReader(br));
  }

  /* Jump into state machine. */
  if (s->state == BROTLI_STATE_COMMAND_BEGIN) {
    goto CommandBegin;
  } else if (s->state == BROTLI_STATE_COMMAND_INNER) {
    goto CommandInner;
  } else if (s->state == BROTLI_STATE_COMMAND_POST_DECODE_LITERALS) {
    goto CommandPostDecodeLiterals;
  } else if (s->state == BROTLI_STATE_COMMAND_POST_WRAP_COPY) {
    goto CommandPostWrapCopy;
  } else {
    return BROTLI_FAILURE(BROTLI_DECODER_ERROR_UNREACHABLE);
  }

CommandBegin:
  if (safe) {
    s->state = BROTLI_STATE_COMMAND_BEGIN;
  }
  if (!CheckInputAmount(safe, br, 28)) {  /* 156 bits + 7 bytes */
    s->state = BROTLI_STATE_COMMAND_BEGIN;
    result = BROTLI_DECODER_NEEDS_MORE_INPUT;
    goto saveStateAndReturn;
  }
  if (BROTLI_PREDICT_FALSE(s->block_length[1] == 0)) {
    BROTLI_SAFE(DecodeCommandBlockSwitch(s));
    goto CommandBegin;
  }
  /* Read the insert/copy length in the command. */
  BROTLI_SAFE(ReadCommand(s, br, &i));
  BROTLI_LOG(("[ProcessCommandsInternal] pos = %d insert = %d copy = %d\n",
              pos, i, s->copy_length));
  if (i == 0) {
    goto CommandPostDecodeLiterals;
  }
  s->meta_block_remaining_len -= i;

CommandInner:
  if (safe) {
    s->state = BROTLI_STATE_COMMAND_INNER;
  }
  /* Read the literals in the command. */
  if (s->trivial_literal_context) {
    uint32_t bits;
    uint32_t value;
    PreloadSymbol(safe, s->literal_htree, br, &bits, &value);
    do {
      if (!CheckInputAmount(safe, br, 28)) {  /* 162 bits + 7 bytes */
        s->state = BROTLI_STATE_COMMAND_INNER;
        result = BROTLI_DECODER_NEEDS_MORE_INPUT;
        goto saveStateAndReturn;
      }
      if (BROTLI_PREDICT_FALSE(s->block_length[0] == 0)) {
        BROTLI_SAFE(DecodeLiteralBlockSwitch(s));
        PreloadSymbol(safe, s->literal_htree, br, &bits, &value);
        if (!s->trivial_literal_context) goto CommandInner;
      }
      if (!safe) {
        s->ringbuffer[pos] =
            (uint8_t)ReadPreloadedSymbol(s->literal_htree, br, &bits, &value);
      } else {
        uint32_t literal;
        if (!SafeReadSymbol(s->literal_htree, br, &literal)) {
          result = BROTLI_DECODER_NEEDS_MORE_INPUT;
          goto saveStateAndReturn;
        }
        s->ringbuffer[pos] = (uint8_t)literal;
      }
      --s->block_length[0];
      BROTLI_LOG_ARRAY_INDEX(s->ringbuffer, pos);
      ++pos;
      if (BROTLI_PREDICT_FALSE(pos == s->ringbuffer_size)) {
        s->state = BROTLI_STATE_COMMAND_INNER_WRITE;
        --i;
        goto saveStateAndReturn;
      }
    } while (--i != 0);
  } else {
    uint8_t p1 = s->ringbuffer[(pos - 1) & s->ringbuffer_mask];
    uint8_t p2 = s->ringbuffer[(pos - 2) & s->ringbuffer_mask];
    do {
      const HuffmanCode* hc;
      uint8_t context;
      if (!CheckInputAmount(safe, br, 28)) {  /* 162 bits + 7 bytes */
        s->state = BROTLI_STATE_COMMAND_INNER;
        result = BROTLI_DECODER_NEEDS_MORE_INPUT;
        goto saveStateAndReturn;
      }
      if (BROTLI_PREDICT_FALSE(s->block_length[0] == 0)) {
        BROTLI_SAFE(DecodeLiteralBlockSwitch(s));
        if (s->trivial_literal_context) goto CommandInner;
      }
      context = BROTLI_CONTEXT(p1, p2, s->context_lookup);
      BROTLI_LOG_UINT(context);
      hc = s->literal_hgroup.htrees[s->context_map_slice[context]];
      p2 = p1;
      if (!safe) {
        p1 = (uint8_t)ReadSymbol(hc, br);
      } else {
        uint32_t literal;
        if (!SafeReadSymbol(hc, br, &literal)) {
          result = BROTLI_DECODER_NEEDS_MORE_INPUT;
          goto saveStateAndReturn;
        }
        p1 = (uint8_t)literal;
      }
      s->ringbuffer[pos] = p1;
      --s->block_length[0];
      BROTLI_LOG_UINT(s->context_map_slice[context]);
      BROTLI_LOG_ARRAY_INDEX(s->ringbuffer, pos & s->ringbuffer_mask);
      ++pos;
      if (BROTLI_PREDICT_FALSE(pos == s->ringbuffer_size)) {
        s->state = BROTLI_STATE_COMMAND_INNER_WRITE;
        --i;
        goto saveStateAndReturn;
      }
    } while (--i != 0);
  }
  BROTLI_LOG_UINT(s->meta_block_remaining_len);
  if (BROTLI_PREDICT_FALSE(s->meta_block_remaining_len <= 0)) {
    s->state = BROTLI_STATE_METABLOCK_DONE;
    goto saveStateAndReturn;
  }

CommandPostDecodeLiterals:
  if (safe) {
    s->state = BROTLI_STATE_COMMAND_POST_DECODE_LITERALS;
  }
  if (s->distance_code >= 0) {
    /* Implicit distance case. */
    s->distance_context = s->distance_code ? 0 : 1;
    --s->dist_rb_idx;
    s->distance_code = s->dist_rb[s->dist_rb_idx & 3];
  } else {
    /* Read distance code in the command, unless it was implicitly zero. */
    if (BROTLI_PREDICT_FALSE(s->block_length[2] == 0)) {
      BROTLI_SAFE(DecodeDistanceBlockSwitch(s));
    }
    BROTLI_SAFE(ReadDistance(s, br));
  }
  BROTLI_LOG(("[ProcessCommandsInternal] pos = %d distance = %d\n",
              pos, s->distance_code));
  if (s->max_distance != s->max_backward_distance) {
    s->max_distance =
        (pos < s->max_backward_distance) ? pos : s->max_backward_distance;
  }
  i = s->copy_length;
  /* Apply copy of LZ77 back-reference, or static dictionary reference if
     the distance is larger than the max LZ77 distance */
  if (s->distance_code > s->max_distance) {
    /* The maximum allowed distance is BROTLI_MAX_ALLOWED_DISTANCE = 0x7FFFFFFC.
       With this choice, no signed overflow can occur after decoding
       a special distance code (e.g., after adding 3 to the last distance). */
    if (s->distance_code > BROTLI_MAX_ALLOWED_DISTANCE) {
      BROTLI_LOG(("Invalid backward reference. pos: %d distance: %d "
          "len: %d bytes left: %d\n",
          pos, s->distance_code, i, s->meta_block_remaining_len));
      return BROTLI_FAILURE(BROTLI_DECODER_ERROR_FORMAT_DISTANCE);
    }
    if (i >= BROTLI_MIN_DICTIONARY_WORD_LENGTH &&
        i <= BROTLI_MAX_DICTIONARY_WORD_LENGTH) {
      int address = s->distance_code - s->max_distance - 1;
      const BrotliDictionary* words = s->dictionary;
      const BrotliTransforms* transforms = s->transforms;
      int offset = (int)s->dictionary->offsets_by_length[i];
      uint32_t shift = s->dictionary->size_bits_by_length[i];

      int mask = (int)BitMask(shift);
      int word_idx = address & mask;
      int transform_idx = address >> shift;
      /* Compensate double distance-ring-buffer roll. */
      s->dist_rb_idx += s->distance_context;
      offset += word_idx * i;
      if (BROTLI_PREDICT_FALSE(!words->data)) {
        return BROTLI_FAILURE(BROTLI_DECODER_ERROR_DICTIONARY_NOT_SET);
      }
      if (transform_idx < (int)transforms->num_transforms) {
        const uint8_t* word = &words->data[offset];
        int len = i;
        if (transform_idx == transforms->cutOffTransforms[0]) {
          memcpy(&s->ringbuffer[pos], word, (size_t)len);
          BROTLI_LOG(("[ProcessCommandsInternal] dictionary word: [%.*s]\n",
                      len, word));
        } else {
          len = BrotliTransformDictionaryWord(&s->ringbuffer[pos], word, len,
              transforms, transform_idx);
          BROTLI_LOG(("[ProcessCommandsInternal] dictionary word: [%.*s],"
                      " transform_idx = %d, transformed: [%.*s]\n",
                      i, word, transform_idx, len, &s->ringbuffer[pos]));
        }
        pos += len;
        s->meta_block_remaining_len -= len;
        if (pos >= s->ringbuffer_size) {
          s->state = BROTLI_STATE_COMMAND_POST_WRITE_1;
          goto saveStateAndReturn;
        }
      } else {
        BROTLI_LOG(("Invalid backward reference. pos: %d distance: %d "
            "len: %d bytes left: %d\n",
            pos, s->distance_code, i, s->meta_block_remaining_len));
        return BROTLI_FAILURE(BROTLI_DECODER_ERROR_FORMAT_TRANSFORM);
      }
    } else {
      BROTLI_LOG(("Invalid backward reference. pos: %d distance: %d "
          "len: %d bytes left: %d\n",
          pos, s->distance_code, i, s->meta_block_remaining_len));
      return BROTLI_FAILURE(BROTLI_DECODER_ERROR_FORMAT_DICTIONARY);
    }
  } else {
    int src_start = (pos - s->distance_code) & s->ringbuffer_mask;
    uint8_t* copy_dst = &s->ringbuffer[pos];
    uint8_t* copy_src = &s->ringbuffer[src_start];
    int dst_end = pos + i;
    int src_end = src_start + i;
    /* Update the recent distances cache. */
    s->dist_rb[s->dist_rb_idx & 3] = s->distance_code;
    ++s->dist_rb_idx;
    s->meta_block_remaining_len -= i;
    /* There are 32+ bytes of slack in the ring-buffer allocation.
       Also, we have 16 short codes, that make these 16 bytes irrelevant
       in the ring-buffer. Let's copy over them as a first guess. */
    memmove16(copy_dst, copy_src);
    if (src_end > pos && dst_end > src_start) {
      /* Regions intersect. */
      goto CommandPostWrapCopy;
    }
    if (dst_end >= s->ringbuffer_size || src_end >= s->ringbuffer_size) {
      /* At least one region wraps. */
      goto CommandPostWrapCopy;
    }
    pos += i;
    if (i > 16) {
      if (i > 32) {
        memcpy(copy_dst + 16, copy_src + 16, (size_t)(i - 16));
      } else {
        /* This branch covers about 45% cases.
           Fixed size short copy allows more compiler optimizations. */
        memmove16(copy_dst + 16, copy_src + 16);
      }
    }
  }
  BROTLI_LOG_UINT(s->meta_block_remaining_len);
  if (s->meta_block_remaining_len <= 0) {
    /* Next metablock, if any. */
    s->state = BROTLI_STATE_METABLOCK_DONE;
    goto saveStateAndReturn;
  } else {
    goto CommandBegin;
  }
CommandPostWrapCopy:
  {
    int wrap_guard = s->ringbuffer_size - pos;
    while (--i >= 0) {
      s->ringbuffer[pos] =
          s->ringbuffer[(pos - s->distance_code) & s->ringbuffer_mask];
      ++pos;
      if (BROTLI_PREDICT_FALSE(--wrap_guard == 0)) {
        s->state = BROTLI_STATE_COMMAND_POST_WRITE_2;
        goto saveStateAndReturn;
      }
    }
  }
  if (s->meta_block_remaining_len <= 0) {
    /* Next metablock, if any. */
    s->state = BROTLI_STATE_METABLOCK_DONE;
    goto saveStateAndReturn;
  } else {
    goto CommandBegin;
  }

saveStateAndReturn:
  s->pos = pos;
  s->loop_counter = i;
  return result;
}

#undef BROTLI_SAFE

static BROTLI_NOINLINE BrotliDecoderErrorCode ProcessCommands(
    BrotliDecoderState* s) {
  return ProcessCommandsInternal(0, s);
}

static BROTLI_NOINLINE BrotliDecoderErrorCode SafeProcessCommands(
    BrotliDecoderState* s) {
  return ProcessCommandsInternal(1, s);
}

/* Returns the maximum number of distance symbols which can only represent
   distances not exceeding BROTLI_MAX_ALLOWED_DISTANCE. */
static uint32_t BrotliMaxDistanceSymbol(uint32_t ndirect, uint32_t npostfix) {
  static const uint32_t bound[BROTLI_MAX_NPOSTFIX + 1] = {0, 4, 12, 28};
  static const uint32_t diff[BROTLI_MAX_NPOSTFIX + 1] = {73, 126, 228, 424};
  uint32_t postfix = 1U << npostfix;
  if (ndirect < bound[npostfix]) {
    return ndirect + diff[npostfix] + postfix;
  } else if (ndirect > bound[npostfix] + postfix) {
    return ndirect + diff[npostfix];
  } else {
    return bound[npostfix] + diff[npostfix] + postfix;
  }
}

BrotliDecoderResult BrotliDecoderDecompress(
    size_t encoded_size, const uint8_t encoded_buffer[encoded_size], size_t* decoded_size,
    uint8_t decoded_buffer[*decoded_size]) {
  BrotliDecoderState s;
  BrotliDecoderResult result;
  size_t total_out = 0;
  size_t available_in = encoded_size;
  const uint8_t* next_in = encoded_buffer;
  size_t available_out = *decoded_size;
  uint8_t* next_out = decoded_buffer;
  if (!BrotliDecoderStateInit(&s, 0, 0, 0)) {
    return BROTLI_DECODER_RESULT_ERROR;
  }
  result = BrotliDecoderDecompressStream(
      &s, &available_in, &next_in, &available_out, &next_out, &total_out);
  *decoded_size = total_out;
  BrotliDecoderStateCleanup(&s);
  if (result != BROTLI_DECODER_RESULT_SUCCESS) {
    result = BROTLI_DECODER_RESULT_ERROR;
  }
  return result;
}

/* Invariant: input stream is never overconsumed:
    - invalid input implies that the whole stream is invalid -> any amount of
      input could be read and discarded
    - when result is "needs more input", then at least one more byte is REQUIRED
      to complete decoding; all input data MUST be consumed by decoder, so
      client could swap the input buffer
    - when result is "needs more output" decoder MUST ensure that it doesn't
      hold more than 7 bits in bit reader; this saves client from swapping input
      buffer ahead of time
    - when result is "success" decoder MUST return all unused data back to input
      buffer; this is possible because the invariant is held on enter */
BrotliDecoderResult BrotliDecoderDecompressStream(
    BrotliDecoderState* s, size_t* available_in, const uint8_t** next_in,
    size_t* available_out, uint8_t** next_out, size_t* total_out) {
  BrotliDecoderErrorCode result = BROTLI_DECODER_SUCCESS;
  BrotliBitReader* br = &s->br;
  /* Ensure that |total_out| is set, even if no data will ever be pushed out. */
  if (total_out) {
    *total_out = s->partial_pos_out;
  }
  /* Do not try to process further in a case of unrecoverable error. */
  if ((int)s->error_code < 0) {
    return BROTLI_DECODER_RESULT_ERROR;
  }
  if (*available_out && (!next_out || !*next_out)) {
    return SaveErrorCode(
        s, BROTLI_FAILURE(BROTLI_DECODER_ERROR_INVALID_ARGUMENTS));
  }
  if (!*available_out) next_out = 0;
  if (s->buffer_length == 0) {  /* Just connect bit reader to input stream. */
    br->avail_in = *available_in;
    br->next_in = *next_in;
  } else {
    /* At least one byte of input is required. More than one byte of input may
       be required to complete the transaction -> reading more data must be
       done in a loop -> do it in a main loop. */
    result = BROTLI_DECODER_NEEDS_MORE_INPUT;
    br->next_in = &s->buffer.u8[0];
  }
  /* State machine */
  for (;;) {
    if (result != BROTLI_DECODER_SUCCESS) {
      /* Error, needs more input/output. */
      if (result == BROTLI_DECODER_NEEDS_MORE_INPUT) {
        if (s->ringbuffer != 0) {  /* Pro-actively push output. */
          BrotliDecoderErrorCode intermediate_result = WriteRingBuffer(s,
              available_out, next_out, total_out, BROTLI_TRUE);
          /* WriteRingBuffer checks s->meta_block_remaining_len validity. */
          if ((int)intermediate_result < 0) {
            result = intermediate_result;
            break;
          }
        }
        if (s->buffer_length != 0) {  /* Used with internal buffer. */
          if (br->avail_in == 0) {
            /* Successfully finished read transaction.
               Accumulator contains less than 8 bits, because internal buffer
               is expanded byte-by-byte until it is enough to complete read. */
            s->buffer_length = 0;
            /* Switch to input stream and restart. */
            result = BROTLI_DECODER_SUCCESS;
            br->avail_in = *available_in;
            br->next_in = *next_in;
            continue;
          } else if (*available_in != 0) {
            /* Not enough data in buffer, but can take one more byte from
               input stream. */
            result = BROTLI_DECODER_SUCCESS;
            s->buffer.u8[s->buffer_length] = **next_in;
            s->buffer_length++;
            br->avail_in = s->buffer_length;
            (*next_in)++;
            (*available_in)--;
            /* Retry with more data in buffer. */
            continue;
          }
          /* Can't finish reading and no more input. */
          break;
        } else {  /* Input stream doesn't contain enough input. */
          /* Copy tail to internal buffer and return. */
          *next_in = br->next_in;
          *available_in = br->avail_in;
          while (*available_in) {
            s->buffer.u8[s->buffer_length] = **next_in;
            s->buffer_length++;
            (*next_in)++;
            (*available_in)--;
          }
          break;
        }
        /* Unreachable. */
      }

      /* Fail or needs more output. */

      if (s->buffer_length != 0) {
        /* Just consumed the buffered input and produced some output. Otherwise
           it would result in "needs more input". Reset internal buffer. */
        s->buffer_length = 0;
      } else {
        /* Using input stream in last iteration. When decoder switches to input
           stream it has less than 8 bits in accumulator, so it is safe to
           return unused accumulator bits there. */
        BrotliBitReaderUnload(br);
        *available_in = br->avail_in;
        *next_in = br->next_in;
      }
      break;
    }
    switch (s->state) {
      case BROTLI_STATE_UNINITED:
        /* Prepare to the first read. */
        if (!BrotliWarmupBitReader(br)) {
          result = BROTLI_DECODER_NEEDS_MORE_INPUT;
          break;
        }
        /* Decode window size. */
        result = DecodeWindowBits(s, br);  /* Reads 1..8 bits. */
        if (result != BROTLI_DECODER_SUCCESS) {
          break;
        }
        if (s->large_window) {
          s->state = BROTLI_STATE_LARGE_WINDOW_BITS;
          break;
        }
        s->state = BROTLI_STATE_INITIALIZE;
        break;

      case BROTLI_STATE_LARGE_WINDOW_BITS:
        if (!BrotliSafeReadBits(br, 6, &s->window_bits)) {
          result = BROTLI_DECODER_NEEDS_MORE_INPUT;
          break;
        }
        if (s->window_bits < BROTLI_LARGE_MIN_WBITS ||
            s->window_bits > BROTLI_LARGE_MAX_WBITS) {
          result = BROTLI_FAILURE(BROTLI_DECODER_ERROR_FORMAT_WINDOW_BITS);
          break;
        }
        s->state = BROTLI_STATE_INITIALIZE;
      /* Fall through. */

      case BROTLI_STATE_INITIALIZE:
        BROTLI_LOG_UINT(s->window_bits);
        /* Maximum distance, see section 9.1. of the spec. */
        s->max_backward_distance = (1 << s->window_bits) - BROTLI_WINDOW_GAP;

        /* Allocate memory for both block_type_trees and block_len_trees. */
        s->block_type_trees = (HuffmanCode*)BROTLI_DECODER_ALLOC(s,
            sizeof(HuffmanCode) * 3 *
                (BROTLI_HUFFMAN_MAX_SIZE_258 + BROTLI_HUFFMAN_MAX_SIZE_26));
        if (s->block_type_trees == 0) {
          result = BROTLI_FAILURE(BROTLI_DECODER_ERROR_ALLOC_BLOCK_TYPE_TREES);
          break;
        }
        s->block_len_trees =
            s->block_type_trees + 3 * BROTLI_HUFFMAN_MAX_SIZE_258;

        s->state = BROTLI_STATE_METABLOCK_BEGIN;
      /* Fall through. */

      case BROTLI_STATE_METABLOCK_BEGIN:
        BrotliDecoderStateMetablockBegin(s);
        BROTLI_LOG_UINT(s->pos);
        s->state = BROTLI_STATE_METABLOCK_HEADER;
      /* Fall through. */

      case BROTLI_STATE_METABLOCK_HEADER:
        result = DecodeMetaBlockLength(s, br);  /* Reads 2 - 31 bits. */
        if (result != BROTLI_DECODER_SUCCESS) {
          break;
        }
        BROTLI_LOG_UINT(s->is_last_metablock);
        BROTLI_LOG_UINT(s->meta_block_remaining_len);
        BROTLI_LOG_UINT(s->is_metadata);
        BROTLI_LOG_UINT(s->is_uncompressed);
        if (s->is_metadata || s->is_uncompressed) {
          if (!BrotliJumpToByteBoundary(br)) {
            result = BROTLI_FAILURE(BROTLI_DECODER_ERROR_FORMAT_PADDING_1);
            break;
          }
        }
        if (s->is_metadata) {
          s->state = BROTLI_STATE_METADATA;
          break;
        }
        if (s->meta_block_remaining_len == 0) {
          s->state = BROTLI_STATE_METABLOCK_DONE;
          break;
        }
        BrotliCalculateRingBufferSize(s);
        if (s->is_uncompressed) {
          s->state = BROTLI_STATE_UNCOMPRESSED;
          break;
        }
        s->loop_counter = 0;
        s->state = BROTLI_STATE_HUFFMAN_CODE_0;
        break;

      case BROTLI_STATE_UNCOMPRESSED: {
        result = CopyUncompressedBlockToOutput(
            available_out, next_out, total_out, s);
        if (result != BROTLI_DECODER_SUCCESS) {
          break;
        }
        s->state = BROTLI_STATE_METABLOCK_DONE;
        break;
      }

      case BROTLI_STATE_METADATA:
        for (; s->meta_block_remaining_len > 0; --s->meta_block_remaining_len) {
          uint32_t bits;
          /* Read one byte and ignore it. */
          if (!BrotliSafeReadBits(br, 8, &bits)) {
            result = BROTLI_DECODER_NEEDS_MORE_INPUT;
            break;
          }
        }
        if (result == BROTLI_DECODER_SUCCESS) {
          s->state = BROTLI_STATE_METABLOCK_DONE;
        }
        break;

      case BROTLI_STATE_HUFFMAN_CODE_0:
        if (s->loop_counter >= 3) {
          s->state = BROTLI_STATE_METABLOCK_HEADER_2;
          break;
        }
        /* Reads 1..11 bits. */
        result = DecodeVarLenUint8(s, br, &s->num_block_types[s->loop_counter]);
        if (result != BROTLI_DECODER_SUCCESS) {
          break;
        }
        s->num_block_types[s->loop_counter]++;
        BROTLI_LOG_UINT(s->num_block_types[s->loop_counter]);
        if (s->num_block_types[s->loop_counter] < 2) {
          s->loop_counter++;
          break;
        }
        s->state = BROTLI_STATE_HUFFMAN_CODE_1;
      /* Fall through. */

      case BROTLI_STATE_HUFFMAN_CODE_1: {
        uint32_t alphabet_size = s->num_block_types[s->loop_counter] + 2;
        int tree_offset = s->loop_counter * BROTLI_HUFFMAN_MAX_SIZE_258;
        result = ReadHuffmanCode(alphabet_size, alphabet_size,
            &s->block_type_trees[tree_offset], NULL, s);
        if (result != BROTLI_DECODER_SUCCESS) break;
        s->state = BROTLI_STATE_HUFFMAN_CODE_2;
      }
      /* Fall through. */

      case BROTLI_STATE_HUFFMAN_CODE_2: {
        uint32_t alphabet_size = BROTLI_NUM_BLOCK_LEN_SYMBOLS;
        int tree_offset = s->loop_counter * BROTLI_HUFFMAN_MAX_SIZE_26;
        result = ReadHuffmanCode(alphabet_size, alphabet_size,
            &s->block_len_trees[tree_offset], NULL, s);
        if (result != BROTLI_DECODER_SUCCESS) break;
        s->state = BROTLI_STATE_HUFFMAN_CODE_3;
      }
      /* Fall through. */

      case BROTLI_STATE_HUFFMAN_CODE_3: {
        int tree_offset = s->loop_counter * BROTLI_HUFFMAN_MAX_SIZE_26;
        if (!SafeReadBlockLength(s, &s->block_length[s->loop_counter],
            &s->block_len_trees[tree_offset], br)) {
          result = BROTLI_DECODER_NEEDS_MORE_INPUT;
          break;
        }
        BROTLI_LOG_UINT(s->block_length[s->loop_counter]);
        s->loop_counter++;
        s->state = BROTLI_STATE_HUFFMAN_CODE_0;
        break;
      }

      case BROTLI_STATE_METABLOCK_HEADER_2: {
        uint32_t bits;
        if (!BrotliSafeReadBits(br, 6, &bits)) {
          result = BROTLI_DECODER_NEEDS_MORE_INPUT;
          break;
        }
        s->distance_postfix_bits = bits & BitMask(2);
        bits >>= 2;
        s->num_direct_distance_codes = BROTLI_NUM_DISTANCE_SHORT_CODES +
            (bits << s->distance_postfix_bits);
        BROTLI_LOG_UINT(s->num_direct_distance_codes);
        BROTLI_LOG_UINT(s->distance_postfix_bits);
        s->distance_postfix_mask = (int)BitMask(s->distance_postfix_bits);
        s->context_modes =
            (uint8_t*)BROTLI_DECODER_ALLOC(s, (size_t)s->num_block_types[0]);
        if (s->context_modes == 0) {
          result = BROTLI_FAILURE(BROTLI_DECODER_ERROR_ALLOC_CONTEXT_MODES);
          break;
        }
        s->loop_counter = 0;
        s->state = BROTLI_STATE_CONTEXT_MODES;
      }
      /* Fall through. */

      case BROTLI_STATE_CONTEXT_MODES:
        result = ReadContextModes(s);
        if (result != BROTLI_DECODER_SUCCESS) {
          break;
        }
        s->state = BROTLI_STATE_CONTEXT_MAP_1;
      /* Fall through. */

      case BROTLI_STATE_CONTEXT_MAP_1:
        result = DecodeContextMap(
            s->num_block_types[0] << BROTLI_LITERAL_CONTEXT_BITS,
            &s->num_literal_htrees, &s->context_map, s);
        if (result != BROTLI_DECODER_SUCCESS) {
          break;
        }
        DetectTrivialLiteralBlockTypes(s);
        s->state = BROTLI_STATE_CONTEXT_MAP_2;
      /* Fall through. */

      case BROTLI_STATE_CONTEXT_MAP_2: {
        uint32_t num_direct_codes =
            s->num_direct_distance_codes - BROTLI_NUM_DISTANCE_SHORT_CODES;
        uint32_t num_distance_codes = BROTLI_DISTANCE_ALPHABET_SIZE(
            s->distance_postfix_bits, num_direct_codes,
            (s->large_window ? BROTLI_LARGE_MAX_DISTANCE_BITS :
                               BROTLI_MAX_DISTANCE_BITS));
        uint32_t max_distance_symbol = (s->large_window ?
            BrotliMaxDistanceSymbol(
                num_direct_codes, s->distance_postfix_bits) :
            num_distance_codes);
        BROTLI_BOOL allocation_success = BROTLI_TRUE;
        result = DecodeContextMap(
            s->num_block_types[2] << BROTLI_DISTANCE_CONTEXT_BITS,
            &s->num_dist_htrees, &s->dist_context_map, s);
        if (result != BROTLI_DECODER_SUCCESS) {
          break;
        }
        allocation_success &= BrotliDecoderHuffmanTreeGroupInit(
            s, &s->literal_hgroup, BROTLI_NUM_LITERAL_SYMBOLS,
            BROTLI_NUM_LITERAL_SYMBOLS, s->num_literal_htrees);
        allocation_success &= BrotliDecoderHuffmanTreeGroupInit(
            s, &s->insert_copy_hgroup, BROTLI_NUM_COMMAND_SYMBOLS,
            BROTLI_NUM_COMMAND_SYMBOLS, s->num_block_types[1]);
        allocation_success &= BrotliDecoderHuffmanTreeGroupInit(
            s, &s->distance_hgroup, num_distance_codes,
            max_distance_symbol, s->num_dist_htrees);
        if (!allocation_success) {
          return SaveErrorCode(s,
              BROTLI_FAILURE(BROTLI_DECODER_ERROR_ALLOC_TREE_GROUPS));
        }
        s->loop_counter = 0;
        s->state = BROTLI_STATE_TREE_GROUP;
      }
      /* Fall through. */

      case BROTLI_STATE_TREE_GROUP: {
        HuffmanTreeGroup* hgroup = NULL;
        switch (s->loop_counter) {
          case 0: hgroup = &s->literal_hgroup; break;
          case 1: hgroup = &s->insert_copy_hgroup; break;
          case 2: hgroup = &s->distance_hgroup; break;
          default: return SaveErrorCode(s, BROTLI_FAILURE(
              BROTLI_DECODER_ERROR_UNREACHABLE));
        }
        result = HuffmanTreeGroupDecode(hgroup, s);
        if (result != BROTLI_DECODER_SUCCESS) break;
        s->loop_counter++;
        if (s->loop_counter >= 3) {
          PrepareLiteralDecoding(s);
          s->dist_context_map_slice = s->dist_context_map;
          s->htree_command = s->insert_copy_hgroup.htrees[0];
          if (!BrotliEnsureRingBuffer(s)) {
            result = BROTLI_FAILURE(BROTLI_DECODER_ERROR_ALLOC_RING_BUFFER_2);
            break;
          }
          s->state = BROTLI_STATE_COMMAND_BEGIN;
        }
        break;
      }

      case BROTLI_STATE_COMMAND_BEGIN:
      /* Fall through. */
      case BROTLI_STATE_COMMAND_INNER:
      /* Fall through. */
      case BROTLI_STATE_COMMAND_POST_DECODE_LITERALS:
      /* Fall through. */
      case BROTLI_STATE_COMMAND_POST_WRAP_COPY:
        result = ProcessCommands(s);
        if (result == BROTLI_DECODER_NEEDS_MORE_INPUT) {
          result = SafeProcessCommands(s);
        }
        break;

      case BROTLI_STATE_COMMAND_INNER_WRITE:
      /* Fall through. */
      case BROTLI_STATE_COMMAND_POST_WRITE_1:
      /* Fall through. */
      case BROTLI_STATE_COMMAND_POST_WRITE_2:
        result = WriteRingBuffer(
            s, available_out, next_out, total_out, BROTLI_FALSE);
        if (result != BROTLI_DECODER_SUCCESS) {
          break;
        }
        WrapRingBuffer(s);
        if (s->ringbuffer_size == 1 << s->window_bits) {
          s->max_distance = s->max_backward_distance;
        }
        if (s->state == BROTLI_STATE_COMMAND_POST_WRITE_1) {
          if (s->meta_block_remaining_len == 0) {
            /* Next metablock, if any. */
            s->state = BROTLI_STATE_METABLOCK_DONE;
          } else {
            s->state = BROTLI_STATE_COMMAND_BEGIN;
          }
          break;
        } else if (s->state == BROTLI_STATE_COMMAND_POST_WRITE_2) {
          s->state = BROTLI_STATE_COMMAND_POST_WRAP_COPY;
        } else {  /* BROTLI_STATE_COMMAND_INNER_WRITE */
          if (s->loop_counter == 0) {
            if (s->meta_block_remaining_len == 0) {
              s->state = BROTLI_STATE_METABLOCK_DONE;
            } else {
              s->state = BROTLI_STATE_COMMAND_POST_DECODE_LITERALS;
            }
            break;
          }
          s->state = BROTLI_STATE_COMMAND_INNER;
        }
        break;

      case BROTLI_STATE_METABLOCK_DONE:
        if (s->meta_block_remaining_len < 0) {
          result = BROTLI_FAILURE(BROTLI_DECODER_ERROR_FORMAT_BLOCK_LENGTH_2);
          break;
        }
        BrotliDecoderStateCleanupAfterMetablock(s);
        if (!s->is_last_metablock) {
          s->state = BROTLI_STATE_METABLOCK_BEGIN;
          break;
        }
        if (!BrotliJumpToByteBoundary(br)) {
          result = BROTLI_FAILURE(BROTLI_DECODER_ERROR_FORMAT_PADDING_2);
          break;
        }
        if (s->buffer_length == 0) {
          BrotliBitReaderUnload(br);
          *available_in = br->avail_in;
          *next_in = br->next_in;
        }
        s->state = BROTLI_STATE_DONE;
      /* Fall through. */

      case BROTLI_STATE_DONE:
        if (s->ringbuffer != 0) {
          result = WriteRingBuffer(
              s, available_out, next_out, total_out, BROTLI_TRUE);
          if (result != BROTLI_DECODER_SUCCESS) {
            break;
          }
        }
        return SaveErrorCode(s, result);
    }
  }
  return SaveErrorCode(s, result);
}

BROTLI_BOOL BrotliDecoderHasMoreOutput(const BrotliDecoderState* s) {
  /* After unrecoverable error remaining output is considered nonsensical. */
  if ((int)s->error_code < 0) {
    return BROTLI_FALSE;
  }
  return TO_BROTLI_BOOL(
      s->ringbuffer != 0 && UnwrittenBytes(s, BROTLI_FALSE) != 0);
}

const uint8_t* BrotliDecoderTakeOutput(BrotliDecoderState* s, size_t* size) {
  uint8_t* result = 0;
  size_t available_out = *size ? *size : 1u << 24;
  size_t requested_out = available_out;
  BrotliDecoderErrorCode status;
  if ((s->ringbuffer == 0) || ((int)s->error_code < 0)) {
    *size = 0;
    return 0;
  }
  WrapRingBuffer(s);
  status = WriteRingBuffer(s, &available_out, &result, 0, BROTLI_TRUE);
  /* Either WriteRingBuffer returns those "success" codes... */
  if (status == BROTLI_DECODER_SUCCESS ||
      status == BROTLI_DECODER_NEEDS_MORE_OUTPUT) {
    *size = requested_out - available_out;
  } else {
    /* ... or stream is broken. Normally this should be caught by
       BrotliDecoderDecompressStream, this is just a safeguard. */
    if ((int)status < 0) SaveErrorCode(s, status);
    *size = 0;
    result = 0;
  }
  return result;
}

BROTLI_BOOL BrotliDecoderIsUsed(const BrotliDecoderState* s) {
  return TO_BROTLI_BOOL(s->state != BROTLI_STATE_UNINITED ||
      BrotliGetAvailableBits(&s->br) != 0);
}

BROTLI_BOOL BrotliDecoderIsFinished(const BrotliDecoderState* s) {
  return TO_BROTLI_BOOL(s->state == BROTLI_STATE_DONE) &&
      !BrotliDecoderHasMoreOutput(s);
}

BrotliDecoderErrorCode BrotliDecoderGetErrorCode(const BrotliDecoderState* s) {
  return (BrotliDecoderErrorCode)s->error_code;
}

const char* BrotliDecoderErrorString(BrotliDecoderErrorCode c) {
  switch (c) {
#define BROTLI_ERROR_CODE_CASE_(PREFIX, NAME, CODE) \
    case BROTLI_DECODER ## PREFIX ## NAME: return #NAME;
#define BROTLI_NOTHING_
    BROTLI_DECODER_ERROR_CODES_LIST(BROTLI_ERROR_CODE_CASE_, BROTLI_NOTHING_)
#undef BROTLI_ERROR_CODE_CASE_
#undef BROTLI_NOTHING_
    default: return "INVALID";
  }
}

uint32_t BrotliDecoderVersion() {
  return BROTLI_VERSION;
}

#if defined(__cplusplus) || defined(c_plusplus)
}  /* extern "C" */
#endif
