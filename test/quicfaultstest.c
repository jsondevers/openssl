/*
 * Copyright 2022 The OpenSSL Project Authors. All Rights Reserved.
 *
 * Licensed under the Apache License 2.0 (the "License").  You may not use
 * this file except in compliance with the License.  You can obtain a copy
 * in the file LICENSE in the source distribution or at
 * https://www.openssl.org/source/license.html
 */

#include <string.h>
#include <openssl/ssl.h>
#include "helpers/quictestlib.h"
#include "internal/quic_error.h"
#include "testutil.h"

static char *cert = NULL;
static char *privkey = NULL;

/*
 * Basic test that just creates a connection and sends some data without any
 * faults injected.
 */
static int test_basic(void)
{
    int testresult = 0;
    SSL_CTX *cctx = SSL_CTX_new(OSSL_QUIC_client_method());
    QUIC_TSERVER *qtserv = NULL;
    SSL *cssl = NULL;
    char *msg = "Hello World!";
    size_t msglen = strlen(msg);
    unsigned char buf[80];
    size_t bytesread;

    if (!TEST_ptr(cctx))
        goto err;

    if (!TEST_true(qtest_create_quic_objects(cctx, cert, privkey, &qtserv,
                                             &cssl, NULL)))
        goto err;

    if (!TEST_true(qtest_create_quic_connection(qtserv, cssl)))
        goto err;

    if (!TEST_int_eq(SSL_write(cssl, msg, msglen), msglen))
        goto err;

    ossl_quic_tserver_tick(qtserv);
    if (!TEST_true(ossl_quic_tserver_read(qtserv, buf, sizeof(buf), &bytesread)))
        goto err;

    /*
     * We assume the entire message is read from the server in one go. In
     * theory this could get fragmented but its a small message so we assume
     * not.
     */
    if (!TEST_mem_eq(msg, msglen, buf, bytesread))
        goto err;

    testresult = 1;
 err:
    SSL_free(cssl);
    ossl_quic_tserver_free(qtserv);
    SSL_CTX_free(cctx);
    return testresult;
}

/*
 * Test that adding an unknown frame type is handled correctly
 */
static int add_unknown_frame_cb(OSSL_QUIC_FAULT *fault, QUIC_PKT_HDR *hdr,
                                unsigned char *buf, size_t len, void *cbarg)
{
    size_t done = 0;

    /*
     * There are no "reserved" frame types which are definitately safe for us
     * to use for testing purposes - but we just use the highest possible
     * value (8 byte length integer) and with no payload bytes
     */
    unsigned char unknown_frame[] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
    };

    /* We only ever add the unknown frame to one packet */
    if (done)
        return 1;
    done++;

    /* Extend the size of the packet by the size of the new frame */
    if (!TEST_true(ossl_quic_fault_resize_plain_packet(fault,
                                                       len + sizeof(unknown_frame))))
        return 0;

    /*
     * We prepend the new frame to the start of the packet. We add it to the
     * start rather than the end because stream frames that are already in the
     * packet may not have an explicit length, and instead may just extend to
     * the end of the packet. We could fix-up such frames to have an explicit
     * length and add our new frame after it. But it is probably simpler just to
     * add it to the beginning of the packet. This means moving the existing
     * packet data.
     */
    memmove(buf + sizeof(unknown_frame), buf, len);
    memcpy(buf, unknown_frame, sizeof(unknown_frame));

    return 1;
}

static int test_unknown_frame(void)
{
    int testresult = 0, ret;
    SSL_CTX *cctx = SSL_CTX_new(OSSL_QUIC_client_method());
    QUIC_TSERVER *qtserv = NULL;
    SSL *cssl = NULL;
    char *msg = "Hello World!";
    size_t msglen = strlen(msg);
    unsigned char buf[80];
    size_t byteswritten;
    OSSL_QUIC_FAULT *fault = NULL;

    if (!TEST_ptr(cctx))
        goto err;

    if (!TEST_true(qtest_create_quic_objects(cctx, cert, privkey, &qtserv,
                                             &cssl, &fault)))
        goto err;

    if (!TEST_true(qtest_create_quic_connection(qtserv, cssl)))
        goto err;

    /*
     * Write a message from the server to the client and add an unknown frame
     * type
     */
    if (!TEST_true(ossl_quic_fault_set_packet_plain_listener(fault,
                                                             add_unknown_frame_cb,
                                                             NULL)))
        goto err;

    if (!TEST_true(ossl_quic_tserver_write(qtserv, (unsigned char *)msg, msglen,
                                           &byteswritten)))
        goto err;

    if (!TEST_size_t_eq(msglen, byteswritten))
        goto err;

    ossl_quic_tserver_tick(qtserv);
    if (!TEST_true(SSL_tick(cssl)))
        goto err;

    if (!TEST_int_le(ret = SSL_read(cssl, buf, sizeof(buf)), 0))
        goto err;

    if (!TEST_int_eq(SSL_get_error(cssl, ret), SSL_ERROR_SSL))
        goto err;

#if 0
    /*
     * TODO(QUIC): We should expect an error on the queue after this - but we
     * don't have it yet.
     * Note, just raising the error in the obvious place causes SSL_tick() to
     * succeed, but leave a suprious error on the stack. We need to either
     * allow SSL_tick() to fail, or somehow delay the raising of the error
     * until the SSL_read() call.
     */
    if (!TEST_int_eq(ERR_GET_REASON(ERR_peek_error()),
                     SSL_R_UNKNOWN_FRAME_TYPE_RECEIVED))
        goto err;
#endif

    if (!TEST_true(qtest_check_server_protocol_err(qtserv)))
        goto err;

    testresult = 1;
 err:
    ossl_quic_fault_free(fault);
    SSL_free(cssl);
    ossl_quic_tserver_free(qtserv);
    SSL_CTX_free(cctx);
    return testresult;
}

/*
 * Test that a server that fails to provide transport params cannot be
 * connected to.
 */
static int drop_transport_params_cb(OSSL_QUIC_FAULT *fault,
                                    OSSL_QF_ENCRYPTED_EXTENSIONS *ee,
                                    size_t eelen, void *encextcbarg)
{
    if (!ossl_quic_fault_delete_extension(fault,
                                          TLSEXT_TYPE_quic_transport_parameters,
                                          ee->extensions, &ee->extensionslen))
        return 0;

    return 1;
}

static int test_no_transport_params(void)
{
    int testresult = 0;
    SSL_CTX *cctx = SSL_CTX_new(OSSL_QUIC_client_method());
    QUIC_TSERVER *qtserv = NULL;
    SSL *cssl = NULL;
    OSSL_QUIC_FAULT *fault = NULL;

    if (!TEST_ptr(cctx))
        goto err;

    if (!TEST_true(qtest_create_quic_objects(cctx, cert, privkey, &qtserv,
                                             &cssl, &fault)))
        goto err;

    if (!TEST_true(ossl_quic_fault_set_hand_enc_ext_listener(fault,
                                                             drop_transport_params_cb,
                                                             NULL)))
        goto err;

    /*
     * We expect the connection to fail because the server failed to provide
     * transport parameters
     */
    if (!TEST_false(qtest_create_quic_connection(qtserv, cssl)))
        goto err;

    if (!TEST_true(qtest_check_server_protocol_err(qtserv)))
        goto err;

    testresult = 1;
 err:
    ossl_quic_fault_free(fault);
    SSL_free(cssl);
    ossl_quic_tserver_free(qtserv);
    SSL_CTX_free(cctx);
    return testresult;

}

OPT_TEST_DECLARE_USAGE("certsdir\n")

int setup_tests(void)
{
    char *certsdir = NULL;

    if (!test_skip_common_options()) {
        TEST_error("Error parsing test options\n");
        return 0;
    }

    if (!TEST_ptr(certsdir = test_get_argument(0)))
        return 0;


    cert = test_mk_file_path(certsdir, "servercert.pem");
    if (cert == NULL)
        goto err;

    privkey = test_mk_file_path(certsdir, "serverkey.pem");
    if (privkey == NULL)
        goto err;

    ADD_TEST(test_basic);
    ADD_TEST(test_unknown_frame);
    ADD_TEST(test_no_transport_params);

    return 1;

 err:
    OPENSSL_free(cert);
    OPENSSL_free(privkey);
    return 0;
}

void cleanup_tests(void)
{
    OPENSSL_free(cert);
    OPENSSL_free(privkey);
}