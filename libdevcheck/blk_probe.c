#define _FILE_OFFSET_BITS 64

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/fs.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "ata.h"
#include "procedure.h"
#include "scsi.h"
#include "utils.h"

static int open_read_direct(const char *path) {
    int fd = open(path, O_RDONLY | O_DIRECT | O_LARGEFILE | O_NOATIME);
    if (fd == -1 && errno == EPERM)
        fd = open(path, O_RDONLY | O_DIRECT | O_LARGEFILE);
    return fd;
}

int dc_dev_logical_sector_size(const char *dev_path, unsigned int *out) {
    int fd;
    int ret;

    if (!dev_path || !out) {
        errno = EINVAL;
        return -1;
    }
    fd = open(dev_path, O_RDONLY | O_LARGEFILE);
    if (fd == -1)
        return -1;
    ret = ioctl(fd, BLKSSZGET, out);
    close(fd);
    if (ret == -1)
        return -1;
    if (*out == 0) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int dc_dev_physical_sector_size(const char *dev_path, unsigned int *out) {
    int fd;
    int ret;

    if (!dev_path || !out) {
        errno = EINVAL;
        return -1;
    }
    fd = open(dev_path, O_RDONLY | O_LARGEFILE);
    if (fd == -1)
        return -1;
    ret = ioctl(fd, BLKPBSZGET, out);
    close(fd);
    if (ret == -1)
        return -1;
    if (*out == 0) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int dc_dev_capacity_bytes(const char *dev_path, uint64_t *out) {
    int fd;
    int ret;

    if (!dev_path || !out) {
        errno = EINVAL;
        return -1;
    }
    fd = open(dev_path, O_RDONLY | O_LARGEFILE);
    if (fd == -1)
        return -1;
    ret = ioctl(fd, BLKGETSIZE64, out);
    close(fd);
    return ret == -1 ? -1 : 0;
}

int dc_blk_read(DC_Dev *dev, enum Api api, uint64_t start_lba, uint64_t sectors,
        DC_BlockReport *report, int *posix_errno, void **out_buf) {
    unsigned int sector_size;
    uint64_t capacity_bytes;
    uint64_t sector_count;
    uint64_t byte_count;
    uint64_t byte_offset;
    void *buf = NULL;
    int fd = -1;
    int open_flags;
    struct timespec time_pre;
    struct timespec time_post;
    AtaCommand ata_command;
    ScsiCommand scsi_command;

    if (!report || !dev || !dev->dev_path || sectors == 0) {
        errno = EINVAL;
        return -1;
    }
    memset(report, 0, sizeof(*report));
    report->lba = start_lba;
    report->sectors_processed = sectors;
    if (posix_errno)
        *posix_errno = 0;
    if (out_buf)
        *out_buf = NULL;

    if (dc_dev_logical_sector_size(dev->dev_path, &sector_size))
        return -1;
    if (dc_dev_capacity_bytes(dev->dev_path, &capacity_bytes))
        return -1;
    if (sector_size == 0 || capacity_bytes % sector_size != 0) {
        errno = EINVAL;
        return -1;
    }
    sector_count = capacity_bytes / sector_size;
    if (start_lba >= sector_count || sectors > sector_count - start_lba) {
        errno = EINVAL;
        return -1;
    }
    if (sectors > SIZE_MAX / sector_size
            || start_lba > UINT64_MAX / sector_size) {
        errno = EOVERFLOW;
        return -1;
    }
    byte_count = sectors * (uint64_t)sector_size;
    byte_offset = start_lba * (uint64_t)sector_size;
    if (byte_count > SIZE_MAX || byte_offset > (uint64_t)LLONG_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    if (api == Api_eAta && !dev->ata_capable) {
        errno = EOPNOTSUPP;
        return -1;
    }
    if (api == Api_eAta && (sectors > 0xffff || byte_count > UINT_MAX)) {
        errno = EINVAL;
        return -1;
    }

    {
        long page_size = sysconf(_SC_PAGESIZE);
        size_t alignment;
        if (page_size <= 0)
            page_size = 4096;
        alignment = (size_t)page_size;
        if (alignment < sector_size)
            alignment = sector_size;
        if (posix_memalign(&buf, alignment, (size_t)byte_count))
            return -1;
    }

    if (api == Api_eAta) {
        open_flags = O_RDWR | O_LARGEFILE;
        fd = open(dev->dev_path, open_flags);
    } else {
        fd = open_read_direct(dev->dev_path);
    }
    if (fd == -1)
        goto fail;

    if (clock_gettime(DC_BEST_CLOCK, &time_pre))
        goto fail_close;

    if (api == Api_eAta) {
        int ioctl_ret;
        memset(&ata_command, 0, sizeof(ata_command));
        memset(&scsi_command, 0, sizeof(scsi_command));
        prepare_ata_command(&ata_command, 0x25 /* READ DMA EXT */,
                start_lba, (int)sectors);
        prepare_scsi_command_from_ata(&scsi_command, &ata_command);
        scsi_command.io_hdr.dxfer_direction = SG_DXFER_FROM_DEV;
        scsi_command.io_hdr.dxferp = buf;
        scsi_command.io_hdr.dxfer_len = (unsigned int)byte_count;
        scsi_command.scsi_cmd[1] = (6 << 1) + 1;  /* DMA protocol + EXTEND */
        scsi_command.scsi_cmd[2] = 0x0e;  /* data-in, block transfer */
        ioctl_ret = ioctl(fd, SG_IO, &scsi_command);
        if (clock_gettime(DC_BEST_CLOCK, &time_post))
            goto fail_close;
        report->blk_access_time = (time_post.tv_sec - time_pre.tv_sec) * 1000000
            + (time_post.tv_nsec - time_pre.tv_nsec) / 1000;
        if (ioctl_ret)
            report->blk_status = DC_BlockStatus_eError;
        else
            report->blk_status = scsi_ata_check_return_status(&scsi_command);
    } else {
        ssize_t read_ret;
        int read_errno;
        errno = 0;
        read_ret = pread(fd, buf, (size_t)byte_count, (off_t)byte_offset);
        read_errno = errno;
        if (clock_gettime(DC_BEST_CLOCK, &time_post))
            goto fail_close;
        report->blk_access_time = (time_post.tv_sec - time_pre.tv_sec) * 1000000
            + (time_post.tv_nsec - time_pre.tv_nsec) / 1000;
        if (read_ret == (ssize_t)byte_count) {
            report->blk_status = DC_BlockStatus_eOk;
        } else {
            report->blk_status = DC_BlockStatus_eError;
            if (posix_errno)
                *posix_errno = read_errno;
        }
    }

    close(fd);
    if (out_buf)
        *out_buf = buf;
    else
        free(buf);
    return 0;

fail_close:
    close(fd);
fail:
    free(buf);
    return -1;
}
