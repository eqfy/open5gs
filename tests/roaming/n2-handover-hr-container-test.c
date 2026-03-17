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
 * N2 HANDOVER HR TEST — CONTAINER DEPLOYMENT VARIANT
 *
 * This test validates the same inter-PLMN N2 handover (Home-Routed,
 * V-SMF insertion per TS 23.502 §4.9.1.3 + §4.23.7.3) as the original
 * n2-handover-hr-test.c, but adapted for a Docker Compose deployment
 * where NFs run in separate containers on a Docker bridge network
 * instead of all on loopback addresses in-process.
 *
 * ───────────────────────────────────────────────────────────────────
 * DESIGN NOTE — KEY DIFFERENCES FROM THE LOOPBACK VERSION
 * ───────────────────────────────────────────────────────────────────
 *
 * 1. NO IN-PROCESS NF SPAWNING
 *    The loopback test uses app_initialize() to spawn NRF, SCP, AMF,
 *    SMF, UPF, SEPP, etc. as child threads from the same binary.
 *    This container variant does NOT spawn NFs. The NFs are already
 *    running as Docker containers. The test binary only acts as the
 *    gNB / UE simulator, connecting to the already-running AMFs and
 *    sending/receiving GTP-U to/from the already-running UPFs.
 *
 * 2. NGAP CONNECTION TARGETS
 *    Original: testngap_client(1) → test_self()->ngap_addr (from YAML
 *    amf.ngap.server[0], e.g. 127.0.1.5:38412 or 127.0.0.5:38412)
 *    Container: We override test_self()->ngap_addr / ngap2_addr with
 *    the Docker DNS names / IPs of h-amf and v-amf respectively.
 *    The SCTP connection then goes to the container's NGAP listener.
 *
 * 3. GTP-U SOURCE BIND
 *    Original: test_gtpu_server(1) binds 127.0.0.2:2152, (2) 127.0.0.3
 *    Container: We override test_self()->gnb1_addr / gnb2_addr with a
 *    local IP that is routable from the Docker bridge network (e.g.
 *    the test host's interface on 10.33.33.0/24, or we run the test
 *    binary inside a container on that bridge network).
 *
 * 4. PLMN CONFIGURATION
 *    Original: Reads test.serving[] from sample.yaml which provides
 *    PLMN IDs used for NG-Setup. Container: We still need this config
 *    so we create a custom YAML or set it programmatically after init.
 *
 * 5. DATABASE
 *    Original: test_db_* connects to MongoDB at db_uri from YAML
 *    (mongodb://localhost/open5gs). Container: The db container is on
 *    the Docker network, so db_uri should point to it by DNS name or
 *    IP (mongodb://db.open5gs.org/open5gs or mongodb://10.33.33.X/open5gs).
 *
 * 6. SCTP SOURCE ADDRESS
 *    testngap_client() binds the SCTP socket to TEST_GNB1_IPV4 /
 *    TEST_GNB2_IPV4 for source differentiation. In the container env,
 *    these must be routable addresses on the Docker bridge. If the test
 *    runs inside a container with a single IP, we skip source binding
 *    (both gNBs share the container's IP; the AMF distinguishes by
 *    gNB-ID in NG-Setup, not by source IP).
 *
 * DEPLOYMENT ASSUMPTIONS:
 *   - Docker bridge network: open5gs (10.33.33.0/24)
 *   - The test binary runs inside a container on the same network
 *     (or on the Docker host with br-ogs bridge access)
 *   - h-amf: amf.5gc.mnc001.mcc001.3gppnetwork.org:38412
 *   - v-amf: amf.5gc.mnc070.mcc999.3gppnetwork.org:38412
 *   - h-upf: upf.5gc.mnc001.mcc001.3gppnetwork.org:2152
 *   - v-upf: upf.5gc.mnc070.mcc999.3gppnetwork.org:2152
 *   - MongoDB: db.open5gs.org:27017
 *
 * ───────────────────────────────────────────────────────────────────
 */

#include "test-common.h"

/* ═══════════════════════════════════════════════════════════════════
 * CONTAINER ENDPOINT CONFIGURATION
 *
 * Override these macros to match your Docker Compose deployment.
 * They can also be overridden at compile time with -D flags:
 *   meson ... -Dc_args='-DHOME_AMF_HOST="10.33.33.10"'
 * ═══════════════════════════════════════════════════════════════════ */

/* ── Home PLMN (MCC=001, MNC=01) ── */
#ifndef HOME_AMF_HOST
#define HOME_AMF_HOST      "amf.5gc.mnc001.mcc001.3gppnetwork.org"
#endif
#ifndef HOME_AMF_PORT
#define HOME_AMF_PORT       OGS_NGAP_SCTP_PORT    /* 38412 */
#endif

/* ── Visiting PLMN (MCC=999, MNC=70) ── */
#ifndef VISITING_AMF_HOST
#define VISITING_AMF_HOST  "amf.5gc.mnc070.mcc999.3gppnetwork.org"
#endif
#ifndef VISITING_AMF_PORT
#define VISITING_AMF_PORT   OGS_NGAP_SCTP_PORT    /* 38412 */
#endif

/* ── GTP-U simulated gNB bind addresses ──
 * These are the local IPs the test process binds for GTP-U.
 * Must be reachable from the UPF containers (on the Docker bridge).
 * If running inside a Docker container on the bridge, use "0.0.0.0"
 * or the container's assigned IP (e.g. 10.33.33.200). */
#ifndef HOME_GTPU_BIND_ADDR
#define HOME_GTPU_BIND_ADDR   "10.33.33.50"
#endif
#ifndef VISITING_GTPU_BIND_ADDR
#define VISITING_GTPU_BIND_ADDR  "10.33.33.51"
#endif
#ifndef HOME_GTPU_BIND_PORT
#define HOME_GTPU_BIND_PORT    OGS_GTPV1_U_UDP_PORT   /* 2152 */
#endif
#ifndef VISITING_GTPU_BIND_PORT
#define VISITING_GTPU_BIND_PORT  OGS_GTPV1_U_UDP_PORT   /* Different port for 2nd gNB */
#endif

/* ── SCTP source bind for simulated gNBs ──
 * If the test host/container only has a single IP, set both to "0.0.0.0"
 * or the same IP. The AMF differentiates gNBs by gNB-ID, not source IP. */
#ifndef GNB1_SCTP_BIND_ADDR
#define GNB1_SCTP_BIND_ADDR   "10.33.33.50"
#endif
#ifndef GNB2_SCTP_BIND_ADDR
#define GNB2_SCTP_BIND_ADDR   "10.33.33.51"
#endif

/* ── SEPP hostnames (informational, not directly used by gNB) ── */
#ifndef HOME_SEPP_HOST
#define HOME_SEPP_HOST    "sepp.5gc.mnc001.mcc001.3gppnetwork.org"
#endif
#ifndef VISITING_SEPP_HOST
#define VISITING_SEPP_HOST "sepp.5gc.mnc070.mcc999.3gppnetwork.org"
#endif

/* ── MongoDB ── */
#ifndef DB_URI_CONTAINER
#define DB_URI_CONTAINER   "mongodb://db.open5gs.org/open5gs"
#endif

/* ── Home PLMN identity ── */
#ifndef HOME_MCC
#define HOME_MCC   1     /* MCC 001 */
#endif
#ifndef HOME_MNC
#define HOME_MNC   1     /* MNC 01 */
#endif
#ifndef HOME_MNC_LEN
#define HOME_MNC_LEN  2
#endif

/* ── Visiting PLMN identity ── */
#ifndef VISITING_MCC
#define VISITING_MCC   999
#endif
#ifndef VISITING_MNC
#define VISITING_MNC   70
#endif
#ifndef VISITING_MNC_LEN
#define VISITING_MNC_LEN  2
#endif

/* ── Ping target for data plane verification ── */
#ifndef CONTAINER_PING_IPV4
#define CONTAINER_PING_IPV4  "8.8.8.8"   /* H-UPF gateway in HR */
#endif

/* Phase-level timing macros (identical to original) */
#define TIMING_PHASE_START(t) do { (t) = ogs_get_monotonic_time(); } while (0)
#define TIMING_PHASE_END(tag, name, t) \
    ogs_info("[%s][TIMING] %s: %lld ms", tag, name, \
            (long long)(ogs_get_monotonic_time() - (t)) / 1000)
#define TIMING_TOTAL(tag, t) \
    ogs_info("[%s][TIMING] Total: %lld ms", tag, \
            (long long)(ogs_get_monotonic_time() - (t)) / 1000)

/* ═══════════════════════════════════════════════════════════════════
 * CONTAINER-AWARE SOCKET HELPERS
 *
 * Mirrors testngap_client() and test_gtpu_server() from the framework,
 * but resolves Docker DNS hostnames instead of copying from test_self().
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * Create an SCTP client connection to a remote AMF container.
 * Same structure as testngap_client() — uses ogs_getaddrinfo()
 * to resolve the AMF hostname instead of ogs_copyaddrinfo().
 */
static ogs_socknode_t *container_ngap_client(
        const char *amf_host, int amf_port, const char *bind_ip)
{
    int rv;
    ogs_sockaddr_t *addr = NULL;
    ogs_sockaddr_t *bind_addr = NULL;
    ogs_socknode_t *node = NULL;
    ogs_sock_t *sock = NULL;

    rv = ogs_getaddrinfo(&addr, AF_UNSPEC, amf_host, amf_port, 0);
    ogs_assert(rv == OGS_OK);
    ogs_assert(addr);

    /* Bind to distinct gNB source IPs so pcap shows separate gNBs */
    rv = ogs_getaddrinfo(&bind_addr, AF_UNSPEC, bind_ip, 0, 0);
    ogs_assert(rv == OGS_OK);

    node = ogs_socknode_new(addr);
    ogs_assert(node);

    sock = ogs_sctp_client(SOCK_STREAM, node->addr, bind_addr, NULL);
    ogs_assert(sock);

    ogs_freeaddrinfo(bind_addr);

    node->sock = sock;
    node->cleanup = ogs_sctp_destroy;

    return node;
}

/*
 * Create a GTP-U UDP server socket for receiving downlink packets from UPF.
 * Same structure as test_gtpu_server() — uses ogs_getaddrinfo()
 * to resolve the bind address instead of ogs_copyaddrinfo().
 */
static ogs_socknode_t *container_gtpu_server(
        const char *bind_ip, int bind_port)
{
    int rv;
    ogs_sockaddr_t *addr = NULL;
    ogs_socknode_t *node = NULL;
    ogs_sock_t *sock = NULL;

    rv = ogs_getaddrinfo(&addr, AF_UNSPEC, bind_ip, bind_port, 0);
    ogs_assert(rv == OGS_OK);

    node = ogs_socknode_new(addr);
    ogs_assert(node);

    sock = ogs_udp_server(node->addr, NULL);
    ogs_assert(sock);

    node->sock = sock;

    return node;
}

/* ═══════════════════════════════════════════════════════════════════
 * PLMN CONTEXT HELPERS (same logic as original)
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * Helper function to switch PLMN context for NG-Setup and NAS signaling.
 * Updates all PLMN-related fields that affect NG-Setup Request.
 *
 * @param plmn_index: Index into ogs_local_conf()->serving_plmn_id[] array
 *                    0 = Home PLMN, 1 = Visiting PLMN
 */
static void switch_plmn_context(int plmn_index)
{
    ogs_plmn_id_t *plmn_id;

    ogs_assert(plmn_index < ogs_local_conf()->num_of_serving_plmn_id);
    plmn_id = &ogs_local_conf()->serving_plmn_id[plmn_index];

    memcpy(&test_self()->plmn_support[0].plmn_id, plmn_id, OGS_PLMN_ID_LEN);
    memcpy(&test_self()->nr_tai.plmn_id, plmn_id, OGS_PLMN_ID_LEN);
    memcpy(&test_self()->nr_cgi.plmn_id, plmn_id, OGS_PLMN_ID_LEN);
    memcpy(&test_self()->nr_served_tai[0].list0.tai[0].plmn_id,
           plmn_id, OGS_PLMN_ID_LEN);
}

/* ═══════════════════════════════════════════════════════════════════
 * UE / SESSION HELPERS (identical to original — pure NAS building)
 * ═══════════════════════════════════════════════════════════════════ */

static void setup_mobile_identity_suci(
        ogs_nas_5gs_mobile_identity_suci_t *suci)
{
    ogs_assert(suci);
    memset(suci, 0, sizeof(*suci));
    suci->h.supi_format = OGS_NAS_5GS_SUPI_FORMAT_IMSI;
    suci->h.type = OGS_NAS_5GS_MOBILE_IDENTITY_SUCI;
    suci->routing_indicator1 = 0;
    suci->routing_indicator2 = 0xf;
    suci->routing_indicator3 = 0xf;
    suci->routing_indicator4 = 0xf;
    suci->protection_scheme_id = OGS_PROTECTION_SCHEME_NULL;
    suci->home_network_pki_value = 0; 
}

static test_ue_t *create_test_ue(const char *imsi)
{
    ogs_nas_5gs_mobile_identity_suci_t mobile_identity_suci;
    test_ue_t *test_ue = NULL;

    setup_mobile_identity_suci(&mobile_identity_suci);

    test_ue = test_ue_add_by_suci(&mobile_identity_suci, imsi);
    ogs_assert(test_ue);

    test_ue->nr_cgi.cell_id = 0x40001;
    test_ue->nas.registration.tsc = 0;
    test_ue->nas.registration.ksi = OGS_NAS_KSI_NO_KEY_IS_AVAILABLE;
    test_ue->nas.registration.follow_on_request = 1;
    test_ue->nas.registration.value = OGS_NAS_5GS_REGISTRATION_TYPE_INITIAL;
    test_ue->k_string = "465b5ce8b199b49faa5f0a2ee238a6bc";
    test_ue->opc_string = "e8ed289deba952e4283b54e88e6183ca";

    /* HR test: subscriber uses home-routed roaming */
    test_ue->lbo_roaming_allowed = false;

    return test_ue;
}

/* ═══════════════════════════════════════════════════════════════════
 * SUBSCRIBER DB DOCUMENT WITH S-NSSAI (SST=1, SD=000001)
 *
 * The standard test_db_new_simple() does not include an SD field in
 * the slice definition.  Our Docker deployment uses sst:1 sd:000001,
 * so we provide a custom BSON document builder that mirrors
 * test_db_new_simple() but adds the SD.
 * ═══════════════════════════════════════════════════════════════════ */

static bson_t *test_db_new_simple_with_sd(test_ue_t *test_ue)
{
    bson_t *doc = NULL;

    ogs_assert(test_ue);

    doc = BCON_NEW(
            "imsi", BCON_UTF8(test_ue->imsi),
            "msisdn", "[",
                BCON_UTF8(TEST_MSISDN),
                BCON_UTF8(TEST_ADDITIONAL_MSISDN),
            "]",
            "ambr", "{",
                "downlink", "{",
                    "value", BCON_INT32(1),
                    "unit", BCON_INT32(3),
                "}",
                "uplink", "{",
                    "value", BCON_INT32(1),
                    "unit", BCON_INT32(3),
                "}",
            "}",
            "slice", "[", "{",
                "sst", BCON_INT32(1),
                "sd", BCON_UTF8("000001"),
                "default_indicator", BCON_BOOL(true),
                "session", "[", "{",
                    "name", BCON_UTF8("internet"),
                    "type", BCON_INT32(3),
                    "ambr", "{",
                        "downlink", "{",
                            "value", BCON_INT32(1),
                            "unit", BCON_INT32(3),
                        "}",
                        "uplink", "{",
                            "value", BCON_INT32(1),
                            "unit", BCON_INT32(3),
                        "}",
                    "}",
                    "qos", "{",
                        "index", BCON_INT32(9),
                        "arp", "{",
                            "priority_level", BCON_INT32(8),
                            "pre_emption_vulnerability", BCON_INT32(1),
                            "pre_emption_capability", BCON_INT32(1),
                        "}",
                    "}",
                    "lbo_roaming_allowed", BCON_BOOL(
                            test_ue->lbo_roaming_allowed),
                "}", "]",
            "}", "]",
            "security", "{",
                "k", BCON_UTF8(test_ue->k_string),
                "opc", BCON_UTF8(test_ue->opc_string),
                "amf", BCON_UTF8("8000"),
                "sqn", BCON_INT64(64),
            "}",
            "subscribed_rau_tau_timer", BCON_INT32(12),
            "network_access_mode", BCON_INT32(0),
            "subscriber_status", BCON_INT32(0),
            "operator_determined_barring", BCON_INT32(0),
            "access_restriction_data", BCON_INT32(32)
          );
    ogs_assert(doc);

    return doc;
}

/* ═══════════════════════════════════════════════════════════════════
 * NG-SETUP / REGISTRATION / SESSION HELPERS
 *
 * These use the standard testngap_build_* / testgmm_build_* functions
 * which are purely message-construction — they do not depend on NF
 * addresses. The socket I/O goes through the socknode_t returned by
 * container_ngap_client() above, so it reaches the container AMF.
 * ═══════════════════════════════════════════════════════════════════ */

static void perform_ng_setup(abts_case *tc, test_ue_t *test_ue,
        ogs_socknode_t *ngap, uint32_t gnb_id, uint32_t tac)
{
    int rv;
    ogs_pkbuf_t *sendbuf, *recvbuf;

    sendbuf = testngap_build_ng_setup_request(gnb_id, tac);
    ABTS_PTR_NOTNULL(tc, sendbuf);
    rv = testgnb_ngap_send(ngap, sendbuf);
    ABTS_INT_EQUAL(tc, OGS_OK, rv);

    recvbuf = testgnb_ngap_read(ngap);
    ABTS_PTR_NOTNULL(tc, recvbuf);
    testngap_recv(test_ue, recvbuf);
}

static void wait_for_ue_context_release_on_source(abts_case *tc,
        test_ue_t *test_ue, ogs_socknode_t *ngap_home,
        const char *label)
{
    ogs_pkbuf_t *recvbuf;

    recvbuf = testgnb_ngap_read(ngap_home);
    ABTS_PTR_NOTNULL(tc, recvbuf);
    testngap_recv(test_ue, recvbuf);

    if (test_ue->ngap_procedure_code !=
            NGAP_ProcedureCode_id_UEContextRelease) {
        ogs_info("[%s] Consumed extra NGAP msg (proc=%ld) before "
                "UEContextReleaseCommand",
                label, (long)test_ue->ngap_procedure_code);
        recvbuf = testgnb_ngap_read(ngap_home);
        ABTS_PTR_NOTNULL(tc, recvbuf);
        testngap_recv(test_ue, recvbuf);
    }

    ABTS_INT_EQUAL(tc, NGAP_ProcedureCode_id_UEContextRelease,
            test_ue->ngap_procedure_code);
}

static void perform_full_registration(abts_case *tc, test_ue_t *test_ue,
        ogs_socknode_t *ngap)
{
    int rv;
    ogs_pkbuf_t *gmmbuf, *nasbuf, *sendbuf, *recvbuf;

    /* Registration Request */
    test_ue->registration_request_param.guti = 1;
    gmmbuf = testgmm_build_registration_request(test_ue, NULL, false, false);
    ABTS_PTR_NOTNULL(tc, gmmbuf);

    test_ue->registration_request_param.gmm_capability = 1;
    test_ue->registration_request_param.s1_ue_network_capability = 1;
    test_ue->registration_request_param.requested_nssai = 1;
    test_ue->registration_request_param.last_visited_registered_tai = 1;
    test_ue->registration_request_param.ue_usage_setting = 1;
    nasbuf = testgmm_build_registration_request(test_ue, NULL, false, false);
    ABTS_PTR_NOTNULL(tc, nasbuf);

    sendbuf = testngap_build_initial_ue_message(test_ue, gmmbuf,
                NGAP_RRCEstablishmentCause_mo_Signalling, false, true);
    ABTS_PTR_NOTNULL(tc, sendbuf);
    rv = testgnb_ngap_send(ngap, sendbuf);
    ABTS_INT_EQUAL(tc, OGS_OK, rv);

    /* Identity Request/Response */
    recvbuf = testgnb_ngap_read(ngap);
    ABTS_PTR_NOTNULL(tc, recvbuf);
    testngap_recv(test_ue, recvbuf);

    gmmbuf = testgmm_build_identity_response(test_ue);
    ABTS_PTR_NOTNULL(tc, gmmbuf);
    sendbuf = testngap_build_uplink_nas_transport(test_ue, gmmbuf);
    ABTS_PTR_NOTNULL(tc, sendbuf);
    rv = testgnb_ngap_send(ngap, sendbuf);
    ABTS_INT_EQUAL(tc, OGS_OK, rv);

    /* Authentication Request/Response */
    recvbuf = testgnb_ngap_read(ngap);
    ABTS_PTR_NOTNULL(tc, recvbuf);
    testngap_recv(test_ue, recvbuf);

    gmmbuf = testgmm_build_authentication_response(test_ue);
    ABTS_PTR_NOTNULL(tc, gmmbuf);
    sendbuf = testngap_build_uplink_nas_transport(test_ue, gmmbuf);
    ABTS_PTR_NOTNULL(tc, sendbuf);
    rv = testgnb_ngap_send(ngap, sendbuf);
    ABTS_INT_EQUAL(tc, OGS_OK, rv);

    /* Security Mode Command/Complete */
    recvbuf = testgnb_ngap_read(ngap);
    ABTS_PTR_NOTNULL(tc, recvbuf);
    testngap_recv(test_ue, recvbuf);

    gmmbuf = testgmm_build_security_mode_complete(test_ue, nasbuf);
    ABTS_PTR_NOTNULL(tc, gmmbuf);
    sendbuf = testngap_build_uplink_nas_transport(test_ue, gmmbuf);
    ABTS_PTR_NOTNULL(tc, sendbuf);
    rv = testgnb_ngap_send(ngap, sendbuf);
    ABTS_INT_EQUAL(tc, OGS_OK, rv);

    /* Initial Context Setup */
    recvbuf = testgnb_ngap_read(ngap);
    ABTS_PTR_NOTNULL(tc, recvbuf);
    testngap_recv(test_ue, recvbuf);

    sendbuf = testngap_build_ue_radio_capability_info_indication(test_ue);
    ABTS_PTR_NOTNULL(tc, sendbuf);
    rv = testgnb_ngap_send(ngap, sendbuf);
    ABTS_INT_EQUAL(tc, OGS_OK, rv);

    sendbuf = testngap_build_initial_context_setup_response(test_ue, false);
    ABTS_PTR_NOTNULL(tc, sendbuf);
    rv = testgnb_ngap_send(ngap, sendbuf);
    ABTS_INT_EQUAL(tc, OGS_OK, rv);

    /* Registration Complete */
    gmmbuf = testgmm_build_registration_complete(test_ue);
    ABTS_PTR_NOTNULL(tc, gmmbuf);
    sendbuf = testngap_build_uplink_nas_transport(test_ue, gmmbuf);
    ABTS_PTR_NOTNULL(tc, sendbuf);
    rv = testgnb_ngap_send(ngap, sendbuf);
    ABTS_INT_EQUAL(tc, OGS_OK, rv);

    /* Configuration Update Command */
    recvbuf = testgnb_ngap_read(ngap);
    ABTS_PTR_NOTNULL(tc, recvbuf);
    testngap_recv(test_ue, recvbuf);
}

static test_sess_t *establish_pdu_session(abts_case *tc, test_ue_t *test_ue,
        ogs_socknode_t *ngap, const char *dnn, uint8_t psi)
{
    int rv;
    ogs_pkbuf_t *gsmbuf, *gmmbuf, *sendbuf, *recvbuf;
    test_sess_t *sess;

    sess = test_sess_add_by_dnn_and_psi(test_ue, dnn, psi);
    ogs_assert(sess);

    sess->ul_nas_transport_param.request_type =
        OGS_NAS_5GS_REQUEST_TYPE_INITIAL;
    sess->ul_nas_transport_param.dnn = 1;
    sess->ul_nas_transport_param.s_nssai = 1;
    sess->pdu_session_establishment_param.ssc_mode = 1;
    sess->pdu_session_establishment_param.epco = 0;

    gsmbuf = testgsm_build_pdu_session_establishment_request(sess);
    ABTS_PTR_NOTNULL(tc, gsmbuf);
    gmmbuf = testgmm_build_ul_nas_transport(sess,
            OGS_NAS_PAYLOAD_CONTAINER_N1_SM_INFORMATION, gsmbuf);
    ABTS_PTR_NOTNULL(tc, gmmbuf);
    sendbuf = testngap_build_uplink_nas_transport(test_ue, gmmbuf);
    ABTS_PTR_NOTNULL(tc, sendbuf);
    rv = testgnb_ngap_send(ngap, sendbuf);
    ABTS_INT_EQUAL(tc, OGS_OK, rv);

    recvbuf = testgnb_ngap_read(ngap);
    ABTS_PTR_NOTNULL(tc, recvbuf);
    testngap_recv(test_ue, recvbuf);

    sendbuf = testngap_sess_build_pdu_session_resource_setup_response(sess);
    ABTS_PTR_NOTNULL(tc, sendbuf);
    rv = testgnb_ngap_send(ngap, sendbuf);
    ABTS_INT_EQUAL(tc, OGS_OK, rv);

    return sess;
}

static void verify_gtpu_post_handover(abts_case *tc,
        ogs_socknode_t *gtpu, test_sess_t *sess, const char *label)
{
        int rv;
        ogs_pkbuf_t *recvbuf;
        test_bearer_t *qos_flow;

        qos_flow = test_qos_flow_find_by_qfi(sess, 1);
        ogs_assert(qos_flow);

        ogs_info("[%s] Verifying GTP-U data path post-handover", label);

                rv = test_gtpu_send_ping(gtpu, qos_flow, CONTAINER_PING_IPV4);
                ABTS_INT_EQUAL(tc, OGS_OK, rv);

                recvbuf = testgnb_gtpu_read(gtpu);
                ABTS_PTR_NOTNULL(tc, recvbuf);
                ogs_pkbuf_free(recvbuf);

    rv = test_gtpu_send_ping(gtpu, qos_flow, CONTAINER_PING_IPV4);
    ABTS_INT_EQUAL(tc, OGS_OK, rv);

    recvbuf = testgnb_gtpu_read(gtpu);
    ABTS_PTR_NOTNULL(tc, recvbuf);
                                ogs_pkbuf_free(recvbuf);

    ogs_info("[%s] GTP-U data path verified", label);
}

/* ═══════════════════════════════════════════════════════════════════
 * TEST 1: Basic HR Inter-PLMN N2 Handover (Container)
 *
 * Same handover logic as the loopback test1_func, but using
 * container_ngap_client() to connect to Docker AMF containers
 * and container_gtpu_server() for GTP-U.
 *
 * 3GPP: TS 23.502 §4.9.1.3.2, §4.23.7.3
 * ═══════════════════════════════════════════════════════════════════ */
static void test1_hr_container_func(abts_case *tc, void *data)
{
    int rv;
    ogs_socknode_t *ngap_home, *ngap_visiting;
    ogs_socknode_t *gtpu_home, *gtpu_visiting;
    ogs_pkbuf_t *sendbuf;
    ogs_pkbuf_t *recvbuf;

    test_ue_t *test_ue = NULL;
    test_sess_t *sess = NULL;
    test_bearer_t *qos_flow = NULL;

    bson_t *doc = NULL;

    ogs_time_t t_total, t_phase;

    ogs_assert(ogs_local_conf()->num_of_serving_plmn_id >= 2);

    TIMING_PHASE_START(t_total);

    /**************************************************************************
     * PHASE 0: SETUP — CONNECT TO CONTAINER AMFs, BIND GTP-U
     **************************************************************************/

    TIMING_PHASE_START(t_phase);
    ogs_info("[HR-CT1] ========================================");
    ogs_info("[HR-CT1] Phase 0: Container infrastructure setup");
    ogs_info("[HR-CT1] ========================================");

    /* Connect to Home AMF (h-amf container) */
    ogs_info("[HR-CT1] Connecting to Home AMF: %s:%d",
            HOME_AMF_HOST, HOME_AMF_PORT);
    ngap_home = container_ngap_client(
            HOME_AMF_HOST, HOME_AMF_PORT, GNB1_SCTP_BIND_ADDR);
    ABTS_PTR_NOTNULL(tc, ngap_home);

    /* Bind GTP-U for home gNB */
    gtpu_home = container_gtpu_server(
            HOME_GTPU_BIND_ADDR, HOME_GTPU_BIND_PORT);
    ABTS_PTR_NOTNULL(tc, gtpu_home);

    /* Connect to Visiting AMF (v-amf container) */
    ogs_info("[HR-CT1] Connecting to Visiting AMF: %s:%d",
            VISITING_AMF_HOST, VISITING_AMF_PORT);
    ngap_visiting = container_ngap_client(
            VISITING_AMF_HOST, VISITING_AMF_PORT, GNB2_SCTP_BIND_ADDR);
    ABTS_PTR_NOTNULL(tc, ngap_visiting);

    /* Bind GTP-U for visiting gNB */
    gtpu_visiting = container_gtpu_server(
            VISITING_GTPU_BIND_ADDR, VISITING_GTPU_BIND_PORT);
    ABTS_PTR_NOTNULL(tc, gtpu_visiting);

    /* Create test UE (lbo_roaming_allowed=false for HR) */
    test_ue = create_test_ue("0000203191");
    doc = test_db_new_simple_with_sd(test_ue);
    ABTS_PTR_NOTNULL(tc, doc);
    ABTS_INT_EQUAL(tc, OGS_OK, test_db_insert_ue(test_ue, doc));

    /* NG-Setup for Home gNB → h-amf */
    switch_plmn_context(0);
    ogs_info("[HR-CT1] NG-Setup for Home gNB 0x4000 → h-amf");
    perform_ng_setup(tc, test_ue, ngap_home, 0x4000, 22);
    ogs_info("[HR-CT1] Home gNB 0x4000 connected to h-amf");

    /* NG-Setup for Visiting gNB → v-amf */
    switch_plmn_context(1);
    ogs_info("[HR-CT1] NG-Setup for Visiting gNB 0x4001 → v-amf");
    sendbuf = testngap_build_ng_setup_request(0x4001, 22);
    ABTS_PTR_NOTNULL(tc, sendbuf);
    rv = testgnb_ngap_send(ngap_visiting, sendbuf);
    ABTS_INT_EQUAL(tc, OGS_OK, rv);

    recvbuf = testgnb_ngap_read(ngap_visiting);
    ABTS_PTR_NOTNULL(tc, recvbuf);
    testngap_recv(test_ue, recvbuf);

    /* Switch back to home PLMN for UE registration */
    switch_plmn_context(0);

    TIMING_PHASE_END("HR-CT1", "Phase 0 (setup)", t_phase);

    /**************************************************************************
     * PHASE 1: REGISTER AND ESTABLISH SESSION IN HOME NETWORK
     **************************************************************************/

    TIMING_PHASE_START(t_phase);
    ogs_info("[HR-CT1] ========================================");
    ogs_info("[HR-CT1] Phase 1: Home network registration");
    ogs_info("[HR-CT1] ========================================");

    perform_full_registration(tc, test_ue, ngap_home);

    sess = establish_pdu_session(tc, test_ue, ngap_home, "internet", 5);
    ogs_info("[HR-CT1] Waiting for PDU session to stabilize...");
    ogs_msleep(300);   // Delay in milliseconds (1 second)
    
    /* Verify data path in home network */
    qos_flow = test_qos_flow_find_by_qfi(sess, 1);
    ogs_assert(qos_flow);

    rv = test_gtpu_send_ping(gtpu_home, qos_flow, CONTAINER_PING_IPV4);
    ABTS_INT_EQUAL(tc, OGS_OK, rv);

    recvbuf = testgnb_gtpu_read(gtpu_home);
    ABTS_PTR_NOTNULL(tc, recvbuf);
    ogs_pkbuf_free(recvbuf);

    ogs_info("[HR-CT1] Phase 1 complete - UE registered with active session");
    TIMING_PHASE_END("HR-CT1", "Phase 1 (registration)", t_phase);

    /**************************************************************************
     * PHASE 2: INTER-PLMN N2 HANDOVER WITH V-SMF INSERTION
     *
     * Same message flow as loopback test. The server-side processing
     * happens inside the Docker containers:
     *   S-AMF(h-amf) → SEPP → T-AMF(v-amf) → V-SMF → H-SMF → V-UPF
     **************************************************************************/

    TIMING_PHASE_START(t_phase);
    ogs_info("[HR-CT1] ========================================");
    ogs_info("[HR-CT1] Phase 2: Inter-PLMN N2 handover (HR)");
    ogs_info("[HR-CT1] ========================================");

    ogs_plmn_id_t target_plmn;
    ogs_5gs_tai_t target_tai;
    uint64_t visiting_amf_ue_ngap_id;
    uint32_t visiting_ran_ue_ngap_id;

    memset(&target_plmn, 0, sizeof(target_plmn));
    memset(&target_tai, 0, sizeof(target_tai));

    memcpy(&target_plmn, &ogs_local_conf()->serving_plmn_id[1], OGS_PLMN_ID_LEN);
    memcpy(&target_tai.plmn_id, &target_plmn, OGS_PLMN_ID_LEN);
    target_tai.tac.v = 22;

    /* Step 1: HandoverRequired → S-AMF (h-amf container) */
    ogs_info("[HR-CT1] Step 1: HandoverRequired → h-amf");
    sendbuf = testngap_build_handover_required_with_target_plmn(
            test_ue,
            NGAP_HandoverType_intra5gs,
            0x4001, 24,
            NGAP_Cause_PR_radioNetwork,
            NGAP_CauseRadioNetwork_handover_desirable_for_radio_reason,
            true, &target_plmn, &target_tai);
    ABTS_PTR_NOTNULL(tc, sendbuf);
    rv = testgnb_ngap_send(ngap_home, sendbuf);
    ABTS_INT_EQUAL(tc, OGS_OK, rv);

    /* Step 7: Receive HandoverRequest on target gNB (from v-amf)
     * Server-side chain: h-amf → SEPP → v-amf → V-SMF → H-SMF done */
    ogs_info("[HR-CT1] Step 7: ← Waiting for HandoverRequest from v-amf...");
    recvbuf = testgnb_ngap_read(ngap_visiting);
    ABTS_PTR_NOTNULL(tc, recvbuf);
    testngap_recv(test_ue, recvbuf);
    ABTS_INT_EQUAL(tc, NGAP_ProcedureCode_id_HandoverResourceAllocation,
            test_ue->ngap_procedure_code);
    ogs_info("[HR-CT1] ← Received HandoverRequest (V-UPF N3 tunnel)");

    visiting_amf_ue_ngap_id = test_ue->amf_ue_ngap_id;

    /* Step 8: HandoverRequestAck WITH PDU sessions
     *
     * CONTAINER NOTE: The HandoverRequestAckTransfer carries the
     * target gNB's DL transport layer address. In the loopback test
     * this is gnb2_addr (127.0.0.3). In container mode, we need
     * the test host's GTP-U address that the V-UPF container can
     * reach. We set gnb_n3_addr to test_self()->gnb2_addr which was
     * overridden in our init to the container-reachable address. */
    ogs_info("[HR-CT1] Step 8: → HandoverRequestAck (with sessions)");
    { test_sess_t *s; ogs_list_for_each(&test_ue->sess_list, s)
        s->gnb_n3_addr = test_self()->gnb2_addr; }
    sendbuf = testngap_build_handover_request_ack(test_ue);
    ABTS_PTR_NOTNULL(tc, sendbuf);
    rv = testgnb_ngap_send(ngap_visiting, sendbuf);
    ABTS_INT_EQUAL(tc, OGS_OK, rv);

    visiting_ran_ue_ngap_id = test_ue->ran_ue_ngap_id;

    /* Step 12: Receive HandoverCommand on source gNB (from h-amf) */
    ogs_info("[HR-CT1] Step 12: ← Waiting for HandoverCommand...");
    recvbuf = testgnb_ngap_read(ngap_home);
    ABTS_PTR_NOTNULL(tc, recvbuf);
    testngap_recv(test_ue, recvbuf);
    ABTS_INT_EQUAL(tc, NGAP_ProcedureCode_id_HandoverPreparation,
            test_ue->ngap_procedure_code);
    ogs_info("[HR-CT1] ← Received HandoverCommand");

    /* Step 13: UplinkRANStatusTransfer → S-AMF (h-amf) */
    ogs_info("[HR-CT1] Step 13: → UplinkRANStatusTransfer");
    sendbuf = testngap_build_uplink_ran_status_transfer(test_ue);
    ABTS_PTR_NOTNULL(tc, sendbuf);
    rv = testgnb_ngap_send(ngap_home, sendbuf);
    ABTS_INT_EQUAL(tc, OGS_OK, rv);

    /* Restore visiting AMF context for target-side messages */
    test_ue->amf_ue_ngap_id = visiting_amf_ue_ngap_id;
    test_ue->ran_ue_ngap_id = visiting_ran_ue_ngap_id;
    test_ue->nr_cgi.cell_id = 0x40011;

    /* Step 15: DownlinkRANStatusTransfer on target gNB */
    ogs_info("[HR-CT1] Step 15: ← DownlinkRANStatusTransfer");
    recvbuf = testgnb_ngap_read(ngap_visiting);
    ABTS_PTR_NOTNULL(tc, recvbuf);
    testngap_recv(test_ue, recvbuf);
    ABTS_INT_EQUAL(tc, NGAP_ProcedureCode_id_DownlinkRANStatusTransfer,
            test_ue->ngap_procedure_code);

    /* Step 16: HandoverNotify on target gNB → v-amf */
    ogs_info("[HR-CT1] Step 16: → HandoverNotify on visiting gNB");
    sendbuf = testngap_build_handover_notify(test_ue);
    ABTS_PTR_NOTNULL(tc, sendbuf);
    rv = testgnb_ngap_send(ngap_visiting, sendbuf);
    ABTS_INT_EQUAL(tc, OGS_OK, rv);

    /* Step 21: UEContextReleaseCommand on source gNB */
    ogs_info("[HR-CT1] Step 21: ← Waiting for UEContextReleaseCommand...");
    wait_for_ue_context_release_on_source(tc, test_ue, ngap_home, "HR-CT1");
    ogs_info("[HR-CT1] ← Received UEContextReleaseCommand");

    sendbuf = testngap_build_ue_context_release_complete(test_ue);
    ABTS_PTR_NOTNULL(tc, sendbuf);
    rv = testgnb_ngap_send(ngap_home, sendbuf);
    ABTS_INT_EQUAL(tc, OGS_OK, rv);

    ogs_msleep(300);

    ogs_info("[HR-CT1] Inter-PLMN HR N2 handover completed");
    TIMING_PHASE_END("HR-CT1", "Phase 2 (handover)", t_phase);

    /**************************************************************************
     * PHASE 3: VERIFY DATA PATH IN VISITING NETWORK
     **************************************************************************/

    TIMING_PHASE_START(t_phase);
    ogs_info("[HR-CT1] ========================================");
    ogs_info("[HR-CT1] Phase 3: Verify data path in visiting network");
    ogs_info("[HR-CT1] ========================================");

    test_ue->amf_ue_ngap_id = visiting_amf_ue_ngap_id;
    test_ue->ran_ue_ngap_id = visiting_ran_ue_ngap_id;

    verify_gtpu_post_handover(tc, gtpu_visiting, sess, "HR-CT1");
    
    ogs_info("[HR-CT1] Phase 3 complete - data path verified");
    TIMING_PHASE_END("HR-CT1", "Phase 3 (data path verification)", t_phase);

    /********** Cleanup visiting AMF UE context */
    test_ue->amf_ue_ngap_id = visiting_amf_ue_ngap_id;
    test_ue->ran_ue_ngap_id = visiting_ran_ue_ngap_id;

    sendbuf = testngap_build_ue_context_release_request(test_ue,
            NGAP_Cause_PR_radioNetwork,
            NGAP_CauseRadioNetwork_user_inactivity,
            true);
    ABTS_PTR_NOTNULL(tc, sendbuf);
    rv = testgnb_ngap_send(ngap_visiting, sendbuf);
    ABTS_INT_EQUAL(tc, OGS_OK, rv);

    recvbuf = testgnb_ngap_read(ngap_visiting);
    ABTS_PTR_NOTNULL(tc, recvbuf);
    testngap_recv(test_ue, recvbuf);

    sendbuf = testngap_build_ue_context_release_complete(test_ue);
    ABTS_PTR_NOTNULL(tc, sendbuf);
    rv = testgnb_ngap_send(ngap_visiting, sendbuf);
    ABTS_INT_EQUAL(tc, OGS_OK, rv);

    ogs_msleep(300);

    /********** Final cleanup */
//     ABTS_INT_EQUAL(tc, OGS_OK, test_db_remove_ue(test_ue));
    test_gtpu_close(gtpu_home);
    testgnb_ngap_close(ngap_home); 
    test_gtpu_close(gtpu_visiting);
    testgnb_ngap_close(ngap_visiting);
    test_ue_remove(test_ue);

    ogs_info("[HR-CT1] ========================================");
    ogs_info("[HR-CT1] Test complete - HR inter-PLMN N2 handover OK");
    ogs_info("[HR-CT1] ========================================");
    TIMING_TOTAL("HR-CT1", t_total);
}


abts_suite *test_n2_handover_hr_container(abts_suite *suite)
{
    suite = ADD_SUITE(suite)

    ogs_info("========================================");
    ogs_info("Inter-PLMN N2 Handover HR — Container Test Suite");
    ogs_info("  V-SMF Insertion per TS 23.502 §4.23.7.3");
    ogs_info("========================================");
    ogs_info("Container Deployment Under Test:");
    ogs_info("  - H-AMF: %s:%d", HOME_AMF_HOST, HOME_AMF_PORT);
    ogs_info("  - V-AMF: %s:%d", VISITING_AMF_HOST, VISITING_AMF_PORT);
    ogs_info("  - Home GTP-U bind: %s:%d",
            HOME_GTPU_BIND_ADDR, HOME_GTPU_BIND_PORT);
    ogs_info("  - Visiting GTP-U bind: %s:%d",
            VISITING_GTPU_BIND_ADDR, VISITING_GTPU_BIND_PORT);
    ogs_info("  - Home PLMN: MCC=%d MNC=%d", HOME_MCC, HOME_MNC);
    ogs_info("  - Visiting PLMN: MCC=%d MNC=%d",
            VISITING_MCC, VISITING_MNC);
    ogs_info("  - Home-Routed: V-SMF insertion + PDU session preservation");
    ogs_info(" ");

    abts_run_test(suite, test1_hr_container_func, NULL);


    ogs_info(" ");
    ogs_info("========================================");
    ogs_info("HR Container Test Suite Summary:");
    ogs_info(" ");
    ogs_info("1. Basic HR Inter-PLMN Handover (V-SMF Insertion)");
    ogs_info("   Single PDU session + RANStatusTransfer");
    ogs_info(" ");

    return suite;
}
