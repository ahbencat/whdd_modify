#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include "procedure.h"
#include "ata.h"
#include "scsi.h"

struct read_priv {
    const char *api_str;
    const char *report_file;
    int64_t start_lba;
    int64_t sectors_at_once;
    enum Api api;
    int64_t end_lba;
    int64_t lba_to_process;
    int fd;
    void *buf;
    AtaCommand ata_command;
    ScsiCommand scsi_command;
    int old_readahead;
    uint64_t current_lba;
    // Report accumulators
    uint64_t access_time_stats[6];  // same thresholds as cui/vis.c bs_vis[]
    uint64_t error_stats[7];  // 0th is unused, the rest are as in DC_BlockStatus enum
    uint64_t blocks_processed;
    uint64_t blocks_with_errors;
    uint64_t total_access_time;  // in mcs
    int64_t first_error_lba;
    int64_t last_error_lba;
};
typedef struct read_priv ReadPriv;

#define DEFAULT_SECTORS_AT_ONCE 256

static int SuggestDefaultValue(DC_Dev *dev, DC_OptionSetting *setting) {
    (void)dev;
    if (!strcmp(setting->name, "api")) {
        if (dev->ata_capable)
            setting->value = strdup("ata");
        else
            setting->value = strdup("posix");
    } else if (!strcmp(setting->name, "start_lba")) {
        setting->value = strdup("0");
    } else if (!strcmp(setting->name, "sectors_at_once")) {
        char *string;
        int r = asprintf(&string, "%d", DEFAULT_SECTORS_AT_ONCE);
        assert(r != -1);
        setting->value = string;
    } else if (!strcmp(setting->name, "report_file")) {
        char *string;
        int r = asprintf(&string, "whdd_read_test_report__%s__%s",
                dev->model_str ? dev->model_str : "unknown",
                dev->serial_no ? dev->serial_no : "unknown");
        assert(r != -1);
        setting->value = string;
    } else {
        return 1;
    }
    return 0;
}

static int Open(DC_ProcedureCtx *ctx) {
    int r;
    int open_flags;
    ReadPriv *priv = ctx->priv;

    // Setting context
    if (!strcmp(priv->api_str, "ata"))
        priv->api = Api_eAta;
    else if (!strcmp(priv->api_str, "posix"))
        priv->api = Api_ePosix;
    else
        return 1;
    if (priv->api == Api_eAta && !ctx->dev->ata_capable)
        return 1;
    if (priv->sectors_at_once <= 0)
        return 1;
    priv->first_error_lba = -1;
    ctx->blk_size = priv->sectors_at_once * 512;
    priv->current_lba = priv->start_lba;
    priv->end_lba = ctx->dev->capacity / 512;
    priv->lba_to_process = priv->end_lba - priv->start_lba;
    if (priv->lba_to_process <= 0)
        return 1;
    ctx->progress.den = priv->lba_to_process / priv->sectors_at_once;
    if (priv->lba_to_process % priv->sectors_at_once)
        ctx->progress.den++;

    if (priv->api == Api_eAta) {
        open_flags = O_RDWR;
    } else {
        r = posix_memalign(&priv->buf, sysconf(_SC_PAGESIZE), ctx->blk_size);
        if (r)
            return 1;

        open_flags = O_RDONLY | O_DIRECT | O_LARGEFILE | O_NOATIME;
    }

    priv->fd = open(ctx->dev->dev_path, open_flags);
    if (priv->fd == -1) {
        dc_log(DC_LOG_FATAL, "open %s fail\n", ctx->dev->dev_path);
        return 1;
    }

    lseek(priv->fd, 512 * priv->start_lba, SEEK_SET);
    r = ioctl(priv->fd, BLKFLSBUF, NULL);
    if (r == -1)
      dc_log(DC_LOG_WARNING, "Flushing block device buffers failed\n");
    r = ioctl(priv->fd, BLKRAGET, &priv->old_readahead);
    if (r == -1)
      dc_log(DC_LOG_WARNING, "Getting block device readahead setting failed\n");
    r = ioctl(priv->fd, BLKRASET, 0);
    if (r == -1)
      dc_log(DC_LOG_WARNING, "Disabling block device readahead setting failed\n");

    return 0;
}

static int Perform(DC_ProcedureCtx *ctx) {
    ssize_t read_ret;
    int ioctl_ret;
    int ret = 0;
    ReadPriv *priv = ctx->priv;
    size_t sectors_to_read = (priv->lba_to_process < priv->sectors_at_once) ? priv->lba_to_process : priv->sectors_at_once;

    // Updating context
    ctx->report.lba = priv->current_lba;
    ctx->report.sectors_processed = sectors_to_read;
    ctx->report.blk_status = DC_BlockStatus_eOk;

    // Preparing to act
    if (priv->api == Api_eAta) {
        memset(&priv->ata_command, 0, sizeof(priv->ata_command));
        memset(&priv->scsi_command, 0, sizeof(priv->scsi_command));
        prepare_ata_command(&priv->ata_command, WIN_VERIFY_EXT /* 42h */, priv->current_lba, sectors_to_read);
        prepare_scsi_command_from_ata(&priv->scsi_command, &priv->ata_command);
    }

    // Timing
    _dc_proc_time_pre(ctx);

    // Acting
    if (priv->api == Api_eAta)
        ioctl_ret = ioctl(priv->fd, SG_IO, &priv->scsi_command);
    else
        read_ret = read(priv->fd, priv->buf, sectors_to_read * 512);

    // Timing
    _dc_proc_time_post(ctx);

    // Error handling
    if (priv->api == Api_eAta) {
        // Updating context
        if (ioctl_ret) {
            ctx->report.blk_status = DC_BlockStatus_eError;
            ret = 1;
        }
        ctx->report.blk_status = scsi_ata_check_return_status(&priv->scsi_command);
    } else {
        if ((int)read_ret != (int)sectors_to_read * 512) {
            // Position of fd is undefined. Set fd position to read next block
            lseek(priv->fd, 512 * priv->current_lba, SEEK_SET);

            // Updating context
            ctx->report.blk_status = DC_BlockStatus_eError;
        }
    }

    // Updating context
    ctx->progress.num++;
    priv->lba_to_process -= sectors_to_read;
    priv->current_lba += sectors_to_read;

    // Report accumulators
    priv->blocks_processed++;
    priv->total_access_time += ctx->report.blk_access_time;
    if (ctx->report.blk_status) {
        priv->error_stats[ctx->report.blk_status]++;
        priv->blocks_with_errors++;
        if (priv->first_error_lba < 0)
            priv->first_error_lba = ctx->report.lba;
        priv->last_error_lba = ctx->report.lba + ctx->report.sectors_processed - 1;
    } else {
        static const uint64_t vis_thresholds[6] = {3000, 10000, 50000, 150000, 500000, 0};
        int i;
        for (i = 0; i < 5; i++)
            if (ctx->report.blk_access_time < vis_thresholds[i])
                break;
        priv->access_time_stats[i]++;
    }

    return ret;
}

static void write_report(DC_ProcedureCtx *ctx) {
    ReadPriv *priv = ctx->priv;
    FILE *f;
    time_t now;
    struct tm tm_buf;
    char timestamp[40];

    if (!priv->report_file || !strcmp(priv->report_file, "none"))
        return;

    f = fopen(priv->report_file, "w");
    if (!f) {
        dc_log(DC_LOG_ERROR, "Cannot open report file '%s'", priv->report_file);
        return;
    }

    now = time(NULL);
    localtime_r(&now, &tm_buf);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &tm_buf);

    uint64_t bytes_processed = priv->blocks_processed * ctx->blk_size;
    fprintf(f, "WHDD read test report\n");
    fprintf(f, "Time: %s\n", timestamp);
    fprintf(f, "Device: %s (%s)\n", ctx->dev->dev_path,
            ctx->dev->model_str ? ctx->dev->model_str : "unknown model");
    fprintf(f, "API: %s\n", priv->api_str);
    fprintf(f, "Block size: %" PRIu64 " bytes (%" PRId64 " sectors)\n",
            ctx->blk_size, priv->sectors_at_once);
    fprintf(f, "Start LBA: %" PRId64 ", End LBA: %" PRId64 "\n",
            priv->start_lba, priv->end_lba);
    fprintf(f, "Blocks processed: %" PRIu64 "\n", priv->blocks_processed);
    if (priv->blocks_processed) {
        fprintf(f, "Average block access time: %" PRIu64 " mcs\n",
                priv->total_access_time / priv->blocks_processed);
        fprintf(f, "Total access time: %" PRIu64 " mcs\n", priv->total_access_time);
        if (priv->total_access_time)
            fprintf(f, "Average speed: %" PRIu64 " bytes/s\n",
                    bytes_processed * 1000000 / priv->total_access_time);
    }
    fprintf(f, "Blocks with errors: %" PRIu64 "\n", priv->blocks_with_errors);
    fprintf(f, "Blocks OK by access time:\n");
    fprintf(f, "  <3ms    : %" PRIu64 "\n", priv->access_time_stats[0]);
    fprintf(f, "  <10ms   : %" PRIu64 "\n", priv->access_time_stats[1]);
    fprintf(f, "  <50ms   : %" PRIu64 "\n", priv->access_time_stats[2]);
    fprintf(f, "  <150ms  : %" PRIu64 "\n", priv->access_time_stats[3]);
    fprintf(f, "  <500ms  : %" PRIu64 "\n", priv->access_time_stats[4]);
    fprintf(f, "  >500ms  : %" PRIu64 "\n", priv->access_time_stats[5]);
    fprintf(f, "Blocks by error type:\n");
    fprintf(f, "  Error   : %" PRIu64 "\n", priv->error_stats[DC_BlockStatus_eError]);
    fprintf(f, "  Timeout : %" PRIu64 "\n", priv->error_stats[DC_BlockStatus_eTimeout]);
    fprintf(f, "  Unc     : %" PRIu64 "\n", priv->error_stats[DC_BlockStatus_eUnc]);
    fprintf(f, "  Idnf    : %" PRIu64 "\n", priv->error_stats[DC_BlockStatus_eIdnf]);
    fprintf(f, "  Abrt    : %" PRIu64 "\n", priv->error_stats[DC_BlockStatus_eAbrt]);
    fprintf(f, "  Amnf    : %" PRIu64 "\n", priv->error_stats[DC_BlockStatus_eAmnf]);
    if (priv->first_error_lba >= 0)
        fprintf(f, "Error LBA range: %" PRId64 " .. %" PRId64 "\n",
                priv->first_error_lba, priv->last_error_lba);

    fclose(f);
    dc_log(DC_LOG_INFO, "Report written to '%s'", priv->report_file);
}

static void Close(DC_ProcedureCtx *ctx) {
    ReadPriv *priv = ctx->priv;
    write_report(ctx);
    int r = ioctl(priv->fd, BLKRASET, priv->old_readahead);
    if (r == -1)
      dc_log(DC_LOG_WARNING, "Restoring block device readahead setting failed\n");
    free(priv->buf);
    close(priv->fd);
}

static const char * const api_choices[] = {"ata", "posix", NULL};
static const char * const sectors_choices[] = {"256", "1024", "4096", NULL};
static DC_ProcedureOption options[] = {
    { "api", "select operation API: \"posix\" for POSIX read(), \"ata\" for ATA \"READ VERIFY EXT\" command", offsetof(ReadPriv, api_str), DC_ProcedureOptionType_eString, api_choices },
    { "start_lba", "set LBA address to begin from", offsetof(ReadPriv, start_lba), DC_ProcedureOptionType_eInt64 },
    { "sectors_at_once", "sectors per block: 256=128KB, 1024=512KB, 4096=2MB", offsetof(ReadPriv, sectors_at_once), DC_ProcedureOptionType_eInt64, sectors_choices },
    { "report_file", "path of summary report file, or \"none\" to disable", offsetof(ReadPriv, report_file), DC_ProcedureOptionType_eString },
    { NULL }
};


DC_Procedure read_test = {
    .name = "read_test",
    .display_name = "Read test",
    .help = "Verifies entire device with reading. It reads data sequentially, from given start LBA up to end. To get data from source device, it may use ATA \"READ VERIFY EXT\" command, or POSIX read() function, by user choice.",
    .suggest_default_value = SuggestDefaultValue,
    .open = Open,
    .perform = Perform,
    .close = Close,
    .priv_data_size = sizeof(ReadPriv),
    .options = options,
};

