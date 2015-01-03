/*
 * compress_common.c
 *
 * Code for compression shared among multiple compression formats.
 *
 * Author:  Eric Biggers
 * Year:    2012 - 2014
 *
 * The author dedicates this file to the public domain.
 * You can do whatever you want with this file.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "wimlib/compress_common.h"
#include "wimlib/util.h"

#include <string.h>

/* Given the binary tree node A[subtree_idx] whose children already
 * satisfy the maxheap property, swap the node with its greater child
 * until it is greater than both its children, so that the maxheap
 * property is satisfied in the subtree rooted at A[subtree_idx].  */
static void
heapify_subtree(u32 A[], unsigned length, unsigned subtree_idx)
{
	unsigned parent_idx;
	unsigned child_idx;
	u32 v;

	v = A[subtree_idx];
	parent_idx = subtree_idx;
	while ((child_idx = parent_idx * 2) <= length) {
		if (child_idx < length && A[child_idx + 1] > A[child_idx])
			child_idx++;
		if (v >= A[child_idx])
			break;
		A[parent_idx] = A[child_idx];
		parent_idx = child_idx;
	}
	A[parent_idx] = v;
}

/* Rearrange the array 'A' so that it satisfies the maxheap property.
 * 'A' uses 1-based indices, so the children of A[i] are A[i*2] and A[i*2 + 1].
 */
static void
heapify_array(u32 A[], unsigned length)
{
	for (unsigned subtree_idx = length / 2; subtree_idx >= 1; subtree_idx--)
		heapify_subtree(A, length, subtree_idx);
}

/* Sort the array 'A', which contains 'length' unsigned 32-bit integers.  */
static void
heapsort(u32 A[], unsigned length)
{
	A--; /* Use 1-based indices  */

	heapify_array(A, length);

	while (length >= 2) {
		swap(A[1], A[length]);
		length--;
		heapify_subtree(A, length, 1);
	}
}

#define NUM_SYMBOL_BITS 10
#define SYMBOL_MASK ((1 << NUM_SYMBOL_BITS) - 1)

/*
 * Sort the symbols primarily by frequency and secondarily by symbol
 * value.  Discard symbols with zero frequency and fill in an array with
 * the remaining symbols, along with their frequencies.  The low
 * NUM_SYMBOL_BITS bits of each array entry will contain the symbol
 * value, and the remaining bits will contain the frequency.
 *
 * @num_syms
 *	Number of symbols in the alphabet.
 *	Can't be greater than (1 << NUM_SYMBOL_BITS).
 *
 * @freqs[num_syms]
 *	The frequency of each symbol.
 *
 * @lens[num_syms]
 *	An array that eventually will hold the length of each codeword.
 *	This function only fills in the codeword lengths for symbols that
 *	have zero frequency, which are not well defined per se but will
 *	be set to 0.
 *
 * @symout[num_syms]
 *	The output array, described above.
 *
 * Returns the number of entries in 'symout' that were filled.  This is
 * the number of symbols that have nonzero frequency.
 */
static unsigned
sort_symbols(unsigned num_syms, const u32 freqs[restrict],
	     u8 lens[restrict], u32 symout[restrict])
{
	unsigned num_used_syms;
	unsigned num_counters;

	/* We rely on heapsort, but with an added optimization.  Since
	 * it's common for most symbol frequencies to be low, we first do
	 * a count sort using a limited number of counters.  High
	 * frequencies will be counted in the last counter, and only they
	 * will be sorted with heapsort.
	 *
	 * Note: with more symbols, it is generally beneficial to have more
	 * counters.  About 1 counter per 4 symbols seems fast.
	 *
	 * Note: I also tested radix sort, but even for large symbol
	 * counts (> 255) and frequencies bounded at 16 bits (enabling
	 * radix sort by just two base-256 digits), it didn't seem any
	 * faster than the method implemented here.
	 *
	 * Note: I tested the optimized quicksort implementation from
	 * glibc (with indirection overhead removed), but it was only
	 * marginally faster than the simple heapsort implemented here.
	 *
	 * Tests were done with building the codes for LZX.  Results may
	 * vary for different compression algorithms...!  */

	num_counters = (DIV_ROUND_UP(num_syms, 4) + 3) & ~3;

	unsigned counters[num_counters];

	memset(counters, 0, sizeof(counters));

	/* Count the frequencies.  */
	for (unsigned sym = 0; sym < num_syms; sym++)
		counters[min(freqs[sym], num_counters - 1)]++;

	/* Make the counters cumulative, ignoring the zero-th, which
	 * counted symbols with zero frequency.  As a side effect, this
	 * calculates the number of symbols with nonzero frequency.  */
	num_used_syms = 0;
	for (unsigned i = 1; i < num_counters; i++) {
		unsigned count = counters[i];
		counters[i] = num_used_syms;
		num_used_syms += count;
	}

	/* Sort nonzero-frequency symbols using the counters.  At the
	 * same time, set the codeword lengths of zero-frequency symbols
	 * to 0.  */
	for (unsigned sym = 0; sym < num_syms; sym++) {
		u32 freq = freqs[sym];
		if (freq != 0) {
			symout[counters[min(freq, num_counters - 1)]++] =
				sym | (freq << NUM_SYMBOL_BITS);
		} else {
			lens[sym] = 0;
		}
	}

	/* Sort the symbols counted in the last counter.  */
	heapsort(symout + counters[num_counters - 2],
		 counters[num_counters - 1] - counters[num_counters - 2]);

	return num_used_syms;
}

/*
 * Build the Huffman tree.
 *
 * This is an optimized implementation that
 *	(a) takes advantage of the frequencies being already sorted;
 *	(b) only generates non-leaf nodes, since the non-leaf nodes of a
 *	    Huffman tree are sufficient to generate a canonical code;
 *	(c) Only stores parent pointers, not child pointers;
 *	(d) Produces the nodes in the same memory used for input
 *	    frequency information.
 *
 * Array 'A', which contains 'sym_count' entries, is used for both input
 * and output.  For this function, 'sym_count' must be at least 2.
 *
 * For input, the array must contain the frequencies of the symbols,
 * sorted in increasing order.  Specifically, each entry must contain a
 * frequency left shifted by NUM_SYMBOL_BITS bits.  Any data in the low
 * NUM_SYMBOL_BITS bits of the entries will be ignored by this function.
 * Although these bits will, in fact, contain the symbols that correspond
 * to the frequencies, this function is concerned with frequencies only
 * and keeps the symbols as-is.
 *
 * For output, this function will produce the non-leaf nodes of the
 * Huffman tree.  These nodes will be stored in the first (sym_count - 1)
 * entries of the array.  Entry A[sym_count - 2] will represent the root
 * node.  Each other node will contain the zero-based index of its parent
 * node in 'A', left shifted by NUM_SYMBOL_BITS bits.  The low
 * NUM_SYMBOL_BITS bits of each entry in A will be kept as-is.  Again,
 * note that although these low bits will, in fact, contain a symbol
 * value, this symbol will have *no relationship* with the Huffman tree
 * node that happens to occupy the same slot.  This is because this
 * implementation only generates the non-leaf nodes of the tree.
 */
static void
build_tree(u32 A[], unsigned sym_count)
{
	/* Index, in 'A', of next lowest frequency symbol that has not
	 * yet been processed.  */
	unsigned i = 0;

	/* Index, in 'A', of next lowest frequency parentless non-leaf
	 * node; or, if equal to 'e', then no such node exists yet.  */
	unsigned b = 0;

	/* Index, in 'A', of next node to allocate as a non-leaf.  */
	unsigned e = 0;

	do {
		unsigned m, n;
		u32 freq_shifted;

		/* Choose the two next lowest frequency entries.  */

		if (i != sym_count &&
		    (b == e || (A[i] >> NUM_SYMBOL_BITS) <= (A[b] >> NUM_SYMBOL_BITS)))
			m = i++;
		else
			m = b++;

		if (i != sym_count &&
		    (b == e || (A[i] >> NUM_SYMBOL_BITS) <= (A[b] >> NUM_SYMBOL_BITS)))
			n = i++;
		else
			n = b++;

		/* Allocate a non-leaf node and link the entries to it.
		 *
		 * If we link an entry that we're visiting for the first
		 * time (via index 'i'), then we're actually linking a
		 * leaf node and it will have no effect, since the leaf
		 * will be overwritten with a non-leaf when index 'e'
		 * catches up to it.  But it's not any slower to
		 * unconditionally set the parent index.
		 *
		 * We also compute the frequency of the non-leaf node as
		 * the sum of its two children's frequencies.  */

		freq_shifted = (A[m] & ~SYMBOL_MASK) + (A[n] & ~SYMBOL_MASK);

		A[m] = (A[m] & SYMBOL_MASK) | (e << NUM_SYMBOL_BITS);
		A[n] = (A[n] & SYMBOL_MASK) | (e << NUM_SYMBOL_BITS);
		A[e] = (A[e] & SYMBOL_MASK) | freq_shifted;
		e++;
	} while (sym_count - e > 1);
		/* When just one entry remains, it is a "leaf" that was
		 * linked to some other node.  We ignore it, since the
		 * rest of the array contains the non-leaves which we
		 * need.  (Note that we're assuming the cases with 0 or 1
		 * symbols were handled separately.) */
}

/*
 * Given the stripped-down Huffman tree constructed by build_tree(),
 * determine the number of codewords that should be assigned each
 * possible length, taking into account the length-limited constraint.
 *
 * @A
 *	The array produced by build_tree(), containing parent index
 *	information for the non-leaf nodes of the Huffman tree.  Each
 *	entry in this array is a node; a node's parent always has a
 *	greater index than that node itself.  This function will
 *	overwrite the parent index information in this array, so
 *	essentially it will destroy the tree.  However, the data in the
 *	low NUM_SYMBOL_BITS of each entry will be preserved.
 *
 * @root_idx
 *	The 0-based index of the root node in 'A', and consequently one
 *	less than the number of tree node entries in 'A'.  (Or, really 2
 *	less than the actual length of 'A'.)
 *
 * @len_counts
 *	An array of length ('max_codeword_len' + 1) in which the number of
 *	codewords having each length <= max_codeword_len will be
 *	returned.
 *
 * @max_codeword_len
 *	The maximum permissible codeword length.
 */
static void
compute_length_counts(u32 A[restrict], unsigned root_idx,
		      unsigned len_counts[restrict], unsigned max_codeword_len)
{
	/* The key observations are:
	 *
	 * (1) We can traverse the non-leaf nodes of the tree, always
	 * visiting a parent before its children, by simply iterating
	 * through the array in reverse order.  Consequently, we can
	 * compute the depth of each node in one pass, overwriting the
	 * parent indices with depths.
	 *
	 * (2) We can initially assume that in the real Huffman tree,
	 * both children of the root are leaves.  This corresponds to two
	 * codewords of length 1.  Then, whenever we visit a (non-leaf)
	 * node during the traversal, we modify this assumption to
	 * account for the current node *not* being a leaf, but rather
	 * its two children being leaves.  This causes the loss of one
	 * codeword for the current depth and the addition of two
	 * codewords for the current depth plus one.
	 *
	 * (3) We can handle the length-limited constraint fairly easily
	 * by simply using the largest length available when a depth
	 * exceeds max_codeword_len.
	 */

	for (unsigned len = 0; len <= max_codeword_len; len++)
		len_counts[len] = 0;
	len_counts[1] = 2;

	/* Set the root node's depth to 0.  */
	A[root_idx] &= SYMBOL_MASK;

	for (int node = root_idx - 1; node >= 0; node--) {

		/* Calculate the depth of this node.  */

		unsigned parent = A[node] >> NUM_SYMBOL_BITS;
		unsigned parent_depth = A[parent] >> NUM_SYMBOL_BITS;
		unsigned depth = parent_depth + 1;
		unsigned len = depth;

		/* Set the depth of this node so that it is available
		 * when its children (if any) are processed.  */

		A[node] = (A[node] & SYMBOL_MASK) | (depth << NUM_SYMBOL_BITS);

		/* If needed, decrease the length to meet the
		 * length-limited constraint.  This is not the optimal
		 * method for generating length-limited Huffman codes!
		 * But it should be good enough.  */
		if (len >= max_codeword_len) {
			len = max_codeword_len;
			do {
				len--;
			} while (len_counts[len] == 0);
		}

		/* Account for the fact that we have a non-leaf node at
		 * the current depth.  */
		len_counts[len]--;
		len_counts[len + 1] += 2;
	}
}

/*
 * Generate the codewords for a canonical Huffman code.
 *
 * @A
 *	The output array for codewords.  In addition, initially this
 *	array must contain the symbols, sorted primarily by frequency and
 *	secondarily by symbol value, in the low NUM_SYMBOL_BITS bits of
 *	each entry.
 *
 * @len
 *	Output array for codeword lengths.
 *
 * @len_counts
 *	An array that provides the number of codewords that will have
 *	each possible length <= max_codeword_len.
 *
 * @max_codeword_len
 *	Maximum length, in bits, of each codeword.
 *
 * @num_syms
 *	Number of symbols in the alphabet, including symbols with zero
 *	frequency.  This is the length of the 'A' and 'len' arrays.
 */
static void
gen_codewords(u32 A[restrict], u8 lens[restrict],
	      const unsigned len_counts[restrict],
	      unsigned max_codeword_len, unsigned num_syms)
{
	u32 next_codewords[max_codeword_len + 1];

	/* Given the number of codewords that will have each length,
	 * assign codeword lengths to symbols.  We do this by assigning
	 * the lengths in decreasing order to the symbols sorted
	 * primarily by increasing frequency and secondarily by
	 * increasing symbol value.  */
	for (unsigned i = 0, len = max_codeword_len; len >= 1; len--) {
		unsigned count = len_counts[len];
		while (count--)
			lens[A[i++] & SYMBOL_MASK] = len;
	}

	/* Generate the codewords themselves.  We initialize the
	 * 'next_codewords' array to provide the lexicographically first
	 * codeword of each length, then assign codewords in symbol
	 * order.  This produces a canonical code.  */
	next_codewords[0] = 0;
	next_codewords[1] = 0;
	for (unsigned len = 2; len <= max_codeword_len; len++)
		next_codewords[len] =
			(next_codewords[len - 1] + len_counts[len - 1]) << 1;

	for (unsigned sym = 0; sym < num_syms; sym++)
		A[sym] = next_codewords[lens[sym]]++;
}

/*
 * ---------------------------------------------------------------------
 *			make_canonical_huffman_code()
 * ---------------------------------------------------------------------
 *
 * Given an alphabet and the frequency of each symbol in it, construct a
 * length-limited canonical Huffman code.
 *
 * @num_syms
 *	The number of symbols in the alphabet.  The symbols are the
 *	integers in the range [0, num_syms - 1].  This parameter must be
 *	at least 2 and can't be greater than (1 << NUM_SYMBOL_BITS).
 *
 * @max_codeword_len
 *	The maximum permissible codeword length.
 *
 * @freqs
 *	An array of @num_syms entries, each of which specifies the
 *	frequency of the corresponding symbol.  It is valid for some,
 *	none, or all of the frequencies to be 0.
 *
 * @lens
 *	An array of @num_syms entries in which this function will return
 *	the length, in bits, of the codeword assigned to each symbol.
 *	Symbols with 0 frequency will not have codewords per se, but
 *	their entries in this array will be set to 0.  No lengths greater
 *	than @max_codeword_len will be assigned.
 *
 * @codewords
 *	An array of @num_syms entries in which this function will return
 *	the codeword for each symbol, right-justified and padded on the
 *	left with zeroes.  Codewords for symbols with 0 frequency will be
 *	undefined.
 *
 * ---------------------------------------------------------------------
 *
 * This function builds a length-limited canonical Huffman code.
 *
 * A length-limited Huffman code contains no codewords longer than some
 * specified length, and has exactly (with some algorithms) or
 * approximately (with the algorithm used here) the minimum weighted path
 * length from the root, given this constraint.
 *
 * A canonical Huffman code satisfies the properties that a longer
 * codeword never lexicographically precedes a shorter codeword, and the
 * lexicographic ordering of codewords of the same length is the same as
 * the lexicographic ordering of the corresponding symbols.  A canonical
 * Huffman code, or more generally a canonical prefix code, can be
 * reconstructed from only a list containing the codeword length of each
 * symbol.
 *
 * The classic algorithm to generate a Huffman code creates a node for
 * each symbol, then inserts these nodes into a min-heap keyed by symbol
 * frequency.  Then, repeatedly, the two lowest-frequency nodes are
 * removed from the min-heap and added as the children of a new node
 * having frequency equal to the sum of its two children, which is then
 * inserted into the min-heap.  When only a single node remains in the
 * min-heap, it is the root of the Huffman tree.  The codeword for each
 * symbol is determined by the path needed to reach the corresponding
 * node from the root.  Descending to the left child appends a 0 bit,
 * whereas descending to the right child appends a 1 bit.
 *
 * The classic algorithm is relatively easy to understand, but it is
 * subject to a number of inefficiencies.  In practice, it is fastest to
 * first sort the symbols by frequency.  (This itself can be subject to
 * an optimization based on the fact that most frequencies tend to be
 * low.)  At the same time, we sort secondarily by symbol value, which
 * aids the process of generating a canonical code.  Then, during tree
 * construction, no heap is necessary because both the leaf nodes and the
 * unparented non-leaf nodes can be easily maintained in sorted order.
 * Consequently, there can never be more than two possibilities for the
 * next-lowest-frequency node.
 *
 * In addition, because we're generating a canonical code, we actually
 * don't need the leaf nodes of the tree at all, only the non-leaf nodes.
 * This is because for canonical code generation we don't need to know
 * where the symbols are in the tree.  Rather, we only need to know how
 * many leaf nodes have each depth (codeword length).  And this
 * information can, in fact, be quickly generated from the tree of
 * non-leaves only.
 *
 * Furthermore, we can build this stripped-down Huffman tree directly in
 * the array in which the codewords are to be generated, provided that
 * these array slots are large enough to hold a symbol and frequency
 * value.
 *
 * Still furthermore, we don't even need to maintain explicit child
 * pointers.  We only need the parent pointers, and even those can be
 * overwritten in-place with depth information as part of the process of
 * extracting codeword lengths from the tree.  So in summary, we do NOT
 * need a big structure like:
 *
 *	struct huffman_tree_node {
 *		unsigned int symbol;
 *		unsigned int frequency;
 *		unsigned int depth;
 *		struct huffman_tree_node *left_child;
 *		struct huffman_tree_node *right_child;
 *	};
 *
 *
 *   ... which often gets used in "naive" implementations of Huffman code
 *   generation.
 *
 * Most of these optimizations are based on the implementation in 7-Zip
 * (source file: C/HuffEnc.c), which has been placed in the public domain
 * by Igor Pavlov.  But I've rewritten the code with extensive comments,
 * as it took me a while to figure out what it was doing...!
 *
 * ---------------------------------------------------------------------
 *
 * NOTE: in general, the same frequencies can be used to generate
 * different length-limited canonical Huffman codes.  One choice we have
 * is during tree construction, when we must decide whether to prefer a
 * leaf or non-leaf when there is a tie in frequency.  Another choice we
 * have is how to deal with codewords that would exceed @max_codeword_len
 * bits in length.  Both of these choices affect the resulting codeword
 * lengths, which otherwise can be mapped uniquely onto the resulting
 * canonical Huffman code.
 *
 * Normally, there is no problem with choosing one valid code over
 * another, provided that they produce similar compression ratios.
 * However, the LZMS compression format uses adaptive Huffman coding.  It
 * requires that both the decompressor and compressor build a canonical
 * code equivalent to that which can be generated by using the classic
 * Huffman tree construction algorithm and always processing leaves
 * before non-leaves when there is a frequency tie.  Therefore, we make
 * sure to do this.  This method also has the advantage of sometimes
 * shortening the longest codeword that is generated.
 *
 * There also is the issue of how codewords longer than @max_codeword_len
 * are dealt with.  Fortunately, for LZMS this is irrelevant because
 * because for the LZMS alphabets no codeword can ever exceed
 * LZMS_MAX_CODEWORD_LEN (= 15).  Since the LZMS algorithm regularly
 * halves all frequencies, the frequencies cannot become high enough for
 * a length 16 codeword to be generated.  Specifically, I think that if
 * ties are broken in favor of non-leaves (as we do), the lowest total
 * frequency that would give a length-16 codeword would be the sum of the
 * frequencies 1 1 1 3 4 7 11 18 29 47 76 123 199 322 521 843 1364, which
 * is 3570.  And in LZMS we can't get a frequency that high based on the
 * alphabet sizes, rebuild frequencies, and scaling factors.  This
 * worst-case scenario is based on the following degenerate case (only
 * the bottom of the tree shown):
 *
 *                          ...
 *                        17
 *                       /  \
 *                      10   7
 *                     / \
 *                    6   4
 *                   / \
 *                  3   3
 *                 / \
 *                2   1
 *               / \
 *              1   1
 *
 * Excluding the first leaves (those with value 1), each leaf value must
 * be greater than the non-leaf up 1 and down 2 from it; otherwise that
 * leaf would have taken precedence over that non-leaf and been combined
 * with the leaf below, thereby decreasing the height compared to that
 * shown.
 *
 * Interesting fact: if we were to instead prioritize non-leaves over
 * leaves, then the worst case frequencies would be the Fibonacci
 * sequence, plus an extra frequency of 1.  In this hypothetical
 * scenario, it would be slightly easier for longer codewords to be
 * generated.
 */
void
make_canonical_huffman_code(unsigned num_syms, unsigned max_codeword_len,
			    const u32 freqs[restrict],
			    u8 lens[restrict], u32 codewords[restrict])
{
	u32 *A = codewords;
	unsigned num_used_syms;

	/* We begin by sorting the symbols primarily by frequency and
	 * secondarily by symbol value.  As an optimization, the array
	 * used for this purpose ('A') shares storage with the space in
	 * which we will eventually return the codewords.  */

	num_used_syms = sort_symbols(num_syms, freqs, lens, A);

	/* 'num_used_syms' is the number of symbols with nonzero
	 * frequency.  This may be less than @num_syms.  'num_used_syms'
	 * is also the number of entries in 'A' that are valid.  Each
	 * entry consists of a distinct symbol and a nonzero frequency
	 * packed into a 32-bit integer.  */

	/* Handle special cases where only 0 or 1 symbols were used (had
	 * nonzero frequency).  */

	if (unlikely(num_used_syms == 0)) {
		/* Code is empty.  sort_symbols() already set all lengths
		 * to 0, so there is nothing more to do.  */
		return;
	}

	if (unlikely(num_used_syms == 1)) {
		/* Only one symbol was used, so we only need one
		 * codeword.  But two codewords are needed to form the
		 * smallest complete Huffman code, which uses codewords 0
		 * and 1.  Therefore, we choose another symbol to which
		 * to assign a codeword.  We use 0 (if the used symbol is
		 * not 0) or 1 (if the used symbol is 0).  In either
		 * case, the lesser-valued symbol must be assigned
		 * codeword 0 so that the resulting code is canonical.  */

		unsigned sym = A[0] & SYMBOL_MASK;
		unsigned nonzero_idx = sym ? sym : 1;

		codewords[0] = 0;
		lens[0] = 1;
		codewords[nonzero_idx] = 1;
		lens[nonzero_idx] = 1;
		return;
	}

	/* Build a stripped-down version of the Huffman tree, sharing the
	 * array 'A' with the symbol values.  Then extract length counts
	 * from the tree and use them to generate the final codewords.  */

	build_tree(A, num_used_syms);

	{
		unsigned len_counts[max_codeword_len + 1];

		compute_length_counts(A, num_used_syms - 2,
				      len_counts, max_codeword_len);

		gen_codewords(A, lens, len_counts, max_codeword_len, num_syms);
	}
}
