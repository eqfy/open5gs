/*
 * Copyright (C) 2026 Eric Yan <qfyan@uwaterloo.ca>
 *
 * This file is part of Open5GS.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * Container test entry point
 *
 * This is the main driver for the container-based inter-PLMN N2 handover
 * test suite. Unlike the standard abts-main.c, this variant:
 *
 *   1. Does NOT spawn NFs (they are already running in Docker containers)
 *   2. Overrides gnb_addr fields so that the GTP-U transport layer
 *      addresses encoded in NGAP messages are reachable from the
 *      Docker-networked UPF containers (not loopback)
 *   3. Still initializes SCTP, MongoDB, and the test context from YAML
 *      so that test_db_* and test framework helpers work correctly
 *
 * Usage:
 *   ./roaming-container [-c /path/to/container.yaml]
 *
 *   If -c is not given, it defaults to the built-in
 *   configs/container.yaml config.
 */

#include "test-app.h"

/* ── Advertised gNB GTP-U addresses ──
 * Read at runtime from container.yaml "container_test" section.
 * Compile-time defaults used only if YAML key is absent. */
static struct {
    char gnb1_advertised_addr[256];
    int  gnb1_advertised_port;
    char gnb2_advertised_addr[256];
    int  gnb2_advertised_port;
} g_gnb_cfg = {
    .gnb1_advertised_addr = "10.33.33.50",
    .gnb1_advertised_port = 2152,
    .gnb2_advertised_addr = "10.33.33.51",
    .gnb2_advertised_port = 2152,
};

/*
 * Read gnb advertised addresses from the "container_test" section
 * of the already-parsed YAML document. Called AFTER ogs_app_initialize().
 */
static void load_gnb_config_from_yaml(void)
{
    yaml_document_t *document = ogs_app()->document;
    ogs_yaml_iter_t root_iter;

    if (!document) return;

    ogs_yaml_iter_init(&root_iter, document);
    while (ogs_yaml_iter_next(&root_iter)) {
        const char *root_key = ogs_yaml_iter_key(&root_iter);
        if (!root_key || strcmp(root_key, "container_test") != 0)
            continue;

        ogs_yaml_iter_t ct_iter;
        ogs_yaml_iter_recurse(&root_iter, &ct_iter);
        while (ogs_yaml_iter_next(&ct_iter)) {
            const char *k = ogs_yaml_iter_key(&ct_iter);
            const char *v = ogs_yaml_iter_value(&ct_iter);
            if (!k || !v) continue;

            if (!strcmp(k, "gnb1_advertised_addr"))
                ogs_cpystrn(g_gnb_cfg.gnb1_advertised_addr, v,
                        sizeof(g_gnb_cfg.gnb1_advertised_addr));
            else if (!strcmp(k, "gnb1_advertised_port"))
                g_gnb_cfg.gnb1_advertised_port = atoi(v);
            else if (!strcmp(k, "gnb2_advertised_addr"))
                ogs_cpystrn(g_gnb_cfg.gnb2_advertised_addr, v,
                        sizeof(g_gnb_cfg.gnb2_advertised_addr));
            else if (!strcmp(k, "gnb2_advertised_port"))
                g_gnb_cfg.gnb2_advertised_port = atoi(v);
        }
        break;
    }
}

/* Suite declaration */
abts_suite *test_n2_handover_hr_generic(abts_suite *suite);

const struct testlist {
    abts_suite *(*func)(abts_suite *suite);
} alltests[] = {
    {test_n2_handover_hr_generic},
    {NULL},
};

static void terminate(void)
{
    ogs_msleep(50);

    /* No NF child processes to terminate — they're in Docker containers.
     * We only finalize the local test framework resources. */
    test_5gc_final();

    ogs_app_terminate();
}

static int test_udm_context_parse_config(void)
{
    int rv;
    yaml_document_t *document = NULL;
    ogs_yaml_iter_t root_iter;

    document = ogs_app()->document;
    ogs_assert(document);

    ogs_yaml_iter_init(&root_iter, document);
    while (ogs_yaml_iter_next(&root_iter)) {
        const char *root_key = ogs_yaml_iter_key(&root_iter);
        ogs_assert(root_key);
        if (!strcmp(root_key, "udm")) {
            ogs_yaml_iter_t udm_iter;
            ogs_yaml_iter_recurse(&root_iter, &udm_iter);
            while (ogs_yaml_iter_next(&udm_iter)) {
                const char *udm_key = ogs_yaml_iter_key(&udm_iter);
                ogs_assert(udm_key);
                if (!strcmp(udm_key, "sbi")) {
                    /* handle config in sbi library */
                } else if (!strcmp(udm_key, "service_name")) {
                    /* handle config in sbi library */
                } else if (!strcmp(udm_key, "discovery")) {
                    /* handle config in sbi library */
                } else if (!strcmp(udm_key, "hnet")) {
                    rv = ogs_sbi_context_parse_hnet_config(&udm_iter);
                    if (rv != OGS_OK) return rv;
                } else
                    ogs_warn("unknown key `%s`", udm_key);
            }
        }
    }

    return OGS_OK;
}

/*
 * Override the gNB GTP-U advertised addresses.
 *
 * test_context_init() hardcodes gnb1_addr=127.0.0.2:2152 and
 * gnb2_addr=127.0.0.3:2152, which are only valid for loopback.
 * In the Docker deployment, the UPF containers need to reach our
 * GTP-U server, so we replace these with Docker-routable addresses.
 *
 * This is called AFTER test_app_run() which calls test_context_init().
 */
static void override_gnb_addresses(void)
{
    int rv;

    ogs_info("[CONTAINER] Overriding gNB GTP-U advertised addresses:");
    ogs_info("[CONTAINER]   gnb1: %s:%d", g_gnb_cfg.gnb1_advertised_addr,
            g_gnb_cfg.gnb1_advertised_port);
    ogs_info("[CONTAINER]   gnb2: %s:%d", g_gnb_cfg.gnb2_advertised_addr,
            g_gnb_cfg.gnb2_advertised_port);

    /* Free the hardcoded loopback addresses */
    if (test_self()->gnb1_addr)
        ogs_freeaddrinfo(test_self()->gnb1_addr);
    if (test_self()->gnb1_addr6)
        ogs_freeaddrinfo(test_self()->gnb1_addr6);
    if (test_self()->gnb2_addr)
        ogs_freeaddrinfo(test_self()->gnb2_addr);
    if (test_self()->gnb2_addr6)
        ogs_freeaddrinfo(test_self()->gnb2_addr6);

    /* Set the container-reachable addresses */
    rv = ogs_getaddrinfo(&test_self()->gnb1_addr, AF_UNSPEC,
            g_gnb_cfg.gnb1_advertised_addr, g_gnb_cfg.gnb1_advertised_port, 0);
    ogs_assert(rv == OGS_OK);

    rv = ogs_getaddrinfo(&test_self()->gnb2_addr, AF_UNSPEC,
            g_gnb_cfg.gnb2_advertised_addr, g_gnb_cfg.gnb2_advertised_port, 0);
    ogs_assert(rv == OGS_OK);

    /* Clear IPv6 — container bridge is IPv4 only */
    test_self()->gnb1_addr6 = NULL;
    test_self()->gnb2_addr6 = NULL;

    ogs_info("[CONTAINER] gNB address override complete");
}

static void initialize(const char *const argv[])
{
    int rv;

    rv = ogs_app_initialize(NULL, NULL, argv);
    ogs_assert(rv == OGS_OK);

    test_5gc_init();

    ogs_assert(OGS_OK == test_udm_context_parse_config());

    /* ────── KEY DIFFERENCE FROM STANDARD abts-main.c ──────
     *
     * We do NOT call app_initialize(argv) here.
     * app_initialize() spawns NRF, SCP, SEPP, AMF, SMF, UPF, etc.
     * as child processes from the test binary.
     *
     * In the container variant, those NFs are already running
     * in Docker containers. We only need the test framework
     * (SCTP, MongoDB, test_context) to be initialized.
     *
     * ──────────────────────────────────────────────────────── */

    ogs_info("[CONTAINER] ========================================");
    ogs_info("[CONTAINER] Container test mode — NF spawning SKIPPED");
    ogs_info("[CONTAINER] NFs expected to be running in Docker");
    ogs_info("[CONTAINER] ========================================");
}

int main(int argc, const char *const argv[])
{
    int i;
    abts_suite *suite = NULL;

    atexit(terminate);
    test_app_run(argc, argv, "container.yaml", initialize);

    /* Load gNB advertised addresses from YAML before overriding */
    load_gnb_config_from_yaml();

    /* Override gNB GTP-U addresses AFTER test_app_run has called
     * test_context_init() (which hardcodes 127.0.0.x addresses) */
    override_gnb_addresses();

    for (i = 0; alltests[i].func; i++)
        suite = alltests[i].func(suite);

    return abts_report(suite);
}
