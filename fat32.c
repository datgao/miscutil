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
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

#include <linux/fs.h>
#if !defined(NO_EXTENT_MAP) && defined(FS_IOC_FIEMAP)
#include <linux/fiemap.h>
#define TRY_EXTENT_MAP
#endif

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a)	(sizeof (a) / sizeof (a)[0])
#endif

#define FAT32_BS		512		// min sector size
#define FAT32_MS		4096		// max sector size
#define FAT32_MIN_RSVD_SECTORS	3

#define FAT32_BOOTMBR_SIG	0xaa55
#define FAT32_CLUSTER_FREE	0
#define FAT32_CLUSTER_END	0x0fffffff
#define FAT32_CLUSTER_BAD	0x0ffffff7

// dirent initial char
#define FAT32_DIRENT_SUB	0x05
#define FAT32_DIRENT_DEL	0xe5
#define FAT32_DIRENT_FREE	0x00
#define FAT32_DIRENT_PAD	' '

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

struct fat32_dirent {				// short dirent
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

// duplicates must not exist across both long and short namespaces
// long name becomes invalid if short name exists

#define FAT32_LFN_ORD_LAST	0x40

struct fat32_lfnent {				// unused chars: 0x0000 terminate then 0xffff fill
	uint8_t		ord;			// sequence: ORD_LAST | n, n - 1, ..., 1, short entry
	uint8_t		name1[10];		// chars 1-5
	uint8_t		attr;
	uint8_t		type;			// zero - other values reserved
	uint8_t		cksum;
	uint8_t		name2[12];		// chars 6-11
	uint16_t	clustlo;		// zero
	uint8_t		name3[4];		// chars 12-13
};


struct fat32_em {				// one extent mapped from logical file to FS partition
	unsigned	len;
	unsigned	logical;		// block within file
	unsigned	physical;		// corresponding block relative to FS partition
};

#define FAT32_EXTENT_BATCH	4096		// only one syscall if avg fragment size in blocks, for 4GB-1 file and FS block of 4 KB
#define FAT32_BATCH_CLUSTER	256		// independent values esp. smaller will use extent cache when possible

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
	unsigned	cluster_size;
	unsigned	fsblksz;		// block size of mounted FS
	dev_t		devid;			// device ID of mounted FS

	bool		quiet;			// log only errors and warnings
	bool		dbg;			// debug messages
	bool		direct;			// modify on the fly - do not buffer changes in private memory

	const char	*devpath;
	const char	*mntpath;
	const char	*partdev;

#ifdef TRY_EXTENT_MAP
	bool		blkmap;			// fallback to block mapping - extent mapping not supported by mounted FS
	bool		extmap;			// extent mapping worked the first time - successive errors are fatal

	unsigned	ext_num;
	unsigned	ext_idx;
	struct fat32_em	ext_arr[FAT32_EXTENT_BATCH];
#endif

	unsigned	cluster[FAT32_BATCH_CLUSTER];	// split out extents up to limit - if full then some extents may remain
};


#define CMD_NCH			(1 << 8)

#define F32BCH(c)		[(unsigned char)(c)] = 1	// short dirent - but valid for long name
#define F32BCH_(c)		[(unsigned char)(c)] = -1	// long name - also invalid for short name

static const signed char fat32_dirent_badch[CMD_NCH] = {
	F32BCH_(0x00), F32BCH_(0x01), F32BCH_(0x02), F32BCH_(0x03), F32BCH_(0x04), F32BCH_(0x05), F32BCH_(0x06), F32BCH_(0x07),
	F32BCH_(0x08), F32BCH_(0x09), F32BCH_(0x0a), F32BCH_(0x0b), F32BCH_(0x0c), F32BCH_(0x0d), F32BCH_(0x0e), F32BCH_(0x0f),
	F32BCH_(0x10), F32BCH_(0x11), F32BCH_(0x12), F32BCH_(0x13), F32BCH_(0x14), F32BCH_(0x15), F32BCH_(0x16), F32BCH_(0x17),
	F32BCH_(0x18), F32BCH_(0x19), F32BCH_(0x1a), F32BCH_(0x1b), F32BCH_(0x1c), F32BCH_(0x1d), F32BCH_(0x1e), F32BCH_(0x1f),
	F32BCH(' '),     /* '!' */    F32BCH_('"'),    /* 0x22 < ch < 0x2a */
	F32BCH_('*'),  F32BCH('+'),   F32BCH(','),     /* '-' */    F32BCH('.'),   F32BCH_('/'),    /* 0x2f < ch < 0x3a */
	F32BCH_(':'),  F32BCH(';'),   F32BCH_('<'),  F32BCH('='),   F32BCH_('>'),  F32BCH_('?'),    /* 0x3f < ch < 0x5b */
	F32BCH('['),   F32BCH_('\\'), F32BCH(']'),     /* 0x5d < ch < 0x7c */
	F32BCH_('|'),    /* 0x7c < ch <= 0xff */
};

static bool fat32_badch_dirent(unsigned char uch)
{
	return (fat32_dirent_badch[uch] != 0);
}

static bool fat32_badch_lfn(unsigned char uch)
{
	return (fat32_dirent_badch[uch] < 0);
}


enum fat32_level {
	FAT32_LOG_NONE,		// always output but skip prefix since level is N/A
	FAT32_LOG_FATAL,
	FAT32_LOG_ERROR,
	FAT32_LOG_WARN,
	FAT32_LOG_NOTICE,
	FAT32_LOG_INFO,
	FAT32_LOG_DEBUG,
	FAT32_LOG_LEVELS
};

static const char *const fat32_level_pfx[FAT32_LOG_LEVELS] = {
	[FAT32_LOG_FATAL]	= "Fatal",
	[FAT32_LOG_ERROR]	= "Error",
	[FAT32_LOG_WARN]	= "Warning",
	[FAT32_LOG_NOTICE]	= "Notice",
	[FAT32_LOG_INFO]	= "Info",
	[FAT32_LOG_DEBUG]	= "Debug",
};


static int fat32_vsnprintf(char *buf, int size, int pos, const char *fmt, va_list ap)
{
	int ret = -1, amt = 0;

	if (pos < 0 || size <= 0 || pos >= size) goto fail;

	if (pos == size - 1) goto success;

	amt = vsnprintf(buf + pos, size - pos, fmt, ap);
	if (amt < 0) goto fail;

success:
	ret = pos + amt;
	if (ret >= size) ret = size - 1;
fail:
	return ret;
}

#if defined(__GNUC__) || defined(__clang__)
# define FAT32_PRINTF_LIKE(fmtargidx, varargidx)	__attribute__((format(printf, (fmtargidx), (varargidx))))
#else
# define FAT32_PRINTF_LIKE(fmtargidx, varargidx)
#endif

static int FAT32_PRINTF_LIKE(4,5) fat32_snprintf(char *buf, int size, int pos, const char *fmt, ...)
{
	va_list ap;
	int ret;

	va_start(ap, fmt);
	ret = fat32_vsnprintf(buf, size, pos, fmt, ap);
	va_end(ap);
	return ret;
}

static void fat32_vlog(unsigned level, const char *fmt, va_list ap)
{
	const char *pfx = NULL;
	char buf[4096] = "";
	static const char ellipsis[] = " ... <truncated>";
	int pos = 0, size = sizeof buf, rsv = sizeof ellipsis, amt = rsv - 1, adj = size - rsv;

	if (level < ARRAY_SIZE(fat32_level_pfx)) pfx = fat32_level_pfx[level];
	if (pfx != NULL) pos = fat32_snprintf(buf, size, pos, "%s: ", pfx);

	pos = fat32_vsnprintf(buf, size, pos, fmt, ap);
	if (pos < 0) goto fail;

	if (pos > adj) { pos = adj; memcpy(buf + pos, ellipsis, rsv); pos += amt; }
fail:
	fprintf(stderr, "%s\n", buf);
}

static void FAT32_PRINTF_LIKE(2,3) fat32_log(unsigned level, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fat32_vlog(level, fmt, ap);
	va_end(ap);
}

static void fat32_vmsg(const struct fat32_meta *fm, unsigned level, const char *fmt, va_list ap)
{
	switch (level) {
	case FAT32_LOG_DEBUG:
		if (!fm->dbg) break;
		// fall through
	case FAT32_LOG_INFO:
		if (fm->quiet) break;
		// fall through
	case FAT32_LOG_NOTICE:
	case FAT32_LOG_WARN:
	case FAT32_LOG_ERROR:
	case FAT32_LOG_FATAL:
	case FAT32_LOG_NONE:
	default:
		fat32_vlog(level, fmt, ap);
		break;
	}
}

static void FAT32_PRINTF_LIKE(3,4) fat32_msg(const struct fat32_meta *fm, unsigned level, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fat32_vmsg(fm, level, fmt, ap);
	va_end(ap);
}

static void FAT32_PRINTF_LIKE(2,3) fat32_dbg(const struct fat32_meta *fm, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fat32_vmsg(fm, FAT32_LOG_DEBUG, fmt, ap);
	va_end(ap);
}


// read and write little endian - unaligned

static void fat32_wr_n(void *p, unsigned v, unsigned n)
{
	uint8_t *b = p;

	while (n-- > 0) {
		*b++ = v;
		v >>= 8;
	}
}

static void fat32_wr16(void *p, unsigned v)
{
	fat32_wr_n(p, v, 2);
}

static void fat32_wr32(void *p, unsigned v)
{
	fat32_wr_n(p, v, 4);
}

static unsigned fat32_rd_n(const void *p, unsigned n)
{
	const uint8_t *b = (const uint8_t *)p + n;
	unsigned v = 0;

	while (n-- > 0) {
		v = (v << 8) | *(--b);
	}

	return v;
}

static unsigned fat32_rd16(const void *p)
{
	return fat32_rd_n(p, 2);
}

static unsigned fat32_rd32(const void *p)
{
	return fat32_rd_n(p, 4);
}


// MBR or BS signature

// check for only one
static bool fat32_check_sig(const void *b, unsigned sector_size)
{
	return fat32_rd16((const uint8_t *)b + sector_size - 2) == FAT32_BOOTMBR_SIG;
}

#if 0

// has any
static bool fat32_has_sig(const void *p, unsigned sector_size)
{
	return (fat32_check_sig(p, FAT32_BS) || fat32_check_sig(p, sector_size));
}

#endif

// all valid
static bool fat32_valid_sig(const void *p, unsigned sector_size)
{
	return (fat32_check_sig(p, FAT32_BS) && fat32_check_sig(p, sector_size));
}


static void fat32_write_sig(void *b, unsigned sector_size)
{
	fat32_wr16((uint8_t *)b + FAT32_BS - 2, FAT32_BOOTMBR_SIG);
	fat32_wr16((uint8_t *)b + sector_size - 2, FAT32_BOOTMBR_SIG);
}


// try to read up to requested size - return amount possible, or -1 on any error
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


// read entire size requested, or fail
static bool readall(int fd, void *buf, size_t size)
{
	bool ret = false;
	ssize_t rlen;

	rlen = readloop(fd, buf, size);
	if (rlen < 0) goto fail;
	if ((size_t)rlen != size) { fat32_log(FAT32_LOG_FATAL, "short read"); goto fail; }

	ret = true;
fail:
	return ret;
}


// try full write - return amount actually satisfied or -1 on any error
static ssize_t writeloop(int fd, const void *buf, size_t size)
{
	ssize_t rlen, ret = -1;
	size_t len = 0;

	while (size > 0) {
		rlen = write(fd, buf, size);
		if (rlen < 0) { if (errno == EINTR) continue; perror("write()"); goto fail; }
		if (rlen == 0) break;	// avoid infinite loop - TODO: when vs. ENOSPC?
		buf += rlen;
		len += rlen;
		size -= rlen;
	}

	ret = len;
fail:
	return ret;
}


// write complete buffer size, or fail
static bool writeall(int fd, const void *buf, size_t size)
{
	bool ret = false;
	ssize_t rlen;

	rlen = writeloop(fd, buf, size);
	if (rlen >= 0 && (size_t)rlen != size) fat32_log(FAT32_LOG_FATAL, "short write");
	if (rlen < 0 || (size_t)rlen != size) goto fail;

	ret = true;
fail:
	return ret;
}


// pass PT entry to fetch partition offset and size - error if not: empty, Linux EXT2/3/4, or FAT32 with BS/MBR
static bool fat32_pt(const uint8_t *buf, unsigned extpt[2])
{
	bool ret = false;
	unsigned i, k, v;

	if (buf[0] != 0 && buf[0] != 0x80) { fat32_log(FAT32_LOG_FATAL, "invalid partition marker"); goto fail; }
	for (i = 0; i < 2; i++) {
		v = 0;
		for (k = 0; k < 4; k++)		// read 32-bit little endian
			v = (v << 8) | buf[8 + i * 4 + 3 - k];
		extpt[i] = v;			// extpt[0] = start, extpt[1] = count
	}
	if (extpt[1] != 0 && buf[4] != 0x83) {	// non-empty but not EXT2/3/4
		if (buf[4] != 0x0c) {		// also not FAT32
			fat32_log(FAT32_LOG_FATAL, "unexpected non-Linux partition");
			goto fail;
		}
		if (extpt[0] != 0) {		// if FAT32 must be BS and not in MBR PT
			fat32_log(FAT32_LOG_FATAL, "unexpected FAT32 partition");
			goto fail;
		}
	}

	ret = true;
fail:
	return ret;
}


// reads MBR and fetches Linux partition extent - return slot of FAT32 BS/MBR, selected free slot, or signal empty table
static int fat32_mbr(const struct fat32_meta *fm, uint8_t *buf, unsigned extfs[2])
{
	int ret = -1, i, extpte = -1, fatpte = -1, freepte = -1;
	unsigned extpt[2];

	if (!readall(fm->fd, buf, fm->sector_size)) goto fail;

	if (!fat32_valid_sig(buf, fm->sector_size)) { fat32_msg(fm, FAT32_LOG_FATAL, "missing MBR signature"); goto fail; }

	for (i = 0; i < 4; i++) {		// all MBR partitions
		if (!fat32_pt(buf + 0x1be + i * 16, extpt)) goto fail;	// each PT entry
		if (extpt[0] > extfs[1] || extpt[1] > extfs[1] - extfs[0]) { fat32_msg(fm, FAT32_LOG_FATAL, "bad partition"); goto fail; }
		if (extpt[1] == 0) {		// empty
			if (freepte < 0)
				freepte = i;	// free slot
			continue;
		}
		if (extpt[0] == 0) {		// hybrid MBR/BS has PT entry
			if (fatpte >= 0) { fat32_msg(fm, FAT32_LOG_FATAL, "too many FAT32 partitions"); goto fail; }
			fatpte = i;		// FAT32 slot
			continue;
		}
		if (extpte >= 0) { fat32_msg(fm, FAT32_LOG_FATAL, "too many Linux partitions"); goto fail; }
		extpte = i;			// EXT2/3/4
		extfs[0] = extpt[0];		// FS start
		extfs[1] = extpt[1];		// FS count
	}

	if (fatpte >= 0) { fat32_msg(fm, FAT32_LOG_FATAL, "unexpected FAT32 partition present"); goto fail; }

	if (extpte < 0) { fat32_msg(fm, FAT32_LOG_FATAL, "empty partition table"); goto fail; }

	if (fatpte < 0) fatpte = freepte;	// if no FAT32 entry then select unused

	ret = fatpte + 1;			// distinct from empty == 0
fail:
	return ret;
}


// calculate FAT32 parameters and create partition table if empty
static bool fat32_sb(struct fat32_meta *fm, uint8_t *buf, unsigned extfs[2], unsigned retpte)
{
	bool ret = false;
	unsigned ptbase = 0x1be, fatpte = retpte - 1, unixts = time(NULL);
	unsigned sector_size = fm->sector_size, cluster_sectors = fm->cluster_sectors, fat32ent_per_sector = sector_size / sizeof(uint32_t);
	unsigned min = FAT32_MIN_RSVD_SECTORS, rsvd_sectors = min, start_cluster = 2, root_dir_sectors = 32, root_dir_clusters = root_dir_sectors / cluster_sectors;
	unsigned clus, sect, cluster_count, min_fat_sectors, fat_sectors, overlap_start_sector;
	uint8_t *ptr, *ptslot = buf + ptbase + fatpte * 16, *pttype = ptslot + 4, *ptnum = ptslot + 12;

	cluster_count = extfs[1] / cluster_sectors;
	if (cluster_count++ >= 0x0ffffff0 - start_cluster - root_dir_clusters) {
		fat32_msg(fm, FAT32_LOG_FATAL, "FS too large");
		goto fail;
	}
	min_fat_sectors = (0xfff0 - start_cluster) / fat32ent_per_sector;
	fat_sectors = (cluster_count + start_cluster + root_dir_clusters + fat32ent_per_sector - 1) / fat32ent_per_sector;
	overlap_start_sector = rsvd_sectors + fat_sectors + root_dir_sectors;
	sect = overlap_start_sector % cluster_sectors;
	if (sect != 0)
		sect = cluster_sectors - sect;
	rsvd_sectors += sect;
	overlap_start_sector += sect;

	if (extfs[0] < overlap_start_sector || extfs[0] <= min_fat_sectors) { fat32_msg(fm, FAT32_LOG_FATAL, "slack too small"); goto fail; }
	overlap_start_sector = extfs[0];
	cluster_count = extfs[1];
	cluster_count = (cluster_count + cluster_sectors - 1) / cluster_sectors;
	fat_sectors = cluster_count + start_cluster + root_dir_clusters;
	fat_sectors = (fat_sectors + fat32ent_per_sector - 1) / fat32ent_per_sector;
	rsvd_sectors = overlap_start_sector - root_dir_sectors - fat_sectors;

	clus = (rsvd_sectors - min) / cluster_sectors;
	sect = (clus + fat32ent_per_sector - 1) / fat32ent_per_sector;
	clus -= (sect + cluster_sectors - 1) / cluster_sectors;
	sect = (clus + fat32ent_per_sector - 1) / fat32ent_per_sector;
	root_dir_clusters += clus;
	root_dir_sectors = root_dir_clusters * cluster_sectors;
	fat_sectors += sect;
	rsvd_sectors -= sect + clus * cluster_sectors;
	sect = (rsvd_sectors + fat_sectors) % cluster_sectors;
	if (sect != 0)
		rsvd_sectors += cluster_sectors - sect;
	fat32_msg(fm, FAT32_LOG_INFO, "creating FAT32 FS: rsvd = %u, FAT = %u, root = %u, data = # %u @ %u", rsvd_sectors, fat_sectors, root_dir_clusters, cluster_count, overlap_start_sector);

	if (lseek(fm->fd, 0, SEEK_SET) != 0) { perror("lseek()"); goto fail; }

	if (*pttype != 0 || fat32_rd32(ptnum) != 0) { buf[0] = 0xeb; buf[1] = 0xfe; memset(buf + 2, 0, ptbase - 2); }

	fm->start_cluster = start_cluster;
	fm->rsvd_sectors = rsvd_sectors;
	fm->fsinfo_sector = 1;
#if FAT32_MIN_RSVD_SECTORS > 2
	fm->backup_sector = 2;
#else
	fm->backup_sector = 0;
#endif
	fm->fat_sectors = fat_sectors;
	fm->overlap_start_sector = overlap_start_sector;
	fm->root_dir_clusters = root_dir_clusters;
	//fm->extc = extfs[0];//???
	//fm->extl = extfs[1];//???

	memset(ptslot, 0, 16);

	memset(buf, 0, ptbase);
	buf[0] = 0xeb;
	buf[1] = 0xfe;
	buf[2] = 0x90;
	memcpy(buf + 3, "MSWIN4.1", 8);
	fat32_wr16(buf + 0x0b, sector_size);
	buf[0x0d] = cluster_sectors;
	fat32_wr16(buf + 0x0e, rsvd_sectors);
	buf[0x10] = 1;				// number of FATs
	// 0x11 = root entries for FAT12/16
	// 0x13 = total sectors < 32 MB
	buf[0x15] = 0xf8;
	// 0x16 = sectors per FAT for FAT12/16
	buf[0x18] = 63;				// sectors per track/cylinder
	buf[0x1a] = 255;			// number of heads
	// 0x1c = hidden sectors

	fat32_wr32(buf + 0x20, extfs[1]);	// sectors in partition
	fat32_wr32(buf + 0x24, fat_sectors);	// sectors per FAT

	// 0x28 = active FAT
	// 0x2a = FAT32 version
	buf[0x2c] = start_cluster;		// root start cluster
	buf[0x30] = fm->fsinfo_sector;
	buf[0x32] = fm->backup_sector;
	// 0x34 = reserved
	buf[0x40] = 0x80;			// drive number
	buf[0x42] = 0x29;			// extended signature
	fat32_wr32(buf + 0x43, unixts);		// serial number
	memcpy(buf + 0x47, "FAT32MIRROR", 11);	// volume name
	memcpy(buf + 0x52, "FAT32   ", 8);	// FAT name

	*pttype = 0x0c;				// partition type
	fat32_wr32(ptnum, extfs[1]);		// partition sectors

	if (fm->backup_sector != 0) memcpy(buf + fm->backup_sector * sector_size, buf, sector_size);

	ptr = buf + fm->fsinfo_sector * sector_size;
	fat32_write_sig(ptr, sector_size);
	*ptr++ = 'R';
	*ptr++ = 'R';
	*ptr++ = 'a';
	*ptr++ = 'A';
	ptr += 0x1e4 - 4;
	*ptr++ = 'r';
	*ptr++ = 'r';
	*ptr++ = 'A';
	*ptr++ = 'a';
	// keep zero free clusters

	ret = true;
fail:
	return ret;
}


// prepare for source FS - need mounted path, corresponding block device for partition, and size from MBR PT entry
static bool fat32_prep(struct fat32_meta *fm, unsigned extsz)
{
	bool ret = false;
	int fd = -1;
	unsigned sectsz;
	uint64_t devsz;
	struct stat st;

	fd = open(fm->mntpath, O_RDONLY);
	if (fd < 0) { perror("open()"); goto fail; }

	if (fstat(fd, &st) != 0) { perror("fstat()"); goto fail; }

	if (!S_ISDIR(st.st_mode)) { fat32_msg(fm, FAT32_LOG_FATAL, "not a directory: '%s'", fm->mntpath); goto fail; }
	fm->devid = st.st_dev;

	if (ioctl(fd, FIGETBSZ, &fm->fsblksz) != 0) { perror("ioctl(FIGETBSZ"); goto fail; }

	if (fm->fsblksz < FAT32_BS || fm->fsblksz > FAT32_MS || ((fm->fsblksz - 1) & fm->fsblksz) != 0) {	// too small/large or not power of 2
		fat32_msg(fm, FAT32_LOG_FATAL, "unsupported FS block size %u", fm->fsblksz);
		goto fail;
	}

	if (fm->fsblksz != fm->cluster_size) {
		fat32_msg(fm, FAT32_LOG_FATAL, "unexpected FS block size %u mismatch for cluster size %u", fm->fsblksz, fm->cluster_size);
		goto fail;
	}

	fat32_dbg(fm, "got block size %u for device ID %llu", fm->fsblksz, (unsigned long long)fm->devid);

	if (fm->partdev == NULL) { fat32_msg(fm, FAT32_LOG_WARN, "must specify partition device path"); goto fail; }

	if (fd >= 0) close(fd); fd = -1;

	fd = open(fm->partdev, O_RDONLY);
	if (fd < 0) { perror("open()"); goto fail; }

	if (fstat(fd, &st) != 0) { perror("fstat()"); goto fail; }

	if (!S_ISBLK(st.st_mode)) { fat32_msg(fm, FAT32_LOG_FATAL, "mounted partition path is not block device"); goto fail; }

	if (st.st_rdev != fm->devid) { fat32_msg(fm, FAT32_LOG_FATAL, "mismatch for mount '%s' and device '%s'", fm->mntpath, fm->partdev); goto fail; }

	if (ioctl(fd, BLKSSZGET, &sectsz) != 0) { perror("ioctl(BLKSSZGET)"); goto fail; }

	if (sectsz != fm->sector_size) { fat32_msg(fm, FAT32_LOG_FATAL, "sector size mismatch for mounted partition and device node"); goto fail; }

	if (ioctl(fd, BLKGETSIZE64, &devsz) != 0) { perror("ioctl(BLKGETSIZE64)"); goto fail; }

	if (devsz != (unsigned long long)extsz * sectsz) { fat32_msg(fm, FAT32_LOG_FATAL, "mismatch for mounted partition %u and MBR slot size %u", (unsigned)devsz, extsz); goto fail; }

	ret = true;
fail:
	if (fd >= 0) close(fd);
	return ret;
}


// convert file block to cluster
static int fat32_blk(int fd, unsigned blkstart, unsigned blkcount, struct fat32_meta *fm)
{
	int ret = -1, i;

	if (blkcount > ARRAY_SIZE(fm->cluster)) blkcount = ARRAY_SIZE(fm->cluster);

	for (i = 0; i < (int)blkcount; i++) {
		fm->cluster[i] = blkstart++;
		if (ioctl(fd, FIBMAP, &fm->cluster[i]) != 0) { perror("ioctl(FIBMAP)"); goto fail; }
		if (fm->cluster[i] == 0) { fat32_msg(fm, FAT32_LOG_FATAL, "unexpected special or non-contiguous block"); goto fail; }
		fm->cluster[i] = (unsigned long long)fm->cluster[i] * fm->cluster_size / fm->fsblksz + fm->start_cluster + fm->root_dir_clusters;
	}
	ret = blkcount;
fail:
	return ret;
}


#ifdef TRY_EXTENT_MAP
// convert file sub-extent to cluster
static int fat32_ext(int fd, unsigned blkstart, unsigned blkcount, struct fat32_meta *fm)
{
	int ret = -1;
	unsigned i, k, adj;
	struct fat32_em *pext;
	struct fiemap *emap;
	struct fiemap_extent *fext;
	union {
		struct fiemap emap;
		char arr[sizeof *emap + ARRAY_SIZE(fm->ext_arr) * sizeof *emap->fm_extents];
	} umap;

	if (fm->ext_num != 0 && fm->ext_arr[fm->ext_idx].logical != blkstart) fm->ext_num = 0;

	if (fm->ext_num == 0) {
		fm->ext_idx = 0;
		memset(&umap, 0, sizeof umap);
		emap = &umap.emap;
		emap->fm_start = blkstart * (unsigned long long)fm->fsblksz;
		emap->fm_length = blkcount * (unsigned long long)fm->fsblksz;
		emap->fm_flags = FIEMAP_FLAG_SYNC;
		emap->fm_mapped_extents = 0;
		emap->fm_extent_count = ARRAY_SIZE(fm->ext_arr);

#ifdef FS_IOC_FIEMAP
		if (ioctl(fd, FS_IOC_FIEMAP, emap) != 0) {
			perror("ioctl(FS_IOC_FIEMAP)");
#else
			fat32_msg(fm, FAT32_LOG_INFO, "build could not include extent mapping");
#endif
			if (fm->extmap) goto fail;
			fm->extmap = true;
			ret = 0;
			goto fail;
		}

		fm->extmap = true;

		fat32_dbg(fm, "batched %u from %u of %u extents at %u", (unsigned)emap->fm_mapped_extents, (unsigned)emap->fm_extent_count, blkcount, blkstart);

		for (i = 0, fext = emap->fm_extents; i < emap->fm_mapped_extents; i++, fext++) {
			if ((fext->fe_logical % fm->fsblksz) != 0 || (fext->fe_physical % fm->fsblksz) != 0 || (fext->fe_length % fm->fsblksz) != 0
			|| (fext->fe_flags & ~(FIEMAP_EXTENT_LAST | FIEMAP_EXTENT_MERGED)) != 0) { fat32_msg(fm, FAT32_LOG_FATAL, "unexpected special or non-contiguous block"); goto fail; }

			k = fm->ext_num++;
			fm->ext_arr[k].len = fext->fe_length / fm->cluster_size;
			fm->ext_arr[k].logical = fext->fe_logical / fm->cluster_size;
			fm->ext_arr[k].physical = fext->fe_physical / fm->cluster_size;

			if ((fext->fe_flags & FIEMAP_EXTENT_LAST) != 0) break;
		}
	}

	for (k = 0, i = fm->ext_idx, pext = fm->ext_arr + i; i < fm->ext_idx + fm->ext_num; i++, pext++) {
		while (k < ARRAY_SIZE(fm->cluster)) {
			if (pext->logical + pext->len < blkstart) { fat32_msg(fm, FAT32_LOG_FATAL, "unexpected end of extent"); goto fail; }
			adj = blkstart - pext->logical;
			pext->physical += adj;
			pext->len -= adj;
			if (pext->len == 0) break;
			pext->logical = blkstart++;
			fm->cluster[k++] = pext->physical + fm->start_cluster + fm->root_dir_clusters;
		}
		if (k >= ARRAY_SIZE(fm->cluster)) break;
	}
	fm->ext_num -= i - fm->ext_idx;
	fm->ext_idx = i;

	if (k == 0) goto fail;

	ret = k;
fail:
	return ret;
}
#endif


static int fat32_clusters(int fd, unsigned blkstart, unsigned blkcount, struct fat32_meta *fm)
{
	int ret = -1;

	if (blkcount <= 0) { fat32_msg(fm, FAT32_LOG_FATAL, "refusing zero block map count"); goto fail; }
#ifdef TRY_EXTENT_MAP
	if (!fm->blkmap) {
		ret = fat32_ext(fd, blkstart, blkcount, fm);
		if (ret != 0) goto fail;	// clusters > 0, or failed < 0
		fm->blkmap = true;		// fallback to block mapping and try below
		fat32_msg(fm, FAT32_LOG_INFO, "extent mapping attempt failed, fallback to block mapping");
	}
	if (!fm->blkmap) goto fail;
#endif
	ret = fat32_blk(fd, blkstart, blkcount, fm);
fail:
	return ret;
}


// memory map FAT32 structures
static bool fat32_map(struct fat32_meta *fm)
{
	bool ret = false;
	int fd = fm->direct ? fm->fd : -1, flags = fm->direct ? MAP_SHARED : (MAP_ANONYMOUS | MAP_PRIVATE);
	uint64_t len = (fm->rsvd_sectors + fm->fat_sectors + fm->root_dir_clusters * fm->cluster_sectors) * (unsigned long long)fm->sector_size;

	if (len >= INT32_MAX) { fat32_msg(fm, FAT32_LOG_FATAL, "unable to map oversized metadata"); goto fail; }

	fm->ptr = mmap(NULL, len, PROT_READ | PROT_WRITE, flags, fd, 0);
	if (fm->ptr == MAP_FAILED) { perror("mmap()"); goto fail; }
	fm->len = len;

	ret = true;
fail:
	return ret;
}


// clean up
static bool fat32_unmap(struct fat32_meta *fm)
{
	bool ret = true;

	if (fm->ptr != MAP_FAILED) {
		if (fm->direct && msync(fm->ptr, fm->len, MS_SYNC) != 0) { perror("msync()"); ret = false; }
		if (munmap(fm->ptr, fm->len) != 0) { perror("munmap()"); ret = false; }
		fm->ptr = MAP_FAILED;
	}

	return ret;
}

static bool fat32_free(struct fat32_meta *fm)
{
	bool ret = fat32_unmap(fm);

	if (fm->fd >= 0) {
		if (close(fm->fd) != 0) { perror("close()"); ret = false; }
		fm->fd = -1;
	}

	return ret;
}


static void fat32_dirent_short_part(struct fat32_dirent *dirent, unsigned idx, unsigned len, const char *fpath, unsigned fpathlen, unsigned pos)
{
	char ch;

	while (idx < len && pos > 0 && pos < fpathlen) {
		ch = fpath[pos++];
		if (ch == '/' || ch == '.') break;
		if (fat32_badch_dirent(ch)) continue;
		if (ch == (char)FAT32_DIRENT_DEL) ch = FAT32_DIRENT_SUB;
		if (ch >= 'a' && ch <= 'z') ch -= 'a' - 'A';
		dirent->name[idx++] = ch;
	}
}


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


#if 0

// lfn must be NUL terminated, and basis buffer must hold 11 bytes and will not be terminated
static unsigned fat32_lfn_basis(uint8_t *basis, const uint8_t *lfn)
{
	unsigned ret = 0, pos = 0;
	uint8_t ch;
	const uint8_t *str = lfn;

	if (*str == '\0') goto fail;

	while ((ch = *str++) != '\0') {
		if (fat32_badch_lfn(ch)) goto fail;
		if (pos == 0 && (ch == ' ' || ch == '.')) continue;	// skip leading spaces and periods
		if (ch == (char)FAT32_DIRENT_DEL) ch = FAT32_DIRENT_SUB;
		if (ch >= 'a' || ch <= 'z') ch -= 'a' - 'A';
		basis[pos] = ch;
		if (++pos >= 11) goto fail;
	}

fail:
	return ret;
}

#endif


// assumes # elements = 20 = ceil(255 / 13)
static int fat32_lfn(struct fat32_dirent *dirent, const char *fpath, unsigned len)
{
	int ret = -1;
	static const unsigned m[] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };	// char pos per dirent of LFN
	unsigned p, n = ARRAY_SIZE(m), i = 0, k = 0, num = 0;
	char tmp[sizeof *dirent], name[255], ch;

	if (len > ARRAY_SIZE(name)) goto fail;

	while (i < len && k < ARRAY_SIZE(name)) {
		ch = fpath[i++];
		if (fat32_badch_lfn(ch)) continue;
		name[k++] = ch;
	}
	if (k == 0) goto fail;

	p = k - 1;
	p -= p % n;

	while (k > 0 && num < (ARRAY_SIZE(name) + n - 1) / n) {
		memset(tmp, 0, sizeof tmp);
		i = 0;
		while (i < n && p < k) {
			tmp[m[i]] = name[p++];
			tmp[m[i++] + 1] = 0;
		}
		if (num == 0 && i < n) {
			tmp[m[i]] = 0x00;
			tmp[m[i++] + 1] = 0x00;
			while (i < n) {
				tmp[m[i]] = 0xff;
				tmp[m[i++] + 1] = 0xff;
			}
		}
		memcpy(&dirent[num++], tmp, sizeof *dirent);
		k--;
		k -= k % n;
		p = k - n;
	}

	ret = num;
fail:
	return ret;
}


// expects # elements = 21 = LFN + short
static int fat32_name(struct fat32_dirent *dirarr, unsigned first_cluster, unsigned filesize, const char *fpath)
{
	int ret = -1, slash = -1, dot = -1, amt;
	uint8_t cksum;
	unsigned i, k, num = 0, fpathlen = strlen(fpath);
	struct fat32_dirent dirent = { 0 };
	struct fat32_lfnent lfnent;

	if (fpathlen == 0) goto fail;

	dirent.name[0] = '_';
	memset(dirent.name + 1, ' ', sizeof dirent.name - 1);
	dirent.attr = FAT32_ATTR_RO;
	fat32_wr16(&dirent.clusthi, first_cluster >> 16);
	fat32_wr16(&dirent.clustlo, first_cluster);
	fat32_wr32(&dirent.filesz, filesize);

	for (k = 0; k < fpathlen && (slash < 0 || dot < 0); k++) {
		i = fpathlen - 1 - k;
		switch (fpath[i]) {
		case '/':
			if (slash < 0) slash = i;
			dot = -1;
			break;
		case '.':
			if (dot < 0) dot = i;
			break;
		default:
			break;
		}
	}

	amt = fat32_lfn(dirarr, fpath + slash + 1, fpathlen - slash - 1);
	if (amt <= 0) goto fail;
	num = amt;

	fat32_dirent_short_part(&dirent, 0, 8, fpath, fpathlen, slash + 1);
	fat32_dirent_short_part(&dirent, 8, 11, fpath, fpathlen, dot + 1);

	cksum = fat32_cksum_short(&dirent);

	for (i = 0; i < num; i++) {
		memcpy(&lfnent, &dirarr[i], sizeof lfnent);
		lfnent.ord = (num - i) | ((i == 0) ? FAT32_LFN_ORD_LAST : 0);
		lfnent.attr = FAT32_ATTR_LFN;
		lfnent.cksum = cksum;
		memcpy(&dirarr[i], &lfnent, sizeof *dirarr);
	}
	memcpy(&dirarr[num++], &dirent, sizeof *dirarr);

	ret = num;
fail:
	return ret;
}


// pass path to link file blocks into FAT32
static bool fat32_file(struct fat32_meta *fm, const char *fpath)
{
	bool ret = false;
	int retblks, num, i, fd = -1;
	unsigned k, blkcount, blk = 0, cluster = 0, first_cluster = 0;
	uint8_t *pfat = (uint8_t *)fm->ptr + fm->rsvd_sectors * (unsigned long long)fm->sector_size, *pdir = pfat + fm->fat_sectors * (unsigned long long)fm->sector_size;
	struct fat32_dirent dirent[21];
	struct stat st;

	fd = open(fpath, O_RDONLY);
	if (fd < 0) { perror("open()"); goto fail; }

	if (fstat(fd, &st) != 0) { perror("fstat()"); goto fail; }

	if (st.st_dev != fm->devid) { fat32_msg(fm, FAT32_LOG_FATAL, "wrong mount point for file '%s'", fpath); goto fail; }

	if (S_ISDIR(st.st_mode)) { fat32_msg(fm, FAT32_LOG_WARN, "skipping directory '%s'", fpath); goto success; }

	if (!S_ISREG(st.st_mode)) { fat32_msg(fm, FAT32_LOG_FATAL, "not a regular file: '%s'", fpath); goto fail; }

	if (st.st_size > UINT32_MAX) { fat32_msg(fm, FAT32_LOG_FATAL, "oversized file '%s'", fpath); goto fail; }

	if (fm->dbg && st.st_size == 0) fat32_msg(fm, FAT32_LOG_INFO, "empty file '%s'", fpath);

#ifdef TRY_EXTENT_MAP
	fm->ext_num = 0;
#endif
	for (blk = 0, blkcount = (st.st_size + fm->cluster_size - 1) / (fm->cluster_size); blkcount > 0; blk += retblks, blkcount -= retblks) {
		retblks = fat32_clusters(fd, blk, blkcount, fm);
		if (retblks <= 0 || retblks > (int)blkcount) goto fail;

		for (i = 0; i < retblks; i++) {
			if (fat32_rd32(pfat + cluster * sizeof(uint32_t)) != FAT32_CLUSTER_FREE) { fat32_msg(fm, FAT32_LOG_FATAL, "overlapping cluster, perhaps duplicate file path or copy on write?"); goto fail; }
			if (cluster != 0) fat32_wr32(pfat + cluster * sizeof(uint32_t), fm->cluster[i]);
			cluster = fm->cluster[i];
			if (first_cluster == 0) first_cluster = cluster;
			fat32_dbg(fm, "got file block %u as cluster %u", blk + i, cluster);
		}
	}

	if (cluster != 0) fat32_wr32(pfat + cluster * sizeof(uint32_t), FAT32_CLUSTER_END);

	num = fat32_name(dirent, first_cluster, st.st_size, fpath);
	if (num < 0) {
		fat32_msg(fm, FAT32_LOG_FATAL, "bad filename '%s'\n", fpath);
		goto fail;
	}

	for (i = 0, k = 0; k < fm->root_dir_clusters * fm->sector_size; k += sizeof *dirent, pdir += sizeof *dirent) {
		if (*pdir == FAT32_DIRENT_FREE) {
			memcpy(pdir, &dirent[i++], sizeof *dirent);
			if (i == num) break;
		}
	}
	if (i != num) {
		fat32_msg(fm, FAT32_LOG_FATAL, "insufficient space for directory entry\n");
		goto fail;
	}

success:
	ret = true;
fail:
	if (fd >= 0) close(fd);
	return ret;
}


// open and validate base block device - will handle partition sub-nodes later
static unsigned fat32_open(struct fat32_meta *fm)
{
	unsigned ret = 0, sectsz = 0;
	uint64_t devsz;
	struct stat st;

	fm->fd = open(fm->devpath, O_RDWR);
	if (fm->fd < 0) { perror("open()"); goto fail; }

	if (fstat(fm->fd, &st) != 0) { perror("fstat()"); goto fail; }

	if (!S_ISBLK(st.st_mode)) {
		if (S_ISREG(st.st_mode)) { fat32_msg(fm, FAT32_LOG_FATAL, "not a block device node"); goto fail; }
		fat32_msg(fm, FAT32_LOG_INFO, "Operating on regular file as block device");
		sectsz = FAT32_BS;	// FIXME: allow override e.g. 4096
		if (st.st_size % sectsz != 0) { fat32_msg(fm, FAT32_LOG_FATAL, "file size is not multiple of sector size"); goto fail; }
		devsz = st.st_size;
	}

	if (sectsz == 0) {
		if (ioctl(fm->fd, BLKSSZGET, &sectsz) != 0) { perror("ioctl(BLKSSZGET)"); goto fail; }
		if (sectsz < FAT32_BS || sectsz > FAT32_MS || ((sectsz - 1) & sectsz) != 0) {	// too small/large or not power of 2
			fat32_msg(fm, FAT32_LOG_FATAL, "unsupported sector size %u", sectsz);
			goto fail;
		}

		if (ioctl(fm->fd, BLKGETSIZE64, &devsz) != 0) { perror("ioctl(BLKGETSIZE64)"); goto fail; }
		if (devsz == 0) { fat32_msg(fm, FAT32_LOG_FATAL, "device has zero capacity"); goto fail; }
	}
	fm->sector_size = sectsz;

	if (devsz % sectsz != 0) { fat32_msg(fm, FAT32_LOG_FATAL, "bad device size"); goto fail; }

	if (devsz >= (uint64_t)UINT32_MAX * sectsz) { fat32_msg(fm, FAT32_LOG_FATAL, "rejecting oversized device"); goto fail; }

	ret = devsz / sectsz;
fail:
	return ret;
}


// main loop
static bool fat32_prepend(struct fat32_meta *fm)
{
	bool ret = false;
	int fatpte;
	unsigned long long i;
	unsigned k, cluster_sectors = 8, extfs[2] = { 0 };
	uint8_t *pfat, buf[FAT32_MS * FAT32_MIN_RSVD_SECTORS];
	ssize_t rlen;
	size_t len = 0;
	char *line = NULL;

	extfs[1] = fat32_open(fm);			// base device node, parent of partition - TODO: default O_EXCL if block device?
	if (extfs[1] == 0) goto fail;

	fatpte = fat32_mbr(fm, buf, extfs);		// read MBR into buf and validate
	if (fatpte < 0) goto fail;

	if ((extfs[0] % cluster_sectors) != 0) cluster_sectors = 1;
	fm->cluster_sectors = cluster_sectors;
	fm->cluster_size = cluster_sectors * fm->sector_size;

	if (!fat32_sb(fm, buf, extfs, fatpte)) goto fail;	// BS

	if (!fat32_prep(fm, extfs[1])) goto fail;	// checks against partition device node

	if (!fat32_map(fm)) goto fail;			// memory map FAT32 metadata regions

	pfat = (uint8_t *)fm->ptr + fm->rsvd_sectors * (unsigned long long)fm->sector_size;
	memset(pfat, 0, (fm->fat_sectors + fm->root_dir_clusters * fm->cluster_sectors) * (unsigned long long)fm->sector_size);

	for (i = 0; i < fm->root_dir_clusters - 1; i++) {
		k = fm->start_cluster + i;
		fat32_wr32(pfat + k * sizeof(uint32_t), k + 1);
	}
	fat32_wr32(pfat + (fm->start_cluster + i) * sizeof(uint32_t), FAT32_CLUSTER_END);

	while ((rlen = getline(&line, &len, stdin)) >= 0) {
		if (rlen > 0 && line[rlen - 1] == '\n') line[--rlen] = '\0';
		if (rlen > 0 && line[rlen - 1] == '\r') line[--rlen] = '\0';
		if (rlen == 0) continue;
		if (!fat32_file(fm, line)) goto fail;	// add file from mounted path
	}

	for (i = 0; i < fm->fat_sectors * (unsigned long long)fm->sector_size; i += sizeof(uint32_t), pfat += sizeof(uint32_t)) {
		if (fat32_rd32(pfat) == FAT32_CLUSTER_FREE) fat32_wr32(pfat, FAT32_CLUSTER_BAD);
	}

	memcpy(fm->ptr, buf, FAT32_MIN_RSVD_SECTORS * fm->sector_size);

	if (!fm->direct && !writeall(fm->fd, fm->ptr, (fm->rsvd_sectors + fm->fat_sectors + fm->root_dir_clusters * fm->cluster_sectors) * (unsigned long long)fm->sector_size)) goto fail;

	ret = true;
fail:
	if (line != NULL) free(line);
	(void)fat32_unmap(fm);
	return ret;
}


#define CMD_CH_STR(...)		(char []) { __VA_ARGS__ '\0' }
#define CMD_CH_STR_(...)	(char []) { __VA_ARGS__, '\0' }
#define CMD_ARG_REQ(c)		(c), ':'

#define CMD_IDX(c)		[(unsigned char)(c)]
#define CMD_ARG(a, c)		((a)CMD_IDX(c))
#define CMD_ARG_PRESENT(a, c)	(CMD_ARG((a), (c)) != NULL)

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


struct fat32_opt {
	unsigned	nerr;
	int		argc;
	char *const	*argv;
	const char	*stropt;
	bool 		(*cb_fn)(struct fat32_opt *);
	void		*cb_opq;
	const char	*argopt[CMD_NCH];
	const char	*msgopt[CMD_NCH];
	const int	reqarg[CMD_NCH];
};


// extract command line parameters - enforce mandatory settings and required parameters for optional, reject duplicates, other validation
static bool fat32_getopt(struct fat32_opt *prm)
{
	bool ret = false;
	int opt, nerr = 0, argc = prm->argc;
	char *const *argv = prm->argv;
	const int *reqarg = prm->reqarg;
	const char **const argopt = prm->argopt;
	const char *const *msgopt = prm->msgopt;

	opterr = 0;	// inhibit getopt() output via global
	while ((opt = getopt(argc, argv, prm->stropt)) >= 0) {
		if (opt >= CMD_NCH) goto fail;

		switch (reqarg[opt]) {
		case CMD_REQ_OPT_NOARG:
		case CMD_REQ_NOOPT_NOARG:
			optarg = "";	// force non-NULL for below and to flag presence
			// fall through
		case CMD_REQ_OPT_ARG:
		case CMD_REQ_NOOPT_ARG:
			if (CMD_ARG(argopt, opt) != NULL) {
				fat32_log(FAT32_LOG_FATAL, "unexpected option '-%c' parameter '%s', already have '%s' (%s)", opt, optarg, argopt[opt], msgopt[opt]);
				nerr++;
				break;
			}
			CMD_ARG(argopt, opt) = optarg;
			break;
		case CMD_REQ_UNDEF:
		default:
			goto missing;
		}
	}

	if (optind < argc) { fat32_log(FAT32_LOG_FATAL, "unexpected non-option parameter '%s'", argv[optind]); nerr++; }

	if (optind <= 1) goto usage;

	for (opt = 0; opt < CMD_NCH; opt++) {
		if (CMD_ARG(argopt, opt) != NULL || !CMD_REQ_OPT(reqarg[opt])) continue;
		fat32_log(FAT32_LOG_FATAL, "missing required option '-%c' (%s)", opt, msgopt[opt]);
		nerr++;
	}

	prm->nerr = nerr;
	if (prm->cb_fn != NULL && !(*prm->cb_fn)(prm)) nerr++;

	if (nerr != 0) goto usage;

	ret = true;
fail:
	return ret;

missing:
	opt = optopt;
	if (opt >= 0 && opt < CMD_NCH) {
		if (CMD_REQ_ARG(reqarg[opt])) { fat32_log(FAT32_LOG_FATAL, "missing required parameter for option '-%c' (%s)", opt, msgopt[opt]); goto usage; }
		fat32_log(FAT32_LOG_FATAL, "unknown option '-%c'", opt);
	}
usage:
	fat32_log(FAT32_LOG_NOTICE, "usage - %s -d /dev/mmcblkX -m /mnt/ext4", argv[0]);
	for (opt = 0; opt < CMD_NCH; opt++)
		if (reqarg[opt] != 0) fat32_log(FAT32_LOG_NOTICE, "\t%s\t-%c\t%s\t\t%s", CMD_REQ_OPT(reqarg[opt]) ? "REQUIRED:" : "optional:", opt, CMD_REQ_ARG(reqarg[opt]) ? "<arg>" : "", msgopt[opt]);
	goto fail;
}


/* TODO: consider following sketch for future options
 * '1'-'4' - select EXT4 partition by MBR PT index
 * 'a' - don't auto-partition
 * 'f' - overwrite any existing FAT32 FS
 * 'n' - dry run, discard all changes - conflicts with -w
 * 'r' - remove EXT4 partition entry - required for macOS
 * 's' - don't partition if missing MBR signature (0x55 0xaa)
 * 't' - manually specify offset into device of mounted FS with optional length and ignore partition table
 */
#define CMDOPT_GNU		'+'	// stop at first non-option

#define CMDOPT_BLKMAP		'b'	// force block mapping instead of trying extent mapping
#define CMDOPT_DEVPATH		'd'	// string
#define CMDOPT_DBGMSG		'g'
#define CMDOPT_MNTPATH		'm'	// string
#define CMDOPT_PARTDEV		'p'	// string
#define CMDOPT_QUIET		'q'
#define CMDOPT_WRLIVE		'w'


static bool fat32_parse(struct fat32_opt *prm)
{
	bool ret = false;
	const char **const argopt = prm->argopt;
	struct fat32_meta *fm = prm->cb_opq;

#ifdef TRY_EXTENT_MAP
	fm->blkmap = CMD_ARG_PRESENT(argopt, CMDOPT_BLKMAP);
	if (fm->blkmap) fat32_msg(fm, FAT32_LOG_INFO, "forcing block mapping, will not try extent mapping");
#endif
	fm->devpath = CMD_ARG(argopt, CMDOPT_DEVPATH);
	fm->dbg = CMD_ARG_PRESENT(argopt, CMDOPT_DBGMSG);
	fm->mntpath = CMD_ARG(argopt, CMDOPT_MNTPATH);
	fm->partdev = CMD_ARG(argopt, CMDOPT_PARTDEV);
	fm->quiet = CMD_ARG_PRESENT(argopt, CMDOPT_QUIET);
	fm->direct = CMD_ARG_PRESENT(argopt, CMDOPT_WRLIVE);

	if (fm->dbg && fm->quiet) { fat32_msg(fm, FAT32_LOG_FATAL, "options conflict: -g (debug) and -q (quiet)"); goto fail; }

	ret = true;
fail:
	return ret;
}


// user options
static bool fat32_cmdline(struct fat32_meta *fm, int argc, char *const argv[])
{
	bool ret = false;
	struct fat32_opt prm = {
		.nerr = 0,
		.argc = argc,
		.argv = argv,
		.stropt = CMD_CH_STR( CMDOPT_GNU,
			CMDOPT_BLKMAP,
			CMD_ARG_REQ(CMDOPT_DEVPATH),
			CMDOPT_DBGMSG,
			CMD_ARG_REQ(CMDOPT_MNTPATH),
			CMD_ARG_REQ(CMDOPT_PARTDEV),
			CMDOPT_QUIET,
			CMDOPT_WRLIVE,
		),
		.cb_fn = &fat32_parse,
		.cb_opq = fm,
		.argopt = { NULL },
		.msgopt = {
			CMD_IDX(CMDOPT_BLKMAP)	= "force block map",
			CMD_IDX(CMDOPT_DEVPATH)	= "device path",
			CMD_IDX(CMDOPT_DBGMSG)	= "debug messages",
			CMD_IDX(CMDOPT_MNTPATH)	= "mount path",
			CMD_IDX(CMDOPT_PARTDEV) = "mounted partition",
			CMD_IDX(CMDOPT_QUIET)	= "quiet",
			CMD_IDX(CMDOPT_WRLIVE)	= "write metadata unsafely",
		},
		.reqarg = {
			CMD_IDX(CMDOPT_BLKMAP)	= CMD_REQ_NOOPT_NOARG,
			CMD_IDX(CMDOPT_DEVPATH)	= CMD_REQ_OPT_ARG,
			CMD_IDX(CMDOPT_DBGMSG)	= CMD_REQ_NOOPT_NOARG,
			CMD_IDX(CMDOPT_MNTPATH)	= CMD_REQ_OPT_ARG,
			CMD_IDX(CMDOPT_PARTDEV)	= CMD_REQ_OPT_ARG,	// was: CMD_REQ_NOOPT_ARG,
			CMD_IDX(CMDOPT_QUIET)	= CMD_REQ_NOOPT_NOARG,
			CMD_IDX(CMDOPT_WRLIVE)	= CMD_REQ_NOOPT_NOARG,
		},
	};

	if (!fat32_getopt(&prm)) goto fail;

	ret = true;
fail:
	return ret;
}


// minimal
int main(int argc, char *argv[])
{
	int ret = EXIT_FAILURE;
	struct fat32_meta fm = { .fd = -1, .ptr = MAP_FAILED, };

	if (!fat32_cmdline(&fm, argc, argv)) goto fail;

	if (!fat32_prepend(&fm)) goto fail;

	ret = EXIT_SUCCESS;
fail:
	if (!fat32_free(&fm)) ret = EXIT_FAILURE;
	return ret;
}
