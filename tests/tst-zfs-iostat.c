#include <pthread.h>
#include <unistd.h>
#include <stdio.h>

#include <sys/spa.h>

static void
nicenum(uint64_t num, char *buf)
{
    uint64_t n = num;
    int index = 0;
    char u;

    while (n >= 1024) {
        n = (n + (1024 / 2)) / 1024; /* Round up or down */
        index++;
    }

    u = " KMGTPE"[index];

    if (index == 0) {
        (void) sprintf(buf, "%llu", (u_longlong_t)n);
    } else if (n < 10 && (num & (num - 1)) != 0) {
        (void) sprintf(buf, "%.2f%c",
            (double)num / (1ULL << 10 * index), u);
    } else if (n < 100 && (num & (num - 1)) != 0) {
        (void) sprintf(buf, "%.1f%c",
            (double)num / (1ULL << 10 * index), u);
    } else {
        (void) sprintf(buf, "%llu%c", (u_longlong_t)n, u);
    }
}

static void
show_vdev_stats(const char *desc, const char *ctype, nvlist_t *nv, int indent)
{
    vdev_stat_t *vs;
    vdev_stat_t v0 = { 0 };
    uint64_t sec;
    uint64_t is_log = 0;
    nvlist_t **child;
    uint_t c, children;
    char used[6], avail[6];
    char rops[6], wops[6], rbytes[6], wbytes[6], rerr[6], werr[6], cerr[6];
    char *prefix = "";

    if (indent == 0 && desc != NULL) {
        (void) printf("                           "
            " capacity   operations   bandwidth  ---- errors ----\n");
        (void) printf("description                "
            "used avail  read write  read write  read write cksum\n");
    }

    if (desc != NULL) {
        (void) nvlist_lookup_uint64(nv, ZPOOL_CONFIG_IS_LOG, &is_log);

        if (is_log)
            prefix = "log ";

        if (nvlist_lookup_uint64_array(nv, ZPOOL_CONFIG_VDEV_STATS,
            (uint64_t **)&vs, &c) != 0)
            vs = &v0;

        sec = 1;
        // sec = MAX(1, vs->vs_timestamp / NANOSEC);

        nicenum(vs->vs_alloc, used);
        nicenum(vs->vs_space - vs->vs_alloc, avail);
        nicenum(vs->vs_ops[ZIO_TYPE_READ] / sec, rops);
        nicenum(vs->vs_ops[ZIO_TYPE_WRITE] / sec, wops);
        nicenum(vs->vs_bytes[ZIO_TYPE_READ] / sec, rbytes);
        nicenum(vs->vs_bytes[ZIO_TYPE_WRITE] / sec, wbytes);
        nicenum(vs->vs_read_errors, rerr);
        nicenum(vs->vs_write_errors, werr);
        nicenum(vs->vs_checksum_errors, cerr);

        (void) printf("%*s%s%*s%*s%*s %5s %5s %5s %5s %5s %5s %5s\n",
            indent, "",
            prefix,
            (int) (indent + strlen(prefix) - 25 - (vs->vs_space ? 0 : 12)),
            desc,
            vs->vs_space ? 6 : 0, vs->vs_space ? used : "",
            vs->vs_space ? 6 : 0, vs->vs_space ? avail : "",
            rops, wops, rbytes, wbytes, rerr, werr, cerr);
    }

    if (nvlist_lookup_nvlist_array(nv, ctype, &child, &children) != 0)
        return;

    for (c = 0; c < children; c++) {
        nvlist_t *cnv = child[c];
        char *cname, *tname;
        uint64_t np;
        if (nvlist_lookup_string(cnv, ZPOOL_CONFIG_PATH, &cname) &&
            nvlist_lookup_string(cnv, ZPOOL_CONFIG_TYPE, &cname))
            cname = "<unknown>";
        tname = calloc(1, strlen(cname) + 2);
        (void) strcpy(tname, cname);
        if (nvlist_lookup_uint64(cnv, ZPOOL_CONFIG_NPARITY, &np) == 0)
            tname[strlen(tname)] = '0' + np;
        show_vdev_stats(tname, ctype, cnv, indent + 2);
        free(tname);
    }
}

static void
show_pool_stats(spa_t *spa)
{
    nvlist_t *config, *nvroot;
    char *name;

    VERIFY(spa_get_stats(spa_name(spa), &config, NULL, 0) == 0);

    VERIFY(nvlist_lookup_nvlist(config, ZPOOL_CONFIG_VDEV_TREE,
        &nvroot) == 0);
    VERIFY(nvlist_lookup_string(config, ZPOOL_CONFIG_POOL_NAME,
        &name) == 0);

    show_vdev_stats(name, ZPOOL_CONFIG_CHILDREN, nvroot, 0);
    show_vdev_stats(NULL, ZPOOL_CONFIG_L2CACHE, nvroot, 0);
    show_vdev_stats(NULL, ZPOOL_CONFIG_SPARES, nvroot, 0);

    nvlist_free(config);
}

int main(int argc, char const *argv[])
{
    spa_t* spa = spa_lookup("osv");
    printf("spa = %x\n", spa);

    for(;;) {
        show_pool_stats(spa);
        sleep(1);
    }

    return 0;
}
