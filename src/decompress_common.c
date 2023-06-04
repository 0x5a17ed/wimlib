/*
 * decompress_common.c
 *
 * Code for decompression shared among multiple compression formats.
 *
 * Copyright 2022 Eric Biggers
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <string.h>

#ifdef __SSE2__
#  include <emmintrin.h>
#endif

#include "wimlib/decompress_common.h"

/*
 * make_huffman_decode_table() -
 *
 * Given an alphabet of symbols and the length of each symbol's codeword in a
 * canonical prefix code, build a table for quickly decoding symbols that were
 * encoded with that code.
 *
 * A _prefix code_ is an assignment of bitstrings called _codewords_ to symbols
 * such that no whole codeword is a prefix of any other.  A prefix code might be
 * a _Huffman code_, which means that it is an optimum prefix code for a given
 * list of symbol frequencies and was generated by the Huffman algorithm.
 * Although the prefix codes processed here will ordinarily be "Huffman codes",
 * strictly speaking the decoder cannot know whether a given code was actually
 * generated by the Huffman algorithm or not.
 *
 * A prefix code is _canonical_ if and only if a longer codeword never
 * lexicographically precedes a shorter codeword, and the lexicographic ordering
 * of codewords of equal length is the same as the lexicographic ordering of the
 * corresponding symbols.  The advantage of using a canonical prefix code is
 * that the codewords can be reconstructed from only the symbol => codeword
 * length mapping.  This eliminates the need to transmit the codewords
 * explicitly.  Instead, they can be enumerated in lexicographic order after
 * sorting the symbols primarily by increasing codeword length and secondarily
 * by increasing symbol value.
 *
 * However, the decoder's real goal is to decode symbols with the code, not just
 * generate the list of codewords.  Consequently, this function directly builds
 * a table for efficiently decoding symbols using the code.  The basic idea is
 * that given the next 'max_codeword_len' bits of input, the decoder can look up
 * the next decoded symbol by indexing a table containing '2^max_codeword_len'
 * entries.  A codeword with length 'max_codeword_len' will have exactly one
 * entry in this table, whereas a codeword shorter than 'max_codeword_len' will
 * have multiple entries in this table.  Precisely, a codeword of length 'n'
 * will have '2^(max_codeword_len - n)' entries.  The index of each such entry,
 * considered as a bitstring of length 'max_codeword_len', will contain the
 * corresponding codeword as a prefix.
 *
 * That's the basic idea, but we extend it in two ways:
 *
 * - Often the maximum codeword length is too long for it to be efficient to
 *   build the full decode table whenever a new code is used.  Instead, we build
 *   a "root" table using only '2^table_bits' entries, where 'table_bits <=
 *   max_codeword_len'.  Then, a lookup of 'table_bits' bits produces either a
 *   symbol directly (for codewords not longer than 'table_bits'), or the index
 *   of a subtable which must be indexed with additional bits of input to fully
 *   decode the symbol (for codewords longer than 'table_bits').
 *
 * - Whenever the decoder decodes a symbol, it needs to know the codeword length
 *   so that it can remove the appropriate number of input bits.  The obvious
 *   solution would be to simply retain the codeword lengths array and use the
 *   decoded symbol as an index into it.  However, that would require two array
 *   accesses when decoding each symbol.  Our strategy is to instead store the
 *   codeword length directly in the decode table entry along with the symbol.
 *
 * See MAKE_DECODE_TABLE_ENTRY() for full details on the format of decode table
 * entries, and see read_huffsym() for full details on how symbols are decoded.
 *
 * @decode_table:
 *	The array in which to build the decode table.  This must have been
 *	declared by the DECODE_TABLE() macro.  This may alias @lens, since all
 *	@lens are consumed before the decode table is written to.
 *
 * @num_syms:
 *	The number of symbols in the alphabet.
 *
 * @table_bits:
 *	The log base 2 of the number of entries in the root table.
 *
 * @lens:
 *	An array of length @num_syms, indexed by symbol, that gives the length
 *	of the codeword, in bits, for each symbol.  The length can be 0, which
 *	means that the symbol does not have a codeword assigned.  In addition,
 *	@lens may alias @decode_table, as noted above.
 *
 * @max_codeword_len:
 *	The maximum codeword length permitted for this code.  All entries in
 *	'lens' must be less than or equal to this value.
 *
 * @working_space
 *	A temporary array that was declared with DECODE_TABLE_WORKING_SPACE().
 *
 * Returns 0 on success, or -1 if the lengths do not form a valid prefix code.
 */
int
make_huffman_decode_table(u16 decode_table[],
                          unsigned num_syms,
                          unsigned table_bits,
                          const u8 lens[],
                          unsigned max_codeword_len,
                          u16 working_space[])
{
	u16 *const len_counts  = &working_space[0];
	u16 *const offsets     = &working_space[1 * (max_codeword_len + 1)];
	u16 *const sorted_syms = &working_space[2 * (max_codeword_len + 1)];
	s32 remainder          = 1;
	void *entry_ptr        = decode_table;
	unsigned codeword_len  = 1;
	unsigned sym_idx;
	unsigned codeword;
	unsigned subtable_pos;
	unsigned subtable_bits;
	unsigned subtable_prefix;

	/* Count how many codewords have each length, including 0.  */
	for (unsigned len = 0; len <= max_codeword_len; len++)
		len_counts[len] = 0;
	for (unsigned sym = 0; sym < num_syms; sym++)
		len_counts[lens[sym]]++;

	/* It is already guaranteed that all lengths are <= max_codeword_len,
	 * but it cannot be assumed they form a complete prefix code.  A
	 * codeword of length n should require a proportion of the codespace
	 * equaling (1/2)^n.  The code is complete if and only if, by this
	 * measure, the codespace is exactly filled by the lengths.  */
	for (unsigned len = 1; len <= max_codeword_len; len++) {
		remainder = (remainder << 1) - len_counts[len];
		/* Do the lengths overflow the codespace? */
		if (unlikely(remainder < 0))
			return -1;
	}

	if (remainder != 0) {
		/* The lengths do not fill the codespace; that is, they form an
		 * incomplete code.  This is permitted only if the code is empty
		 * (contains no symbols). */

		if (unlikely(remainder != 1U << max_codeword_len))
			return -1;

		/* The code is empty.  When processing a well-formed stream, the
		 * decode table need not be initialized in this case.  However,
		 * we cannot assume the stream is well-formed, so we must
		 * initialize the decode table anyway.  Setting all entries to 0
		 * makes the decode table always produce symbol '0' without
		 * consuming any bits, which is good enough. */
		memset(decode_table, 0, sizeof(decode_table[0]) << table_bits);
		return 0;
	}

	/* Sort the symbols primarily by increasing codeword length and
	 * secondarily by increasing symbol value. */

	/* Initialize 'offsets' so that 'offsets[len]' is the number of
	 * codewords shorter than 'len' bits, including length 0. */
	offsets[0] = 0;
	for (unsigned len = 0; len < max_codeword_len; len++)
		offsets[len + 1] = offsets[len] + len_counts[len];

	/* Use the 'offsets' array to sort the symbols. */
	for (unsigned sym = 0; sym < num_syms; sym++)
		sorted_syms[offsets[lens[sym]]++] = sym;

	/*
	 * Fill the root table entries for codewords no longer than table_bits.
	 *
	 * The table will start with entries for the shortest codeword(s), which
	 * will have the most entries.  From there, the number of entries per
	 * codeword will decrease.  As an optimization, we may begin filling
	 * entries with SSE2 vector accesses (8 entries/store), then change to
	 * word accesses (2 or 4 entries/store), then change to 16-bit accesses
	 * (1 entry/store).
	 */
	sym_idx = offsets[0];

#ifdef __SSE2__
	/* Fill entries one 128-bit vector (8 entries) at a time. */
	for (unsigned stores_per_loop =
	             (1U << (table_bits - codeword_len)) /
	             (sizeof(__m128i) / sizeof(decode_table[0]));
	     stores_per_loop != 0;
	     codeword_len++, stores_per_loop >>= 1)
	{
		unsigned end_sym_idx = sym_idx + len_counts[codeword_len];
		for (; sym_idx < end_sym_idx; sym_idx++) {
			/* Note: unlike in the "word" version below, the __m128i
			 * type already has __attribute__((may_alias)), so using
			 * it to access an array of u16 will not violate strict
			 * aliasing.  */
			__m128i v  = _mm_set1_epi16(MAKE_DECODE_TABLE_ENTRY(
                                sorted_syms[sym_idx], codeword_len));
			unsigned n = stores_per_loop;
			do {
				*(__m128i *)entry_ptr = v;
				entry_ptr += sizeof(v);
			} while (--n);
		}
	}
#endif /* __SSE2__ */

#ifdef __GNUC__
	/* Fill entries one word (2 or 4 entries) at a time. */
	for (unsigned stores_per_loop = (1U << (table_bits - codeword_len)) /
	                                (WORDBYTES / sizeof(decode_table[0]));
	     stores_per_loop != 0;
	     codeword_len++, stores_per_loop >>= 1)
	{
		unsigned end_sym_idx = sym_idx + len_counts[codeword_len];
		for (; sym_idx < end_sym_idx; sym_idx++) {
			/* Accessing the array of u16 as u32 or u64 would
			 * violate strict aliasing and would require compiling
			 * the code with -fno-strict-aliasing to guarantee
			 * correctness.  To work around this problem, use the
			 * gcc 'may_alias' extension.  */
			typedef machine_word_t __attribute__((may_alias))
			aliased_word_t;
			aliased_word_t v = repeat_u16(MAKE_DECODE_TABLE_ENTRY(
				sorted_syms[sym_idx], codeword_len));
			unsigned n       = stores_per_loop;
			do {
				*(aliased_word_t *)entry_ptr = v;
				entry_ptr += sizeof(v);
			} while (--n);
		}
	}
#endif /* __GNUC__ */

	/* Fill entries one at a time. */
	for (unsigned stores_per_loop = (1U << (table_bits - codeword_len));
	     stores_per_loop != 0;
	     codeword_len++, stores_per_loop >>= 1)
	{
		unsigned end_sym_idx = sym_idx + len_counts[codeword_len];
		for (; sym_idx < end_sym_idx; sym_idx++) {
			u16 v = MAKE_DECODE_TABLE_ENTRY(sorted_syms[sym_idx],
			                                codeword_len);
			unsigned n = stores_per_loop;
			do {
				*(u16 *)entry_ptr = v;
				entry_ptr += sizeof(v);
			} while (--n);
		}
	}

	/* If all symbols were processed, then no subtables are required. */
	if (sym_idx == num_syms)
		return 0;

	/* At least one subtable is required.  Process the remaining symbols. */
	codeword        = ((u16 *)entry_ptr - decode_table) << 1;
	subtable_pos    = 1U << table_bits;
	subtable_bits   = table_bits;
	subtable_prefix = -1;
	do {
		while (len_counts[codeword_len] == 0) {
			codeword_len++;
			codeword <<= 1;
		}

		unsigned prefix = codeword >> (codeword_len - table_bits);

		/* Start a new subtable if the first 'table_bits' bits of the
		 * codeword don't match the prefix for the previous subtable, or
		 * if this will be the first subtable. */
		if (prefix != subtable_prefix) {
			subtable_prefix = prefix;

			/*
			 * Calculate the subtable length.  If the codeword
			 * length exceeds 'table_bits' by n, then the subtable
			 * needs at least 2^n entries.  But it may need more; if
			 * there are fewer than 2^n codewords of length
			 * 'table_bits + n' remaining, then n will need to be
			 * incremented to bring in longer codewords until the
			 * subtable can be filled completely.  Note that it
			 * always will, eventually, be possible to fill the
			 * subtable, since it was previously verified that the
			 * code is complete.
			 */
			subtable_bits = codeword_len - table_bits;
			remainder     = (s32)1 << subtable_bits;
			for (;;) {
				remainder -=
					len_counts[table_bits + subtable_bits];
				if (remainder <= 0)
					break;
				subtable_bits++;
				remainder <<= 1;
			}

			/* Create the entry that points from the root table to
			 * the subtable.  This entry contains the index of the
			 * start of the subtable and the number of bits with
			 * which the subtable is indexed (the log base 2 of the
			 * number of entries it contains).  */
			decode_table[subtable_prefix] = MAKE_DECODE_TABLE_ENTRY(
				subtable_pos, subtable_bits);
		}

		/* Fill the subtable entries for this symbol. */
		u16 entry  = MAKE_DECODE_TABLE_ENTRY(sorted_syms[sym_idx],
                                                    codeword_len - table_bits);
		unsigned n = 1U
		             << (subtable_bits - (codeword_len - table_bits));
		do {
			decode_table[subtable_pos++] = entry;
		} while (--n);

		len_counts[codeword_len]--;
		codeword++;
	} while (++sym_idx < num_syms);

	return 0;
}
