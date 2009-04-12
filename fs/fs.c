#include <inc/string.h>

#include "fs.h"

#define debug 0

struct Super *super;		// superblock
uint32_t *bitmap;		// bitmap blocks mapped in memory

void file_flush(struct File *f);
bool block_is_free(uint32_t blockno);

// Return the virtual address of this disk block.
char*
diskaddr(uint32_t blockno)
{
	if (super && blockno >= super->s_nblocks)
		panic("bad block number %08x in diskaddr", blockno);
	return (char*) (DISKMAP + blockno * BLKSIZE);
}

bool
va_is_mapped(void *va)
{
	return (vpd[PDX(va)] & PTE_P) && (vpt[VPN(va)] & PTE_P);
}

bool
block_is_mapped(uint32_t blockno)
{
	char *va = diskaddr(blockno);
	return va_is_mapped(va) && va != 0;
}

bool
va_is_dirty(void *va)
{
	return (vpt[VPN(va)] & PTE_D) != 0;
}

bool
block_is_dirty(uint32_t blockno)
{
	char *va = diskaddr(blockno);
	return va_is_mapped(va) && va_is_dirty(va);
}

// Allocate a page to hold the disk block
int
map_block(uint32_t blockno)
{
	if (block_is_mapped(blockno))
		return 0;
	return sys_page_alloc(0, diskaddr(blockno), PTE_U|PTE_P|PTE_W);
}

// Make sure a particular disk block is loaded into memory.
// Returns 0 on success, or a negative error code on error.
// 
// If blk != 0, set *blk to the address of the block in memory.
static int
read_block(uint32_t blockno, char **blk)
{
	int r;
	char *addr;
	pte_t *pte;

	if (super && blockno >= super->s_nblocks)
		panic("reading non-existent block %08x\n", blockno);

	if (bitmap && block_is_free(blockno))
		panic("reading free block %08x\n", blockno);

	// LAB 5: Your code here.
	addr = diskaddr(blockno);
	if (block_is_mapped(blockno))
		goto done;
	if (debug)
		cprintf("read block %d from disk\n", blockno);

	if ((r = sys_page_alloc(0, addr, PTE_U | PTE_P | PTE_W)) < 0)
		return r;
	
	if ((r = ide_read(blockno * BLKSECTS, addr, BLKSECTS)) < 0)
		return r;
	/* ide_read() has dirtied the page, so we remap it */
	sys_page_map(0, addr, 0, addr, PTE_U | PTE_P | PTE_W);
done:
	if (blk)
		*blk = addr;
	return 0;
}

// Copy the current contents of the block out to disk.
// Then clear the PTE_D bit using sys_page_map.
void
write_block(uint32_t blockno)
{
	char *addr;

	if (!block_is_mapped(blockno))
		panic("write unmapped block %08x", blockno);
	
	addr = diskaddr(blockno);

	if (ide_write(blockno * BLKSECTS, addr, BLKSECTS) < 0)
		panic("ATA write error");
	if (debug)
		cprintf("write block %x to disk\n", blockno);
	sys_page_map(0, addr, 0, addr, PTE_USER);
	/**********************************************
	 *             ATTENTION PLEASE               *
	 **********************************************
	 * Now we don't unmap the block, so it is still cached out there
	 * occupying the ram though it speeds up the 'file-mapping'.
	 * Seriously need a mechanism to support kind of garbage collection
	 * in order to ensure ram is NOT used up by block cache in the case
	 * some buggy apps.
	 * 
	 * Liu Yuan reviewed it at Mar 8 2009, 3:47 pm.
	 */
}

// Make sure this block is unmapped.
void
unmap_block(uint32_t blockno)
{
	int r;

	if (!block_is_mapped(blockno))
		return;

	assert(block_is_free(blockno) || !block_is_dirty(blockno));

	if ((r = sys_page_unmap(0, diskaddr(blockno))) < 0)
		panic("unmap_block: sys_mem_unmap: %e", r);
	assert(!block_is_mapped(blockno));
}

// Check to see if the block bitmap indicates that block 'blockno' is free.
// Return 1 if the block is free, 0 if not.
bool
block_is_free(uint32_t blockno)
{
	if (super == 0 || blockno >= super->s_nblocks)
		return 0;
	if (bitmap[blockno / 32] & (1 << (blockno % 32)))
		return 1;
	return 0;
}

// Mark a block free in the bitmap
void
free_block(uint32_t blockno)
{
	// Blockno zero is the null pointer of block numbers.
	if (blockno == 0)
		panic("attempt to free zero block");
	bitmap[blockno/32] |= 1<<(blockno%32);
}

// Search the bitmap for a free block and allocate it.
// 
// Return block number allocated on success,
// -E_NO_DISK if we are out of blocks.
int
alloc_block_num(void)
{
	int i, j;
	/* Try to find a element of bitmap that indicates containing free blocks
	 * We stride in 8 bits for speedup
	 */
	for (i = 0; i < super->s_nblocks / 32; i++) {
		if (!bitmap[i])
			continue;
		goto found;
	}
	/* Out of blocks */
	return -E_NO_DISK;
found:
	for (j = 0; j < 32; j++)
		if (bitmap[i] & (1 << j))
			break;

	bitmap[i] &= ~(1 << j); /* Mark it in use */

	return i * 32 + j;
}

// Allocate a block -- first find a free block in the bitmap,
// then map it into memory.
//
// Return block number on success
// -E_NO_DISK if we are out of blocks
int
alloc_block(void)
{

	int r, bno;
	if ((bno = r = alloc_block_num()) < 0)
		return r;

	if ((r = map_block(bno)) < 0) {
		free_block(bno);
		return r;
	}
	write_block(2 + bno / BLKBITSIZE); /* Flush the changed bitmap block */
	return bno;
}

// Read and validate the file system super-block.
void
read_super(void)
{
	int r;
	char *blk;

	if ((r = read_block(1, &blk)) < 0)
		panic("cannot read superblock: %e", r);

	super = (struct Super*) blk;
	if (super->s_magic != FS_MAGIC)
		panic("bad file system magic number");

	if (super->s_nblocks > DISKSIZE/BLKSIZE)
		panic("file system is too large");

	//cprintf("superblock is good\n");
}

// Read and validate the file system bitmap.
void
read_bitmap(void)
{
	int r;
	uint32_t i;
	char *blk;

	for (i = 0; i * BLKBITSIZE < super->s_nblocks; i++) {
		if ((r = read_block(2+i, &blk)) < 0)
			panic("cannot read bitmap block %d: %e", i, r);
		if (i == 0)
			bitmap = (uint32_t*) blk;
		// Make sure all bitmap blocks are marked in-use
		assert(!block_is_free(2+i));
	}
	
	// Make sure the reserved and root blocks are marked in-use.
	assert(!block_is_free(0));
	assert(!block_is_free(1));
	assert(bitmap);
}

// Initialize the file system
void
fs_init(void)
{
	static_assert(sizeof(struct File) == 256);

	// Find a JOS disk.  Use the second IDE disk (number 1) if available.
	if (ide_probe_disk1())
		ide_set_disk(1);
	else
		ide_set_disk(0);
	
	read_super();
	read_bitmap();
}

// Find the disk block number slot for the 'filebno'th block in file 'f'.
// Set '*ppdiskbno' to point to that slot.
// The slot will be one of the f->f_direct[] entries,
// or an entry in the indirect block.
// When 'alloc' is set, this function will allocate an indirect block
// if necessary.
//
// Returns:
//	0 on success (but note that *ppdiskbno might equal 0).
//	-E_NOT_FOUND if the function needed to allocate an indirect block, but
//		alloc was 0.
//	-E_NO_DISK if there's no space on the disk for an indirect block.
//	-E_NO_MEM if there's no space in memory for an indirect block.
//	-E_INVAL if filebno is out of range (it's >= NINDIRECT).
//
// Analogy: This is like pgdir_walk for files.  
int
file_block_walk(struct File *f, uint32_t filebno, uint32_t **ppdiskbno, bool alloc)
{
	int r;
	uint32_t *ptr;		/* Points to the block that holds filebno'th entry */
	switch (filebno) {
	case 0 ... (NDIRECT - 1):
		ptr = &f->f_direct[0];
		break;

	case NDIRECT ... (NINDIRECT - 1):
		if (f->f_indirect == 0) {/* The indirect block doesn't exist */
			if(alloc == 0)
				return -E_NOT_FOUND;
			else {
				if ((f->f_indirect = r = alloc_block()) < 0) {
					return r;
				}
				ptr = (uint32_t *)diskaddr(f->f_indirect);
			}
		} else		/* The indirect block exists */
			if ((r = read_block(f->f_indirect, (char **)&ptr)) < 0)
				return 0;
		break;
	default:
		return -E_INVAL;
	}
	/* Found the corresponding entry */
	*ppdiskbno = &ptr[filebno];
	return 0;
}

// Set '*diskbno' to the disk block number for the 'filebno'th block
// in file 'f'.
// If 'alloc' is set and the block does not exist, allocate it.
//
// Returns 0 on success, < 0 on error.  Errors are:
//	-E_NOT_FOUND if alloc was 0 but the block did not exist.
//	-E_NO_DISK if a block needed to be allocated but the disk is full.
//	-E_NO_MEM if we're out of memory.
//	-E_INVAL if filebno is out of range.
int
file_map_block(struct File *f, uint32_t filebno, uint32_t *diskbno, bool alloc)
{
	int r;
	uint32_t *ptr;
	if ((r = file_block_walk(f, filebno, &ptr, alloc)) < 0)
		return r;
	if (*ptr == 0) {
		if (alloc == 0)
			return -E_NOT_FOUND;
		if ((r = alloc_block()) < 0)
			return r;
		*ptr = r;
	}
	*diskbno = *ptr;
	return 0;
}

// Remove a block from file f.  If it's not there, just silently succeed.
// Returns 0 on success, < 0 on error.
int
file_clear_block(struct File *f, uint32_t filebno)
{
	int r;
	uint32_t *ptr;

	if ((r = file_block_walk(f, filebno, &ptr, 0)) < 0)
		return r;
	if (*ptr) {
		free_block(*ptr);
		*ptr = 0;
	}
	return 0;
}

// Set *blk to point at the filebno'th block in file 'f'.
// Allocate the block if it doesn't yet exist.
// Returns 0 on success, < 0 on error.
int
file_get_block(struct File *f, uint32_t filebno, char **blk)
{
	int r;
	uint32_t diskbno;

	if ((r = file_map_block(f, filebno, &diskbno, 1)) < 0)
		return r;

	if ((r = read_block(diskbno, blk)) < 0)
		return r;
	return 0;
}

// Mark the block at offset as dirty in file f
int
file_dirty(struct File *f, off_t offset)
{
	int r;
	char *blk;
	struct File d;
	if ((r = file_get_block(f, offset / BLKSIZE, &blk)) < 0)
		return r;

	*(volatile char*)blk = *(volatile char*)blk;		/* Write to dirty it */

	return 0;
}

// Try to find a file named "name" in dir.  If so, set *file to it.
int
dir_lookup(struct File *dir, const char *name, struct File **file)
{
	int r;
	uint32_t i, j, nblock;
	char *blk;
	struct File *f;
	// Search dir for name.
	// We maintain the invariant that the size of a directory-file
	// is always a multiple of the file system's block size.
	if (debug)
		cprintf("Look at directory %s for %s\n", dir->f_name, name);

	assert((dir->f_size % BLKSIZE) == 0);
	nblock = dir->f_size / BLKSIZE;
	for (i = 0; i < nblock; i++) {
		if ((r = file_get_block(dir, i, &blk)) < 0)
			return r;
		f = (struct File*) blk;
		for (j = 0; j < BLKFILES; j++)
			if (strcmp(f[j].f_name, name) == 0) {
				*file = &f[j];
				f[j].f_dir = dir; 
				/* Dirty the directory file. This means dir files
				 * concerning the pathname will _always_ get flushed
				 */
				return 0;
			}
	}
	return -E_NOT_FOUND;
}

// Set *file to point at a free File structure in dir.
int
dir_alloc_file(struct File *dir, struct File **file)
{
	int r;
	uint32_t nblock, i, j;
	char *blk;
	struct File *f;

	assert((dir->f_size % BLKSIZE) == 0);
	nblock = dir->f_size / BLKSIZE;
	for (i = 0; i < nblock; i++) {
		if ((r = file_get_block(dir, i, &blk)) < 0)
			return r;
		f = (struct File*) blk;
		for (j = 0; j < BLKFILES; j++)
			if (f[j].f_name[0] == '\0') {
				*file = &f[j];
				f[j].f_dir = dir;
				return 0;
			}
	}
	dir->f_size += BLKSIZE;
	if ((r = file_get_block(dir, i, &blk)) < 0)
		return r;
	f = (struct File*) blk;
	*file = &f[0];
	f[0].f_dir = dir;
	return 0;
}

// Skip over slashes.
static inline const char*
skip_slash(const char *p)
{
	while (*p == '/')
		p++;
	return p;
}

// Evaluate a path name, starting at the root.
// On success, set *pf to the file we found
// and set *pdir to the directory the file is in.
// If we cannot find the file but find the directory
// it should be in, set *pdir and copy the final path
// element into lastelem.
static int
walk_path(const char *path, struct File **pdir, struct File **pf, char *lastelem)
{
	const char *p;
	char name[MAXNAMELEN];
	struct File *dir, *f;
	int r;

	path = skip_slash(path);
	f = &super->s_root;
	dir = 0;
	name[0] = 0;

	if (pdir)
		*pdir = 0;
	*pf = 0;
	while (*path != '\0') {
		dir = f;
		p = path;
		while (*path != '/' && *path != '\0')
			path++;
		if (path - p >= MAXNAMELEN)
			return -E_BAD_PATH;
		memmove(name, p, path - p);
		name[path - p] = '\0';
		path = skip_slash(path);

		if (dir->f_type != FTYPE_DIR)
			return -E_NOT_FOUND;

		if ((r = dir_lookup(dir, name, &f)) < 0) {
			if (r == -E_NOT_FOUND && *path == '\0') {
				if (pdir)
					*pdir = dir;
				if (lastelem)
					strcpy(lastelem, name);
				*pf = 0;
			}
			return r;
		}
	}

	if (pdir)
		*pdir = dir;
	*pf = f;
	return 0;
}

// Create "path".  On success set *pf to point at the file and return 0.
// On error return < 0.
int
file_create(const char *path, struct File **pf)
{
	char name[MAXNAMELEN];
	int r;
	struct File *dir, *f;

	if ((r = walk_path(path, &dir, &f, name)) == 0)
		return -E_FILE_EXISTS;
	if (r != -E_NOT_FOUND || dir == 0)
		return r;
	if (dir_alloc_file(dir, &f) < 0)
		return r;
	strcpy(f->f_name, name);
	*pf = f;
	return 0;
}

// Open "path".  On success set *pf to point at the file and return 0.
// On error return < 0.
int
file_open(const char *path, struct File **pf)
{
	return walk_path(path, 0, pf, 0);
}

// Remove any blocks currently used by file 'f',
// but not necessary for a file of size 'newsize'.
static void
file_truncate_blocks(struct File *f, off_t newsize)
{
	int r;
	uint32_t bno, old_nblocks, new_nblocks;

	old_nblocks = (f->f_size + BLKSIZE - 1) / BLKSIZE;
	new_nblocks = (newsize + BLKSIZE - 1) / BLKSIZE;
	for (bno = new_nblocks; bno < old_nblocks; bno++)
		if ((r = file_clear_block(f, bno)) < 0)
			cprintf("warning: file_clear_block: %e", r);

	if (new_nblocks <= NDIRECT && f->f_indirect) {
		free_block(f->f_indirect);
		f->f_indirect = 0;
	}
}

int
file_set_size(struct File *f, off_t newsize)
{
	if (f->f_size > newsize)
		file_truncate_blocks(f, newsize);
	f->f_size = newsize;
	if (f->f_dir)
		file_flush(f->f_dir);
	return 0;
}

// Flush the contents of file f out to disk.
// Loop over all the blocks in file.
// Translate the file block number into a disk block number
// and then check whether that disk block is dirty.  If so, write it out.
void
file_flush(struct File *f)
{
	int i;
	uint32_t diskbno;
	
	for (i = 0; i < (f->f_size + BLKSIZE - 1) / BLKSIZE; i++) {
		if ( file_map_block(f, i, &diskbno, 0) < 0)
			continue;
		if (block_is_dirty(diskbno)){
			if (debug)
				cprintf("File %s flushes, blockno %x\n", f->f_name, diskbno);
			write_block(diskbno);
		}
	}
}

// Sync the entire file system.  A big hammer.
void
fs_sync(void)
{
	int i;
	for (i = 0; i < super->s_nblocks; i++)
		if (block_is_dirty(i))
			write_block(i);
}

// Close a file.
void
file_close(struct File *f)
{
	file_flush(f);
	if (f->f_dir)
		file_flush(f->f_dir);
}

// Remove a file by truncating it and then zeroing the name.
int
file_remove(const char *path)
{
	int r;
	struct File *f;

	if ((r = walk_path(path, 0, &f, 0)) < 0)
		return r;

	file_truncate_blocks(f, 0);
	f->f_name[0] = '\0';
	f->f_size = 0;
	if (f->f_dir)
		file_flush(f->f_dir);

	return 0;
}

