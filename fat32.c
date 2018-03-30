/*
	This file is part of miscutil.
	Copyright (C) 2017-2018, Robert L. Thompson

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#define _FILE_OFFSET_BITS	64
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <linux/fs.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>


// dirent initial char
#define FAT32_DIRENT_SUB	0x05
#define FAT32_DIRENT_DEL	0xe5
#define FAT32_DIRENT_FREE	0x00
#define FAT32_DIRENT_PAD	' '

/*
#define FAT32_DIRENT_BADCH(c)	((c) == '"' \
				|| ((c) >= 0x2a && (c) <= 0x2f && (c) != 0x2d) \
				|| ((c) >= 0x3a && (c) <= 0x3f) \
				|| ((c) >= 0x5b && (c) <= 0x5d) \
				|| (c) == 0x7c)
*/

// allow for LFN but not short: + , ; = [ ]
#define FAT32_LFN_BADCH(c)	((c) == '"' || (c) == '*' || (c) == '/' \
				|| (c) == ':' || (c) == '<' || (c) == '>' \
				|| (c) == '?' || (c) == '\\' || (c) == '|')
// allow: letters, digits, >127, and: $ % ' - _ @ ~ ` ! ( ) { } ^ # & -- disallows '.' ???
#define FAT32_DIRENT_BADCH(c)	(FAT32_LFN_BADCH(c) || (c) == '+' \
				|| (c) == ',' || (c) == '.' || (c) == ';' \
				(c) == '=' || (c) == '[' || (c) == ']')
// duplicates must not exist across both long and short namespaces
// long name becomes invalid if short name exists

// dirent attributes
#define FAT32_ATTR_RO		0x01
#define FAT32_ATTR_HIDDEN	0x02
#define FAT32_ATTR_SYS		0x04
#define FAT32_ATTR_VOL		0x08
#define FAT32_ATTR_DIR		0x10
#define FAT32_ATTR_ARCHIVE	0x20
#define FAT32_ATTR_LFN		(FAT32_ATTR_RO | FAT32_ATTR_HIDDEN | FAT32_ATTR_SYS | FAT32_ATTR_VOL)
#define FAT32_ATTR_MASK		(FAT32_ATTR_LFN | FAT32_ATTR_DIR | FAT32_ATTR_ARCHIVE)

// dirent date
#define FAT32_DT_DOM_BITS	5
#define FAT32_DT_DOM_SHIFT	0
#define FAT32_DT_MOY_BITS	4
#define FAT32_DT_MOY_SHIFT	(FAT32_DT_DOM_BITS + FAT32_DT_DOM_SHIFT)
#define FAT32_DT_Y1980_BITS	7
#define FAT32_DT_Y1980_SHIFT	(FAT32_DT_MOY_BITS + FAT32_DT_MOY_SHIFT)

// dirent time
#define FAT32_TM_SECHALF_BITS	5
#define FAT32_TM_SECHALF_SHIFT	0
#define FAT32_TM_MIN_BITS	6
#define FAT32_TM_MIN_SHIFT	(FAT32_TM_SECHALF_BITS + FAT32_TM_SECHALF_SHIFT)
#define FAT32_TM_HR_BITS	5
#define FAT32_TM_HR_SHIFT	(FAT32_TM_MIN_BITS + FAT32_TM_MIN_SHIFT)

struct fat32_dirent {			// short dirent
	uint8_t		name[11];
	uint8_t		attr;
	uint8_t		resvnt;
	uint8_t		crtm200;
	uint16_t	crtm;
	uint16_t	crdt;
	uint16_t	acdt;
	uint16_t	clusthi;
	uint16_t	wrtm;
	uint16_t	wrdt;
	uint16_t	clustlo;
	uint32_t	filesz;
};

#define FAT32_LFN_ORD_LAST	0x40

struct fat32_lfnent {			// unused chars: 0x0000 terminate then 0xffff fill
	uint8_t		ord;		// sequence: ORD_LAST | n, n - 1, ..., 1, short entry
	uint8_t		name1[10];	// chars 1-5
	uint8_t		attr;
	uint8_t		type;		// zero - other values reserved
	uint8_t		cksum;
	uint8_t		name2[12];	// chars 6-11
	uint16_t	clustlo;	// zero
	uint8_t		name3[4];	// chars 12-13
};

struct fat32_meta {
	int		fd;			// block device
	void		*ptr;			// mmap of device from BS/MBR to start of data
	unsigned	len;			// mmap length

	unsigned	start_cluster;		// start cluster = 2
	unsigned	cluster_sectors;	// sectors per cluster
	unsigned	rsvd_sectors;		// reserved sectors = BS + FSINFO + align (but no backup BS)
	unsigned	fat_sectors;		// FAT sector count
	unsigned	overlap_start_sector;	// data start sector (beyond region allocated for root dir)
	unsigned	root_dir_clusters;	// root dir cluster count
	unsigned	fsinfo_sector;		// FSINFO sector
	unsigned	backup_sector;		// boot backup sector

	unsigned	sector_size;
	unsigned	fsblksz;		// block size of mounted FS
	dev_t		devid;			// device ID of mounted FS

	bool		dbg;			// debug messages
	bool		hexdump;		// write hexdump
	bool		rdzero;			// fake read zeroes
};


#define FAT32_MS	4096//32768
#define FAT32_BS	512

const unsigned fat32_bs = FAT32_BS;


#define FAT32_WN(n, p, v)				\
	do {						\
		unsigned t_v_ = (v);			\
		unsigned t_i_, t_n_ = (n);		\
		uint8_t *t_p_ = (p);			\
		for (t_i_ = 0; t_i_ < t_n_; t_i_++) {	\
			*t_p_++ = t_v_;			\
			t_v_ >>= 8;			\
		}					\
	} while (0)

#define FAT32_W2(p, v)	FAT32_WN(2, p, v)
#define FAT32_W4(p, v)	FAT32_WN(4, p, v)

#define FAT32_RN(n, p, v)				\
	do {						\
		unsigned t_v_ = 0;			\
		unsigned t_i_, t_n_ = (n);		\
		const uint8_t *t_p_ = (p) + t_n_;	\
		for (t_i_ = 0; t_i_ < t_n_; t_i_++) {	\
			t_v_ = (t_v_ << 8) | *(--t_p_);	\
		}					\
		(v) = t_v_;				\
	} while (0)

#define FAT32_R2(p, v)	FAT32_RN(2, p, v)
#define FAT32_R4(p, v)	FAT32_RN(4, p, v)

static inline unsigned fat32_rd2(const void *p)
{
	unsigned v;
	FAT32_R2(p, v);
	return v;
}

static inline unsigned fat32_rd4(const void *p)
{
	unsigned v;
	FAT32_R4(p, v);
	return v;
}


// match MBR or BS signature
static inline bool fat32_sig(const void *b, unsigned sector_size)
{
	const uint8_t *p = b + sector_size, *v = --p, *u = --p;
	return *u == 0x55 && *v == 0xaa;
}


// try complete read
static ssize_t readloop(int fd, void *buf, size_t size)
{
	ssize_t rlen, ret = -1;
	size_t len = 0;

	while (size > 0) {
		rlen = read(fd, buf, size);
		if (rlen < 0) { if (errno == EINTR) continue; perror("read()"); goto fail; }
		if (rlen == 0) break;
		buf += rlen;
		len += rlen;
		size -= rlen;
	}

	ret = len;
fail:
	return ret;
}


// fail if incomplete read
static bool readall(int fd, void *buf, size_t size, const struct fat32_meta *fm)
{
	bool ret = false;
	ssize_t rlen;

	if (fm->rdzero) { memset(buf, 0, size); goto success; }

	rlen = readloop(fd, buf, size);
	if (rlen < 0) goto fail;
	if ((size_t)rlen != size) { fprintf(stderr, "Short read\n"); goto fail; }

success:
	ret = true;
fail:
	return ret;
}


// write hex dump
static bool writehex(const void *buf, size_t size)
{
	size_t len = 0;

	while (len < size) {
		if ((len % 16) == 0)
			printf(" ");
		if ((len++ % 32) == 0)
			printf("\n");
		printf(" %02x", *(const uint8_t *)(buf++));
	}
	printf("\n");

	return true;
}


// try complete write
static ssize_t writeloop(int fd, const void *buf, size_t size)
{
	ssize_t rlen, ret = -1;
	size_t len = 0;

	while (size > 0) {
		rlen = write(fd, buf, size);
		if (rlen < 0) { if (errno == EINTR) continue; perror("write()"); goto fail; }
		buf += rlen;
		len += rlen;
		size -= rlen;
	}

	ret = len;
fail:
	return ret;
}


// fail if incomplete write
static bool writeall(int fd, const void *buf, size_t size, const struct fat32_meta *fm)
{
	bool ret = false;
	ssize_t rlen;

	if (fm->hexdump) { ret = writehex(buf, size); goto fail; }

	rlen = writeloop(fd, buf, size);
	if (rlen >= 0 && (size_t)rlen != size) fprintf(stderr, "Short write\n");
	if (rlen < 0 || (size_t)rlen != size) goto fail;

	ret = true;
fail:
	return ret;
}



// ===== ===== ===== BEGIN : skip LFN for now ===== ===== =====



#if 0

// compute checksum from short name to place in long entries
static uint8_t fat32_cksum_short(const struct fat32_dirent *dnt)
{
	unsigned nch = sizeof dnt->name;
	const uint8_t *ptr = dnt->name;
	uint8_t sum = 0;

	do {
		sum = ((sum & 0x01) << 7) + (sum >> 1) + *ptr++;	// rotate right + byte
	} while (--nch != 0);

	return sum;
}


// short or LFN - don't pass Unicode bytes or deleted or Kanji substituted entries
static bool fat32_badch(uint8_t ch, bool lfn)
{
	bool ret = false;

	if (ch < 0x20 || ch >= 0x7f) goto fail;	// rejects NUL bytes incl. Unicode, or extended set until UTF-8 and Unicode aware

	switch (ch) {
	case '+':
	case ',':
	//case '.':
	case ';':
	case '=':
	case '[':
	case ']':
		if (lfn) break;
		// fall through
	case '"':
	case '*':
	case '/':
	case ':':
	case '<':
	case '>':
	case '?':
	case '\\':
	case '|':
		goto fail;
	default:
		break;
	}

out:
	return ret;
fail:
	ret = true;
	goto out;
}


// lfn must be NUL terminated, and basis buffer must hold 11 bytes and will not be terminated
static unsigned fat32_lfn_basis(uint8_t *basis, const uint8_t *lfn)
{
	unsigned ret = 0, pos = 0;
	uint8_t ch;
	const uint8_t *str = lfn;

	if (*str == '\0') goto fail;

	while ((ch = *str) != '\0') {
		if (fat32_badch(ch)) goto fail;
		if (pos == 0 && (ch == ' ' || ch == '.')) continue;	// skip leading spaces and periods
		if (ch >= 'a' || ch <= 'z')
			ch -= 'a' - 'A';
		basis[pos] = ch;
		if (++pos >= 11) goto fail;
	}

fail:
	return ret;
}

#endif



// ===== ===== ===== END : skip LFN for now ===== ===== =====



// look for Linux partition or FAT32 with BS/MBR
static bool fat32_pt(const uint8_t *buf, unsigned extpt[2])
{
	bool ret = false;
	unsigned i, k, v;

	if (buf[0] != 0 && buf[0] != 0x80) { fprintf(stderr, "Invalid partition marker\n"); goto fail; }
	for (i = 0; i < 2; i++) {
		v = 0;
		for (k = 0; k < 4; k++)		// read 32-bit little endian
			v = (v << 8) | buf[8 + i * 4 + 3 - k];
		extpt[i] = v;			// extpt[0] = start, extpt[1] = count
	}
	if (extpt[1] != 0 && buf[4] != 0x83) {	// non-empty but not EXT2/3/4
		if (buf[4] != 0x0c) {		// also not FAT32
			fprintf(stderr, "Unexpected non-Linux partition\n");
			goto fail;
		}
		if (extpt[0] != 0) {		// if FAT32 must be BS and not in MBR PT
			fprintf(stderr, "Unexpected FAT32 partition\n");
			goto fail;
		}
	}

	ret = true;
fail:
	return ret;
}


// return Linux partition extent and signal FAT32 or free slot, or signal empty table
static int fat32_mbr(struct fat32_meta *fm, uint8_t *buf, unsigned extfs[2])
{
	int ret = -1, i, extpte = -1, fatpte = -1, freepte = -1;
	unsigned extpt[2];

	if (!readall(fm->fd, buf, fm->sector_size, fm)) goto fail;

	for (i = 0; i < 4; i++) {		// all MBR partitions
		if (!fat32_pt(buf + 0x1be + i * 16, extpt)) goto fail;	// each PT entry
		if (extpt[0] > extfs[1] || extpt[1] > extfs[1] - extfs[0]) {
			fprintf(stderr, "Bad partition\n");
			goto fail;
		}
		if (extpt[1] == 0) {		// empty
			if (freepte < 0)
				freepte = i;	// free slot
			continue;
		}
		if (extpt[0] == 0) {		// hybrid MBR/BS has PT entry
			if (fatpte >= 0) {
				fprintf(stderr, "Too many FAT32 partitions\n");
				goto fail;
			}
			fatpte = i;		// FAT32 slot
			continue;
		}
		if (extpte >= 0) {
			fprintf(stderr, "Too many Linux partitions\n");
			goto fail;
		}
		extpte = i;			// EXT2/3/4
		extfs[0] = extpt[0];		// FS start
		extfs[1] = extpt[1];		// FS count
	}

	if (extpte < 0) {			// no EXT2/3/4
		if (fatpte >= 0) {		// but have FAT32
			fprintf(stderr, "Missing Linux partition\n");
			goto fail;
		}
		fprintf(stderr, "Will add Linux partition entry...\n");
		ret = 0;			// empty table
		goto fail;			// success
	}

	if (!fat32_sig(buf, fat32_bs) || !fat32_sig(buf, fm->sector_size)) {
		fprintf(stderr, "Missing MBR signature\n");
		goto fail;
	}

	if (fatpte < 0)
		fatpte = freepte;		// if no FAT32 entry then select unused

	ret = fatpte + 1;			// should not return empty == 0
fail:
	return ret;
}


// calculate FAT32 parameters and create partition table if empty
static bool fat32_sb(struct fat32_meta *fm, uint8_t *buf, unsigned extfs[2], unsigned retpte)
{
	bool ret = false, empty = (retpte == 0);
	unsigned ptbase = 0x1be, unixts = time(NULL), fatpte = empty ? 0 : (retpte - 1);
	unsigned sector_size = fm->sector_size, cluster_sectors = fm->cluster_sectors;
	unsigned rsvd_sectors = 3, start_cluster = 2, root_dir_sectors = 32, root_dir_clusters = root_dir_sectors / cluster_sectors;
	unsigned as, cluster_count, min_fat_sectors, fat_sectors, overlap_start_sector;
	uint8_t *ptr, *ptslot = buf + ptbase + fatpte * 16, *pttype = ptslot + 4, *ptnum = ptslot + 12;

	cluster_count = extfs[1] / cluster_sectors;
	if (cluster_count++ >= 0x0ffffff0 - start_cluster - root_dir_clusters) {
		fprintf(stderr, "FS too large\n");
		goto fail;
	}
	min_fat_sectors = (0xfff0 - start_cluster) * sizeof(uint32_t) / sector_size;
	fat_sectors = (cluster_count + start_cluster + root_dir_clusters) * sizeof(uint32_t) / sector_size;
	overlap_start_sector = rsvd_sectors + fat_sectors + root_dir_sectors;
	as = overlap_start_sector % cluster_sectors;
	if (as != 0)
		as = cluster_sectors - as;
	rsvd_sectors += as;
	overlap_start_sector += as;

	if (!empty) {
		if (extfs[0] < overlap_start_sector || extfs[0] <= min_fat_sectors) { fprintf(stderr, "Slack too small\n"); goto fail; }
		overlap_start_sector = extfs[0];
		cluster_count = extfs[1];
		cluster_count = (cluster_count + cluster_sectors - 1) / cluster_sectors;
		fat_sectors = cluster_count + start_cluster + root_dir_clusters;
		as = sector_size / sizeof(uint32_t);
		fat_sectors = (fat_sectors + as - 1) / as;
		rsvd_sectors = overlap_start_sector - root_dir_sectors - fat_sectors;
		fprintf(stderr, "Creating FAT32 FS: rsvd = %u, FAT = %u, root = %u, data = # %u @ %u\n", rsvd_sectors, fat_sectors, root_dir_clusters, cluster_count, overlap_start_sector);
	} else {
		if (fat32_sig(buf, fat32_bs) || fat32_sig(buf, fm->sector_size)) { fprintf(stderr, "Unexpected MBR/BS signature\n"); goto fail; }
		if ((buf[0] == 0xe9 || (buf[0] == 0xeb && buf[2] == 0x90))
		&& buf[0x0b] == (sector_size & 0xff) && buf[0x0c] == (sector_size >> 8)
		&& (buf[0x0e] != 0 || buf[0x0f] != 0)
		&& (buf[0x10] == 1 || buf[0x10] == 2)) { fprintf(stderr, "Unexpected FAT FS likely present\n"); goto fail; }
		fprintf(stderr, "Creating Linux partition at sector %u\n", overlap_start_sector);
	}

	if (lseek(fm->fd, 0, SEEK_SET) != 0) { perror("lseek()"); goto fail; }

	if (*pttype != 0 || fat32_rd4(ptnum) != 0) { buf[0] = 0xeb; buf[1] = 0xfe; memset(buf + 2, 0, ptbase - 2); }

	fm->start_cluster = start_cluster;
	fm->cluster_sectors = cluster_sectors;
	fm->rsvd_sectors = rsvd_sectors;
	fm->fsinfo_sector = 1;
	fm->backup_sector = 2;
	fm->fat_sectors = fat_sectors;
	fm->overlap_start_sector = overlap_start_sector;
	fm->root_dir_clusters = root_dir_clusters;
	//fm->extc = extfs[0];//???
	//fm->extl = extfs[1];//???

	memset(ptslot, 0, 16);

	if (empty) {
		//fm->extc = overlap_start_sector;
		//fm->extl = extfs[1] - overlap_start_sector;//???
		*pttype = 0x83;
		FAT32_W4(ptnum - 4, overlap_start_sector);		// Linux partition start
		FAT32_W4(ptnum, extfs[1] - overlap_start_sector);	// Linux partition sectors
		buf[fat32_bs - 2] = buf[sector_size - 2] = 0x55;
		buf[fat32_bs - 1] = buf[sector_size - 1] = 0xaa;
	}

	if (empty) goto success;

	memset(buf, 0, ptbase);
	buf[0] = 0xeb;
	buf[1] = 0xfe;
	buf[2] = 0x90;
	memcpy(buf + 3, "MSWIN4.1", 8);
	buf[0x0b] = sector_size;
	buf[0x0c] = sector_size >> 8;
	buf[0x0d] = cluster_sectors;
	buf[0x0e] = rsvd_sectors;
	buf[0x10] = 1;				// number of FATs
	// 0x11 = root entries for FAT12/16
	// 0x13 = total sectors < 32 MB
	buf[0x15] = 0xf8;
	// 0x16 = sectors per FAT for FAT12/16
	buf[0x18] = 63;				// sectors per track/cylinder
	buf[0x1a] = 0xff;			// number of heads
	// 0x1c = hidden sectors

	FAT32_W4(buf + 0x20, extfs[1]);		// sectors in partition
	FAT32_W4(buf + 0x24, fat_sectors);	// sectors per FAT

	// 0x28 = active FAT
	// 0x2a = FAT32 version
	buf[0x2c] = start_cluster;		// root start cluster
	buf[0x30] = fm->fsinfo_sector;
	buf[0x32] = fm->backup_sector;
	// 0x34 = reserved
	buf[0x40] = 0x80;			// drive number
	buf[0x42] = 0x29;			// extended signature
	FAT32_W4(buf + 0x43, unixts);		// serial number
	memcpy(buf + 0x47, "FAT32MIRROR", 11);	// volume name
	memcpy(buf + 0x52, "FAT32   ", 8);	// FAT name

	*pttype = 0x0c;				// partition type
	FAT32_W4(ptnum, extfs[1]);		// partition sectors

	if (fm->backup_sector != 0)
		memcpy(buf + fm->backup_sector * sector_size, buf, sector_size);

	ptr = buf + fm->fsinfo_sector * sector_size;
	ptr[fat32_bs - 2] = ptr[sector_size - 2] = 0x55;
	ptr[fat32_bs - 1] = ptr[sector_size - 1] = 0xaa;
	*ptr++ = 'R';
	*ptr++ = 'R';
	*ptr++ = 'a';
	*ptr++ = 'A';
	ptr += 0x1e4 - 4;
	*ptr++ = 'r';
	*ptr++ = 'r';
	*ptr++ = 'A';
	*ptr++ = 'a';
	*ptr++ = 0xff;	// -1 = unknown free clusters
	*ptr++ = 0xff;
	*ptr++ = 0xff;
	*ptr++ = 0xff;

success:
	if (!writeall(fm->fd, buf, rsvd_sectors * sector_size, fm)) goto fail;

	ret = true;
fail:
	return ret;
}


// prepare to map files from source FS
static bool fat32_prep(struct fat32_meta *fm, const char *mntpath, const char *partdev, unsigned extsz)
{
	bool ret = false;
	int fd = -1;
	unsigned sectsz;
	uint64_t devsz;
	struct stat st;

	fd = open(mntpath, O_RDONLY);
	if (fd < 0) { perror("open()"); goto fail; }

	if (fstat(fd, &st) != 0) { perror("fstat()"); goto fail; }

	if (!S_ISDIR(st.st_mode)) { fprintf(stderr, "Not a directory: '%s'\n", mntpath); goto fail; }
	fm->devid = st.st_dev;

	if (ioctl(fd, FIGETBSZ, &fm->fsblksz) != 0) { perror("ioctl(FIGETBSZ"); goto fail; }

	if (fm->fsblksz < fat32_bs || fm->fsblksz > FAT32_MS || ((fm->fsblksz - 1) & fm->fsblksz) != 0) {	// too small/large or not power of 2
		fprintf(stderr, "Unsupported FS block size %u\n", fm->fsblksz);
		goto fail;
	}

	if (fm->fsblksz != fm->sector_size * fm->cluster_sectors) {
		fprintf(stderr, "Unexpected FS block size %u mismatch for cluster size %u\n", fm->fsblksz, fm->sector_size * fm->cluster_sectors);
		goto fail;
	}

	if (fm->dbg) fprintf(stderr, "Got block size %u for device ID %llu\n", fm->fsblksz, (unsigned long long)fm->devid);

	if (partdev == NULL) { fprintf(stderr, "Skipping checks on mounted partition\n"); goto success; }
	if (fd >= 0) close(fd);

	fd = open(partdev, O_RDONLY);
	if (fd < 0) { perror("open()"); goto fail; }

	if (fstat(fd, &st) != 0) { perror("fstat()"); goto fail; }

	if (!S_ISBLK(st.st_mode)) { fprintf(stderr, "Mounted partition path is not block device\n"); goto fail; }

	if (st.st_rdev != fm->devid) { fprintf(stderr, "Mismatch for mount '%s' and device '%s'\n", mntpath, partdev); goto fail; }

	if (ioctl(fd, BLKSSZGET, &sectsz) != 0) { perror("ioctl(BLKSSZGET)"); goto fail; }

	if (sectsz != fm->sector_size) { fprintf(stderr, "Sector size mismatch for mounted partition and device node\n"); goto fail; }

	if (ioctl(fd, BLKGETSIZE64, &devsz) != 0) { perror("ioctl(BLKGETSIZE64)"); goto fail; }

	if (devsz != extsz) { fprintf(stderr, "Mismatch for mounted partition and MBR slot size\n"); goto fail; }
success:
	ret = true;
fail:
	if (fd >= 0) close(fd);
	return ret;
}


#if 1

#include <linux/fiemap.h>

// ...
static unsigned fat32_blk(int fd, unsigned blk, struct fat32_meta *fm)
{
	struct fiemap tmap;
	unsigned ret = 0, i, numext = 256, sz = sizeof tmap + numext * sizeof *tmap.fm_extents;
	union {
		struct fiemap emap;
		char arr[sz];
	} umap;
	struct fiemap *extmap = &umap.emap; 
	struct fiemap_extent *fext;

	memset(extmap, 0, sz);

	memset(extmap, 0, sizeof *extmap);
	extmap->fm_start = blk * fm->fsblksz;//0;
	extmap->fm_length = fm->fsblksz;//stfile.st_size;
	extmap->fm_flags = 0;
	extmap->fm_mapped_extents = 0;
	extmap->fm_extent_count = numext;
	if (ioctl(fd, FS_IOC_FIEMAP, extmap) != 0) { perror("ioctl(FS_IOC_FIEMAP)"); goto fail; }

	for (i = 0, fext = extmap->fm_extents
	; i < extmap->fm_mapped_extents
	; i++, fext++) {
		printf("FIEMAP\t%llx\t%llx\t%llx\t%x\n"
		, fext->fe_logical, fext->fe_physical, fext->fe_length
		, fext->fe_flags & ~FIEMAP_EXTENT_LAST);
		if ((fext->fe_flags & FIEMAP_EXTENT_LAST) != 0) break;
	}

	fext = extmap->fm_extents;
	if (extmap->fm_mapped_extents < 1
	|| (fext->fe_physical % fm->fsblksz) != 0
	|| (fext->fe_flags & ~(FIEMAP_EXTENT_LAST | FIEMAP_EXTENT_MERGED)) != 0) { fprintf(stderr, "Unexpected special or non-contiguous block\n"); goto fail; }
	ret = fext->fe_physical / (fm->sector_size * fm->cluster_sectors) + fm->root_dir_clusters + fm->start_cluster;
fail:
	return ret;
}

#else

// convert file block to cluster
static unsigned fat32_blk(int fd, unsigned blk, struct fat32_meta *fm)
{
	unsigned ret = 0;

	if (ioctl(fd, FIBMAP, &blk) != 0) { perror("ioctl(FIBMAP)"); goto fail; }

	if (blk == 0) { fprintf(stderr, "Unexpected special or non-contiguous block\n"); goto fail; }

	ret = blk * fm->sector_size * fm->cluster_sectors / fm->fsblksz + fm->root_dir_clusters + fm->start_cluster;	// non-zero cluster
fail:
	return ret;
}

#endif


// memory map FAT32 structures
static bool fat32_map(struct fat32_meta *fm)
{
	bool ret = false;
	uint64_t len = (fm->rsvd_sectors + fm->fat_sectors + (fm->root_dir_clusters * fm->cluster_sectors)) * fm->sector_size;

	if (len >= INT32_MAX) { fprintf(stderr, "Unable to map oversized metadata\n"); goto fail; }

	fm->ptr = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fm->fd, 0);
	if (fm->ptr == MAP_FAILED) { perror("mmap()"); goto fail; }
	fm->len = len;

	ret = true;
fail:
	return ret;
}


// clean up
static bool fat32_free(struct fat32_meta *fm)
{
	bool ret = true;

	if (fm->ptr != MAP_FAILED && munmap(fm->ptr, fm->len) != 0) { perror("munmap()"); ret = false; }

	if (fm->fd >= 0 && close(fm->fd) != 0) { perror("close()"); ret = false; }

	return ret;
}


// TODO
static bool fat32_file(struct fat32_meta *fm, const char *filepath)
{
	bool ret = false;
	int fd = -1;
	unsigned blk, i;
	struct stat st;

	fd = open(filepath, O_RDONLY);
	if (fd < 0) { perror("open()"); goto fail; }

	if (fstat(fd, &st) != 0) { perror("fstat()"); goto fail; }

	if (st.st_dev != fm->devid) { fprintf(stderr, "Wrong mount point for file '%s'\n", filepath); goto fail; }

	if (S_ISDIR(st.st_mode)) { fprintf(stderr, "Skipping directory '%s'\n", filepath); goto success; }

	if (!S_ISREG(st.st_mode)) { fprintf(stderr, "Not a regular file: '%s'\n", filepath); goto fail; }

	if (st.st_size >= UINT32_MAX) { fprintf(stderr, "Oversized file '%s'\n", filepath); goto fail; }

	if (fm->dbg && st.st_size == 0) fprintf(stderr, "Empty file '%s'\n", filepath);

	for (i = 0; i < (st.st_size + fm->fsblksz - 1) / fm->fsblksz; i++) {
		blk = fat32_blk(fd, i, fm);
		if (blk == 0) goto fail;
		if (fm->dbg) fprintf(stderr, "Got file block %u maps to cluster %u\n", fm->fsblksz, blk);
	}

success:
	ret = true;
fail:
	if (fd >= 0) close(fd);
	return ret;
}


static unsigned fat32_open(struct fat32_meta *fm, const char *devpath)
{
	unsigned ret = 0, sectsz = 0;
	uint64_t devsz;
	struct stat st;

	fm->fd = open(devpath, O_RDWR);
	if (fm->fd < 0) { perror("open()"); goto fail; }

	if (fstat(fm->fd, &st) != 0) { perror("fstat()"); goto fail; }

	if (!S_ISBLK(st.st_mode)) {
		if (S_ISREG(st.st_mode)) { fprintf(stderr, "Not a block device node\n"); goto fail; }
		fprintf(stderr, "Operating on regular file as block device\n");
		sectsz = fat32_bs;	// FIXME: allow override e.g. 4096
		if (st.st_size % sectsz != 0) { fprintf(stderr, "File size is not multiple of sector size\n"); goto fail; }
		devsz = st.st_size;
	}

	if (sectsz == 0) {
		if (ioctl(fm->fd, BLKSSZGET, &sectsz) != 0) { perror("ioctl(BLKSSZGET)"); goto fail; }
		if (sectsz < fat32_bs || sectsz > FAT32_MS || ((sectsz - 1) & sectsz) != 0) {	// too small/large or not power of 2
			fprintf(stderr, "Unsupported sector size %u\n", sectsz);
			goto fail;
		}

		if (ioctl(fm->fd, BLKGETSIZE64, &devsz) != 0) { perror("ioctl(BLKGETSIZE64)"); goto fail; }
		if (devsz == 0) { fprintf(stderr, "Device has zero capacity\n"); goto fail; }
	}
	fm->sector_size = sectsz;

	if (devsz % sectsz != 0) { fprintf(stderr, "Bad device size\n"); goto fail; }

	if (devsz >= (uint64_t)UINT32_MAX * sectsz) { fprintf(stderr, "Rejecting oversized device\n"); goto fail; }

	ret = devsz / sectsz;
fail:
	return ret;
}


// main loop
static bool fat32_prepend(struct fat32_meta *fm, const char *devpath, const char *mntpath, const char *partdev)
{
	bool ret = false;
	int fatpte, cluster_sectors = 8;
	unsigned extfs[2] = { 0 };
	uint8_t buf[FAT32_MS * 3] = { 0 };
	ssize_t rlen;
	size_t len = 0;
	char *line = NULL;

	extfs[1] = fat32_open(fm, devpath);
	if (extfs[1] == 0) goto fail;

	fatpte = fat32_mbr(fm, buf, extfs);
	if (fatpte < 0) goto fail;

	if ((extfs[0] % cluster_sectors) != 0)	// TODO: set this in sb with fsblksz from prep
		cluster_sectors = 1;
	fm->cluster_sectors = cluster_sectors;

	if (!fat32_sb(fm, buf, extfs, fatpte)) goto fail;

	if (fatpte == 0 && fm->dbg) {
		fprintf(stderr, "Re-evaluating MBR...\n");
		if (lseek(fm->fd, 0, SEEK_SET) != 0) { perror("lseek()"); goto fail; }
		fatpte = fat32_mbr(fm, buf, extfs);
		if (fatpte < 0) goto fail;
		if (!fat32_sb(fm, buf, extfs, fatpte)) goto fail;
	}

	if (fatpte == 0) goto success;

	if (!fat32_prep(fm, mntpath, partdev, extfs[1])) goto fail;	// too late as sb already wrote

	if (!fat32_map(fm)) goto fail;

	while ((rlen = getline(&line, &len, stdin)) >= 0) {
		if (rlen > 0 && line[rlen - 1] == '\n') line[--rlen] = '\0';
		if (rlen > 0 && line[rlen - 1] == '\r') line[--rlen] = '\0';
		if (rlen == 0) continue;
		if (!fat32_file(fm, line)) goto fail;
	}

success:
	ret = true;
fail:
	if (line != NULL) free(line);
	return ret;
}


#define CMD_STRIFIER(t)		# t
#define CMD_STRIFY(t)		CMD_STRIFIER(t)

#define CMD_DGOCT(c, d)		((((unsigned char)(c)) >> (3 * d)) & ((1u << 3) - 1))
#define CMD_CH(s)		(*(s))

/* ???
 * 'f' - overwrite any existing FAT32 FS
 * 's' - don't partition if missing MBR signature (0x55 0xaa)
 * 'n' - don't auto-partition
 * '1'-'4' - select EXT4 partition by MBR PT index
 * 
 */

#define CMDOPT_DEVPATH		'd'	// string
#define CMDOPT_DBGMSG		'g'
#define CMDOPT_HEXDUMP		'h'
#define CMDOPT_MNTPATH		'm'	// string
#define CMDOPT_PARTDEV		'p'	// string
#define CMDOPT_TESTRDZ		'z'

#define CMD_NCH			(1 << 8)
#define CMD_CH_STR(...)		((char []) { __VA_ARGS__, '\0' })
#define CMD_ARG_REQ(c)		(c), ':'

#define CMD_IDX(c)		[(unsigned char)(c)]
#define CMD_ARG(c)		((argopt)CMD_IDX(c))

#define CMD_REQ_OPT_ARG		1
#define CMD_REQ_OPT_NOARG	2
#define CMD_REQ_NOOPT_NOARG	-CMD_REQ_OPT_ARG
#define CMD_REQ_NOOPT_ARG	-CMD_REQ_OPT_NOARG
#define CMD_REQ_UNDEF		0
#define CMD_REQ_NONE(x)		((x) == CMD_REQ_UNDEF)
#define CMD_REQ_OPT(x)		((x) > CMD_REQ_UNDEF)
#define CMD_REQ_NOOPT(x)	((x) < CMD_REQ_UNDEF)
#define CMD_REQ_ARG(x)		((x) == CMD_REQ_OPT_ARG || (x) == CMD_REQ_NOOPT_ARG)
#define CMD_REQ_NOARG(x)	((x) == CMD_REQ_OPT_NOARG || (x) == CMD_REQ_NOOPT_NOARG)


static int fat32_getopt(int argc, char *const argv[], const char *argopt[CMD_NCH])
{
	bool ret = false;
	int nerr = 0, opt;
	const char *const msgopt[CMD_NCH] = {
		CMD_IDX(CMDOPT_DEVPATH)	= "device path",
		CMD_IDX(CMDOPT_MNTPATH)	= "mount path",
		CMD_IDX(CMDOPT_PARTDEV) = "mounted partition",
		CMD_IDX(CMDOPT_DBGMSG)	= "debug messages",
		CMD_IDX(CMDOPT_HEXDUMP)	= "write hexdump",
		CMD_IDX(CMDOPT_TESTRDZ)	= "fake read zeroes",
	};
	const char *const stropt = CMD_CH_STR('+',	// stop at first non-option
				CMD_ARG_REQ(CMDOPT_DEVPATH),
				CMD_ARG_REQ(CMDOPT_MNTPATH),
				CMD_ARG_REQ(CMDOPT_PARTDEV),
				CMDOPT_DBGMSG,
				CMDOPT_HEXDUMP,
				CMDOPT_TESTRDZ);
	const int reqarg[CMD_NCH] = {
		CMD_IDX(CMDOPT_DEVPATH)	= CMD_REQ_OPT_ARG,
		CMD_IDX(CMDOPT_MNTPATH)	= CMD_REQ_OPT_ARG,
		CMD_IDX(CMDOPT_PARTDEV)	= CMD_REQ_NOOPT_ARG,
		CMD_IDX(CMDOPT_DBGMSG)	= CMD_REQ_NOOPT_NOARG,
		CMD_IDX(CMDOPT_HEXDUMP)	= CMD_REQ_NOOPT_NOARG,
		CMD_IDX(CMDOPT_TESTRDZ)	= CMD_REQ_NOOPT_NOARG,
	};

	opterr = 0;	// inhibit getopt() output via global
	while ((opt = getopt(argc, argv, stropt)) >= 0) {
		if (opt >= CMD_NCH)
			goto fail;
		switch (reqarg[opt]) {
		case CMD_REQ_OPT_NOARG:
		case CMD_REQ_NOOPT_NOARG:
			optarg = "";	// force non-NULL for below and to flag presence
			// fall through
		case CMD_REQ_OPT_ARG:
		case CMD_REQ_NOOPT_ARG:
			if (CMD_ARG(opt) != NULL) {
				fprintf(stderr, "Unexpected option '-%c' parameter '%s', already have '%s' (%s)\n", opt, optarg, argopt[opt], msgopt[opt]);
				nerr++;
				break;
			}
			argopt[opt] = optarg;
			break;
		case CMD_REQ_UNDEF:
		default:
			goto missing;
		}
	}

	if (optind < argc) { fprintf(stderr, "Unexpected non-option parameter '%s'\n", argv[optind]); goto usage; }

	if (optind <= 1) goto usage;

	for (opt = 0; opt < CMD_NCH; opt++) {
		if (CMD_ARG(opt) != NULL || !CMD_REQ_OPT(reqarg[opt])) continue;
		fprintf(stderr, "Missing required option '-%c' (%s)\n", opt, msgopt[opt]);
		nerr++;
	}

	if (nerr != 0) goto usage;

	ret = true;
fail:
	return ret;

missing:
	opt = optopt;
	if (opt >= 0 && opt < CMD_NCH) {
		if (CMD_REQ_ARG(reqarg[opt])) { fprintf(stderr, "Missing required parameter for option '-%c' (%s)\n", opt, msgopt[opt]); goto usage; }
		fprintf(stderr, "Unknown option '-%c'\n", opt);
	}
usage:
	fprintf(stderr, "Usage: %s -d /dev/mmcblkX -m /mnt/ext4\n", argv[0]);
	for (opt = 0; opt < CMD_NCH; opt++)
		if (reqarg[opt] != 0) fprintf(stderr, "\t%s\t-%c\t%s\t\t%s\n", CMD_REQ_OPT(reqarg[opt]) ? "REQUIRED:" : "optional:", opt, CMD_REQ_ARG(reqarg[opt]) ? "<arg>" : "", msgopt[opt]);
	goto fail;
}


int main(int argc, char *argv[])
{
	int ret = EXIT_FAILURE;
	const char *argopt[CMD_NCH] = { NULL };
	struct fat32_meta fm = { .fd = -1, .ptr = MAP_FAILED, };

	if (!fat32_getopt(argc, argv, argopt)) goto fail;

	fm.dbg = (CMD_ARG(CMDOPT_DBGMSG) != NULL);
	fm.hexdump = (CMD_ARG(CMDOPT_HEXDUMP) != NULL);
	fm.rdzero = (CMD_ARG(CMDOPT_TESTRDZ) != NULL);

	if (!fat32_prepend(&fm, CMD_ARG(CMDOPT_DEVPATH), CMD_ARG(CMDOPT_MNTPATH), CMD_ARG(CMDOPT_PARTDEV)))
		goto fail;

	ret = EXIT_SUCCESS;
fail:
	if (!fat32_free(&fm)) ret = EXIT_FAILURE;
	return ret;
}
