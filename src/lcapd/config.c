/*
 * LCAP - Lustre Changelogs Aggregate and Publish
 *
 * Copyright (C)  2013-2014  CEA/DAM
 * Henri DOREAU <henri.doreau@cea.fr>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3.0 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */


#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>

#include "lcapd_internal.h"

#define DEFAULT_CFG_FILE    "/etc/lcap.cfg"
#define DEFAULT_REC_BATCH   64
#define DEFAULT_MAX_BUCKETS 256

/* defined in lcapd.c */
void usage(void);

struct lcap_cfg_statement {
    const char *name;
    int (*handler)(struct lcap_cfg *config, const char *line);
};


static inline int startswith(const char *haystack, const char *needle)
{
    return (strncasecmp(haystack, needle, strlen(needle)) == 0);
}

static char *cfg_get_arg(const char *line)
{
    int rc;
    char arg[256];

    rc = sscanf(line, "%*s %254s", arg);
    if (rc == 0)
        return NULL;

    return strdup(arg);
}

static int lcap_parse_args(int ac, char **av, struct lcap_cfg *config)
{
    int opt;

    while ((opt = getopt(ac, av, "hvc:o")) != -1) {
        switch(opt) {
            case 'c':
                config->ccf_file = strdup(optarg);
                break;

            case 'o':
                config->ccf_oneshot = true;
                break;

            case 'v':
                config->ccf_verbosity += 1;
                break;

            case 'h':
                usage();
                exit(0);

            case '?':
            default:
                fprintf(stderr, "Invalid switch %c.\n", optopt);
                usage();
                return -EINVAL;
        }
    }

#if 0
    ac -= optind;
    av += optind;
#endif

    return 0;
}

static int handle_cfg_batch_records_line(struct lcap_cfg *config, const char *line)
{
    char *count;

    count = cfg_get_arg(line);
    if (count == NULL)
        return -EINVAL;

    config->ccf_rec_batch_count = atoi(count);
    free(count);

    return 0;
}

static int handle_cfg_max_buckets_line(struct lcap_cfg *config, const char *line)
{
    char *count;

    count = cfg_get_arg(line);
    if (count == NULL)
        return -EINVAL;

    config->ccf_max_bkt = atoi(count);
    free(count);

    return 0;
}

static int handle_cfg_logtype_line(struct lcap_cfg *config, const char *line)
{
    if (config->ccf_loggername)
        return -EALREADY;

    config->ccf_loggername = cfg_get_arg(line);
    if (config->ccf_loggername == NULL)
        return -EINVAL;

    return 0;
}

static int handle_cfg_workers_line(struct lcap_cfg *config, const char *line)
{
    char *count;

    if (config->ccf_worker_count)
        return -EALREADY;

    count = cfg_get_arg(line);
    if (count == NULL)
        return -EINVAL;

    config->ccf_worker_count = atoi(count);
    free(count);

    return 0;
}

static int handle_cfg_mdtdevice_line(struct lcap_cfg *config, const char *line)
{
    char *mdtdev;

    if (config->ccf_mdtcount >= MAX_MDT) {
        fprintf(stderr, "Max # of MDT devices reached (%d)\n", MAX_MDT);
        return -EALREADY;
    }

    mdtdev = cfg_get_arg(line);
    if (mdtdev == NULL) {
        fprintf(stderr, "Missing parameter: MDT device name\n");
        return -EINVAL;
    }

    config->ccf_mdt[config->ccf_mdtcount++] = mdtdev;
    return 0;
}

static int handle_cfg_clreader_line(struct lcap_cfg *config, const char *line)
{
    char *clreader;

    if (config->ccf_clreader)
        return -EALREADY;

    clreader = cfg_get_arg(line);
    if (clreader == NULL) {
        fprintf(stderr, "Missing parameter: CL reader index\n");
        return -EINVAL;
    }

    config->ccf_clreader = clreader;
    return 0;
}

static int __parse_config_line(struct lcap_cfg *config, const char *line)
{
    int i;
    const char *begin = line;
    struct lcap_cfg_statement directives[] = {
        /* -- global -- */
        {"batch_records", handle_cfg_batch_records_line},
        {"max_buckets",   handle_cfg_max_buckets_line},
        {"logtype",       handle_cfg_logtype_line},
        {"workers",       handle_cfg_workers_line},
        /* -- lustre filesystem -- */
        {"mdtdevice",     handle_cfg_mdtdevice_line},
        {"clreader",      handle_cfg_clreader_line},
        {NULL, NULL}
    };

    while (*begin && isspace(*begin))
        begin++;

    /* Skip empty lines and comments */
    if ((*begin == '\0') || (*begin == '#'))
        return 0;

    for (i = 0; directives[i].name != NULL; i++) {
        if (startswith(line, directives[i].name) && directives[i].handler) {
            return directives[i].handler(config, line);
        }
    }

    fprintf(stderr, "Unknown configuration statement: %s\n", line);
    return -EINVAL;
}

static int lcap_parse_config_file(struct lcap_cfg *config)
{
    int rc;
    char line[128];
    char *ccf_filename;
    FILE *fin;

    ccf_filename = config->ccf_file ? config->ccf_file : DEFAULT_CFG_FILE;

    fin = fopen(ccf_filename, "r");
    if (fin == NULL) {
        fprintf(stderr, "Can't find a valid configuration file\n");
        return -ENOENT;
    }

    errno = 0;

    while (fgets(line, sizeof(line), fin)) {
        rc = __parse_config_line(config, line);
        if (rc) {
            if (rc == -EALREADY)
                fprintf(stderr, "Multiple param specifications: %s\n", line);
            goto out;
        }
    }

    rc = -errno;

out:
    fclose(fin);
    return rc;
}

static void config_set_defaults(struct lcap_cfg *config)
{
    config->ccf_rec_batch_count = DEFAULT_REC_BATCH;
    config->ccf_max_bkt         = DEFAULT_MAX_BUCKETS;
}

int lcap_cfg_init(int ac, char **av, struct lcap_cfg *config)
{
    int rc;

    memset(config, 0x00, sizeof(*config));

    config_set_defaults(config);

    rc = lcap_parse_args(ac, av, config);
    if (rc)
        return rc;

    return lcap_parse_config_file(config);
}

int lcap_cfg_release(struct lcap_cfg *cfg)
{
    int i;

    for (i = 0; cfg->ccf_mdt[i] && i < MAX_MDT; i++)
        free(cfg->ccf_mdt[i]);

    free(cfg->ccf_clreader);
    free(cfg->ccf_file);
    free(cfg->ccf_loggername);

    memset(cfg, 0, sizeof(*cfg));

    return 0;
}
