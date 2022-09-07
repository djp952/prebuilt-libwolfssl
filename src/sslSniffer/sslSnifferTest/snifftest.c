/* snifftest.c
 *
 * Copyright (C) 2006-2022 wolfSSL Inc.
 *
 * This file is part of wolfSSL.
 *
 * wolfSSL is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * wolfSSL is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335, USA
 */


#ifdef HAVE_CONFIG_H
    #include <config.h>
#endif

#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/types.h>
#include <wolfssl/wolfcrypt/logging.h>
#include <wolfssl/wolfcrypt/error-crypt.h>
#include <wolfssl/version.h>

#ifdef WOLFSSL_SNIFFER_STORE_DATA_CB
    #include <wolfssl/wolfcrypt/memory.h>
#endif

#ifdef _WIN32
    #define WOLFSSL_SNIFFER
#endif

#ifndef WOLFSSL_SNIFFER
#ifndef NO_MAIN_DRIVER
/* blank build */
#include <stdio.h>
#include <stdlib.h>
int main(void)
{
    printf("do ./configure --enable-sniffer to enable build support\n");
    return EXIT_SUCCESS;
}
#endif /* !NO_MAIN_DRIVER */
#else
/* do a full build */

#ifdef _MSC_VER
    /* builds on *nix too, for scanf device and port */
    #define _CRT_SECURE_NO_WARNINGS
#endif

#include <pcap/pcap.h>     /* pcap stuff */
#include <stdio.h>         /* printf */
#include <stdlib.h>        /* EXIT_SUCCESS */
#include <string.h>        /* strcmp */
#include <signal.h>        /* signal */
#include <ctype.h>         /* isprint */

#include <cyassl/sniffer.h>


#ifndef _WIN32
    #include <sys/socket.h>    /* AF_INET */
    #include <arpa/inet.h>
    #include <netinet/in.h>
#endif

typedef unsigned char byte;

enum {
    ETHER_IF_FRAME_LEN = 14,   /* ethernet interface frame length */
    NULL_IF_FRAME_LEN =   4,   /* no link interface frame length  */
};


/* A TLS record can be 16k and change. The chain is broken up into 2K chunks.
 * This covers the TLS record, plus a chunk for TCP/IP headers. */
#ifndef CHAIN_INPUT_CHUNK_SIZE
    #define CHAIN_INPUT_CHUNK_SIZE 2048
#elif (CHAIN_INPUT_CHUNK_SIZE < 256)
    #undef CHAIN_INPUT_CHUNK_SIZE
    #define CHAIN_INPUT_CHUNK_SIZE 256
#elif (CHAIN_INPUT_CHUNK_SIZE > 16384)
    #undef CHAIN_INPUT_CHUNK_SIZE
    #define CHAIN_INPUT_CHUNK_SIZE 16384
#endif
#define CHAIN_INPUT_COUNT ((16384 / CHAIN_INPUT_CHUNK_SIZE) + 1)


#ifndef STORE_DATA_BLOCK_SZ
    #define STORE_DATA_BLOCK_SZ 1024
#endif

#if defined(HAVE_ECC) && !defined(NO_ECC_SECP) && (!defined(NO_ECC256) || defined(HAVE_ALL_CURVES))
    #define DEFAULT_SERVER_EPH_KEY_ECC "../../certs/statickeys/ecc-secp256r1.pem"
#else
    #define DEFAULT_SERVER_EPH_KEY_ECC ""
#endif
#ifndef NO_DH
    #define DEFAULT_SERVER_EPH_KEY_DH "../../certs/statickeys/dh-ffdhe2048.pem"
#else
    #define DEFAULT_SERVER_EPH_KEY_DH ""
#endif
#ifdef HAVE_CURVE25519
    #define DEFAULT_SERVER_EPH_KEY_X25519 "../../certs/statickeys/x25519.pem"
#else
    #define DEFAULT_SERVER_EPH_KEY_X25519 ""
#endif

#ifndef DEFAULT_SERVER_EPH_KEY
    #define DEFAULT_SERVER_EPH_KEY \
                DEFAULT_SERVER_EPH_KEY_ECC "," \
                DEFAULT_SERVER_EPH_KEY_DH "," \
                DEFAULT_SERVER_EPH_KEY_X25519
#endif

#define DEFAULT_SERVER_KEY_RSA "../../certs/server-key.pem"
#define DEFAULT_SERVER_KEY_ECC "../../certs/ecc-key.pem"
#ifndef DEFAULT_SERVER_KEY
    #ifndef NO_RSA
        #define DEFAULT_SERVER_KEY DEFAULT_SERVER_KEY_RSA
    #elif defined(HAVE_ECC)
        #define DEFAULT_SERVER_KEY DEFAULT_SERVER_KEY_ECC
    #endif
#endif


#ifdef WOLFSSL_SNIFFER_WATCH
static const byte rsaHash[] = {
    0x3d, 0x4a, 0x60, 0xfc, 0xbf, 0xe5, 0x4d, 0x3e,
    0x85, 0x62, 0xf2, 0xfc, 0xdb, 0x0d, 0x51, 0xdd,
    0xcd, 0xc2, 0x53, 0x81, 0x1a, 0x67, 0x31, 0xa0,
    0x7f, 0xd2, 0x11, 0x74, 0xbf, 0xea, 0xc9, 0xc5
};
static const byte eccHash[] = {
    0x9e, 0x45, 0xb6, 0xf8, 0xc6, 0x5d, 0x60, 0x90,
    0x40, 0x8f, 0xd2, 0x0e, 0xb1, 0x59, 0xe7, 0xbd,
    0xb0, 0x9b, 0x3c, 0x7a, 0x3a, 0xbe, 0x13, 0x52,
    0x07, 0x4f, 0x1a, 0x64, 0x45, 0xe0, 0x13, 0x34
};
#endif


static pcap_t* pcap = NULL;
static pcap_if_t* alldevs = NULL;
static struct bpf_program pcap_fp;

static void FreeAll(void)
{
    if (pcap) {
        pcap_freecode(&pcap_fp);
        pcap_close(pcap);
    }
    if (alldevs)
        pcap_freealldevs(alldevs);
#ifndef _WIN32
    ssl_FreeSniffer();
#endif
}


#ifdef WOLFSSL_SNIFFER_STATS
static void DumpStats(void)
{
    SSLStats sslStats;
    ssl_ReadStatistics(&sslStats);

    printf("SSL Stats (sslStandardConns):%lu\n",
            sslStats.sslStandardConns);
    printf("SSL Stats (sslClientAuthConns):%lu\n",
            sslStats.sslClientAuthConns);
    printf("SSL Stats (sslResumedConns):%lu\n",
            sslStats.sslResumedConns);
    printf("SSL Stats (sslEphemeralMisses):%lu\n",
            sslStats.sslEphemeralMisses);
    printf("SSL Stats (sslResumptionInserts):%lu\n",
            sslStats.sslResumptionInserts);
    printf("SSL Stats (sslResumeMisses):%lu\n",
            sslStats.sslResumeMisses);
    printf("SSL Stats (sslCiphersUnsupported):%lu\n",
            sslStats.sslCiphersUnsupported);
    printf("SSL Stats (sslKeysUnmatched):%lu\n",
            sslStats.sslKeysUnmatched);
    printf("SSL Stats (sslKeyFails):%lu\n",
            sslStats.sslKeyFails);
    printf("SSL Stats (sslDecodeFails):%lu\n",
            sslStats.sslDecodeFails);
    printf("SSL Stats (sslAlerts):%lu\n",
            sslStats.sslAlerts);
    printf("SSL Stats (sslDecryptedBytes):%lu\n",
            sslStats.sslDecryptedBytes);
    printf("SSL Stats (sslEncryptedBytes):%lu\n",
            sslStats.sslEncryptedBytes);
    printf("SSL Stats (sslEncryptedPackets):%lu\n",
            sslStats.sslEncryptedPackets);
    printf("SSL Stats (sslDecryptedPackets):%lu\n",
            sslStats.sslDecryptedPackets);
    printf("SSL Stats (sslKeyMatches):%lu\n",
            sslStats.sslKeyMatches);
    printf("SSL Stats (sslEncryptedConns):%lu\n",
            sslStats.sslEncryptedConns);
}
#endif /* WOLFSSL_SNIFFER_STATS */


static void sig_handler(const int sig)
{
    printf("SIGINT handled = %d.\n", sig);
    FreeAll();
#ifdef WOLFSSL_SNIFFER_STATS
    DumpStats();
#endif
    if (sig)
        exit(EXIT_SUCCESS);
}


static void err_sys(const char* msg)
{
    fprintf(stderr, "%s\n", msg);
    if (msg)
        exit(EXIT_FAILURE);
}


#ifdef _WIN32
    #define SNPRINTF _snprintf
#else
    #define SNPRINTF snprintf
#endif


static char* iptos(const struct in_addr* addr)
{
    static char output[32];
    byte *p = (byte*)&addr->s_addr;

    snprintf(output, sizeof(output), "%d.%d.%d.%d", p[0], p[1], p[2], p[3]);

    return output;
}

static const char* ip6tos(const struct in6_addr* addr)
{
    static char output[42];
    return inet_ntop(AF_INET6, addr, output, 42);
}


#if defined(WOLFSSL_SNIFFER_STORE_DATA_CB) || defined(WOLFSSL_SNIFFER_CHAIN_INPUT)
static inline unsigned int min(unsigned int a, unsigned int b)
{
    return a > b ? b : a;
}
#endif


#ifdef WOLFSSL_SNIFFER_WATCH
static int myWatchCb(void* vSniffer,
        const unsigned char* certHash, unsigned int certHashSz,
        const unsigned char* certChain, unsigned int certChainSz,
        void* ctx, char* error)
{
    const char* certName = NULL;

    (void)certChain;
    (void)certChainSz;
    (void)ctx;

    if (certHashSz == sizeof(rsaHash) &&
            XMEMCMP(certHash, rsaHash, certHashSz) == 0) {
        certName = DEFAULT_SERVER_KEY_RSA;
    }
    if (certHashSz == sizeof(eccHash) &&
            XMEMCMP(certHash, eccHash, certHashSz) == 0) {
        certName = DEFAULT_SERVER_KEY_ECC;
    }

    if (certName == NULL) {
        /* don't return error if key is not loaded */
        printf("Warning: No matching key found for cert hash\n");
        return 0;
    }

    return ssl_SetWatchKey_file(vSniffer, certName, FILETYPE_PEM, NULL, error);
}
#endif /* WOLFSSL_SNIFFER_WATCH */


#ifdef WOLFSSL_SNIFFER_STORE_DATA_CB
static int myStoreDataCb(const unsigned char* decryptBuf,
        unsigned int decryptBufSz, unsigned int decryptBufOffset, void* ctx)
{
    byte** data = (byte**)ctx;
    unsigned int qty;

    if (data == NULL)
        return -1;

    if (decryptBufSz < decryptBufOffset)
        return -1;

    qty = min(decryptBufSz - decryptBufOffset, STORE_DATA_BLOCK_SZ);

    if (*data == NULL) {
        byte* tmpData;
        tmpData = (byte*)XREALLOC(*data, decryptBufSz + 1,
                NULL, DYNAMIC_TYPE_TMP_BUFFER);
        if (tmpData == NULL) {
            XFREE(*data, NULL, DYNAMIC_TYPE_TMP_BUFFER);
            *data = NULL;
            return -1;
        }
        *data = tmpData;
    }

    XMEMCPY(*data + decryptBufOffset, decryptBuf + decryptBufOffset, qty);

    return qty;
}
#endif /* WOLFSSL_SNIFFER_STORE_DATA_CB */

/* try and load as both static ephemeral and private key */
/* only fail if no key is loaded */
/* Allow comma separated list of files */
static int load_key(const char* name, const char* server, int port,
    const char* keyFiles, const char* passwd, char* err)
{
    int ret = -1;
    int loadCount = 0;
    char *keyFile, *ptr = NULL;

    keyFile = XSTRTOK((char*)keyFiles, ",", &ptr);
    while (keyFile != NULL) {
#ifdef WOLFSSL_STATIC_EPHEMERAL
    #ifdef HAVE_SNI
        ret = ssl_SetNamedEphemeralKey(name, server, port, keyFile,
                            FILETYPE_PEM, passwd, err);
    #else
        ret = ssl_SetEphemeralKey(server, port, keyFile,
                            FILETYPE_PEM, passwd, err);
    #endif
        if (ret == 0)
            loadCount++;
#endif
    #ifdef HAVE_SNI
        ret = ssl_SetNamedPrivateKey(name, server, port, keyFile,
                            FILETYPE_PEM, passwd, err);
    #else
        ret = ssl_SetPrivateKey(server, port, keyFile,
                            FILETYPE_PEM, passwd, err);
    #endif
        if (ret == 0)
            loadCount++;

        if (loadCount == 0) {
            printf("Failed loading private key %s: ret %d\n", keyFile, ret);
            printf("Please run directly from sslSniffer/sslSnifferTest dir\n");
            ret = -1;
        }
        else {
            ret = 0;
        }

        keyFile = XSTRTOK(NULL, ",", &ptr);
    }

    (void)name;
    return ret;
}

static void TrimNewLine(char* str)
{
    word32 strSz = 0;
    if (str)
        strSz = (word32)XSTRLEN(str);
    if (strSz > 0 && (str[strSz-1] == '\n' || str[strSz-1] == '\r'))
        str[strSz-1] = '\0';
}

static void show_appinfo(void)
{
    printf("snifftest %s\n", LIBWOLFSSL_VERSION_STRING);

    /* list enabled sniffer features */
    printf("sniffer features: "
    #ifdef WOLFSSL_SNIFFER_STATS
        "stats, "
    #endif
    #ifdef WOLFSSL_SNIFFER_WATCH
        "watch, "
    #endif
    #ifdef WOLFSSL_SNIFFER_STORE_DATA_CB
        "store_data_cb "
    #endif
    #ifdef WOLFSSL_SNIFFER_CHAIN_INPUT
        "chain_input "
    #endif
    #ifdef WOLFSSL_SNIFFER_KEY_CALLBACK
        "key_callback "
    #endif
    #ifdef DEBUG_SNIFFER
        "debug "
    #endif
    #ifdef WOLFSSL_TLS13
        "tls_v13 "
    #endif
    #ifndef WOLFSSL_NO_TLS12
        "tls_v12 "
    #endif
    #ifdef HAVE_SESSION_TICKET
        "session_ticket "
    #endif
    #ifdef WOLFSSL_STATIC_EPHEMERAL
        "static_ephemeral "
    #endif
    #ifdef WOLFSSL_ENCRYPTED_KEYS
        "encrypted_keys "
    #endif
    #ifdef HAVE_SNI
        "sni "
    #endif
    #ifdef HAVE_EXTENDED_MASTER
        "extended_master "
    #endif
    #ifdef HAVE_MAX_FRAGMENT
        "max fragment "
    #endif
    #ifdef WOLFSSL_ASYNC_CRYPT
        "async_crypt "
    #endif
    #ifndef NO_RSA
        "rsa "
    #endif
    #if !defined(NO_DH) && defined(WOLFSSL_DH_EXTRA)
        "dh "
    #endif
    #ifdef HAVE_ECC
        "ecc "
    #endif
    #ifdef HAVE_CURVE448
        "x448 "
    #endif
    #ifdef HAVE_CURVE22519
        "x22519 "
    #endif
    #ifdef WOLFSSL_STATIC_RSA
        "rsa_static "
    #endif
    #ifdef WOLFSSL_STATIC_DH
        "dh_static "
    #endif
    "\n\n"
    );
}
static void show_usage(void)
{
    printf("usage:\n");
    printf("\t./snifftest\n");
    printf("\t\tprompts for options\n");
    printf("\t./snifftest dump pemKey [server] [port] [password]\n");
}


#ifdef WOLFSSL_ASYNC_CRYPT

typedef struct SnifferPacket {
    byte* packet;
    int   length;
    int   lastRet;
    int   packetNumber;
} SnifferPacket;

static SnifferPacket asyncQueue[WOLF_ASYNC_MAX_PENDING];

/* returns index to queue */
static int SnifferAsyncQueueAdd(int lastRet, void* chain, int chainSz,
    int isChain, int packetNumber)
{
    int ret = MEMORY_E, i, length;
    byte* packet;

#ifdef WOLFSSL_SNIFFER_CHAIN_INPUT
    if (isChain) {
        struct iovec* vchain = (struct iovec*)chain;
        length = 0;
        for (i = 0; i < chainSz; i++)
            length += vchain[i].iov_len;
        packet = (byte*)vchain[0].iov_base;
    }
    else
#endif
    {
        packet = (byte*)chain;
        length = chainSz;
    }

    /* find first free idx */
    for (i=0; i<WOLF_ASYNC_MAX_PENDING; i++) {
        if (asyncQueue[i].packet == NULL) {
            if (ret == MEMORY_E) {
                ret = i;
                break;
            }
        }
    }
    if (ret != MEMORY_E) {
        asyncQueue[ret].packet = XMALLOC(length, NULL, DYNAMIC_TYPE_TMP_BUFFER);
        if (asyncQueue[ret].packet == NULL) {
            return MEMORY_E;
        }
        XMEMCPY(asyncQueue[ret].packet, packet, length);
        asyncQueue[ret].length = length;
        asyncQueue[ret].lastRet = lastRet;
        asyncQueue[ret].packetNumber = packetNumber;
    }
    (void)isChain;

    return ret;
}

static int SnifferAsyncPollQueue(byte** data, char* err, SSLInfo* sslInfo,
    int* queueSz)
{
    int ret = 0, i;
    WOLF_EVENT* events[WOLF_ASYNC_MAX_PENDING];
    int eventCount = 0;

    /* try to process existing items in queue */
    for (i=0; i<WOLF_ASYNC_MAX_PENDING; i++) {
        if (asyncQueue[i].packet != NULL) {
            (*queueSz)++;

            /* do poll for events on hardware */
            ret = ssl_PollSniffer(events, WOLF_ASYNC_MAX_PENDING,
                WOLF_POLL_FLAG_CHECK_HW, &eventCount);
            if (ret == 0) {
                /* attempt to reprocess pending packet */
            #ifdef DEBUG_SNIFFER
                printf("Retrying packet %d\n", asyncQueue[i].packetNumber);
            #endif
                ret = ssl_DecodePacketAsync(asyncQueue[i].packet,
                    asyncQueue[i].length, 0, data, err, sslInfo, NULL);
                asyncQueue[i].lastRet = ret;
                if (ret >= 0) {
                    /* done, so free and break to process below */
                    XFREE(asyncQueue[i].packet, NULL, DYNAMIC_TYPE_TMP_BUFFER);
                    asyncQueue[i].packet = NULL;
                    if (ret > 0) {
                        /* decrypted some data, so return */
                        break;
                    }
                }
            }
        }
    }
    if (ret == WC_PENDING_E) {
        ret = 0; /* nothing new */
    }
    return ret;
}
#endif /* WOLFSSL_ASYNC_CRYPT */

int main(int argc, char** argv)
{
    int          ret = 0;
    int          hadBadPacket = 0;
    int          inum = 0;
    int          port = 0;
    int          saveFile = 0;
    int          i = 0, defDev = 0;
    int          frame = ETHER_IF_FRAME_LEN;
    char         err[PCAP_ERRBUF_SIZE];
    char         filter[32];
    const char  *keyFilesSrc = NULL;
    char         keyFilesBuf[MAX_FILENAME_SZ];
    char         keyFilesUser[MAX_FILENAME_SZ];
    const char  *server = NULL;
    const char  *sniName = NULL;
    pcap_if_t   *d;
    pcap_addr_t *a;
    int          isChain = 0;
    int          j;
#ifdef WOLFSSL_SNIFFER_CHAIN_INPUT
    struct iovec chains[CHAIN_INPUT_COUNT];
    unsigned int remainder;
#endif
    int packetNumber = 0;

    show_appinfo();

    signal(SIGINT, sig_handler);

#ifndef _WIN32
    ssl_InitSniffer();   /* dll load on Windows */
#endif
    ssl_Trace("./tracefile.txt", err);
    ssl_EnableRecovery(1, -1, err);
#ifdef WOLFSSL_SNIFFER_WATCH
    ssl_SetWatchKeyCallback(myWatchCb, err);
#endif
#ifdef WOLFSSL_SNIFFER_STORE_DATA_CB
    ssl_SetStoreDataCallback(myStoreDataCb);
#endif

    if (argc == 1) {
        char cmdLineArg[128];
        /* normal case, user chooses device and port */

        if (pcap_findalldevs(&alldevs, err) == -1)
            err_sys("Error in pcap_findalldevs");

        for (d = alldevs; d; d=d->next) {
            printf("%d. %s", ++i, d->name);
            if (strcmp(d->name, "lo0") == 0) {
                defDev = i;
            }
            if (d->description)
                printf(" (%s)\n", d->description);
            else
                printf(" (No description available)\n");
        }

        if (i == 0)
            err_sys("No interfaces found! Make sure pcap or WinPcap is"
                    " installed correctly and you have sufficient permissions");

        printf("Enter the interface number (1-%d) [default: %d]: ", i, defDev);
        XMEMSET(cmdLineArg, 0, sizeof(cmdLineArg));
        if (XFGETS(cmdLineArg, sizeof(cmdLineArg), stdin))
            inum = XATOI(cmdLineArg);
        if (inum == 0)
            inum = defDev;
        else if (inum < 1 || inum > i)
            err_sys("Interface number out of range");

        /* Jump to the selected adapter */
        for (d = alldevs, i = 0; i < inum - 1; d = d->next, i++);

        pcap = pcap_create(d->name, err);

        if (pcap == NULL) printf("pcap_create failed %s\n", err);

        /* print out addresses for selected interface */
        for (a = d->addresses; a; a = a->next) {
            if (a->addr->sa_family == AF_INET) {
                server =
                    iptos(&((struct sockaddr_in *)a->addr)->sin_addr);
                printf("server = %s\n", server);
            }
            else if (a->addr->sa_family == AF_INET6) {
                server =
                    ip6tos(&((struct sockaddr_in6 *)a->addr)->sin6_addr);
                printf("server = %s\n", server);
            }
        }
        if (server == NULL)
            err_sys("Unable to get device IPv4 or IPv6 address");

        ret = pcap_set_snaplen(pcap, 65536);
        if (ret != 0) printf("pcap_set_snaplen failed %s\n", pcap_geterr(pcap));

        ret = pcap_set_timeout(pcap, 1000);
        if (ret != 0) printf("pcap_set_timeout failed %s\n", pcap_geterr(pcap));

        ret = pcap_set_buffer_size(pcap, 1000000);
        if (ret != 0)
            printf("pcap_set_buffer_size failed %s\n", pcap_geterr(pcap));

        ret = pcap_set_promisc(pcap, 1);
        if (ret != 0) printf("pcap_set_promisc failed %s\n", pcap_geterr(pcap));


        ret = pcap_activate(pcap);
        if (ret != 0) printf("pcap_activate failed %s\n", pcap_geterr(pcap));

        printf("Enter the port to scan [default: 11111]: ");
        XMEMSET(cmdLineArg, 0, sizeof(cmdLineArg));
        if (XFGETS(cmdLineArg, sizeof(cmdLineArg), stdin)) {
            port = XATOI(cmdLineArg);
        }
        if (port <= 0)
            port = 11111;

        SNPRINTF(filter, sizeof(filter), "tcp and port %d", port);

        ret = pcap_compile(pcap, &pcap_fp, filter, 0, 0);
        if (ret != 0) printf("pcap_compile failed %s\n", pcap_geterr(pcap));

        ret = pcap_setfilter(pcap, &pcap_fp);
        if (ret != 0) printf("pcap_setfilter failed %s\n", pcap_geterr(pcap));

        /* optionally enter the private key to use */
    #if defined(WOLFSSL_STATIC_EPHEMERAL) && defined(DEFAULT_SERVER_EPH_KEY)
        keyFilesSrc = DEFAULT_SERVER_EPH_KEY;
    #else
        keyFilesSrc = DEFAULT_SERVER_KEY;
    #endif
        printf("Enter the server key [default: %s]: ", keyFilesSrc);
        XMEMSET(keyFilesBuf, 0, sizeof(keyFilesBuf));
        XMEMSET(keyFilesUser, 0, sizeof(keyFilesUser));
        if (XFGETS(keyFilesUser, sizeof(keyFilesUser), stdin)) {
            TrimNewLine(keyFilesUser);
            if (XSTRLEN(keyFilesUser) > 0) {
                keyFilesSrc = keyFilesUser;
            }
        }
        XSTRNCPY(keyFilesBuf, keyFilesSrc, sizeof(keyFilesBuf));

        /* optionally enter a named key (SNI) */
    #if !defined(WOLFSSL_SNIFFER_WATCH) && defined(HAVE_SNI)
        printf("Enter alternate SNI [default: none]: ");
        XMEMSET(cmdLineArg, 0, sizeof(cmdLineArg));
        if (XFGETS(cmdLineArg, sizeof(cmdLineArg), stdin)) {
            TrimNewLine(cmdLineArg);
            if (XSTRLEN(cmdLineArg) > 0) {
                sniName = cmdLineArg;
            }
        }
    #endif /* !WOLFSSL_SNIFFER_WATCH && HAVE_SNI */

        /* get IPv4 or IPv6 addresses for selected interface */
        for (a = d->addresses; a; a = a->next) {
            server = NULL;
            if (a->addr->sa_family == AF_INET) {
                server =
                    iptos(&((struct sockaddr_in *)a->addr)->sin_addr);
            }
            else if (a->addr->sa_family == AF_INET6) {
                server =
                    ip6tos(&((struct sockaddr_in6 *)a->addr)->sin6_addr);
            }

            if (server) {
                XSTRNCPY(keyFilesBuf, keyFilesSrc, sizeof(keyFilesBuf));
                ret = load_key(sniName, server, port, keyFilesBuf, NULL, err);
                if (ret != 0) {
                    exit(EXIT_FAILURE);
                }
            }
        }
    }
    else if (argc >= 3) {
        saveFile = 1;
        pcap = pcap_open_offline(argv[1], err);
        if (pcap == NULL) {
            printf("pcap_open_offline failed %s\n", err);
            ret = -1;
        }
        else {
            const char* passwd = NULL;

            /* defaults for server and port */
            port = 443;
            server = "127.0.0.1";
            keyFilesSrc = argv[2];

            if (argc >= 4)
                server = argv[3];

            if (argc >= 5)
                port = XATOI(argv[4]);

            if (argc >= 6)
                passwd = argv[5];

            ret = load_key(NULL, server, port, keyFilesSrc, passwd, err);
            if (ret != 0) {
                exit(EXIT_FAILURE);
            }

            /* Only let through TCP/IP packets */
            ret = pcap_compile(pcap, &pcap_fp, "(ip6 or ip) and tcp", 0, 0);
            if (ret != 0) {
                printf("pcap_compile failed %s\n", pcap_geterr(pcap));
                exit(EXIT_FAILURE);
            }

            ret = pcap_setfilter(pcap, &pcap_fp);
            if (ret != 0) {
                printf("pcap_setfilter failed %s\n", pcap_geterr(pcap));
                exit(EXIT_FAILURE);
            }
        }
    }
    else {
        show_usage();
        exit(EXIT_FAILURE);
    }

    if (ret != 0)
        err_sys(err);

    if (pcap_datalink(pcap) == DLT_NULL)
        frame = NULL_IF_FRAME_LEN;

    while (1) {
        struct pcap_pkthdr header;
        const unsigned char* packet = NULL;
        SSLInfo sslInfo;
        void* chain = NULL;
        int   chainSz = 0;
        byte* data = NULL; /* pointer to decrypted data */
#ifdef WOLFSSL_ASYNC_CRYPT
        int queueSz = 0;
#endif

#ifndef WOLFSSL_ASYNC_CRYPT
        ret = 0; /* reset status */
#else
        /* poll hardware and attempt to process items in queue. If returns > 0
         * then data pointer has decrypted something */
        ret = SnifferAsyncPollQueue(&data, err, &sslInfo, &queueSz);
        if (queueSz >= WOLF_ASYNC_MAX_PENDING) {
            /* queue full, poll again */
            continue;
        }
#endif
        if (data == NULL) {
            /* grab next pcap packet */
            packetNumber++;
            packet = pcap_next(pcap, &header);
        #if defined(WOLFSSL_ASYNC_CRYPT) && defined(DEBUG_SNIFFER)
            printf("Packet Number: %d\n", packetNumber);
        #endif
        }
        if (packet) {
            if (header.caplen > 40)  { /* min ip(20) + min tcp(20) */
                packet        += frame;
                header.caplen -= frame;
            }
            else {
                /* packet doesn't contain minimum ip/tcp header */
                continue;
            }

#ifdef WOLFSSL_SNIFFER_CHAIN_INPUT
            isChain = 1;
            j = 0;
            remainder = header.caplen;
            chainSz = 0;
            do {
                unsigned int chunkSz = min(remainder, CHAIN_INPUT_CHUNK_SIZE);
                chains[chainSz].iov_base = (void*)(packet + j);
                chains[chainSz].iov_len = chunkSz;
                j += chunkSz;
                remainder -= chunkSz;
                chainSz++;
            } while (j < (int)header.caplen);
            chain = (void*)chains;
#else
            chain = (void*)packet;
            chainSz = header.caplen;
#endif

#ifdef WOLFSSL_ASYNC_CRYPT
            /* For async call the original API again with same data,
             * or call with different sessions for multiple concurrent
             * stream processing */
            ret = ssl_DecodePacketAsync(chain, chainSz, isChain, &data, err,
                &sslInfo, NULL);

            /* WC_PENDING_E: Hardware is processing or stream is blocked
             *               (waiting on WC_PENDING_E) */
            if (ret == WC_PENDING_E) {
                /* add to queue, for later processing */
            #ifdef DEBUG_SNIFFER
                printf("Steam is pending, queue packet %d\n", packetNumber);
            #endif
                ret = SnifferAsyncQueueAdd(ret, chain, chainSz, isChain,
                    packetNumber);
                if (ret >= 0) {
                    ret = 0; /* mark event just added */
                }
            }

#elif defined(WOLFSSL_SNIFFER_CHAIN_INPUT) && \
      defined(WOLFSSL_SNIFFER_STORE_DATA_CB)
            ret = ssl_DecodePacketWithChainSessionInfoStoreData(chain, chainSz,
                    &data, &sslInfo, err);
#elif defined(WOLFSSL_SNIFFER_CHAIN_INPUT)
            (void)sslInfo;
            ret = ssl_DecodePacketWithChain(chain, chainSz, &data, err);
#else
    #if defined(WOLFSSL_SNIFFER_STORE_DATA_CB)
            ret = ssl_DecodePacketWithSessionInfoStoreData(packet,
                    header.caplen, &data, &sslInfo, err);
    #else
            ret = ssl_DecodePacketWithSessionInfo(packet, header.caplen, &data,
                                                  &sslInfo, err);
    #endif
            (void)chain;
            (void)chainSz;
#endif
        }

        /* check if we are done reading file */
        if (packet == NULL && data == NULL && saveFile) {
            break;
        }

        if (ret < 0) {
            printf("ssl_Decode ret = %d, %s\n", ret, err);
            hadBadPacket = 1;
        }
        if (data != NULL && ret > 0) {
            /* Convert non-printable data to periods. */
            for (j = 0; j < ret; j++) {
                if (isprint(data[j]) || isspace(data[j])) continue;
                data[j] = '.';
            }
            data[ret] = 0;
            printf("SSL App Data(%d:%d):%s\n", packetNumber, ret, data);
            ssl_FreeZeroDecodeBuffer(&data, ret, err);
        }
    }
    FreeAll();
    (void)isChain;

    return hadBadPacket ? EXIT_FAILURE : EXIT_SUCCESS;
}

#endif /* full build */
