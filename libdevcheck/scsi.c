#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "scsi.h"

static uint32_t be32_value(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
        | ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t be64_value(const uint8_t *p) {
    uint64_t value = 0;
    int i;
    for (i = 0; i < 8; i++)
        value = (value << 8) | p[i];
    return value;
}

int dc_scsi_read_capacity16(const char *dev_path, DC_ScsiCapacity16 *out) {
    int fd;
    int ret;
    ScsiCommand command;
    uint8_t response[32];

    if (!dev_path || !out) {
        errno = EINVAL;
        return -1;
    }
    memset(&command, 0, sizeof(command));
    memset(response, 0, sizeof(response));
    fd = open(dev_path, O_RDONLY | O_LARGEFILE);
    if (fd == -1)
        return -1;

    command.io_hdr.interface_id = 'S';
    command.io_hdr.dxfer_direction = SG_DXFER_FROM_DEV;
    command.io_hdr.cmd_len = 16;
    command.io_hdr.mx_sb_len = sizeof(command.sense_buf);
    command.io_hdr.dxfer_len = sizeof(response);
    command.io_hdr.dxferp = response;
    command.io_hdr.cmdp = command.scsi_cmd;
    command.io_hdr.sbp = command.sense_buf;
    command.io_hdr.timeout = 3000;
    /* The response buffer is small and stack-backed; do not request
     * SG_FLAG_DIRECT_IO, which may require stricter alignment. */
    command.io_hdr.flags = 0;
    command.scsi_cmd[0] = 0x9e;  /* SERVICE ACTION IN(16) */
    command.scsi_cmd[1] = 0x10;  /* READ CAPACITY(16) */
    command.scsi_cmd[10] = 0;
    command.scsi_cmd[11] = 0;
    command.scsi_cmd[12] = 0;
    command.scsi_cmd[13] = sizeof(response);

    ret = ioctl(fd, SG_IO, &command);
    if (ret == -1) {
        ret = errno;
        close(fd);
        errno = ret;
        return -1;
    }
    if (command.io_hdr.status != 0 || command.io_hdr.host_status != 0
            || command.io_hdr.driver_status != 0
            || command.io_hdr.resid < 0
            || command.io_hdr.resid > (int)sizeof(response)
            || command.io_hdr.dxfer_len - command.io_hdr.resid < sizeof(response)) {
        close(fd);
        errno = EIO;
        return -1;
    }
    close(fd);

    out->last_lba = be64_value(response);
    out->block_length = be32_value(response + 8);
    /* SBC READ CAPACITY(16): byte 12 bit 0 is PROT_EN, bits 1..3 P_TYPE;
     * byte 13 bits 4..7 carry P_I_EXPONENT. */
    out->prot_en = response[12] & 0x01;
    out->p_type = (response[12] >> 1) & 0x07;
    out->p_i_exponent = (response[13] >> 4) & 0x0f;
    return 0;
}

void prepare_scsi_command_from_ata(ScsiCommand *scsi_cmd, AtaCommand *ata_cmd) {
    memset(scsi_cmd, 0, sizeof(ScsiCommand));
    scsi_cmd->io_hdr.interface_id = 'S';
    scsi_cmd->io_hdr.dxfer_direction = SG_DXFER_NONE;
    scsi_cmd->io_hdr.cmd_len = 16;
    scsi_cmd->io_hdr.mx_sb_len = sizeof(scsi_cmd->sense_buf);
    scsi_cmd->io_hdr.iovec_count = 0;
    scsi_cmd->io_hdr.dxfer_len = 0;
    scsi_cmd->io_hdr.dxferp = NULL;
    scsi_cmd->io_hdr.cmdp = scsi_cmd->scsi_cmd;  // Pointer to command
    scsi_cmd->io_hdr.sbp = scsi_cmd->sense_buf;
    scsi_cmd->io_hdr.timeout = 1000;  // In millisec; MAX_UINT is no timeout
    scsi_cmd->io_hdr.flags = SG_FLAG_DIRECT_IO;
    scsi_cmd->io_hdr.pack_id = 0;  // Unused internally
    scsi_cmd->io_hdr.usr_ptr = 0;  // Unused internally

    scsi_cmd->scsi_cmd[0]  = 0x85;  // ATA PASS-THROUGH 16 bytes
    scsi_cmd->scsi_cmd[1]  = (3 << 1);  // Non-data protocol
    scsi_cmd->scsi_cmd[1] |= 1;  // EXTEND flag
    scsi_cmd->scsi_cmd[2]  = 0x20;  // Check condition, no off-line, no data xfer
    scsi_cmd->scsi_cmd[3]  = ata_cmd->task.hob_ports[1];  // features
    scsi_cmd->scsi_cmd[4]  = ata_cmd->task.io_ports[1];
    scsi_cmd->scsi_cmd[5]  = ata_cmd->task.hob_ports[2];  // sectors count
    scsi_cmd->scsi_cmd[6]  = ata_cmd->task.io_ports[2];
    scsi_cmd->scsi_cmd[7]  = ata_cmd->task.hob_ports[3];  // sector number
    scsi_cmd->scsi_cmd[8]  = ata_cmd->task.io_ports[3];
    scsi_cmd->scsi_cmd[9]  = ata_cmd->task.hob_ports[4];  // low cylinder
    scsi_cmd->scsi_cmd[10] = ata_cmd->task.io_ports[4];
    scsi_cmd->scsi_cmd[11] = ata_cmd->task.hob_ports[5];  // high cylinder
    scsi_cmd->scsi_cmd[12] = ata_cmd->task.io_ports[5];
    scsi_cmd->scsi_cmd[13] = ata_cmd->task.io_ports[6];  // device
    scsi_cmd->scsi_cmd[14] = ata_cmd->task.io_ports[7];  // command
}

void fill_scsi_ata_return_descriptor(ScsiAtaReturnDescriptor *scsi_ata_ret, ScsiCommand *scsi_cmd) {
    uint8_t *descr = &scsi_cmd->sense_buf[8];
    memcpy(scsi_ata_ret->descriptor, descr, sizeof(scsi_ata_ret->descriptor));
    scsi_ata_ret->error = descr[3];
    scsi_ata_ret->status = descr[13];
    scsi_ata_ret->lba  = 0;
    scsi_ata_ret->lba |= (uint64_t)descr[7];
    scsi_ata_ret->lba |= (uint64_t)descr[9]  <<  8;
    scsi_ata_ret->lba |= (uint64_t)descr[11] << 16;
    scsi_ata_ret->lba |= (uint64_t)descr[6]  << 24;
    scsi_ata_ret->lba |= (uint64_t)descr[8]  << 32;
    scsi_ata_ret->lba |= (uint64_t)descr[10] << 40;
}

int get_sense_key_from_sense_buffer(uint8_t *buf) {
    switch (buf[0]) {
        case 0x70:
        case 0x71:
            return buf[2] & 0x0f;
        case 0x72:
        case 0x73:
            return buf[1] & 0x0f;
        default:
            return -1;
    }
}

DC_BlockStatus scsi_ata_check_return_status(ScsiCommand *scsi_command) {
    if (scsi_command->io_hdr.status == 0)
        return DC_BlockStatus_eOk;
    if (scsi_command->io_hdr.status != 0x02 /* CHECK_CONDITION */)
        return DC_BlockStatus_eError;
    int sense_key = get_sense_key_from_sense_buffer(scsi_command->sense_buf);
    ScsiAtaReturnDescriptor scsi_ata_return;
#if 0
    fprintf(stderr, "scsi status: %hhu, msg status %hhu, host status %hu, driver status %hu, duration %u, auxinfo %u\n",
            scsi_command->io_hdr.status,
            scsi_command->io_hdr.msg_status,
            scsi_command->io_hdr.host_status,
            scsi_command->io_hdr.driver_status,
            scsi_command->io_hdr.duration,
            scsi_command->io_hdr.info);
    fprintf(stderr, "sense buffer, in hex: ");
    int i;
    for (i = 0; i < sizeof(scsi_command->sense_buf); i++)
        fprintf(stderr, "%02hhx", scsi_command->sense_buf[i]);
    fprintf(stderr, "\n");
    fprintf(stderr, "sense key is %d", sense_key);
#endif

    fill_scsi_ata_return_descriptor(&scsi_ata_return, scsi_command);
    if (scsi_ata_return.status & STATUS_BIT_ERR) {
        if (scsi_ata_return.error & ERROR_BIT_UNC)
            return DC_BlockStatus_eUnc;
        else if (scsi_ata_return.error & ERROR_BIT_IDNF)
            return DC_BlockStatus_eIdnf;
        else if (scsi_ata_return.error & ERROR_BIT_ABRT)
            return DC_BlockStatus_eAbrt;
        else if (scsi_ata_return.error & ERROR_BIT_AMNF)
            return DC_BlockStatus_eAmnf;
        else
            return DC_BlockStatus_eError;
    } else if (scsi_ata_return.status & STATUS_BIT_DF) {
        return DC_BlockStatus_eError;
    } else if (sense_key > 0x01) {
        if (sense_key == 0x0b)
            return DC_BlockStatus_eAbrt;
        else
            return DC_BlockStatus_eError;
    } else if (scsi_command->io_hdr.duration >= scsi_command->io_hdr.timeout) {
        return DC_BlockStatus_eTimeout;
    }

    return DC_BlockStatus_eOk;
}
