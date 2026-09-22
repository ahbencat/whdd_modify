#define _FILE_OFFSET_BITS 64

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "procedure.h"
#include "scsi.h"
#include "utils.h"

#define QUICK_SAMPLE_COUNT 3
#define QUICK_SUMMARY_SIZE 8192

typedef struct quick_sample {
    uint64_t lba;
    DC_BlockReport report;
    int posix_errno;
} QuickSample;

typedef struct quick_diagnosis_priv {
    unsigned int logical_sector_size;
    unsigned int physical_sector_size;
    uint64_t capacity_bytes;
    uint64_t sector_count;
    int geometry_ok;
    int physical_known;
    int pi_known;
    int pi_mismatch;
    int pi_errno;
    DC_ScsiCapacity16 pi;
    QuickSample samples[QUICK_SAMPLE_COUNT];
    int sample_count;
} QuickDiagnosisPriv;

static void summary_append(char *summary, size_t size, int *used,
        const char *fmt, ...) {
    va_list ap;
    int written;

    if (*used >= (int)size - 1)
        return;
    va_start(ap, fmt);
    written = vsnprintf(summary + *used, size - (size_t)*used, fmt, ap);
    va_end(ap);
    if (written < 0)
        return;
    if ((size_t)written >= size - (size_t)*used)
        *used = (int)size - 1;
    else
        *used += written;
}

static const char *block_status_name(DC_BlockStatus status) {
    switch (status) {
        case DC_BlockStatus_eOk:      return "OK";
        case DC_BlockStatus_eError:   return "ERROR";
        case DC_BlockStatus_eTimeout: return "TIMEOUT";
        case DC_BlockStatus_eUnc:     return "UNC";
        case DC_BlockStatus_eIdnf:    return "IDNF";
        case DC_BlockStatus_eAbrt:    return "ABRT";
        case DC_BlockStatus_eAmnf:    return "AMNF";
    }
    return "UNKNOWN";
}

static const char *geometry_name(unsigned int logical, unsigned int physical) {
    if (logical == 512 && physical == 512)
        return "512n";
    if (logical == 512 && physical > logical)
        return "512e";
    if (logical == 4096 && physical == 4096)
        return "4Kn";
    return "custom/unknown";
}

static void append_pi_result(char *summary, size_t size, int *used,
        QuickDiagnosisPriv *priv) {
    if (!priv->pi_known) {
        summary_append(summary, size, used,
                "PI: not reported (READ CAPACITY(16) unavailable, errno=%d: %s)\n",
                priv->pi_errno, strerror(priv->pi_errno));
        return;
    }
    summary_append(summary, size, used,
            "PI: %s, P_TYPE=%u, P_I_EXPONENT=%u, SCSI block length=%" PRIu32 " B\n",
            priv->pi.prot_en ? "enabled" : "disabled",
            priv->pi.p_type, priv->pi.p_i_exponent, priv->pi.block_length);
    if (priv->pi_mismatch)
        summary_append(summary, size, used,
                "WARNING: SCSI READ CAPACITY metadata differs from Linux geometry; "
                "possible in-band PI/format incompatibility.\n");
}

static void append_samples(char *summary, size_t size, int *used,
        QuickDiagnosisPriv *priv) {
    int i;

    summary_append(summary, size, used, "Read-only samples (one logical sector):\n");
    for (i = 0; i < priv->sample_count; i++) {
        QuickSample *sample = &priv->samples[i];
        summary_append(summary, size, used,
                "  LBA=%" PRIu64 ": %s, time=%" PRIu64 " us",
                sample->lba, block_status_name(sample->report.blk_status),
                sample->report.blk_access_time);
        if (sample->report.blk_status && sample->posix_errno)
            summary_append(summary, size, used, ", errno=%d (%s)",
                    sample->posix_errno, strerror(sample->posix_errno));
        summary_append(summary, size, used, "\n");
    }
}

static int Open(DC_ProcedureCtx *ctx) {
    QuickDiagnosisPriv *priv = ctx->priv;
    char summary[QUICK_SUMMARY_SIZE];
    char geometry[64];
    char physical[32];
    int used = 0;
    int sample_failure = 0;
    int geometry_warning = 0;
    int i;
    int ret;

    memset(priv, 0, sizeof(*priv));
    memset(summary, 0, sizeof(summary));

    ret = dc_dev_logical_sector_size(ctx->dev->dev_path,
            &priv->logical_sector_size);
    if (!ret)
        ret = dc_dev_capacity_bytes(ctx->dev->dev_path, &priv->capacity_bytes);
    if (!ret && priv->logical_sector_size != 0
            && priv->capacity_bytes % priv->logical_sector_size == 0) {
        priv->sector_count = priv->capacity_bytes / priv->logical_sector_size;
        priv->geometry_ok = priv->sector_count > 0;
    } else {
        priv->geometry_ok = 0;
    }

    if (!dc_dev_physical_sector_size(ctx->dev->dev_path,
                &priv->physical_sector_size))
        priv->physical_known = 1;

    ret = dc_scsi_read_capacity16(ctx->dev->dev_path, &priv->pi);
    if (!ret && priv->pi.block_length != 0) {
        priv->pi_known = 1;
        priv->pi_mismatch = priv->geometry_ok
            && priv->pi.block_length != priv->logical_sector_size;
        if (priv->geometry_ok && priv->pi.last_lba != UINT64_MAX
                && priv->pi.last_lba + 1 != priv->sector_count)
            priv->pi_mismatch = 1;
    } else {
        priv->pi_errno = ret ? errno : EPROTO;
        if (priv->pi_errno == 0)
            priv->pi_errno = EIO;
    }

    summary_append(summary, sizeof(summary), &used,
            "WHDD quick disk diagnosis (read-only)\n"
            "Device: %s\n"
            "Model: %s\n"
            "Serial: %s\n",
            ctx->dev->dev_path,
            ctx->dev->model_str ? ctx->dev->model_str : "unknown",
            ctx->dev->serial_no ? ctx->dev->serial_no : "unknown");

    if (!priv->geometry_ok) {
        summary_append(summary, sizeof(summary), &used,
                "Geometry: unavailable or inconsistent (logical sector/capacity query failed)\n");
        geometry_warning = 1;
    } else {
        if (priv->physical_known)
            snprintf(geometry, sizeof(geometry), "%s",
                    geometry_name(priv->logical_sector_size,
                        priv->physical_sector_size));
        else
            snprintf(geometry, sizeof(geometry), "unknown (physical size unavailable)");
        if (priv->physical_known)
            snprintf(physical, sizeof(physical), "%u B", priv->physical_sector_size);
        else
            snprintf(physical, sizeof(physical), "unknown");
        summary_append(summary, sizeof(summary), &used,
                "Geometry: %s; logical=%u B, physical=%s, capacity=%" PRIu64
                " B (%" PRIu64 " logical sectors)\n",
                geometry, priv->logical_sector_size, physical,
                priv->capacity_bytes, priv->sector_count);
        if (priv->physical_known)
            summary_append(summary, sizeof(summary), &used,
                    "Physical sector size: %u B\n", priv->physical_sector_size);
        else
            geometry_warning = 1;
        if (!strcmp(geometry, "custom/unknown"))
            geometry_warning = 1;
    }

    append_pi_result(summary, sizeof(summary), &used, priv);

    if (priv->geometry_ok) {
        uint64_t candidates[QUICK_SAMPLE_COUNT];
        candidates[0] = 0;
        candidates[1] = priv->sector_count / 2;
        candidates[2] = priv->sector_count - 1;
        for (i = 0; i < QUICK_SAMPLE_COUNT; i++) {
            int duplicate = 0;
            int j;
            for (j = 0; j < i; j++)
                if (candidates[j] == candidates[i])
                    duplicate = 1;
            if (duplicate)
                continue;
            priv->samples[priv->sample_count].lba = candidates[i];
            ret = dc_blk_read(ctx->dev, Api_ePosix, candidates[i], 1,
                    &priv->samples[priv->sample_count].report,
                    &priv->samples[priv->sample_count].posix_errno, NULL);
            if (ret) {
                priv->samples[priv->sample_count].report.blk_status =
                    DC_BlockStatus_eError;
                priv->samples[priv->sample_count].posix_errno = errno;
            }
            if (priv->samples[priv->sample_count].report.blk_status)
                sample_failure = 1;
            priv->sample_count++;
        }
        append_samples(summary, sizeof(summary), &used, priv);
    } else {
        summary_append(summary, sizeof(summary), &used,
                "Read-only samples: skipped because geometry is unavailable\n");
    }

    if (sample_failure)
        summary_append(summary, sizeof(summary), &used,
                "Conclusion: POTENTIAL ISSUE — at least one sampled read failed.\n");
    else if (geometry_warning || priv->pi_mismatch)
        summary_append(summary, sizeof(summary), &used,
                "Conclusion: POTENTIAL COMPATIBILITY ISSUE — review the geometry/PI lines above.\n");
    else if (!priv->pi_known)
        summary_append(summary, sizeof(summary), &used,
                "Conclusion: INCONCLUSIVE — PI was not reported by this path; sampled reads passed.\n");
    else if (priv->pi.prot_en)
        summary_append(summary, sizeof(summary), &used,
                "Conclusion: samples passed; PI is enabled and must match the host/controller path; "
                "this is not a full-surface test.\n");
    else
        summary_append(summary, sizeof(summary), &used,
                "Conclusion: no issue found in these samples; this is not a full-surface test.\n");

    summary_append(summary, sizeof(summary), &used,
            "No data was written. For further read-only investigation use smartctl -x, "
            "sg_readcap -l, and the relevant kernel log lines.\n");
    dc_log(DC_LOG_INFO, "%s", summary);
    ctx->blk_size = priv->geometry_ok ? priv->logical_sector_size : 0;
    return 0;
}

static void Close(DC_ProcedureCtx *ctx) {
    (void)ctx;
}

DC_Procedure quick_diagnosis = {
    .name = "quick_diagnosis",
    .display_name = "Quick disk diagnosis",
    .help = "Read-only quick check of 512n/512e/4Kn geometry, SCSI PI metadata, and three representative logical-sector reads. A pass is not a full-surface health test.",
    .open = Open,
    .close = Close,
    .priv_data_size = sizeof(QuickDiagnosisPriv),
};
