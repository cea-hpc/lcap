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


#ifdef HAVE_CONFIG_H
#include "lcap_config.h"
#endif

#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <stdbool.h>

#include "lcap_client.h"

#ifndef LPX64
# define LPX64   "%#llx"

static inline bool fid_is_zero(const lustre_fid *fid)
{
    return fid->f_seq == 0 && fid->f_oid == 0;
}
#endif

void static usage(void)
{
    fprintf(stderr, "Usage: lcap [-d] <mdtname>\n");
}

int main(int ac, char **av)
{
    struct lcap_cl_ctx          *ctx = NULL;
    struct changelog_ext_rec    *rec;
    const char                  *mdtname = NULL;
    int                          flags = LCAP_CL_BLOCK;
    int                          c;
    int                          rc;
    long long                    last_idx = 0;

    if (ac < 2) {
        usage();
        return 1;
    }

    while ((c = getopt(ac, av, "d")) != -1) {
        switch (c) {
            case 'd':
                flags |= LCAP_CL_DIRECT;
                break;

            case '?':
                fprintf(stderr, "Unknown option: %s\n", optopt);
                usage();
                return 1;
        }
    }

    ac -= optind;
    av += optind;

    if (ac < 1) {
        usage();
        return 1;
    }

    mdtname = av[0];

    rc = lcap_changelog_start(&ctx, flags, mdtname, 0LL);
    if (rc) {
        fprintf(stderr, "lcap_changelog_start: %s\n", strerror(-rc));
        return 1;
    }

    while ((rc = lcap_changelog_recv(ctx, &rec)) == 0) {
        time_t      secs;
        struct tm   ts;

        secs = rec->cr_time >> 30;
        gmtime_r(&secs, &ts);

        printf("%llu %02d%-5s %02d:%02d:%02d.%06d %04d.%02d.%02d 0x%x t="DFID,
               rec->cr_index, rec->cr_type, changelog_type2str(rec->cr_type),
               ts.tm_hour, ts.tm_min, ts.tm_sec,
               (int)(rec->cr_time & ((1 << 30) - 1)),
               ts.tm_year + 1900, ts.tm_mon + 1, ts.tm_mday,
               rec->cr_flags & CLF_FLAGMASK, PFID(&rec->cr_tfid));

        if (rec->cr_namelen)
            printf(" p="DFID" %.*s", PFID(&rec->cr_pfid), rec->cr_namelen,
                   rec->cr_name);

        if (!fid_is_zero(&rec->cr_sfid))
            printf(" s="DFID" sp="DFID" %.*s", PFID(&rec->cr_sfid),
                   PFID(&rec->cr_spfid), changelog_rec_snamelen(rec),
                   changelog_rec_sname(rec));

        printf("\n");

        last_idx = rec->cr_index;
        lcap_changelog_free(ctx, &rec);
    }

    if (rc && rc != 1) {
        fprintf(stderr, "lcap_changelog_recv: %s\n", strerror(-rc));
        return 1;
    }

    rc = lcap_changelog_clear(ctx, mdtname, "", last_idx);

    rc = lcap_changelog_fini(ctx);
    if (rc) {
        fprintf(stderr, "lcap_changelog_fini: %s\n", strerror(-rc));
        return 1;
    }

    return 0;
}
