/*
 * dentry.c - see description below
 */

/*
 * Copyright (C) 2012, 2013, 2014, 2015 Eric Biggers
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option) any
 * later version.
 *
 * This file is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this file; if not, see http://www.gnu.org/licenses/.
 */

/*
 * This file contains logic to deal with WIM directory entries, or "dentries":
 *
 *  - Reading a dentry tree from a metadata resource in a WIM file
 *  - Writing a dentry tree to a metadata resource in a WIM file
 *  - Iterating through a tree of WIM dentries
 *  - Path lookup: translating a path into a WIM dentry or inode
 *  - Creating, modifying, and deleting WIM dentries
 *
 * Notes:
 *
 *  - A WIM file can contain multiple images, each of which has an independent
 *    tree of dentries.  "On disk", the dentry tree for an image is stored in
 *    the "metadata resource" for that image.
 *
 *  - Multiple dentries in an image may correspond to the same inode, or "file".
 *    When this occurs, it means that the file has multiple names, or "hard
 *    links".  A dentry is not a file, but rather the name of a file!
 *
 *  - Inodes are not represented explicitly in the WIM file format.  Instead,
 *    the metadata resource provides a "hard link group ID" for each dentry.
 *    wimlib handles pulling out actual inodes from this information, but this
 *    occurs in inode_fixup.c and not in this file.
 *
 *  - wimlib does not allow *directory* hard links, so a WIM image really does
 *    have a *tree* of dentries (and not an arbitrary graph of dentries).
 *
 *  - wimlib indexes dentries both case-insensitively and case-sensitively,
 *    allowing either behavior to be used for path lookup.
 *
 *  - Multiple dentries in a directory might have the same case-insensitive
 *    name.  But wimlib enforces that at most one dentry in a directory can have
 *    a given case-sensitive name.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <errno.h>

#include "wimlib/assert.h"
#include "wimlib/dentry.h"
#include "wimlib/inode.h"
#include "wimlib/encoding.h"
#include "wimlib/endianness.h"
#include "wimlib/metadata.h"
#include "wimlib/paths.h"

/* On-disk format of a WIM dentry (directory entry), located in the metadata
 * resource for a WIM image.  */
struct wim_dentry_on_disk {

	/* Length of this directory entry in bytes, not including any extra
	 * stream entries.  Should be a multiple of 8 so that the following
	 * dentry or extra stream entry is aligned on an 8-byte boundary.  (If
	 * not, wimlib will round it up.)  It must be at least as long as the
	 * fixed-length fields of the dentry (WIM_DENTRY_DISK_SIZE), plus the
	 * lengths of the file name and/or short name if present, plus the size
	 * of any "extra" data.
	 *
	 * It is also possible for this field to be 0.  This case indicates the
	 * end of a list of sibling entries in a directory.  It also means the
	 * real length is 8, because the dentry included only the length field,
	 * but that takes up 8 bytes.  */
	le64 length;

	/* File attributes for the file or directory.  This is a bitwise OR of
	 * the FILE_ATTRIBUTE_* constants and should correspond to the value
	 * retrieved by GetFileAttributes() on Windows. */
	le32 attributes;

	/* A value that specifies the security descriptor for this file or
	 * directory.  If -1, the file or directory has no security descriptor.
	 * Otherwise, it is a 0-based index into the WIM image's table of
	 * security descriptors (see: `struct wim_security_data') */
	sle32 security_id;

	/* Offset, in bytes, from the start of the uncompressed metadata
	 * resource of this directory's child directory entries, or 0 if this
	 * directory entry does not correspond to a directory or otherwise does
	 * not have any children. */
	le64 subdir_offset;

	/* Reserved fields */
	le64 unused_1;
	le64 unused_2;

	/* Creation time, last access time, and last write time, in
	 * 100-nanosecond intervals since 12:00 a.m UTC January 1, 1601.  They
	 * should correspond to the times gotten by calling GetFileTime() on
	 * Windows. */
	le64 creation_time;
	le64 last_access_time;
	le64 last_write_time;

	/*
	 * Usually this is the SHA-1 message digest of the file's "contents"
	 * (the unnamed data stream).
	 *
	 * If the file has FILE_ATTRIBUTE_REPARSE_POINT set, then this is
	 * instead usually the SHA-1 message digest of the uncompressed reparse
	 * point data.
	 *
	 * However, there are some special rules that need to be applied to
	 * interpret this field correctly when extra stream entries are present.
	 * See the code for details.
	 */
	u8 default_hash[SHA1_HASH_SIZE];

	/* The format of the following data is not yet completely known and they
	 * do not correspond to Microsoft's documentation.
	 *
	 * If this directory entry is for a reparse point (has
	 * FILE_ATTRIBUTE_REPARSE_POINT set in the 'attributes' field), then the
	 * version of the following fields containing the reparse tag is valid.
	 * Furthermore, the field notated as not_rpfixed, as far as I can tell,
	 * is supposed to be set to 1 if reparse point fixups (a.k.a. fixing the
	 * targets of absolute symbolic links) were *not* done, and otherwise 0.
	 *
	 * If this directory entry is not for a reparse point, then the version
	 * of the following fields containing the hard_link_group_id is valid.
	 * All MS says about this field is that "If this file is part of a hard
	 * link set, all the directory entries in the set will share the same
	 * value in this field.".  However, more specifically I have observed
	 * the following:
	 *    - If the file is part of a hard link set of size 1, then the
	 *    hard_link_group_id should be set to either 0, which is treated
	 *    specially as indicating "not hardlinked", or any unique value.
	 *    - The specific nonzero values used to identity hard link sets do
	 *    not matter, as long as they are unique.
	 *    - However, due to bugs in Microsoft's software, it is actually NOT
	 *    guaranteed that directory entries that share the same hard link
	 *    group ID are actually hard linked to each either.  See
	 *    inode_fixup.c for the code that handles this.
	 */
	union {
		struct {
			le32 rp_unknown_1;
			le32 reparse_tag;
			le16 rp_unknown_2;
			le16 not_rpfixed;
		} _packed_attribute reparse;
		struct {
			le32 rp_unknown_1;
			le64 hard_link_group_id;
		} _packed_attribute nonreparse;
	};

	/* Number of extra stream entries that directly follow this dentry
	 * on-disk.  */
	le16 num_extra_streams;

	/* If nonzero, this is the length, in bytes, of this dentry's UTF-16LE
	 * encoded short name (8.3 DOS-compatible name), excluding the null
	 * terminator.  If zero, then the long name of this dentry does not have
	 * a corresponding short name (but this does not exclude the possibility
	 * that another dentry for the same file has a short name).  */
	le16 short_name_nbytes;

	/* If nonzero, this is the length, in bytes, of this dentry's UTF-16LE
	 * encoded "long" name, excluding the null terminator.  If zero, then
	 * this file has no long name.  The root dentry should not have a long
	 * name, but all other dentries in the image should have long names.  */
	le16 file_name_nbytes;

	/* Beginning of optional, variable-length fields  */

	/* If file_name_nbytes != 0, the next field will be the UTF-16LE encoded
	 * long file name.  This will be null-terminated, so the size of this
	 * field will really be file_name_nbytes + 2.  */
	/*utf16lechar file_name[];*/

	/* If short_name_nbytes != 0, the next field will be the UTF-16LE
	 * encoded short name.  This will be null-terminated, so the size of
	 * this field will really be short_name_nbytes + 2.  */
	/*utf16lechar short_name[];*/

	/* If there is still space in the dentry (according to the 'length'
	 * field) after 8-byte alignment, then the remaining space will be a
	 * variable-length list of tagged metadata items.  See tagged_items.c
	 * for more information.  */
	/* u8 tagged_items[] _aligned_attribute(8); */

} _packed_attribute;
	/* If num_extra_streams != 0, then there are that many extra stream
	 * entries following the dentry, starting on the next 8-byte aligned
	 * boundary.  They are not counted in the 'length' field of the dentry.
	 */

/* On-disk format of an extra stream entry.  This represents an extra NTFS-style
 * "stream" associated with the file, such as a named data stream.  */
struct wim_extra_stream_entry_on_disk {

	/* Length of this extra stream entry, in bytes.  This includes all
	 * fixed-length fields, plus the name and null terminator if present,
	 * and any needed padding such that the length is a multiple of 8.  */
	le64 length;

	/* Reserved field  */
	le64 reserved;

	/* SHA-1 message digest of this stream's uncompressed data, or all
	 * zeroes if this stream's data is of zero length.  */
	u8 hash[SHA1_HASH_SIZE];

	/* Length of this stream's name, in bytes and excluding the null
	 * terminator; or 0 if this stream is unnamed.  */
	le16 name_nbytes;

	/* Stream name in UTF-16LE.  It is @name_nbytes bytes long, excluding
	 * the null terminator.  There is a null terminator character if
	 * @name_nbytes != 0; i.e., if this stream is named.  */
	utf16lechar name[];
} _packed_attribute;

static void
do_dentry_set_name(struct wim_dentry *dentry, utf16lechar *file_name,
		   size_t file_name_nbytes)
{
	FREE(dentry->file_name);
	dentry->file_name = file_name;
	dentry->file_name_nbytes = file_name_nbytes;

	if (dentry_has_short_name(dentry)) {
		FREE(dentry->short_name);
		dentry->short_name = NULL;
		dentry->short_name_nbytes = 0;
	}
}

/*
 * Set the name of a WIM dentry from a UTF-16LE string.
 *
 * This sets the long name of the dentry.  The short name will automatically be
 * removed, since it may not be appropriate for the new long name.
 *
 * The @name string need not be null-terminated, since its length is specified
 * in @name_nbytes.
 *
 * If @name_nbytes is 0, both the long and short names of the dentry will be
 * removed.
 *
 * Only use this function on unlinked dentries, since it doesn't update the name
 * indices.  For dentries that are currently linked into the tree, use
 * rename_wim_path().
 *
 * Returns 0 or WIMLIB_ERR_NOMEM.
 */
int
dentry_set_name_utf16le(struct wim_dentry *dentry, const utf16lechar *name,
			size_t name_nbytes)
{
	utf16lechar *dup = NULL;

	if (name_nbytes) {
		dup = utf16le_dupz(name, name_nbytes);
		if (!dup)
			return WIMLIB_ERR_NOMEM;
	}
	do_dentry_set_name(dentry, dup, name_nbytes);
	return 0;
}


/*
 * Set the name of a WIM dentry from a 'tchar' string.
 *
 * This sets the long name of the dentry.  The short name will automatically be
 * removed, since it may not be appropriate for the new long name.
 *
 * If @name is NULL or empty, both the long and short names of the dentry will
 * be removed.
 *
 * Only use this function on unlinked dentries, since it doesn't update the name
 * indices.  For dentries that are currently linked into the tree, use
 * rename_wim_path().
 *
 * Returns 0 or an error code resulting from a failed string conversion.
 */
int
dentry_set_name(struct wim_dentry *dentry, const tchar *name)
{
	utf16lechar *name_utf16le = NULL;
	size_t name_utf16le_nbytes = 0;
	int ret;

	if (name && *name) {
		ret = tstr_to_utf16le(name, tstrlen(name) * sizeof(tchar),
				      &name_utf16le, &name_utf16le_nbytes);
		if (ret)
			return ret;
	}

	do_dentry_set_name(dentry, name_utf16le, name_utf16le_nbytes);
	return 0;
}

/* Calculate the minimum unaligned length, in bytes, of an on-disk WIM dentry
 * that has names of the specified lengths.  (Zero length means the
 * corresponding name actually does not exist.)  The returned value excludes
 * tagged metadata items as well as any extra stream entries that may need to
 * follow the dentry.  */
static size_t
dentry_min_len_with_names(u16 file_name_nbytes, u16 short_name_nbytes)
{
	size_t length = sizeof(struct wim_dentry_on_disk);
	if (file_name_nbytes)
		length += (u32)file_name_nbytes + 2;
	if (short_name_nbytes)
		length += (u32)short_name_nbytes + 2;
	return length;
}


/* Return the length, in bytes, required for the specified stream on-disk, when
 * represented as an extra stream entry.  */
static size_t
stream_out_total_length(const struct wim_inode_stream *strm)
{
	/* Account for the fixed length portion  */
	size_t len = sizeof(struct wim_extra_stream_entry_on_disk);

	/* For named streams, account for the variable-length name.  */
	if (stream_is_named(strm))
		len += utf16le_len_bytes(strm->stream_name) + 2;

	/* Account for any necessary padding to the next 8-byte boundary.  */
	return (len + 7) & ~7;
}

/*
 * Calculate the total number of bytes that will be consumed when a dentry is
 * written.  This includes the fixed-length portion of the dentry, the name
 * fields, any tagged metadata items, and any extra stream entries.  This also
 * includes all alignment bytes.
 */
size_t
dentry_out_total_length(const struct wim_dentry *dentry)
{
	const struct wim_inode *inode = dentry->d_inode;
	size_t len;

	len = dentry_min_len_with_names(dentry->file_name_nbytes,
					dentry->short_name_nbytes);
	len = (len + 7) & ~7;

	if (inode->i_extra_size) {
		len += inode->i_extra_size;
		len = (len + 7) & ~7;
	}

	if (!(inode->i_attributes & FILE_ATTRIBUTE_ENCRYPTED)) {
		/*
		 * Extra stream entries:
		 *
		 * - Use one extra stream entry for each named data stream
		 * - Use one extra stream entry for the unnamed data stream when there is either:
		 *	- a reparse point stream
		 *	- at least one named data stream (for Windows PE bug workaround)
		 * - Use one extra stream entry for the reparse point stream if there is one
		 */
		bool have_named_data_stream = false;
		bool have_reparse_point_stream = false;
		for (unsigned i = 0; i < inode->i_num_streams; i++) {
			const struct wim_inode_stream *strm = &inode->i_streams[i];
			if (stream_is_named_data_stream(strm)) {
				len += stream_out_total_length(strm);
				have_named_data_stream = true;
			} else if (strm->stream_type == STREAM_TYPE_REPARSE_POINT) {
				wimlib_assert(inode->i_attributes & FILE_ATTRIBUTE_REPARSE_POINT);
				have_reparse_point_stream = true;
			}
		}

		if (have_named_data_stream || have_reparse_point_stream) {
			if (have_reparse_point_stream)
				len += (sizeof(struct wim_extra_stream_entry_on_disk) + 7) & ~7;
			len += (sizeof(struct wim_extra_stream_entry_on_disk) + 7) & ~7;
		}
	}

	return len;
}

/* Internal version of for_dentry_in_tree() that omits the NULL check  */
static int
do_for_dentry_in_tree(struct wim_dentry *dentry,
		      int (*visitor)(struct wim_dentry *, void *), void *arg)
{
	int ret;
	struct wim_dentry *child;

	ret = (*visitor)(dentry, arg);
	if (unlikely(ret))
		return ret;

	for_dentry_child(child, dentry) {
		ret = do_for_dentry_in_tree(child, visitor, arg);
		if (unlikely(ret))
			return ret;
	}
	return 0;
}

/* Internal version of for_dentry_in_tree_depth() that omits the NULL check  */
static int
do_for_dentry_in_tree_depth(struct wim_dentry *dentry,
			    int (*visitor)(struct wim_dentry *, void *), void *arg)
{
	int ret;
	struct wim_dentry *child;

	for_dentry_child_postorder(child, dentry) {
		ret = do_for_dentry_in_tree_depth(child, visitor, arg);
		if (unlikely(ret))
			return ret;
	}
	return unlikely((*visitor)(dentry, arg));
}

/*
 * Call a function on all dentries in a tree.
 *
 * @arg will be passed as the second argument to each invocation of @visitor.
 *
 * This function does a pre-order traversal --- that is, a parent will be
 * visited before its children.  It also will visit siblings in order of
 * case-sensitive filename.  Equivalently, this function visits the entire tree
 * in the case-sensitive lexicographic order of the full paths.
 *
 * It is safe to pass NULL for @root, which means that the dentry tree is empty.
 * In this case, this function does nothing.
 *
 * @visitor must not modify the structure of the dentry tree during the
 * traversal.
 *
 * The return value will be 0 if all calls to @visitor returned 0.  Otherwise,
 * the return value will be the first nonzero value returned by @visitor.
 */
int
for_dentry_in_tree(struct wim_dentry *root,
		   int (*visitor)(struct wim_dentry *, void *), void *arg)
{
	if (unlikely(!root))
		return 0;
	return do_for_dentry_in_tree(root, visitor, arg);
}

/* Like for_dentry_in_tree(), but do a depth-first traversal of the dentry tree.
 * That is, the visitor function will be called on a dentry's children before
 * itself.  It will be safe to free a dentry when visiting it.  */
static int
for_dentry_in_tree_depth(struct wim_dentry *root,
			 int (*visitor)(struct wim_dentry *, void *), void *arg)
{
	if (unlikely(!root))
		return 0;
	return do_for_dentry_in_tree_depth(root, visitor, arg);
}

/*
 * Calculate the full path to @dentry within the WIM image, if not already done.
 *
 * The full name will be saved in the cached value 'dentry->_full_path'.
 *
 * Whenever possible, use dentry_full_path() instead of calling this and
 * accessing _full_path directly.
 *
 * Returns 0 or an error code resulting from a failed string conversion.
 */
int
calculate_dentry_full_path(struct wim_dentry *dentry)
{
	size_t ulen;
	size_t dummy;
	const struct wim_dentry *d;

	if (dentry->_full_path)
		return 0;

	ulen = 0;
	d = dentry;
	do {
		ulen += d->file_name_nbytes / sizeof(utf16lechar);
		ulen++;
		d = d->d_parent;  /* assumes d == d->d_parent for root  */
	} while (!dentry_is_root(d));

	utf16lechar ubuf[ulen];
	utf16lechar *p = &ubuf[ulen];

	d = dentry;
	do {
		p -= d->file_name_nbytes / sizeof(utf16lechar);
		memcpy(p, d->file_name, d->file_name_nbytes);
		*--p = cpu_to_le16(WIM_PATH_SEPARATOR);
		d = d->d_parent;  /* assumes d == d->d_parent for root  */
	} while (!dentry_is_root(d));

	wimlib_assert(p == ubuf);

	return utf16le_to_tstr(ubuf, ulen * sizeof(utf16lechar),
			       &dentry->_full_path, &dummy);
}

/*
 * Return the full path to the @dentry within the WIM image, or NULL if the full
 * path could not be determined due to a string conversion error.
 *
 * The returned memory will be cached in the dentry, so the caller is not
 * responsible for freeing it.
 */
tchar *
dentry_full_path(struct wim_dentry *dentry)
{
	calculate_dentry_full_path(dentry);
	return dentry->_full_path;
}

static int
dentry_calculate_subdir_offset(struct wim_dentry *dentry, void *_subdir_offset_p)
{
	if (dentry_is_directory(dentry)) {
		u64 *subdir_offset_p = _subdir_offset_p;
		struct wim_dentry *child;

		/* Set offset of directory's child dentries  */
		dentry->subdir_offset = *subdir_offset_p;

		/* Account for child dentries  */
		for_dentry_child(child, dentry)
			*subdir_offset_p += dentry_out_total_length(child);

		/* Account for end-of-directory entry  */
		*subdir_offset_p += 8;
	} else {
		/* Not a directory; set subdir_offset to 0  */
		dentry->subdir_offset = 0;
	}
	return 0;
}

/*
 * Calculate the subdir offsets for a dentry tree, in preparation of writing
 * that dentry tree to a metadata resource.
 *
 * The subdir offset of each dentry is the offset in the uncompressed metadata
 * resource at which its child dentries begin, or 0 if that dentry has no
 * children.
 *
 * The caller must initialize *subdir_offset_p to the first subdir offset that
 * is available to use after the root dentry is written.
 *
 * When this function returns, *subdir_offset_p will have been advanced past the
 * size needed for the dentry tree within the uncompressed metadata resource.
 */
void
calculate_subdir_offsets(struct wim_dentry *root, u64 *subdir_offset_p)
{
	for_dentry_in_tree(root, dentry_calculate_subdir_offset, subdir_offset_p);
}

/* Compare the UTF-16LE long filenames of two dentries case insensitively.  */
static int
dentry_compare_names_case_insensitive(const struct wim_dentry *d1,
				      const struct wim_dentry *d2)
{
	return cmp_utf16le_strings(d1->file_name,
				   d1->file_name_nbytes / 2,
				   d2->file_name,
				   d2->file_name_nbytes / 2,
				   true);
}

/* Compare the UTF-16LE long filenames of two dentries case sensitively.  */
static int
dentry_compare_names_case_sensitive(const struct wim_dentry *d1,
				    const struct wim_dentry *d2)
{
	return cmp_utf16le_strings(d1->file_name,
				   d1->file_name_nbytes / 2,
				   d2->file_name,
				   d2->file_name_nbytes / 2,
				   false);
}

static int
_avl_dentry_compare_names_ci(const struct avl_tree_node *n1,
			     const struct avl_tree_node *n2)
{
	const struct wim_dentry *d1, *d2;

	d1 = avl_tree_entry(n1, struct wim_dentry, d_index_node_ci);
	d2 = avl_tree_entry(n2, struct wim_dentry, d_index_node_ci);
	return dentry_compare_names_case_insensitive(d1, d2);
}

static int
_avl_dentry_compare_names(const struct avl_tree_node *n1,
			  const struct avl_tree_node *n2)
{
	const struct wim_dentry *d1, *d2;

	d1 = avl_tree_entry(n1, struct wim_dentry, d_index_node);
	d2 = avl_tree_entry(n2, struct wim_dentry, d_index_node);
	return dentry_compare_names_case_sensitive(d1, d2);
}

/* Default case sensitivity behavior for searches with
 * WIMLIB_CASE_PLATFORM_DEFAULT specified.  This can be modified by passing
 * WIMLIB_INIT_FLAG_DEFAULT_CASE_SENSITIVE or
 * WIMLIB_INIT_FLAG_DEFAULT_CASE_INSENSITIVE to wimlib_global_init().  */
bool default_ignore_case =
#ifdef __WIN32__
	true
#else
	false
#endif
;

/* Case-sensitive dentry lookup.  Only @file_name and @file_name_nbytes of
 * @dummy must be valid.  */
static struct wim_dentry *
dir_lookup(const struct wim_inode *dir, const struct wim_dentry *dummy)
{
	struct avl_tree_node *node;

	node = avl_tree_lookup_node(dir->i_children,
				    &dummy->d_index_node,
				    _avl_dentry_compare_names);
	if (!node)
		return NULL;
	return avl_tree_entry(node, struct wim_dentry, d_index_node);
}

/* Case-insensitive dentry lookup.  Only @file_name and @file_name_nbytes of
 * @dummy must be valid.  */
static struct wim_dentry *
dir_lookup_ci(const struct wim_inode *dir, const struct wim_dentry *dummy)
{
	struct avl_tree_node *node;

	node = avl_tree_lookup_node(dir->i_children_ci,
				    &dummy->d_index_node_ci,
				    _avl_dentry_compare_names_ci);
	if (!node)
		return NULL;
	return avl_tree_entry(node, struct wim_dentry, d_index_node_ci);
}

/* Given a UTF-16LE filename and a directory, look up the dentry for the file.
 * Return it if found, otherwise NULL.  This has configurable case sensitivity,
 * and @name need not be null-terminated.  */
struct wim_dentry *
get_dentry_child_with_utf16le_name(const struct wim_dentry *dentry,
				   const utf16lechar *name,
				   size_t name_nbytes,
				   CASE_SENSITIVITY_TYPE case_ctype)
{
	const struct wim_inode *dir = dentry->d_inode;
	bool ignore_case = will_ignore_case(case_ctype);
	struct wim_dentry dummy;
	struct wim_dentry *child;

	dummy.file_name = (utf16lechar*)name;
	dummy.file_name_nbytes = name_nbytes;

	if (!ignore_case)
		/* Case-sensitive lookup.  */
		return dir_lookup(dir, &dummy);

	/* Case-insensitive lookup.  */

	child = dir_lookup_ci(dir, &dummy);
	if (!child)
		return NULL;

	if (likely(list_empty(&child->d_ci_conflict_list)))
		/* Only one dentry has this case-insensitive name; return it */
		return child;

	/* Multiple dentries have the same case-insensitive name.  Choose the
	 * dentry with the same case-sensitive name, if one exists; otherwise
	 * print a warning and choose one of the possible dentries arbitrarily.
	 */
	struct wim_dentry *alt = child;
	size_t num_alts = 0;

	do {
		num_alts++;
		if (!dentry_compare_names_case_sensitive(&dummy, alt))
			return alt;
		alt = list_entry(alt->d_ci_conflict_list.next,
				 struct wim_dentry, d_ci_conflict_list);
	} while (alt != child);

	WARNING("Result of case-insensitive lookup is ambiguous\n"
		"          (returning \"%"TS"\" of %zu "
		"possible files, including \"%"TS"\")",
		dentry_full_path(child),
		num_alts,
		dentry_full_path(list_entry(child->d_ci_conflict_list.next,
					    struct wim_dentry,
					    d_ci_conflict_list)));
	return child;
}

/* Given a 'tchar' filename and a directory, look up the dentry for the file.
 * If the filename was successfully converted to UTF-16LE and the dentry was
 * found, return it; otherwise return NULL.  This has configurable case
 * sensitivity.  */
struct wim_dentry *
get_dentry_child_with_name(const struct wim_dentry *dentry, const tchar *name,
			   CASE_SENSITIVITY_TYPE case_type)
{
	int ret;
	const utf16lechar *name_utf16le;
	size_t name_utf16le_nbytes;
	struct wim_dentry *child;

	ret = tstr_get_utf16le_and_len(name, &name_utf16le,
				       &name_utf16le_nbytes);
	if (ret)
		return NULL;

	child = get_dentry_child_with_utf16le_name(dentry,
						   name_utf16le,
						   name_utf16le_nbytes,
						   case_type);
	tstr_put_utf16le(name_utf16le);
	return child;
}

/* This is the UTF-16LE version of get_dentry(), currently private to this file
 * because no one needs it besides get_dentry().  */
static struct wim_dentry *
get_dentry_utf16le(WIMStruct *wim, const utf16lechar *path,
		   CASE_SENSITIVITY_TYPE case_type)
{
	struct wim_dentry *cur_dentry;
	const utf16lechar *name_start, *name_end;

	/* Start with the root directory of the image.  Note: this will be NULL
	 * if an image has been added directly with wimlib_add_empty_image() but
	 * no files have been added yet; in that case we fail with ENOENT.  */
	cur_dentry = wim_get_current_root_dentry(wim);

	name_start = path;
	for (;;) {
		if (cur_dentry == NULL) {
			errno = ENOENT;
			return NULL;
		}

		if (*name_start && !dentry_is_directory(cur_dentry)) {
			errno = ENOTDIR;
			return NULL;
		}

		while (*name_start == cpu_to_le16(WIM_PATH_SEPARATOR))
			name_start++;

		if (!*name_start)
			return cur_dentry;

		name_end = name_start;
		do {
			++name_end;
		} while (*name_end != cpu_to_le16(WIM_PATH_SEPARATOR) && *name_end);

		cur_dentry = get_dentry_child_with_utf16le_name(cur_dentry,
								name_start,
								(u8*)name_end - (u8*)name_start,
								case_type);
		name_start = name_end;
	}
}

/*
 * WIM path lookup: translate a path in the currently selected WIM image to the
 * corresponding dentry, if it exists.
 *
 * @wim
 *	The WIMStruct for the WIM.  The search takes place in the currently
 *	selected image.
 *
 * @path
 *	The path to look up, given relative to the root of the WIM image.
 *	Characters with value WIM_PATH_SEPARATOR are taken to be path
 *	separators.  Leading path separators are ignored, whereas one or more
 *	trailing path separators cause the path to only match a directory.
 *
 * @case_type
 *	The case-sensitivity behavior of this function, as one of the following
 *	constants:
 *
 *    - WIMLIB_CASE_SENSITIVE:  Perform the search case sensitively.  This means
 *	that names must match exactly.
 *
 *    - WIMLIB_CASE_INSENSITIVE:  Perform the search case insensitively.  This
 *	means that names are considered to match if they are equal when
 *	transformed to upper case.  If a path component matches multiple names
 *	case-insensitively, the name that matches the path component
 *	case-sensitively is chosen, if existent; otherwise one
 *	case-insensitively matching name is chosen arbitrarily.
 *
 *    - WIMLIB_CASE_PLATFORM_DEFAULT:  Perform either case-sensitive or
 *	case-insensitive search, depending on the value of the global variable
 *	default_ignore_case.
 *
 *    In any case, no Unicode normalization is done before comparing strings.
 *
 * Returns a pointer to the dentry that is the result of the lookup, or NULL if
 * no such dentry exists.  If NULL is returned, errno is set to one of the
 * following values:
 *
 *	ENOTDIR if one of the path components used as a directory existed but
 *	was not, in fact, a directory.
 *
 *	ENOENT otherwise.
 *
 * Additional notes:
 *
 *    - This function does not consider a reparse point to be a directory, even
 *	if it has FILE_ATTRIBUTE_DIRECTORY set.
 *
 *    - This function does not dereference symbolic links or junction points
 *	when performing the search.
 *
 *    - Since this function ignores leading slashes, the empty path is valid and
 *	names the root directory of the WIM image.
 *
 *    - An image added with wimlib_add_empty_image() does not have a root
 *	directory yet, and this function will fail with ENOENT for any path on
 *	such an image.
 */
struct wim_dentry *
get_dentry(WIMStruct *wim, const tchar *path, CASE_SENSITIVITY_TYPE case_type)
{
	int ret;
	const utf16lechar *path_utf16le;
	struct wim_dentry *dentry;

	ret = tstr_get_utf16le(path, &path_utf16le);
	if (ret)
		return NULL;
	dentry = get_dentry_utf16le(wim, path_utf16le, case_type);
	tstr_put_utf16le(path_utf16le);
	return dentry;
}

/* Modify @path, which is a null-terminated string @len 'tchars' in length,
 * in-place to produce the path to its parent directory.  */
static void
to_parent_name(tchar *path, size_t len)
{
	ssize_t i = (ssize_t)len - 1;
	while (i >= 0 && path[i] == WIM_PATH_SEPARATOR)
		i--;
	while (i >= 0 && path[i] != WIM_PATH_SEPARATOR)
		i--;
	while (i >= 0 && path[i] == WIM_PATH_SEPARATOR)
		i--;
	path[i + 1] = T('\0');
}

/* Similar to get_dentry(), but returns the dentry named by @path with the last
 * component stripped off.
 *
 * Note: The returned dentry is NOT guaranteed to be a directory.  */
struct wim_dentry *
get_parent_dentry(WIMStruct *wim, const tchar *path,
		  CASE_SENSITIVITY_TYPE case_type)
{
	size_t path_len = tstrlen(path);
	tchar buf[path_len + 1];

	tmemcpy(buf, path, path_len + 1);
	to_parent_name(buf, path_len);
	return get_dentry(wim, buf, case_type);
}

/*
 * Create an unlinked dentry.
 *
 * @name specifies the long name to give the new dentry.  If NULL or empty, the
 * new dentry will be given no long name.
 *
 * The new dentry will have no short name and no associated inode.
 *
 * On success, returns 0 and a pointer to the new, allocated dentry is stored in
 * *dentry_ret.  On failure, returns WIMLIB_ERR_NOMEM or an error code resulting
 * from a failed string conversion.
 */
static int
new_dentry(const tchar *name, struct wim_dentry **dentry_ret)
{
	struct wim_dentry *dentry;
	int ret;

	dentry = CALLOC(1, sizeof(struct wim_dentry));
	if (!dentry)
		return WIMLIB_ERR_NOMEM;

	if (name && *name) {
		ret = dentry_set_name(dentry, name);
		if (ret) {
			FREE(dentry);
			return ret;
		}
	}
	dentry->d_parent = dentry;
	*dentry_ret = dentry;
	return 0;
}

/* Like new_dentry(), but also allocate an inode and associate it with the
 * dentry.  If set_timestamps=true, the timestamps for the inode will be set to
 * the current time; otherwise, they will be left 0.  */
int
new_dentry_with_new_inode(const tchar *name, bool set_timestamps,
			  struct wim_dentry **dentry_ret)
{
	struct wim_dentry *dentry;
	struct wim_inode *inode;
	int ret;

	ret = new_dentry(name, &dentry);
	if (ret)
		return ret;

	inode = new_inode(dentry, set_timestamps);
	if (!inode) {
		free_dentry(dentry);
		return WIMLIB_ERR_NOMEM;
	}

	*dentry_ret = dentry;
	return 0;
}

/* Like new_dentry(), but also associate the new dentry with the specified inode
 * and acquire a reference to each of the inode's blobs.  */
int
new_dentry_with_existing_inode(const tchar *name, struct wim_inode *inode,
			       struct wim_dentry **dentry_ret)
{
	int ret = new_dentry(name, dentry_ret);
	if (ret)
		return ret;
	d_associate(*dentry_ret, inode);
	inode_ref_blobs(inode);
	return 0;
}

/* Create an unnamed dentry with a new inode for a directory with the default
 * metadata.  */
int
new_filler_directory(struct wim_dentry **dentry_ret)
{
	int ret;
	struct wim_dentry *dentry;

	ret = new_dentry_with_new_inode(NULL, true, &dentry);
	if (ret)
		return ret;
	/* Leave the inode number as 0; this is allowed for non
	 * hard-linked files. */
	dentry->d_inode->i_attributes = FILE_ATTRIBUTE_DIRECTORY;
	*dentry_ret = dentry;
	return 0;
}

static int
dentry_clear_inode_visited(struct wim_dentry *dentry, void *_ignore)
{
	dentry->d_inode->i_visited = 0;
	return 0;
}

void
dentry_tree_clear_inode_visited(struct wim_dentry *root)
{
	for_dentry_in_tree(root, dentry_clear_inode_visited, NULL);
}

/*
 * Free a WIM dentry.
 *
 * In addition to freeing the dentry itself, this disassociates the dentry from
 * its inode.  If the inode is no longer in use, it will be freed as well.
 */
void
free_dentry(struct wim_dentry *dentry)
{
	if (dentry) {
		d_disassociate(dentry);
		FREE(dentry->file_name);
		FREE(dentry->short_name);
		FREE(dentry->_full_path);
		FREE(dentry);
	}
}

static int
do_free_dentry(struct wim_dentry *dentry, void *_ignore)
{
	free_dentry(dentry);
	return 0;
}

static int
do_free_dentry_and_unref_blobs(struct wim_dentry *dentry, void *blob_table)
{
	inode_unref_blobs(dentry->d_inode, blob_table);
	free_dentry(dentry);
	return 0;
}

/*
 * Free all dentries in a tree.
 *
 * @root:
 *	The root of the dentry tree to free.  If NULL, this function has no
 *	effect.
 *
 * @blob_table:
 *	A pointer to the blob table for the WIM, or NULL if not specified.  If
 *	specified, this function will decrement the reference counts of the
 *	blobs referenced by the dentries.
 *
 * This function also releases references to the corresponding inodes.
 *
 * This function does *not* unlink @root from its parent directory, if it has
 * one.  If @root has a parent, the caller must unlink @root before calling this
 * function.
 */
void
free_dentry_tree(struct wim_dentry *root, struct blob_table *blob_table)
{
	int (*f)(struct wim_dentry *, void *);

	if (blob_table)
		f = do_free_dentry_and_unref_blobs;
	else
		f = do_free_dentry;

	for_dentry_in_tree_depth(root, f, blob_table);
}

/* Insert the @child dentry into the case sensitive index of the @dir directory.
 * Return NULL if successfully inserted, otherwise a pointer to the
 * already-inserted duplicate.  */
static struct wim_dentry *
dir_index_child(struct wim_inode *dir, struct wim_dentry *child)
{
	struct avl_tree_node *duplicate;

	duplicate = avl_tree_insert(&dir->i_children,
				    &child->d_index_node,
				    _avl_dentry_compare_names);
	if (!duplicate)
		return NULL;
	return avl_tree_entry(duplicate, struct wim_dentry, d_index_node);
}

/* Insert the @child dentry into the case insensitive index of the @dir
 * directory.  Return NULL if successfully inserted, otherwise a pointer to the
 * already-inserted duplicate.  */
static struct wim_dentry *
dir_index_child_ci(struct wim_inode *dir, struct wim_dentry *child)
{
	struct avl_tree_node *duplicate;

	duplicate = avl_tree_insert(&dir->i_children_ci,
				    &child->d_index_node_ci,
				    _avl_dentry_compare_names_ci);
	if (!duplicate)
		return NULL;
	return avl_tree_entry(duplicate, struct wim_dentry, d_index_node_ci);
}

/* Remove the specified dentry from its directory's case-sensitive index.  */
static void
dir_unindex_child(struct wim_inode *dir, struct wim_dentry *child)
{
	avl_tree_remove(&dir->i_children, &child->d_index_node);
}

/* Remove the specified dentry from its directory's case-insensitive index.  */
static void
dir_unindex_child_ci(struct wim_inode *dir, struct wim_dentry *child)
{
	avl_tree_remove(&dir->i_children_ci, &child->d_index_node_ci);
}

/* Return true iff the specified dentry is in its parent directory's
 * case-insensitive index.  */
static bool
dentry_in_ci_index(const struct wim_dentry *dentry)
{
	return !avl_tree_node_is_unlinked(&dentry->d_index_node_ci);
}

/*
 * Link a dentry into the tree.
 *
 * @parent:
 *	The dentry that will be the parent of @child.  It must name a directory.
 *
 * @child:
 *	The dentry to link.  It must be currently unlinked.
 *
 * Returns NULL if successful.  If @parent already contains a dentry with the
 * same case-sensitive name as @child, returns a pointer to this duplicate
 * dentry.
 */
struct wim_dentry *
dentry_add_child(struct wim_dentry *parent, struct wim_dentry *child)
{
	struct wim_dentry *duplicate;
	struct wim_inode *dir;

	wimlib_assert(parent != child);

	dir = parent->d_inode;

	wimlib_assert(inode_is_directory(dir));

	duplicate = dir_index_child(dir, child);
	if (duplicate)
		return duplicate;

	duplicate = dir_index_child_ci(dir, child);
	if (duplicate) {
		list_add(&child->d_ci_conflict_list, &duplicate->d_ci_conflict_list);
		avl_tree_node_set_unlinked(&child->d_index_node_ci);
	} else {
		INIT_LIST_HEAD(&child->d_ci_conflict_list);
	}
	child->d_parent = parent;
	return NULL;
}

/* Unlink a dentry from the tree.  */
void
unlink_dentry(struct wim_dentry *dentry)
{
	struct wim_inode *dir;

	/* Do nothing if the dentry is root or it's already unlinked.  Not
	 * actually necessary based on the current callers, but we do the check
	 * here to be safe.  */
	if (unlikely(dentry->d_parent == dentry))
		return;

	dir = dentry->d_parent->d_inode;

	dir_unindex_child(dir, dentry);

	if (dentry_in_ci_index(dentry)) {

		dir_unindex_child_ci(dir, dentry);

		if (!list_empty(&dentry->d_ci_conflict_list)) {
			/* Make a different case-insensitively-the-same dentry
			 * be the "representative" in the search index.  */
			struct list_head *next;
			struct wim_dentry *other;
			struct wim_dentry *existing;

			next = dentry->d_ci_conflict_list.next;
			other = list_entry(next, struct wim_dentry, d_ci_conflict_list);
			existing = dir_index_child_ci(dir, other);
			wimlib_assert(existing == NULL);
		}
	}
	list_del(&dentry->d_ci_conflict_list);

	/* Not actually necessary, but to be safe don't retain the now-obsolete
	 * parent pointer.  */
	dentry->d_parent = dentry;
}

static int
read_extra_data(const u8 *p, const u8 *end, struct wim_inode *inode)
{
	while (((uintptr_t)p & 7) && p < end)
		p++;

	if (unlikely(p < end)) {
		inode->i_extra = memdup(p, end - p);
		if (!inode->i_extra)
			return WIMLIB_ERR_NOMEM;
		inode->i_extra_size = end - p;
	}
	return 0;
}

/*
 * Set the type of each stream for an encrypted file.
 *
 * All data streams of the encrypted file should have been packed into a single
 * stream in the format provided by ReadEncryptedFileRaw() on Windows.  We
 * assign this stream type STREAM_TYPE_EFSRPC_RAW_DATA.
 *
 * Encrypted files can't have a reparse point stream.  In the on-disk NTFS
 * format they can, but as far as I know the reparse point stream of an
 * encrypted file can't be stored in the WIM format in a way that's compatible
 * with WIMGAPI, nor is there even any way for it to be read or written on
 * Windows when the process does not have access to the file encryption key.
 */
static void
assign_stream_types_encrypted(struct wim_inode *inode)
{
	for (unsigned i = 0; i < inode->i_num_streams; i++) {
		struct wim_inode_stream *strm = &inode->i_streams[i];
		if (!stream_is_named(strm) && !is_zero_hash(strm->_stream_hash))
		{
			strm->stream_type = STREAM_TYPE_EFSRPC_RAW_DATA;
			return;
		}
	}
}

/*
 * Set the type of each stream for an unencrypted file.
 *
 * There will be an unnamed data stream, a reparse point stream, or both an
 * unnamed data stream and a reparse point stream.  In addition, there may be
 * named data streams.
 */
static void
assign_stream_types_unencrypted(struct wim_inode *inode)
{
	bool found_reparse_point_stream = false;
	bool found_unnamed_data_stream = false;
	struct wim_inode_stream *unnamed_stream_with_zero_hash = NULL;

	for (unsigned i = 0; i < inode->i_num_streams; i++) {
		struct wim_inode_stream *strm = &inode->i_streams[i];

		if (stream_is_named(strm)) {
			/* Named data stream  */
			strm->stream_type = STREAM_TYPE_DATA;
		} else if (!is_zero_hash(strm->_stream_hash)) {
			if ((inode->i_attributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
			    !found_reparse_point_stream) {
				found_reparse_point_stream = true;
				strm->stream_type = STREAM_TYPE_REPARSE_POINT;
			} else if (!found_unnamed_data_stream) {
				found_unnamed_data_stream = true;
				strm->stream_type = STREAM_TYPE_DATA;
			}
		} else {
			/* If no stream name is specified and the hash is zero,
			 * then remember this stream for later so that we can
			 * assign it to the unnamed data stream if we don't find
			 * a better candidate.  */
			unnamed_stream_with_zero_hash = strm;
		}
	}

	if (!found_unnamed_data_stream && unnamed_stream_with_zero_hash != NULL)
		unnamed_stream_with_zero_hash->stream_type = STREAM_TYPE_DATA;
}

/*
 * Read and interpret the collection of streams for the specified inode.
 */
static int
setup_inode_streams(const u8 *p, const u8 *end, struct wim_inode *inode,
		    unsigned num_extra_streams, const u8 *default_hash,
		    u64 *offset_p)
{
	const u8 *orig_p = p;

	inode->i_num_streams = 1 + num_extra_streams;

	if (unlikely(inode->i_num_streams > ARRAY_LEN(inode->i_embedded_streams))) {
		inode->i_streams = CALLOC(inode->i_num_streams,
					  sizeof(inode->i_streams[0]));
		if (!inode->i_streams)
			return WIMLIB_ERR_NOMEM;
	}

	/* Use the default hash field for the first stream  */
	inode->i_streams[0].stream_name = (utf16lechar *)NO_STREAM_NAME;
	copy_hash(inode->i_streams[0]._stream_hash, default_hash);
	inode->i_streams[0].stream_type = STREAM_TYPE_UNKNOWN;
	inode->i_streams[0].stream_id = 0;

	/* Read the extra stream entries  */
	for (unsigned i = 1; i < inode->i_num_streams; i++) {
		struct wim_inode_stream *strm;
		const struct wim_extra_stream_entry_on_disk *disk_strm;
		u64 length;
		u16 name_nbytes;

		strm = &inode->i_streams[i];

		strm->stream_id = i;

		/* Do we have at least the size of the fixed-length data we know
		 * need?  */
		if ((end - p) < sizeof(struct wim_extra_stream_entry_on_disk))
			return WIMLIB_ERR_INVALID_METADATA_RESOURCE;

		disk_strm = (const struct wim_extra_stream_entry_on_disk *)p;

		/* Read the length field  */
		length = le64_to_cpu(disk_strm->length);

		/* 8-byte align the length  */
		length = (length + 7) & ~7;

		/* Make sure the length field is neither so small it doesn't
		 * include all the fixed-length data nor so large it overflows
		 * the metadata resource buffer. */
		if (length < sizeof(struct wim_extra_stream_entry_on_disk) ||
		    length > (end - p))
			return WIMLIB_ERR_INVALID_METADATA_RESOURCE;

		/* Read the rest of the fixed-length data. */

		copy_hash(strm->_stream_hash, disk_strm->hash);
		name_nbytes = le16_to_cpu(disk_strm->name_nbytes);

		/* If stream_name_nbytes != 0, the stream is named.  */
		if (name_nbytes != 0) {
			/* The name is encoded in UTF16-LE, which uses 2-byte
			 * coding units, so the length of the name had better be
			 * an even number of bytes.  */
			if (name_nbytes & 1)
				return WIMLIB_ERR_INVALID_METADATA_RESOURCE;

			/* Add the length of the stream name to get the length
			 * we actually need to read.  Make sure this isn't more
			 * than the specified length of the entry.  */
			if (sizeof(struct wim_extra_stream_entry_on_disk) +
			    name_nbytes > length)
				return WIMLIB_ERR_INVALID_METADATA_RESOURCE;

			strm->stream_name = utf16le_dupz(disk_strm->name,
							 name_nbytes);
			if (!strm->stream_name)
				return WIMLIB_ERR_NOMEM;
		} else {
			strm->stream_name = (utf16lechar *)NO_STREAM_NAME;
		}

		strm->stream_type = STREAM_TYPE_UNKNOWN;

		p += length;
	}

	inode->i_next_stream_id = inode->i_num_streams;

	/* Now, assign a type to each stream.  Unfortunately this requires
	 * various hacks because stream types aren't explicitly provided in the
	 * WIM on-disk format.  */

	if (unlikely(inode->i_attributes & FILE_ATTRIBUTE_ENCRYPTED))
		assign_stream_types_encrypted(inode);
	else
		assign_stream_types_unencrypted(inode);

	*offset_p += p - orig_p;
	return 0;
}

/* Read a dentry, including all extra stream entries that follow it, from an
 * uncompressed metadata resource buffer.  */
static int
read_dentry(const u8 * restrict buf, size_t buf_len,
	    u64 *offset_p, struct wim_dentry **dentry_ret)
{
	u64 offset = *offset_p;
	u64 length;
	const u8 *p;
	const struct wim_dentry_on_disk *disk_dentry;
	struct wim_dentry *dentry;
	struct wim_inode *inode;
	u16 short_name_nbytes;
	u16 file_name_nbytes;
	u64 calculated_size;
	int ret;

	BUILD_BUG_ON(sizeof(struct wim_dentry_on_disk) != WIM_DENTRY_DISK_SIZE);

	/* Before reading the whole dentry, we need to read just the length.
	 * This is because a dentry of length 8 (that is, just the length field)
	 * terminates the list of sibling directory entries. */

	/* Check for buffer overrun.  */
	if (unlikely(offset + sizeof(u64) > buf_len ||
		     offset + sizeof(u64) < offset))
		return WIMLIB_ERR_INVALID_METADATA_RESOURCE;

	/* Get pointer to the dentry data.  */
	p = &buf[offset];
	disk_dentry = (const struct wim_dentry_on_disk*)p;

	/* Get dentry length.  */
	length = (le64_to_cpu(disk_dentry->length) + 7) & ~7;

	/* Check for end-of-directory.  */
	if (length <= 8) {
		*dentry_ret = NULL;
		return 0;
	}

	/* Validate dentry length.  */
	if (unlikely(length < sizeof(struct wim_dentry_on_disk)))
		return WIMLIB_ERR_INVALID_METADATA_RESOURCE;

	/* Check for buffer overrun.  */
	if (unlikely(offset + length > buf_len ||
		     offset + length < offset))
		return WIMLIB_ERR_INVALID_METADATA_RESOURCE;

	/* Allocate new dentry structure, along with a preliminary inode.  */
	ret = new_dentry_with_new_inode(NULL, false, &dentry);
	if (ret)
		return ret;

	inode = dentry->d_inode;

	/* Read more fields: some into the dentry, and some into the inode.  */
	inode->i_attributes = le32_to_cpu(disk_dentry->attributes);
	inode->i_security_id = le32_to_cpu(disk_dentry->security_id);
	dentry->subdir_offset = le64_to_cpu(disk_dentry->subdir_offset);
	inode->i_creation_time = le64_to_cpu(disk_dentry->creation_time);
	inode->i_last_access_time = le64_to_cpu(disk_dentry->last_access_time);
	inode->i_last_write_time = le64_to_cpu(disk_dentry->last_write_time);

	/* I don't know what's going on here.  It seems like M$ screwed up the
	 * reparse points, then put the fields in the same place and didn't
	 * document it.  So we have some fields we read for reparse points, and
	 * some fields in the same place for non-reparse-points.  */
	if (inode->i_attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
		inode->i_rp_unknown_1 = le32_to_cpu(disk_dentry->reparse.rp_unknown_1);
		inode->i_reparse_tag = le32_to_cpu(disk_dentry->reparse.reparse_tag);
		inode->i_rp_unknown_2 = le16_to_cpu(disk_dentry->reparse.rp_unknown_2);
		inode->i_not_rpfixed = le16_to_cpu(disk_dentry->reparse.not_rpfixed);
		/* Leave inode->i_ino at 0.  Note: this means that WIM cannot
		 * represent multiple hard links to a reparse point file.  */
	} else {
		inode->i_rp_unknown_1 = le32_to_cpu(disk_dentry->nonreparse.rp_unknown_1);
		inode->i_ino = le64_to_cpu(disk_dentry->nonreparse.hard_link_group_id);
	}

	/* Now onto reading the names.  There are two of them: the (long) file
	 * name, and the short name.  */

	short_name_nbytes = le16_to_cpu(disk_dentry->short_name_nbytes);
	file_name_nbytes = le16_to_cpu(disk_dentry->file_name_nbytes);

	if (unlikely((short_name_nbytes & 1) | (file_name_nbytes & 1))) {
		ret = WIMLIB_ERR_INVALID_METADATA_RESOURCE;
		goto err_free_dentry;
	}

	/* We now know the length of the file name and short name.  Make sure
	 * the length of the dentry is large enough to actually hold them.  */
	calculated_size = dentry_min_len_with_names(file_name_nbytes,
						    short_name_nbytes);

	if (unlikely(length < calculated_size)) {
		ret = WIMLIB_ERR_INVALID_METADATA_RESOURCE;
		goto err_free_dentry;
	}

	/* Advance p to point past the base dentry, to the first name.  */
	p += sizeof(struct wim_dentry_on_disk);

	/* Read the filename if present.  Note: if the filename is empty, there
	 * is no null terminator following it.  */
	if (file_name_nbytes) {
		dentry->file_name = utf16le_dupz(p, file_name_nbytes);
		if (dentry->file_name == NULL) {
			ret = WIMLIB_ERR_NOMEM;
			goto err_free_dentry;
		}
		dentry->file_name_nbytes = file_name_nbytes;
		p += (u32)file_name_nbytes + 2;
	}

	/* Read the short filename if present.  Note: if there is no short
	 * filename, there is no null terminator following it. */
	if (short_name_nbytes) {
		dentry->short_name = utf16le_dupz(p, short_name_nbytes);
		if (dentry->short_name == NULL) {
			ret = WIMLIB_ERR_NOMEM;
			goto err_free_dentry;
		}
		dentry->short_name_nbytes = short_name_nbytes;
		p += (u32)short_name_nbytes + 2;
	}

	/* Read extra data at end of dentry (but before extra stream entries).
	 * This may contain tagged metadata items.  */
	ret = read_extra_data(p, &buf[offset + length], inode);
	if (ret)
		goto err_free_dentry;

	offset += length;

	/* Set up the inode's collection of streams.  */
	ret = setup_inode_streams(&buf[offset],
				  &buf[buf_len],
				  inode,
				  le16_to_cpu(disk_dentry->num_extra_streams),
				  disk_dentry->default_hash,
				  &offset);
	if (ret)
		goto err_free_dentry;

	*offset_p = offset;  /* Sets offset of next dentry in directory  */
	*dentry_ret = dentry;
	return 0;

err_free_dentry:
	free_dentry(dentry);
	return ret;
}

/* Is the dentry named "." or ".." ?  */
static bool
dentry_is_dot_or_dotdot(const struct wim_dentry *dentry)
{
	if (dentry->file_name_nbytes <= 4) {
		if (dentry->file_name_nbytes == 4) {
			if (dentry->file_name[0] == cpu_to_le16('.') &&
			    dentry->file_name[1] == cpu_to_le16('.'))
				return true;
		} else if (dentry->file_name_nbytes == 2) {
			if (dentry->file_name[0] == cpu_to_le16('.'))
				return true;
		}
	}
	return false;
}

static int
read_dentry_tree_recursive(const u8 * restrict buf, size_t buf_len,
			   struct wim_dentry * restrict dir)
{
	u64 cur_offset = dir->subdir_offset;

	/* Check for cyclic directory structure, which would cause infinite
	 * recursion if not handled.  */
	for (struct wim_dentry *d = dir->d_parent;
	     !dentry_is_root(d); d = d->d_parent)
	{
		if (unlikely(d->subdir_offset == cur_offset)) {
			ERROR("Cyclic directory structure detected: children "
			      "of \"%"TS"\" coincide with children of \"%"TS"\"",
			      dentry_full_path(dir), dentry_full_path(d));
			return WIMLIB_ERR_INVALID_METADATA_RESOURCE;
		}
	}

	for (;;) {
		struct wim_dentry *child;
		struct wim_dentry *duplicate;
		int ret;

		/* Read next child of @dir.  */
		ret = read_dentry(buf, buf_len, &cur_offset, &child);
		if (ret)
			return ret;

		/* Check for end of directory.  */
		if (child == NULL)
			return 0;

		/* All dentries except the root should be named.  */
		if (unlikely(!dentry_has_long_name(child))) {
			WARNING("Ignoring unnamed dentry in "
				"directory \"%"TS"\"", dentry_full_path(dir));
			free_dentry(child);
			continue;
		}

		/* Don't allow files named "." or "..".  */
		if (unlikely(dentry_is_dot_or_dotdot(child))) {
			WARNING("Ignoring file named \".\" or \"..\"; "
				"potentially malicious archive!!!");
			free_dentry(child);
			continue;
		}

		/* Link the child into the directory.  */
		duplicate = dentry_add_child(dir, child);
		if (unlikely(duplicate)) {
			/* We already found a dentry with this same
			 * case-sensitive long name.  Only keep the first one.
			 */
			WARNING("Ignoring duplicate file \"%"TS"\" "
				"(the WIM image already contains a file "
				"at that path with the exact same name)",
				dentry_full_path(duplicate));
			free_dentry(child);
			continue;
		}

		/* If this child is a directory that itself has children, call
		 * this procedure recursively.  */
		if (child->subdir_offset != 0) {
			if (likely(dentry_is_directory(child))) {
				ret = read_dentry_tree_recursive(buf,
								 buf_len,
								 child);
				if (ret)
					return ret;
			} else {
				WARNING("Ignoring children of "
					"non-directory file \"%"TS"\"",
					dentry_full_path(child));
			}
		}
	}
}

/*
 * Read a tree of dentries from a WIM metadata resource.
 *
 * @buf:
 *	Buffer containing an uncompressed WIM metadata resource.
 *
 * @buf_len:
 *	Length of the uncompressed metadata resource, in bytes.
 *
 * @root_offset
 *	Offset in the metadata resource of the root of the dentry tree.
 *
 * @root_ret:
 *	On success, either NULL or a pointer to the root dentry is written to
 *	this location.  The former case only occurs in the unexpected case that
 *	the tree began with an end-of-directory entry.
 *
 * Return values:
 *	WIMLIB_ERR_SUCCESS (0)
 *	WIMLIB_ERR_INVALID_METADATA_RESOURCE
 *	WIMLIB_ERR_NOMEM
 */
int
read_dentry_tree(const u8 *buf, size_t buf_len,
		 u64 root_offset, struct wim_dentry **root_ret)
{
	int ret;
	struct wim_dentry *root;

	DEBUG("Reading dentry tree (root_offset=%"PRIu64")", root_offset);

	ret = read_dentry(buf, buf_len, &root_offset, &root);
	if (ret)
		return ret;

	if (likely(root != NULL)) {
		if (unlikely(dentry_has_long_name(root) ||
			     dentry_has_short_name(root)))
		{
			WARNING("The root directory has a nonempty name; "
				"removing it.");
			dentry_set_name(root, NULL);
		}

		if (unlikely(!dentry_is_directory(root))) {
			ERROR("The root of the WIM image is not a directory!");
			ret = WIMLIB_ERR_INVALID_METADATA_RESOURCE;
			goto err_free_dentry_tree;
		}

		if (likely(root->subdir_offset != 0)) {
			ret = read_dentry_tree_recursive(buf, buf_len, root);
			if (ret)
				goto err_free_dentry_tree;
		}
	} else {
		WARNING("The metadata resource has no directory entries; "
			"treating as an empty image.");
	}
	*root_ret = root;
	return 0;

err_free_dentry_tree:
	free_dentry_tree(root, NULL);
	return ret;
}

static u8 *
write_extra_stream_entry(u8 * restrict p, const utf16lechar * restrict name,
			 const u8 * restrict hash)
{
	struct wim_extra_stream_entry_on_disk *disk_strm =
			(struct wim_extra_stream_entry_on_disk *)p;
	u8 *orig_p = p;
	size_t name_nbytes;

	if (name == NO_STREAM_NAME)
		name_nbytes = 0;
	else
		name_nbytes = utf16le_len_bytes(name);

	disk_strm->reserved = 0;
	copy_hash(disk_strm->hash, hash);
	disk_strm->name_nbytes = cpu_to_le16(name_nbytes);
	p += sizeof(struct wim_extra_stream_entry_on_disk);
	if (name_nbytes != 0)
		p = mempcpy(p, name, name_nbytes + 2);
	/* Align to 8-byte boundary */
	while ((uintptr_t)p & 7)
		*p++ = 0;
	disk_strm->length = cpu_to_le64(p - orig_p);
	return p;
}

/*
 * Write a WIM dentry to an output buffer.
 *
 * This includes any extra stream entries that may follow the dentry itself.
 *
 * @dentry:
 *	The dentry to write.
 *
 * @p:
 *	The memory location to which to write the data.
 *
 * Returns a pointer to the byte following the last written.
 */
static u8 *
write_dentry(const struct wim_dentry * restrict dentry, u8 * restrict p)
{
	const struct wim_inode *inode;
	struct wim_dentry_on_disk *disk_dentry;
	const u8 *orig_p;

	wimlib_assert(((uintptr_t)p & 7) == 0); /* 8 byte aligned */
	orig_p = p;

	inode = dentry->d_inode;
	disk_dentry = (struct wim_dentry_on_disk*)p;

	disk_dentry->attributes = cpu_to_le32(inode->i_attributes);
	disk_dentry->security_id = cpu_to_le32(inode->i_security_id);
	disk_dentry->subdir_offset = cpu_to_le64(dentry->subdir_offset);

	disk_dentry->unused_1 = cpu_to_le64(0);
	disk_dentry->unused_2 = cpu_to_le64(0);

	disk_dentry->creation_time = cpu_to_le64(inode->i_creation_time);
	disk_dentry->last_access_time = cpu_to_le64(inode->i_last_access_time);
	disk_dentry->last_write_time = cpu_to_le64(inode->i_last_write_time);
	if (inode->i_attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
		disk_dentry->reparse.rp_unknown_1 = cpu_to_le32(inode->i_rp_unknown_1);
		disk_dentry->reparse.reparse_tag = cpu_to_le32(inode->i_reparse_tag);
		disk_dentry->reparse.rp_unknown_2 = cpu_to_le16(inode->i_rp_unknown_2);
		disk_dentry->reparse.not_rpfixed = cpu_to_le16(inode->i_not_rpfixed);
	} else {
		disk_dentry->nonreparse.rp_unknown_1 = cpu_to_le32(inode->i_rp_unknown_1);
		disk_dentry->nonreparse.hard_link_group_id =
			cpu_to_le64((inode->i_nlink == 1) ? 0 : inode->i_ino);
	}

	disk_dentry->short_name_nbytes = cpu_to_le16(dentry->short_name_nbytes);
	disk_dentry->file_name_nbytes = cpu_to_le16(dentry->file_name_nbytes);
	p += sizeof(struct wim_dentry_on_disk);

	wimlib_assert(dentry_is_root(dentry) != dentry_has_long_name(dentry));

	if (dentry_has_long_name(dentry))
		p = mempcpy(p, dentry->file_name, (u32)dentry->file_name_nbytes + 2);

	if (dentry_has_short_name(dentry))
		p = mempcpy(p, dentry->short_name, (u32)dentry->short_name_nbytes + 2);

	/* Align to 8-byte boundary */
	while ((uintptr_t)p & 7)
		*p++ = 0;

	if (inode->i_extra_size) {
		/* Extra tagged items --- not usually present.  */
		p = mempcpy(p, inode->i_extra, inode->i_extra_size);

		/* Align to 8-byte boundary */
		while ((uintptr_t)p & 7)
			*p++ = 0;
	}

	disk_dentry->length = cpu_to_le64(p - orig_p);

	/* Streams  */

	if (unlikely(inode->i_attributes & FILE_ATTRIBUTE_ENCRYPTED)) {
		const struct wim_inode_stream *efs_strm;
		const u8 *efs_hash;

		efs_strm = inode_get_unnamed_stream(inode, STREAM_TYPE_EFSRPC_RAW_DATA);
		efs_hash = efs_strm ? stream_hash(efs_strm) : zero_hash;
		copy_hash(disk_dentry->default_hash, efs_hash);
		disk_dentry->num_extra_streams = cpu_to_le16(0);
	} else {
		/*
		 * Extra stream entries:
		 *
		 * - Use one extra stream entry for each named data stream
		 * - Use one extra stream entry for the unnamed data stream when there is either:
		 *	- a reparse point stream
		 *	- at least one named data stream (for Windows PE bug workaround)
		 * - Use one extra stream entry for the reparse point stream if there is one
		 */
		bool have_named_data_stream = false;
		bool have_reparse_point_stream = false;
		const u8 *unnamed_data_stream_hash = zero_hash;
		const u8 *reparse_point_hash;
		for (unsigned i = 0; i < inode->i_num_streams; i++) {
			const struct wim_inode_stream *strm = &inode->i_streams[i];
			if (strm->stream_type == STREAM_TYPE_DATA) {
				if (stream_is_named(strm))
					have_named_data_stream = true;
				else
					unnamed_data_stream_hash = stream_hash(strm);
			} else if (strm->stream_type == STREAM_TYPE_REPARSE_POINT) {
				have_reparse_point_stream = true;
				reparse_point_hash = stream_hash(strm);
			}
		}

		if (unlikely(have_reparse_point_stream || have_named_data_stream)) {

			unsigned num_extra_streams = 0;

			copy_hash(disk_dentry->default_hash, zero_hash);

			if (have_reparse_point_stream) {
				p = write_extra_stream_entry(p, NO_STREAM_NAME,
							     reparse_point_hash);
				num_extra_streams++;
			}

			p = write_extra_stream_entry(p, NO_STREAM_NAME,
						     unnamed_data_stream_hash);
			num_extra_streams++;

			for (unsigned i = 0; i < inode->i_num_streams; i++) {
				const struct wim_inode_stream *strm = &inode->i_streams[i];
				if (stream_is_named_data_stream(strm)) {
					p = write_extra_stream_entry(p, strm->stream_name,
								     stream_hash(strm));
					num_extra_streams++;
				}
			}
			wimlib_assert(num_extra_streams <= 0xFFFF);

			disk_dentry->num_extra_streams = cpu_to_le16(num_extra_streams);
		} else {
			copy_hash(disk_dentry->default_hash, unnamed_data_stream_hash);
			disk_dentry->num_extra_streams = cpu_to_le16(0);
		}
	}

	return p;
}

static int
write_dir_dentries(struct wim_dentry *dir, void *_pp)
{
	if (dir->subdir_offset != 0) {
		u8 **pp = _pp;
		u8 *p = *pp;
		struct wim_dentry *child;

		/* write child dentries */
		for_dentry_child(child, dir)
			p = write_dentry(child, p);

		/* write end of directory entry */
		*(u64*)p = 0;
		p += 8;
		*pp = p;
	}
	return 0;
}

/*
 * Write a directory tree to the metadata resource.
 *
 * @root:
 *	The root of a dentry tree on which calculate_subdir_offsets() has been
 *	called.  This cannot be NULL; if the dentry tree is empty, the caller is
 *	expected to first generate a dummy root directory.
 *
 * @p:
 *	Pointer to a buffer with enough space for the dentry tree.  This size
 *	must have been obtained by calculate_subdir_offsets().
 *
 * Returns a pointer to the byte following the last written.
 */
u8 *
write_dentry_tree(struct wim_dentry *root, u8 *p)
{
	DEBUG("Writing dentry tree.");

	wimlib_assert(root != NULL);

	/* write root dentry and end-of-directory entry following it */
	p = write_dentry(root, p);
	*(u64*)p = 0;
	p += 8;

	/* write the rest of the dentry tree */
	for_dentry_in_tree(root, write_dir_dentries, &p);

	return p;
}
