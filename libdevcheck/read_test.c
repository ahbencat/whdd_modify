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
#include "utils.h"

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
    uint64_t vis_thresholds[DC_VIS_THRESHOLD_COUNT];  // filled in Open() from sectors_at_once
    uint64_t access_time_stats[DC_VIS_THRESHOLD_COUNT + 1];  // one bucket per tier, last is >= top threshold
    uint64_t error_stats[7];  // 0th is unused, the rest are as in DC_BlockStatus enum
    uint64_t blocks_processed;
    uint64_t blocks_with_errors;
    uint64_t total_access_time;  // in mcs
    int64_t first_error_lba;
    int64_t last_error_lba;
    // DiskGenius-style defect intervals: contiguous LBA ranges by severity
    struct dg_interval {
        int64_t begin_lba;
        int64_t end_lba;  // LBA of last sector of the range, inclusive
        int type;  // 1 = severe (slow, >500ms), 2 = damaged (read error)
    } *dg_intervals;
    int nb_dg_intervals;
    int64_t dg_cur_begin;
    int64_t dg_cur_end;
    int dg_cur_type;  // 0 = no open interval, else as in dg_interval.type
};
typedef struct read_priv ReadPriv;

#define DEFAULT_SECTORS_AT_ONCE 256

/* Build the default report basename:
 *   WHDD_REPORT_<serial>_<YYMMDD>_<HHMMSS>
 * Serial is device-supplied data, so it is sanitized; result is malloc'd. */
static char *make_default_report_basename(DC_Dev *dev, time_t now) {
    struct tm tm_buf;
    char ts[32];
    char serial[128];
    char *result;
    int r;
    localtime_r(&now, &tm_buf);
    strftime(ts, sizeof(ts), "%y%m%d_%H%M%S", &tm_buf);
    snprintf(serial, sizeof(serial), "%s",
             dev->serial_no ? dev->serial_no : "unknown");
    dc_sanitize_for_filename(serial);
    r = asprintf(&result, "WHDD_REPORT_%s_%s", serial, ts);
    assert(r != -1);
    return result;
}

// Contiguity-checked interval bookkeeping for the defect-list report.
// Slow-but-OK blocks (>500ms) are "severe", read errors are "damaged";
// damaged wins when a block is both.
static void dg_interval_update(ReadPriv *priv, int64_t lba, int64_t end_lba, int type) {
    if (type) {
        if ((priv->dg_cur_type == type) && (lba == priv->dg_cur_end + 1)) {
            priv->dg_cur_end = end_lba;
            return;
        }
        if (priv->dg_cur_type)
            goto flush;
        priv->dg_cur_begin = lba;
        priv->dg_cur_end = end_lba;
        priv->dg_cur_type = type;
        return;
    }
    if (priv->dg_cur_type)
        goto flush;
    return;
flush:
    {
        void *p = realloc(priv->dg_intervals,
                (priv->nb_dg_intervals + 1) * sizeof(*priv->dg_intervals));
        if (!p)
            return;
        priv->dg_intervals = p;
        priv->dg_intervals[priv->nb_dg_intervals].begin_lba = priv->dg_cur_begin;
        priv->dg_intervals[priv->nb_dg_intervals].end_lba = priv->dg_cur_end;
        priv->dg_intervals[priv->nb_dg_intervals].type = priv->dg_cur_type;
        priv->nb_dg_intervals++;
        priv->dg_cur_type = 0;
        if (type) {
            priv->dg_cur_begin = lba;
            priv->dg_cur_end = end_lba;
            priv->dg_cur_type = type;
        }
    }
}

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
        setting->value = make_default_report_basename(dev, time(NULL));
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
    dc_get_vis_thresholds(priv->sectors_at_once, priv->vis_thresholds);
    // Auto-save: normalize an empty or "none" report_file into a
    // timestamped basename so the reports are always written.
    if (!priv->report_file || !priv->report_file[0]
            || !strcmp(priv->report_file, "none")) {
        free((void *)priv->report_file);
        priv->report_file = make_default_report_basename(ctx->dev, time(NULL));
    }
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
    {
        int64_t block_end_lba = ctx->report.lba + ctx->report.sectors_processed - 1;
        int dg_type = 0;
        if (ctx->report.blk_status)
            dg_type = 2;  // damaged
        else if (ctx->report.blk_access_time >= priv->vis_thresholds[DC_VIS_THRESHOLD_COUNT - 1])
            dg_type = 1;  // severe
        dg_interval_update(priv, ctx->report.lba, block_end_lba, dg_type);
    }
    if (ctx->report.blk_status) {
        priv->error_stats[ctx->report.blk_status]++;
        priv->blocks_with_errors++;
        if (priv->first_error_lba < 0)
            priv->first_error_lba = ctx->report.lba;
        priv->last_error_lba = ctx->report.lba + ctx->report.sectors_processed - 1;
    } else {
        int i;
        for (i = 0; i < DC_VIS_THRESHOLD_COUNT; i++)
            if (ctx->report.blk_access_time < priv->vis_thresholds[i])
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
    char path[4096];

    snprintf(path, sizeof(path), "%s.report", priv->report_file);
    f = fopen(path, "w");
    if (!f) {
        dc_log(DC_LOG_ERROR, "Cannot open report file '%s'", path);
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
    fprintf(f, "Serial number: %s\n",
            ctx->dev->serial_no ? ctx->dev->serial_no : "unknown");
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
    for (int i = 0; i < DC_VIS_THRESHOLD_COUNT; i++)
        fprintf(f, "  <%" PRIu64 "ms   : %" PRIu64 "\n",
                priv->vis_thresholds[i] / 1000, priv->access_time_stats[i]);
    fprintf(f, "  >=%" PRIu64 "ms  : %" PRIu64 "\n",
            priv->vis_thresholds[DC_VIS_THRESHOLD_COUNT - 1] / 1000,
            priv->access_time_stats[DC_VIS_THRESHOLD_COUNT]);
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
}

static void write_dg_report(DC_ProcedureCtx *ctx) {
    ReadPriv *priv = ctx->priv;
    FILE *f;
    char path[4096];

    // Flush the interval being accumulated when the scan ended
    if (priv->dg_cur_type)
        dg_interval_update(priv, 0, -1, 0);

    snprintf(path, sizeof(path), "%s.dg", priv->report_file);
    f = fopen(path, "w");
    if (!f) {
        dc_log(DC_LOG_ERROR, "Cannot open defect list file '%s'", path);
        return;
    }

    fprintf(f, "WHDD read test defect list (DiskGenius-style)\n");
    {
        int i;
        uint64_t severe_count = 0, damaged_count = 0;
        for (i = 0; i < priv->nb_dg_intervals; i++) {
            uint64_t sectors = priv->dg_intervals[i].end_lba - priv->dg_intervals[i].begin_lba + 1;
            if (priv->dg_intervals[i].type == 1)
                severe_count += sectors;
            else
                damaged_count += sectors;
        }
        fprintf(f, "Device: %s (%s)\n", ctx->dev->dev_path,
                ctx->dev->model_str ? ctx->dev->model_str : "unknown model");
        fprintf(f, "Serial number: %s\n",
                ctx->dev->serial_no ? ctx->dev->serial_no : "unknown");
        fprintf(f, "Scanned range: LBA %" PRId64 " .. %" PRId64 "\n",
                priv->start_lba, priv->end_lba - 1);
        fprintf(f, "\n");
        fprintf(f, "Severe (slow blocks, >=%" PRIu64 "ms): %" PRIu64 " sectors\n",
                priv->vis_thresholds[DC_VIS_THRESHOLD_COUNT - 1] / 1000, severe_count);
        fprintf(f, "Damaged (read errors): %" PRIu64 " sectors\n", damaged_count);
        fprintf(f, "\n");
        fprintf(f, "Type     LBA begin       LBA end         Sectors\n");
        for (i = 0; i < priv->nb_dg_intervals; i++) {
            uint64_t sectors = priv->dg_intervals[i].end_lba - priv->dg_intervals[i].begin_lba + 1;
            fprintf(f, "%-8s %15" PRId64 " %15" PRId64 " %13" PRIu64 "\n",
                    priv->dg_intervals[i].type == 1 ? "severe" : "damaged",
                    priv->dg_intervals[i].begin_lba,
                    priv->dg_intervals[i].end_lba,
                    sectors);
        }
        if (!priv->nb_dg_intervals)
            fprintf(f, "(no defective blocks found)\n");
    }
    fclose(f);
}

static void Close(DC_ProcedureCtx *ctx) {
    ReadPriv *priv = ctx->priv;
    write_report(ctx);
    write_dg_report(ctx);
    free(priv->dg_intervals);
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
    { "report_file", "basename for the two report files (saved as <name>.report and <name>.dg). Leave empty for an auto-generated timestamped name.", offsetof(ReadPriv, report_file), DC_ProcedureOptionType_eString },
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

