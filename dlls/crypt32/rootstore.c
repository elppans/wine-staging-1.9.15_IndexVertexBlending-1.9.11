/*
 * Copyright 2007 Juan Lang
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */
#include "config.h"
#include <stdarg.h>
#include <stdio.h>
#include <sys/types.h>
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#ifdef HAVE_DIRENT_H
#include <dirent.h>
#endif
#include <fcntl.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <errno.h>
#include <limits.h>
#ifdef HAVE_SECURITY_SECURITY_H
#include <Security/Security.h>
#endif
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winreg.h"
#include "wincrypt.h"
#include "winternl.h"
#include "wine/debug.h"
#include "crypt32_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(crypt);

#define INITIAL_CERT_BUFFER 1024

struct DynamicBuffer
{
    DWORD allocated;
    DWORD used;
    BYTE *data;
};

static inline void reset_buffer(struct DynamicBuffer *buffer)
{
    buffer->used = 0;
    if (buffer->data) buffer->data[0] = 0;
}

static BOOL add_line_to_buffer(struct DynamicBuffer *buffer, LPCSTR line)
{
    BOOL ret;

    if (buffer->used + strlen(line) + 1 > buffer->allocated)
    {
        if (!buffer->allocated)
        {
            buffer->data = CryptMemAlloc(INITIAL_CERT_BUFFER);
            if (buffer->data)
            {
                buffer->data[0] = 0;
                buffer->allocated = INITIAL_CERT_BUFFER;
            }
        }
        else
        {
            DWORD new_size = max(buffer->allocated * 2,
             buffer->used + strlen(line) + 1);

            buffer->data = CryptMemRealloc(buffer->data, new_size);
            if (buffer->data)
                buffer->allocated = new_size;
        }
    }
    if (buffer->data)
    {
        strcpy((char *)buffer->data + strlen((char *)buffer->data), line);
        /* Not strlen + 1, otherwise we'd count the NULL for every line's
         * addition (but we overwrite the previous NULL character.)  Not an
         * overrun, we allocate strlen + 1 bytes above.
         */
        buffer->used += strlen(line);
        ret = TRUE;
    }
    else
        ret = FALSE;
    return ret;
}

/* Reads any base64-encoded certificates present in fp and adds them to store.
 * Returns TRUE if any certificates were successfully imported.
 */
static BOOL import_base64_certs_from_fp(FILE *fp, HCERTSTORE store)
{
    char line[1024];
    BOOL in_cert = FALSE;
    struct DynamicBuffer saved_cert = { 0, 0, NULL };
    int num_certs = 0;

    TRACE("\n");
    while (fgets(line, sizeof(line), fp))
    {
        static const char header[] = "-----BEGIN CERTIFICATE-----";
        static const char trailer[] = "-----END CERTIFICATE-----";

        if (!strncmp(line, header, strlen(header)))
        {
            TRACE("begin new certificate\n");
            in_cert = TRUE;
            reset_buffer(&saved_cert);
        }
        else if (!strncmp(line, trailer, strlen(trailer)))
        {
            DWORD size;

            TRACE("end of certificate, adding cert\n");
            in_cert = FALSE;
            if (CryptStringToBinaryA((char *)saved_cert.data, saved_cert.used,
             CRYPT_STRING_BASE64, NULL, &size, NULL, NULL))
            {
                LPBYTE buf = CryptMemAlloc(size);

                if (buf)
                {
                    CryptStringToBinaryA((char *)saved_cert.data,
                     saved_cert.used, CRYPT_STRING_BASE64, buf, &size, NULL,
                     NULL);
                    if (CertAddEncodedCertificateToStore(store,
                     X509_ASN_ENCODING, buf, size, CERT_STORE_ADD_NEW, NULL))
                        num_certs++;
                    CryptMemFree(buf);
                }
            }
        }
        else if (in_cert)
            add_line_to_buffer(&saved_cert, line);
    }
    CryptMemFree(saved_cert.data);
    TRACE("Read %d certs\n", num_certs);
    return num_certs > 0;
}

static const char *trust_status_to_str(DWORD status)
{
    static char buf[1024];
    int pos = 0;

    if (status & CERT_TRUST_IS_NOT_TIME_VALID)
        pos += snprintf(buf + pos, sizeof(buf) - pos, "\n\texpired");
    if (status & CERT_TRUST_IS_NOT_TIME_NESTED)
        pos += snprintf(buf + pos, sizeof(buf) - pos, "\n\tbad time nesting");
    if (status & CERT_TRUST_IS_REVOKED)
        pos += snprintf(buf + pos, sizeof(buf) - pos, "\n\trevoked");
    if (status & CERT_TRUST_IS_NOT_SIGNATURE_VALID)
        pos += snprintf(buf + pos, sizeof(buf) - pos, "\n\tbad signature");
    if (status & CERT_TRUST_IS_NOT_VALID_FOR_USAGE)
        pos += snprintf(buf + pos, sizeof(buf) - pos, "\n\tbad usage");
    if (status & CERT_TRUST_IS_UNTRUSTED_ROOT)
        pos += snprintf(buf + pos, sizeof(buf) - pos, "\n\tuntrusted root");
    if (status & CERT_TRUST_REVOCATION_STATUS_UNKNOWN)
        pos += snprintf(buf + pos, sizeof(buf) - pos,
         "\n\tunknown revocation status");
    if (status & CERT_TRUST_IS_CYCLIC)
        pos += snprintf(buf + pos, sizeof(buf) - pos, "\n\tcyclic chain");
    if (status & CERT_TRUST_INVALID_EXTENSION)
        pos += snprintf(buf + pos, sizeof(buf) - pos,
         "\n\tunsupported critical extension");
    if (status & CERT_TRUST_INVALID_POLICY_CONSTRAINTS)
        pos += snprintf(buf + pos, sizeof(buf) - pos, "\n\tbad policy");
    if (status & CERT_TRUST_INVALID_BASIC_CONSTRAINTS)
        pos += snprintf(buf + pos, sizeof(buf) - pos,
         "\n\tbad basic constraints");
    if (status & CERT_TRUST_INVALID_NAME_CONSTRAINTS)
        pos += snprintf(buf + pos, sizeof(buf) - pos,
         "\n\tbad name constraints");
    if (status & CERT_TRUST_HAS_NOT_SUPPORTED_NAME_CONSTRAINT)
        pos += snprintf(buf + pos, sizeof(buf) - pos,
         "\n\tunsupported name constraint");
    if (status & CERT_TRUST_HAS_NOT_DEFINED_NAME_CONSTRAINT)
        pos += snprintf(buf + pos, sizeof(buf) - pos,
         "\n\tundefined name constraint");
    if (status & CERT_TRUST_HAS_NOT_PERMITTED_NAME_CONSTRAINT)
        pos += snprintf(buf + pos, sizeof(buf) - pos,
         "\n\tdisallowed name constraint");
    if (status & CERT_TRUST_HAS_EXCLUDED_NAME_CONSTRAINT)
        pos += snprintf(buf + pos, sizeof(buf) - pos,
         "\n\texcluded name constraint");
    if (status & CERT_TRUST_IS_OFFLINE_REVOCATION)
        pos += snprintf(buf + pos, sizeof(buf) - pos,
         "\n\trevocation server offline");
    if (status & CERT_TRUST_NO_ISSUANCE_CHAIN_POLICY)
        pos += snprintf(buf + pos, sizeof(buf) - pos,
         "\n\tno issuance policy");
    return buf;
}

static const char *get_cert_common_name(PCCERT_CONTEXT cert)
{
    static char buf[1024];
    const char *name = NULL;
    CERT_NAME_INFO *nameInfo;
    DWORD size;
    BOOL ret = CryptDecodeObjectEx(X509_ASN_ENCODING, X509_NAME,
     cert->pCertInfo->Subject.pbData, cert->pCertInfo->Subject.cbData,
     CRYPT_DECODE_NOCOPY_FLAG | CRYPT_DECODE_ALLOC_FLAG, NULL, &nameInfo,
     &size);

    if (ret)
    {
        PCERT_RDN_ATTR commonName = CertFindRDNAttr(szOID_COMMON_NAME,
         nameInfo);

        if (commonName)
        {
            CertRDNValueToStrA(commonName->dwValueType,
             &commonName->Value, buf, sizeof(buf));
            name = buf;
        }
        LocalFree(nameInfo);
    }
    return name;
}

static void check_and_store_certs(HCERTSTORE from, HCERTSTORE to)
{
    DWORD root_count = 0;
    CERT_CHAIN_ENGINE_CONFIG chainEngineConfig =
     { sizeof(chainEngineConfig), 0 };
    HCERTCHAINENGINE engine;

    TRACE("\n");

    CertDuplicateStore(to);
    engine = CRYPT_CreateChainEngine(to, CERT_SYSTEM_STORE_CURRENT_USER, &chainEngineConfig);
    if (engine)
    {
        PCCERT_CONTEXT cert = NULL;

        do {
            cert = CertEnumCertificatesInStore(from, cert);
            if (cert)
            {
                CERT_CHAIN_PARA chainPara = { sizeof(chainPara), { 0 } };
                PCCERT_CHAIN_CONTEXT chain;
                BOOL ret;

                ret = CertGetCertificateChain(engine, cert, NULL, from,
                 &chainPara, CERT_CHAIN_CACHE_ONLY_URL_RETRIEVAL, NULL, &chain);
                if (!ret)
                    TRACE("rejecting %s: %s\n", get_cert_common_name(cert),
                     "chain creation failed");
                else
                {
                    DWORD allowedErrors = CERT_TRUST_IS_UNTRUSTED_ROOT |
                     CERT_TRUST_IS_NOT_VALID_FOR_USAGE |
                     CERT_TRUST_INVALID_BASIC_CONSTRAINTS |
                     CERT_TRUST_IS_NOT_TIME_VALID;

                    /* The certificate chain verification only allows certain
                     * invalid CA certs if they're installed locally:  CA
                     * certs missing the key usage extension, and CA certs
                     * missing the basic constraints extension.  Of course
                     * there's a chicken and egg problem:  we have to accept
                     * them here in order for them to be accepted later.
                     * Expired, locally installed certs are also allowed here,
                     * because we don't know (yet) what date will be checked
                     * for an item signed by one of these certs.
                     * Thus, accept certs with any of the allowed errors.
                     */
                    if (chain->TrustStatus.dwErrorStatus & ~allowedErrors)
                        TRACE("rejecting %s: %s\n", get_cert_common_name(cert),
                         trust_status_to_str(chain->TrustStatus.dwErrorStatus &
                         ~CERT_TRUST_IS_UNTRUSTED_ROOT));
                    else
                    {
                        DWORD i, j;

                        for (i = 0; i < chain->cChain; i++)
                            for (j = 0; j < chain->rgpChain[i]->cElement; j++)
                                if (CertAddCertificateContextToStore(to,
                                 chain->rgpChain[i]->rgpElement[j]->pCertContext,
                                 CERT_STORE_ADD_NEW, NULL))
                                    root_count++;
                    }
                    CertFreeCertificateChain(chain);
                }
            }
        } while (cert);
        CertFreeCertificateChainEngine(engine);
    }
    TRACE("Added %d root certificates\n", root_count);
}

/* Reads the file fd, and imports any certificates in it into store.
 * Returns TRUE if any certificates were successfully imported.
 */
static BOOL import_certs_from_file(int fd, HCERTSTORE store)
{
    BOOL ret = FALSE;
    FILE *fp;

    TRACE("\n");

    fp = fdopen(fd, "r");
    if (fp)
    {
        ret = import_base64_certs_from_fp(fp, store);
        fclose(fp);
    }
    return ret;
}

static BOOL import_certs_from_path(LPCSTR path, HCERTSTORE store,
 BOOL allow_dir);

static BOOL check_buffer_resize(char **ptr_buf, size_t *buf_size, size_t check_size)
{
    if (check_size > *buf_size)
    {
        *buf_size = check_size;

        if (*ptr_buf)
        {
            char *realloc_buf = CryptMemRealloc(*ptr_buf, *buf_size);

            if (!realloc_buf)
                return FALSE;

            *ptr_buf = realloc_buf;
        }
        else
        {
            *ptr_buf = CryptMemAlloc(*buf_size);
            if (!*ptr_buf)
                return FALSE;
        }
    }

    return TRUE;
}

/* Opens path, which must be a directory, and imports certificates from every
 * file in the directory into store.
 * Returns TRUE if any certificates were successfully imported.
 */
static BOOL import_certs_from_dir(LPCSTR path, HCERTSTORE store)
{
#ifdef HAVE_READDIR
    BOOL ret = FALSE;
    DIR *dir;

    TRACE("(%s, %p)\n", debugstr_a(path), store);

    dir = opendir(path);
    if (dir)
    {
        size_t path_len = strlen(path), bufsize = 0;
        char *filebuf = NULL;

        struct dirent *entry;
        while ((entry = readdir(dir)))
        {
            if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, ".."))
            {
                size_t name_len = strlen(entry->d_name);

                if (!check_buffer_resize(&filebuf, &bufsize, path_len + 1 + name_len + 1))
                {
                    ERR("Path buffer (re)allocation failed with out of memory condition\n");
                    break;
                }
                snprintf(filebuf, bufsize, "%s/%s", path, entry->d_name);
                if (import_certs_from_path(filebuf, store, FALSE) && !ret)
                    ret = TRUE;
            }
        }
        CryptMemFree(filebuf);
        closedir(dir);
    }
    return ret;
#else
    FIXME("not implemented without readdir available\n");
    return FALSE;
#endif
}

/* Opens path, which may be a file or a directory, and imports any certificates
 * it finds into store.
 * Returns TRUE if any certificates were successfully imported.
 */
static BOOL import_certs_from_path(LPCSTR path, HCERTSTORE store,
 BOOL allow_dir)
{
    BOOL ret = FALSE;
    int fd;

    TRACE("(%s, %p, %d)\n", debugstr_a(path), store, allow_dir);

    fd = open(path, O_RDONLY);
    if (fd != -1)
    {
        struct stat st;

        if (fstat(fd, &st) == 0)
        {
            if (S_ISREG(st.st_mode))
                ret = import_certs_from_file(fd, store);
            else if (S_ISDIR(st.st_mode))
            {
                if (allow_dir)
                    ret = import_certs_from_dir(path, store);
                else
                    WARN("%s is a directory and directories are disallowed\n",
                     debugstr_a(path));
            }
            else
                ERR("%s: invalid file type\n", path);
        }
        close(fd);
    }
    return ret;
}

static BOOL WINAPI CRYPT_RootWriteCert(HCERTSTORE hCertStore,
 PCCERT_CONTEXT cert, DWORD dwFlags)
{
    /* The root store can't have certs added */
    return FALSE;
}

static BOOL WINAPI CRYPT_RootDeleteCert(HCERTSTORE hCertStore,
 PCCERT_CONTEXT cert, DWORD dwFlags)
{
    /* The root store can't have certs deleted */
    return FALSE;
}

static BOOL WINAPI CRYPT_RootWriteCRL(HCERTSTORE hCertStore,
 PCCRL_CONTEXT crl, DWORD dwFlags)
{
    /* The root store can have CRLs added.  At worst, a malicious application
     * can DoS itself, as the changes aren't persisted in any way.
     */
    return TRUE;
}

static BOOL WINAPI CRYPT_RootDeleteCRL(HCERTSTORE hCertStore,
 PCCRL_CONTEXT crl, DWORD dwFlags)
{
    /* The root store can't have CRLs deleted */
    return FALSE;
}

static void *rootProvFuncs[] = {
    NULL, /* CERT_STORE_PROV_CLOSE_FUNC */
    NULL, /* CERT_STORE_PROV_READ_CERT_FUNC */
    CRYPT_RootWriteCert,
    CRYPT_RootDeleteCert,
    NULL, /* CERT_STORE_PROV_SET_CERT_PROPERTY_FUNC */
    NULL, /* CERT_STORE_PROV_READ_CRL_FUNC */
    CRYPT_RootWriteCRL,
    CRYPT_RootDeleteCRL,
    NULL, /* CERT_STORE_PROV_SET_CRL_PROPERTY_FUNC */
    NULL, /* CERT_STORE_PROV_READ_CTL_FUNC */
    NULL, /* CERT_STORE_PROV_WRITE_CTL_FUNC */
    NULL, /* CERT_STORE_PROV_DELETE_CTL_FUNC */
    NULL, /* CERT_STORE_PROV_SET_CTL_PROPERTY_FUNC */
    NULL, /* CERT_STORE_PROV_CONTROL_FUNC */
};

static const char * const CRYPT_knownLocations[] = {
 "/etc/ssl/certs/ca-certificates.crt",
 "/etc/ssl/certs",
 "/etc/pki/tls/certs/ca-bundle.crt",
 "/usr/share/ca-certificates/ca-bundle.crt",
 "/usr/local/share/certs/",
 "/etc/sfw/openssl/certs",
 "/etc/security/cacerts",  /* Android */
};

static const BYTE authenticode[] = {
0x30,0x82,0x03,0xd6,0x30,0x82,0x02,0xbe,0xa0,0x03,0x02,0x01,0x02,0x02,0x01,0x01,
0x30,0x0d,0x06,0x09,0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x04,0x05,0x00,0x30,
0x50,0x31,0x0b,0x30,0x09,0x06,0x03,0x55,0x04,0x06,0x13,0x02,0x55,0x53,0x31,0x0d,
0x30,0x0b,0x06,0x03,0x55,0x04,0x0a,0x13,0x04,0x4d,0x53,0x46,0x54,0x31,0x32,0x30,
0x30,0x06,0x03,0x55,0x04,0x03,0x13,0x29,0x4d,0x69,0x63,0x72,0x6f,0x73,0x6f,0x66,
0x74,0x20,0x41,0x75,0x74,0x68,0x65,0x6e,0x74,0x69,0x63,0x6f,0x64,0x65,0x28,0x74,
0x6d,0x29,0x20,0x52,0x6f,0x6f,0x74,0x20,0x41,0x75,0x74,0x68,0x6f,0x72,0x69,0x74,
0x79,0x30,0x1e,0x17,0x0d,0x39,0x35,0x30,0x31,0x30,0x31,0x30,0x38,0x30,0x30,0x30,
0x31,0x5a,0x17,0x0d,0x39,0x39,0x31,0x32,0x33,0x31,0x32,0x33,0x35,0x39,0x35,0x39,
0x5a,0x30,0x50,0x31,0x0b,0x30,0x09,0x06,0x03,0x55,0x04,0x06,0x13,0x02,0x55,0x53,
0x31,0x0d,0x30,0x0b,0x06,0x03,0x55,0x04,0x0a,0x13,0x04,0x4d,0x53,0x46,0x54,0x31,
0x32,0x30,0x30,0x06,0x03,0x55,0x04,0x03,0x13,0x29,0x4d,0x69,0x63,0x72,0x6f,0x73,
0x6f,0x66,0x74,0x20,0x41,0x75,0x74,0x68,0x65,0x6e,0x74,0x69,0x63,0x6f,0x64,0x65,
0x28,0x74,0x6d,0x29,0x20,0x52,0x6f,0x6f,0x74,0x20,0x41,0x75,0x74,0x68,0x6f,0x72,
0x69,0x74,0x79,0x30,0x82,0x01,0x22,0x30,0x0d,0x06,0x09,0x2a,0x86,0x48,0x86,0xf7,
0x0d,0x01,0x01,0x01,0x05,0x00,0x03,0x82,0x01,0x0f,0x00,0x30,0x82,0x01,0x0a,0x02,
0x82,0x01,0x01,0x00,0xdf,0x08,0xba,0xe3,0x3f,0x6e,0x64,0x9b,0xf5,0x89,0xaf,0x28,
0x96,0x4a,0x07,0x8f,0x1b,0x2e,0x8b,0x3e,0x1d,0xfc,0xb8,0x80,0x69,0xa3,0xa1,0xce,
0xdb,0xdf,0xb0,0x8e,0x6c,0x89,0x76,0x29,0x4f,0xca,0x60,0x35,0x39,0xad,0x72,0x32,
0xe0,0x0b,0xae,0x29,0x3d,0x4c,0x16,0xd9,0x4b,0x3c,0x9d,0xda,0xc5,0xd3,0xd1,0x09,
0xc9,0x2c,0x6f,0xa6,0xc2,0x60,0x53,0x45,0xdd,0x4b,0xd1,0x55,0xcd,0x03,0x1c,0xd2,
0x59,0x56,0x24,0xf3,0xe5,0x78,0xd8,0x07,0xcc,0xd8,0xb3,0x1f,0x90,0x3f,0xc0,0x1a,
0x71,0x50,0x1d,0x2d,0xa7,0x12,0x08,0x6d,0x7c,0xb0,0x86,0x6c,0xc7,0xba,0x85,0x32,
0x07,0xe1,0x61,0x6f,0xaf,0x03,0xc5,0x6d,0xe5,0xd6,0xa1,0x8f,0x36,0xf6,0xc1,0x0b,
0xd1,0x3e,0x69,0x97,0x48,0x72,0xc9,0x7f,0xa4,0xc8,0xc2,0x4a,0x4c,0x7e,0xa1,0xd1,
0x94,0xa6,0xd7,0xdc,0xeb,0x05,0x46,0x2e,0xb8,0x18,0xb4,0x57,0x1d,0x86,0x49,0xdb,
0x69,0x4a,0x2c,0x21,0xf5,0x5e,0x0f,0x54,0x2d,0x5a,0x43,0xa9,0x7a,0x7e,0x6a,0x8e,
0x50,0x4d,0x25,0x57,0xa1,0xbf,0x1b,0x15,0x05,0x43,0x7b,0x2c,0x05,0x8d,0xbd,0x3d,
0x03,0x8c,0x93,0x22,0x7d,0x63,0xea,0x0a,0x57,0x05,0x06,0x0a,0xdb,0x61,0x98,0x65,
0x2d,0x47,0x49,0xa8,0xe7,0xe6,0x56,0x75,0x5c,0xb8,0x64,0x08,0x63,0xa9,0x30,0x40,
0x66,0xb2,0xf9,0xb6,0xe3,0x34,0xe8,0x67,0x30,0xe1,0x43,0x0b,0x87,0xff,0xc9,0xbe,
0x72,0x10,0x5e,0x23,0xf0,0x9b,0xa7,0x48,0x65,0xbf,0x09,0x88,0x7b,0xcd,0x72,0xbc,
0x2e,0x79,0x9b,0x7b,0x02,0x03,0x01,0x00,0x01,0xa3,0x81,0xba,0x30,0x81,0xb7,0x30,
0x0d,0x06,0x03,0x55,0x1d,0x0a,0x04,0x06,0x30,0x04,0x03,0x02,0x07,0x80,0x30,0x32,
0x06,0x03,0x55,0x04,0x03,0x04,0x2b,0x13,0x29,0x4d,0x69,0x63,0x72,0x6f,0x73,0x6f,
0x66,0x74,0x20,0x41,0x75,0x74,0x68,0x65,0x6e,0x74,0x69,0x63,0x6f,0x64,0x65,0x28,
0x74,0x6d,0x29,0x20,0x52,0x6f,0x6f,0x74,0x20,0x41,0x75,0x74,0x68,0x6f,0x72,0x69,
0x74,0x79,0x30,0x72,0x06,0x03,0x55,0x1d,0x01,0x04,0x6b,0x30,0x69,0x80,0x10,0x1a,
0x1b,0xe7,0x5b,0x9f,0xfd,0x8c,0x2a,0xc3,0x39,0xae,0x0c,0x62,0x2e,0x53,0x32,0xa1,
0x52,0x30,0x50,0x31,0x0b,0x30,0x09,0x06,0x03,0x55,0x04,0x06,0x13,0x02,0x55,0x53,
0x31,0x0d,0x30,0x0b,0x06,0x03,0x55,0x04,0x0a,0x13,0x04,0x4d,0x53,0x46,0x54,0x31,
0x32,0x30,0x30,0x06,0x03,0x55,0x04,0x03,0x13,0x29,0x4d,0x69,0x63,0x72,0x6f,0x73,
0x6f,0x66,0x74,0x20,0x41,0x75,0x74,0x68,0x65,0x6e,0x74,0x69,0x63,0x6f,0x64,0x65,
0x28,0x74,0x6d,0x29,0x20,0x52,0x6f,0x6f,0x74,0x20,0x41,0x75,0x74,0x68,0x6f,0x72,
0x69,0x74,0x79,0x82,0x01,0x01,0x30,0x0d,0x06,0x09,0x2a,0x86,0x48,0x86,0xf7,0x0d,
0x01,0x01,0x04,0x05,0x00,0x03,0x82,0x01,0x01,0x00,0x2d,0xc9,0xe2,0xf6,0x12,0x9e,
0x5d,0x56,0x67,0xfa,0xfa,0x4b,0x9a,0x7e,0xdc,0x29,0x56,0x5c,0x80,0x14,0x02,0x28,
0x85,0x6e,0x26,0xf3,0xcd,0x58,0xda,0x50,0x80,0xc5,0xf8,0x19,0xb3,0xa6,0x7c,0xe2,
0x9d,0x6b,0x5f,0x3b,0x8f,0x22,0x74,0xe6,0x18,0x04,0xfc,0x47,0x40,0xd8,0x7a,0x3f,
0x30,0x66,0xf0,0x12,0xa4,0xd1,0xeb,0x1d,0xe7,0xb6,0xf4,0x98,0xab,0x53,0x22,0x86,
0x51,0x58,0xee,0x23,0x09,0x76,0xe4,0x1d,0x45,0x5c,0x4b,0xff,0x4c,0xe3,0x02,0x50,
0x01,0x13,0xcc,0x41,0xa4,0x52,0x97,0xd4,0x86,0xd5,0xc4,0xfe,0x83,0x83,0x65,0x7d,
0xea,0xbe,0xa2,0x68,0x3b,0xc1,0xb1,0x29,0x98,0xbf,0xa2,0xa5,0xfc,0x9d,0xd3,0x84,
0xee,0x70,0x17,0x50,0xf3,0x0b,0xfa,0x3c,0xef,0xa9,0x27,0x8b,0x91,0xb4,0x48,0xc8,
0x45,0xa0,0xe1,0x01,0x42,0x4b,0x44,0x76,0x04,0x1c,0xc2,0x19,0xa2,0x8e,0x6b,0x20,
0x98,0xc4,0xdd,0x02,0xac,0xb4,0xd2,0xa2,0x0e,0x8d,0x5d,0xb9,0x36,0x8e,0x4a,0x1b,
0x5d,0x6c,0x1a,0xe2,0xcb,0x00,0x7f,0x10,0xf4,0xb2,0x95,0xef,0xe3,0xe8,0xff,0xa1,
0x73,0x58,0xa9,0x75,0x2c,0xa2,0x49,0x95,0x85,0xfe,0xcc,0xda,0x44,0x8a,0xc2,0x12,
0x44,0xd2,0x44,0xc8,0xa5,0xa2,0x1f,0xa9,0x5a,0x8e,0x56,0xc2,0xc3,0x7b,0xcf,0x42,
0x60,0xdc,0x82,0x1f,0xfb,0xce,0x74,0x06,0x7e,0xd6,0xf1,0xac,0x19,0x6a,0x4f,0x74,
0x5c,0xc5,0x15,0x66,0x31,0x6c,0xc1,0x62,0x71,0x91,0x0f,0x59,0x5b,0x7d,0x2a,0x82,
0x1a,0xdf,0xb1,0xb4,0xd8,0x1d,0x37,0xde,0x0d,0x0f };
static const BYTE rootauthority[] = {
0x30,0x82,0x04,0x12,0x30,0x82,0x02,0xfa,0xa0,0x03,0x02,0x01,0x02,0x02,0x0f,0x00,
0xc1,0x00,0x8b,0x3c,0x3c,0x88,0x11,0xd1,0x3e,0xf6,0x63,0xec,0xdf,0x40,0x30,0x0d,
0x06,0x09,0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x04,0x05,0x00,0x30,0x70,0x31,
0x2b,0x30,0x29,0x06,0x03,0x55,0x04,0x0b,0x13,0x22,0x43,0x6f,0x70,0x79,0x72,0x69,
0x67,0x68,0x74,0x20,0x28,0x63,0x29,0x20,0x31,0x39,0x39,0x37,0x20,0x4d,0x69,0x63,
0x72,0x6f,0x73,0x6f,0x66,0x74,0x20,0x43,0x6f,0x72,0x70,0x2e,0x31,0x1e,0x30,0x1c,
0x06,0x03,0x55,0x04,0x0b,0x13,0x15,0x4d,0x69,0x63,0x72,0x6f,0x73,0x6f,0x66,0x74,
0x20,0x43,0x6f,0x72,0x70,0x6f,0x72,0x61,0x74,0x69,0x6f,0x6e,0x31,0x21,0x30,0x1f,
0x06,0x03,0x55,0x04,0x03,0x13,0x18,0x4d,0x69,0x63,0x72,0x6f,0x73,0x6f,0x66,0x74,
0x20,0x52,0x6f,0x6f,0x74,0x20,0x41,0x75,0x74,0x68,0x6f,0x72,0x69,0x74,0x79,0x30,
0x1e,0x17,0x0d,0x39,0x37,0x30,0x31,0x31,0x30,0x30,0x37,0x30,0x30,0x30,0x30,0x5a,
0x17,0x0d,0x32,0x30,0x31,0x32,0x33,0x31,0x30,0x37,0x30,0x30,0x30,0x30,0x5a,0x30,
0x70,0x31,0x2b,0x30,0x29,0x06,0x03,0x55,0x04,0x0b,0x13,0x22,0x43,0x6f,0x70,0x79,
0x72,0x69,0x67,0x68,0x74,0x20,0x28,0x63,0x29,0x20,0x31,0x39,0x39,0x37,0x20,0x4d,
0x69,0x63,0x72,0x6f,0x73,0x6f,0x66,0x74,0x20,0x43,0x6f,0x72,0x70,0x2e,0x31,0x1e,
0x30,0x1c,0x06,0x03,0x55,0x04,0x0b,0x13,0x15,0x4d,0x69,0x63,0x72,0x6f,0x73,0x6f,
0x66,0x74,0x20,0x43,0x6f,0x72,0x70,0x6f,0x72,0x61,0x74,0x69,0x6f,0x6e,0x31,0x21,
0x30,0x1f,0x06,0x03,0x55,0x04,0x03,0x13,0x18,0x4d,0x69,0x63,0x72,0x6f,0x73,0x6f,
0x66,0x74,0x20,0x52,0x6f,0x6f,0x74,0x20,0x41,0x75,0x74,0x68,0x6f,0x72,0x69,0x74,
0x79,0x30,0x82,0x01,0x22,0x30,0x0d,0x06,0x09,0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,
0x01,0x01,0x05,0x00,0x03,0x82,0x01,0x0f,0x00,0x30,0x82,0x01,0x0a,0x02,0x82,0x01,
0x01,0x00,0xa9,0x02,0xbd,0xc1,0x70,0xe6,0x3b,0xf2,0x4e,0x1b,0x28,0x9f,0x97,0x78,
0x5e,0x30,0xea,0xa2,0xa9,0x8d,0x25,0x5f,0xf8,0xfe,0x95,0x4c,0xa3,0xb7,0xfe,0x9d,
0xa2,0x20,0x3e,0x7c,0x51,0xa2,0x9b,0xa2,0x8f,0x60,0x32,0x6b,0xd1,0x42,0x64,0x79,
0xee,0xac,0x76,0xc9,0x54,0xda,0xf2,0xeb,0x9c,0x86,0x1c,0x8f,0x9f,0x84,0x66,0xb3,
0xc5,0x6b,0x7a,0x62,0x23,0xd6,0x1d,0x3c,0xde,0x0f,0x01,0x92,0xe8,0x96,0xc4,0xbf,
0x2d,0x66,0x9a,0x9a,0x68,0x26,0x99,0xd0,0x3a,0x2c,0xbf,0x0c,0xb5,0x58,0x26,0xc1,
0x46,0xe7,0x0a,0x3e,0x38,0x96,0x2c,0xa9,0x28,0x39,0xa8,0xec,0x49,0x83,0x42,0xe3,
0x84,0x0f,0xbb,0x9a,0x6c,0x55,0x61,0xac,0x82,0x7c,0xa1,0x60,0x2d,0x77,0x4c,0xe9,
0x99,0xb4,0x64,0x3b,0x9a,0x50,0x1c,0x31,0x08,0x24,0x14,0x9f,0xa9,0xe7,0x91,0x2b,
0x18,0xe6,0x3d,0x98,0x63,0x14,0x60,0x58,0x05,0x65,0x9f,0x1d,0x37,0x52,0x87,0xf7,
0xa7,0xef,0x94,0x02,0xc6,0x1b,0xd3,0xbf,0x55,0x45,0xb3,0x89,0x80,0xbf,0x3a,0xec,
0x54,0x94,0x4e,0xae,0xfd,0xa7,0x7a,0x6d,0x74,0x4e,0xaf,0x18,0xcc,0x96,0x09,0x28,
0x21,0x00,0x57,0x90,0x60,0x69,0x37,0xbb,0x4b,0x12,0x07,0x3c,0x56,0xff,0x5b,0xfb,
0xa4,0x66,0x0a,0x08,0xa6,0xd2,0x81,0x56,0x57,0xef,0xb6,0x3b,0x5e,0x16,0x81,0x77,
0x04,0xda,0xf6,0xbe,0xae,0x80,0x95,0xfe,0xb0,0xcd,0x7f,0xd6,0xa7,0x1a,0x72,0x5c,
0x3c,0xca,0xbc,0xf0,0x08,0xa3,0x22,0x30,0xb3,0x06,0x85,0xc9,0xb3,0x20,0x77,0x13,
0x85,0xdf,0x02,0x03,0x01,0x00,0x01,0xa3,0x81,0xa8,0x30,0x81,0xa5,0x30,0x81,0xa2,
0x06,0x03,0x55,0x1d,0x01,0x04,0x81,0x9a,0x30,0x81,0x97,0x80,0x10,0x5b,0xd0,0x70,
0xef,0x69,0x72,0x9e,0x23,0x51,0x7e,0x14,0xb2,0x4d,0x8e,0xff,0xcb,0xa1,0x72,0x30,
0x70,0x31,0x2b,0x30,0x29,0x06,0x03,0x55,0x04,0x0b,0x13,0x22,0x43,0x6f,0x70,0x79,
0x72,0x69,0x67,0x68,0x74,0x20,0x28,0x63,0x29,0x20,0x31,0x39,0x39,0x37,0x20,0x4d,
0x69,0x63,0x72,0x6f,0x73,0x6f,0x66,0x74,0x20,0x43,0x6f,0x72,0x70,0x2e,0x31,0x1e,
0x30,0x1c,0x06,0x03,0x55,0x04,0x0b,0x13,0x15,0x4d,0x69,0x63,0x72,0x6f,0x73,0x6f,
0x66,0x74,0x20,0x43,0x6f,0x72,0x70,0x6f,0x72,0x61,0x74,0x69,0x6f,0x6e,0x31,0x21,
0x30,0x1f,0x06,0x03,0x55,0x04,0x03,0x13,0x18,0x4d,0x69,0x63,0x72,0x6f,0x73,0x6f,
0x66,0x74,0x20,0x52,0x6f,0x6f,0x74,0x20,0x41,0x75,0x74,0x68,0x6f,0x72,0x69,0x74,
0x79,0x82,0x0f,0x00,0xc1,0x00,0x8b,0x3c,0x3c,0x88,0x11,0xd1,0x3e,0xf6,0x63,0xec,
0xdf,0x40,0x30,0x0d,0x06,0x09,0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x04,0x05,
0x00,0x03,0x82,0x01,0x01,0x00,0x95,0xe8,0x0b,0xc0,0x8d,0xf3,0x97,0x18,0x35,0xed,
0xb8,0x01,0x24,0xd8,0x77,0x11,0xf3,0x5c,0x60,0x32,0x9f,0x9e,0x0b,0xcb,0x3e,0x05,
0x91,0x88,0x8f,0xc9,0x3a,0xe6,0x21,0xf2,0xf0,0x57,0x93,0x2c,0xb5,0xa0,0x47,0xc8,
0x62,0xef,0xfc,0xd7,0xcc,0x3b,0x3b,0x5a,0xa9,0x36,0x54,0x69,0xfe,0x24,0x6d,0x3f,
0xc9,0xcc,0xaa,0xde,0x05,0x7c,0xdd,0x31,0x8d,0x3d,0x9f,0x10,0x70,0x6a,0xbb,0xfe,
0x12,0x4f,0x18,0x69,0xc0,0xfc,0xd0,0x43,0xe3,0x11,0x5a,0x20,0x4f,0xea,0x62,0x7b,
0xaf,0xaa,0x19,0xc8,0x2b,0x37,0x25,0x2d,0xbe,0x65,0xa1,0x12,0x8a,0x25,0x0f,0x63,
0xa3,0xf7,0x54,0x1c,0xf9,0x21,0xc9,0xd6,0x15,0xf3,0x52,0xac,0x6e,0x43,0x32,0x07,
0xfd,0x82,0x17,0xf8,0xe5,0x67,0x6c,0x0d,0x51,0xf6,0xbd,0xf1,0x52,0xc7,0xbd,0xe7,
0xc4,0x30,0xfc,0x20,0x31,0x09,0x88,0x1d,0x95,0x29,0x1a,0x4d,0xd5,0x1d,0x02,0xa5,
0xf1,0x80,0xe0,0x03,0xb4,0x5b,0xf4,0xb1,0xdd,0xc8,0x57,0xee,0x65,0x49,0xc7,0x52,
0x54,0xb6,0xb4,0x03,0x28,0x12,0xff,0x90,0xd6,0xf0,0x08,0x8f,0x7e,0xb8,0x97,0xc5,
0xab,0x37,0x2c,0xe4,0x7a,0xe4,0xa8,0x77,0xe3,0x76,0xa0,0x00,0xd0,0x6a,0x3f,0xc1,
0xd2,0x36,0x8a,0xe0,0x41,0x12,0xa8,0x35,0x6a,0x1b,0x6a,0xdb,0x35,0xe1,0xd4,0x1c,
0x04,0xe4,0xa8,0x45,0x04,0xc8,0x5a,0x33,0x38,0x6e,0x4d,0x1c,0x0d,0x62,0xb7,0x0a,
0xa2,0x8c,0xd3,0xd5,0x54,0x3f,0x46,0xcd,0x1c,0x55,0xa6,0x70,0xdb,0x12,0x3a,0x87,
0x93,0x75,0x9f,0xa7,0xd2,0xa0 };
static const BYTE rootcertauthority[] = {
0x30,0x82,0x05,0x99,0x30,0x82,0x03,0x81,0xa0,0x03,0x02,0x01,0x02,0x02,0x10,0x79,
0xad,0x16,0xa1,0x4a,0xa0,0xa5,0xad,0x4c,0x73,0x58,0xf4,0x07,0x13,0x2e,0x65,0x30,
0x0d,0x06,0x09,0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x05,0x05,0x00,0x30,0x5f,
0x31,0x13,0x30,0x11,0x06,0x0a,0x09,0x92,0x26,0x89,0x93,0xf2,0x2c,0x64,0x01,0x19,
0x16,0x03,0x63,0x6f,0x6d,0x31,0x19,0x30,0x17,0x06,0x0a,0x09,0x92,0x26,0x89,0x93,
0xf2,0x2c,0x64,0x01,0x19,0x16,0x09,0x6d,0x69,0x63,0x72,0x6f,0x73,0x6f,0x66,0x74,
0x31,0x2d,0x30,0x2b,0x06,0x03,0x55,0x04,0x03,0x13,0x24,0x4d,0x69,0x63,0x72,0x6f,
0x73,0x6f,0x66,0x74,0x20,0x52,0x6f,0x6f,0x74,0x20,0x43,0x65,0x72,0x74,0x69,0x66,
0x69,0x63,0x61,0x74,0x65,0x20,0x41,0x75,0x74,0x68,0x6f,0x72,0x69,0x74,0x79,0x30,
0x1e,0x17,0x0d,0x30,0x31,0x30,0x35,0x30,0x39,0x32,0x33,0x31,0x39,0x32,0x32,0x5a,
0x17,0x0d,0x32,0x31,0x30,0x35,0x30,0x39,0x32,0x33,0x32,0x38,0x31,0x33,0x5a,0x30,
0x5f,0x31,0x13,0x30,0x11,0x06,0x0a,0x09,0x92,0x26,0x89,0x93,0xf2,0x2c,0x64,0x01,
0x19,0x16,0x03,0x63,0x6f,0x6d,0x31,0x19,0x30,0x17,0x06,0x0a,0x09,0x92,0x26,0x89,
0x93,0xf2,0x2c,0x64,0x01,0x19,0x16,0x09,0x6d,0x69,0x63,0x72,0x6f,0x73,0x6f,0x66,
0x74,0x31,0x2d,0x30,0x2b,0x06,0x03,0x55,0x04,0x03,0x13,0x24,0x4d,0x69,0x63,0x72,
0x6f,0x73,0x6f,0x66,0x74,0x20,0x52,0x6f,0x6f,0x74,0x20,0x43,0x65,0x72,0x74,0x69,
0x66,0x69,0x63,0x61,0x74,0x65,0x20,0x41,0x75,0x74,0x68,0x6f,0x72,0x69,0x74,0x79,
0x30,0x82,0x02,0x22,0x30,0x0d,0x06,0x09,0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,
0x01,0x05,0x00,0x03,0x82,0x02,0x0f,0x00,0x30,0x82,0x02,0x0a,0x02,0x82,0x02,0x01,
0x00,0xf3,0x5d,0xfa,0x80,0x67,0xd4,0x5a,0xa7,0xa9,0x0c,0x2c,0x90,0x20,0xd0,0x35,
0x08,0x3c,0x75,0x84,0xcd,0xb7,0x07,0x89,0x9c,0x89,0xda,0xde,0xce,0xc3,0x60,0xfa,
0x91,0x68,0x5a,0x9e,0x94,0x71,0x29,0x18,0x76,0x7c,0xc2,0xe0,0xc8,0x25,0x76,0x94,
0x0e,0x58,0xfa,0x04,0x34,0x36,0xe6,0xdf,0xaf,0xf7,0x80,0xba,0xe9,0x58,0x0b,0x2b,
0x93,0xe5,0x9d,0x05,0xe3,0x77,0x22,0x91,0xf7,0x34,0x64,0x3c,0x22,0x91,0x1d,0x5e,
0xe1,0x09,0x90,0xbc,0x14,0xfe,0xfc,0x75,0x58,0x19,0xe1,0x79,0xb7,0x07,0x92,0xa3,
0xae,0x88,0x59,0x08,0xd8,0x9f,0x07,0xca,0x03,0x58,0xfc,0x68,0x29,0x6d,0x32,0xd7,
0xd2,0xa8,0xcb,0x4b,0xfc,0xe1,0x0b,0x48,0x32,0x4f,0xe6,0xeb,0xb8,0xad,0x4f,0xe4,
0x5c,0x6f,0x13,0x94,0x99,0xdb,0x95,0xd5,0x75,0xdb,0xa8,0x1a,0xb7,0x94,0x91,0xb4,
0x77,0x5b,0xf5,0x48,0x0c,0x8f,0x6a,0x79,0x7d,0x14,0x70,0x04,0x7d,0x6d,0xaf,0x90,
0xf5,0xda,0x70,0xd8,0x47,0xb7,0xbf,0x9b,0x2f,0x6c,0xe7,0x05,0xb7,0xe1,0x11,0x60,
0xac,0x79,0x91,0x14,0x7c,0xc5,0xd6,0xa6,0xe4,0xe1,0x7e,0xd5,0xc3,0x7e,0xe5,0x92,
0xd2,0x3c,0x00,0xb5,0x36,0x82,0xde,0x79,0xe1,0x6d,0xf3,0xb5,0x6e,0xf8,0x9f,0x33,
0xc9,0xcb,0x52,0x7d,0x73,0x98,0x36,0xdb,0x8b,0xa1,0x6b,0xa2,0x95,0x97,0x9b,0xa3,
0xde,0xc2,0x4d,0x26,0xff,0x06,0x96,0x67,0x25,0x06,0xc8,0xe7,0xac,0xe4,0xee,0x12,
0x33,0x95,0x31,0x99,0xc8,0x35,0x08,0x4e,0x34,0xca,0x79,0x53,0xd5,0xb5,0xbe,0x63,
0x32,0x59,0x40,0x36,0xc0,0xa5,0x4e,0x04,0x4d,0x3d,0xdb,0x5b,0x07,0x33,0xe4,0x58,
0xbf,0xef,0x3f,0x53,0x64,0xd8,0x42,0x59,0x35,0x57,0xfd,0x0f,0x45,0x7c,0x24,0x04,
0x4d,0x9e,0xd6,0x38,0x74,0x11,0x97,0x22,0x90,0xce,0x68,0x44,0x74,0x92,0x6f,0xd5,
0x4b,0x6f,0xb0,0x86,0xe3,0xc7,0x36,0x42,0xa0,0xd0,0xfc,0xc1,0xc0,0x5a,0xf9,0xa3,
0x61,0xb9,0x30,0x47,0x71,0x96,0x0a,0x16,0xb0,0x91,0xc0,0x42,0x95,0xef,0x10,0x7f,
0x28,0x6a,0xe3,0x2a,0x1f,0xb1,0xe4,0xcd,0x03,0x3f,0x77,0x71,0x04,0xc7,0x20,0xfc,
0x49,0x0f,0x1d,0x45,0x88,0xa4,0xd7,0xcb,0x7e,0x88,0xad,0x8e,0x2d,0xec,0x45,0xdb,
0xc4,0x51,0x04,0xc9,0x2a,0xfc,0xec,0x86,0x9e,0x9a,0x11,0x97,0x5b,0xde,0xce,0x53,
0x88,0xe6,0xe2,0xb7,0xfd,0xac,0x95,0xc2,0x28,0x40,0xdb,0xef,0x04,0x90,0xdf,0x81,
0x33,0x39,0xd9,0xb2,0x45,0xa5,0x23,0x87,0x06,0xa5,0x55,0x89,0x31,0xbb,0x06,0x2d,
0x60,0x0e,0x41,0x18,0x7d,0x1f,0x2e,0xb5,0x97,0xcb,0x11,0xeb,0x15,0xd5,0x24,0xa5,
0x94,0xef,0x15,0x14,0x89,0xfd,0x4b,0x73,0xfa,0x32,0x5b,0xfc,0xd1,0x33,0x00,0xf9,
0x59,0x62,0x70,0x07,0x32,0xea,0x2e,0xab,0x40,0x2d,0x7b,0xca,0xdd,0x21,0x67,0x1b,
0x30,0x99,0x8f,0x16,0xaa,0x23,0xa8,0x41,0xd1,0xb0,0x6e,0x11,0x9b,0x36,0xc4,0xde,
0x40,0x74,0x9c,0xe1,0x58,0x65,0xc1,0x60,0x1e,0x7a,0x5b,0x38,0xc8,0x8f,0xbb,0x04,
0x26,0x7c,0xd4,0x16,0x40,0xe5,0xb6,0x6b,0x6c,0xaa,0x86,0xfd,0x00,0xbf,0xce,0xc1,
0x35,0x02,0x03,0x01,0x00,0x01,0xa3,0x51,0x30,0x4f,0x30,0x0b,0x06,0x03,0x55,0x1d,
0x0f,0x04,0x04,0x03,0x02,0x01,0xc6,0x30,0x0f,0x06,0x03,0x55,0x1d,0x13,0x01,0x01,
0xff,0x04,0x05,0x30,0x03,0x01,0x01,0xff,0x30,0x1d,0x06,0x03,0x55,0x1d,0x0e,0x04,
0x16,0x04,0x14,0x0e,0xac,0x82,0x60,0x40,0x56,0x27,0x97,0xe5,0x25,0x13,0xfc,0x2a,
0xe1,0x0a,0x53,0x95,0x59,0xe4,0xa4,0x30,0x10,0x06,0x09,0x2b,0x06,0x01,0x04,0x01,
0x82,0x37,0x15,0x01,0x04,0x03,0x02,0x01,0x00,0x30,0x0d,0x06,0x09,0x2a,0x86,0x48,
0x86,0xf7,0x0d,0x01,0x01,0x05,0x05,0x00,0x03,0x82,0x02,0x01,0x00,0xc5,0x11,0x4d,
0x03,0x3a,0x60,0xdd,0x5d,0x52,0x11,0x77,0x8f,0xb2,0xbb,0x36,0xc8,0xb2,0x05,0xbf,
0xb4,0xb7,0xa8,0xd8,0x20,0x9d,0x5c,0x13,0x03,0xb6,0x1c,0x22,0xfa,0x06,0x13,0x35,
0xb6,0xc8,0x63,0xd4,0x9a,0x47,0x6f,0x26,0x57,0xd2,0x55,0xf1,0x04,0xb1,0x26,0x5f,
0xd6,0xa9,0x50,0x68,0xa0,0xbc,0xd2,0xb8,0x6e,0xcc,0xc3,0xe9,0xac,0xdf,0x19,0xcd,
0x78,0xac,0x59,0x74,0xac,0x66,0x34,0x36,0xc4,0x1b,0x3e,0x6c,0x38,0x4c,0x33,0x0e,
0x30,0x12,0x0d,0xa3,0x26,0xfe,0x51,0x53,0x00,0xff,0xaf,0x5a,0x4e,0x84,0x0d,0x0f,
0x1f,0xe4,0x6d,0x05,0x2e,0x4e,0x85,0x4b,0x8d,0x6c,0x33,0x6f,0x54,0xd2,0x64,0xab,
0xbf,0x50,0xaf,0x7d,0x7a,0x39,0xa0,0x37,0xed,0x63,0x03,0x0f,0xfc,0x13,0x06,0xce,
0x16,0x36,0xd4,0x54,0x3b,0x95,0x1b,0x51,0x62,0x3a,0xe5,0x4d,0x17,0xd4,0x05,0x39,
0x92,0x9a,0x27,0xa8,0x5b,0xaa,0xbd,0xec,0xbb,0xbe,0xe3,0x20,0x89,0x60,0x71,0x6c,
0x56,0xb3,0xa5,0x13,0xd0,0x6d,0x0e,0x23,0x7e,0x95,0x03,0xed,0x68,0x3d,0xf2,0xd8,
0x63,0xb8,0x6b,0x4d,0xb6,0xe8,0x30,0xb5,0xe1,0xca,0x94,0x4b,0xf7,0xa2,0xaa,0x5d,
0x99,0x30,0xb2,0x3d,0xa7,0xc2,0x51,0x6c,0x28,0x20,0x01,0x24,0x27,0x2b,0x4b,0x00,
0xb7,0x9d,0x11,0x6b,0x70,0xbe,0xb2,0x10,0x82,0xbc,0x0c,0x9b,0x68,0xd0,0x8d,0x3b,
0x24,0x87,0xaa,0x99,0x28,0x72,0x9d,0x33,0x5f,0x59,0x90,0xbd,0xf5,0xde,0x93,0x9e,
0x3a,0x62,0x5a,0x34,0x39,0xe2,0x88,0x55,0x1d,0xb9,0x06,0xb0,0xc1,0x89,0x6b,0x2d,
0xd7,0x69,0xc3,0x19,0x12,0x36,0x84,0xd0,0xc9,0xa0,0xda,0xff,0x2f,0x69,0x78,0xb2,
0xe5,0x7a,0xda,0xeb,0xd7,0x0c,0xc0,0xf7,0xbd,0x63,0x17,0xb8,0x39,0x13,0x38,0xa2,
0x36,0x5b,0x7b,0xf2,0x85,0x56,0x6a,0x1d,0x64,0x62,0xc1,0x38,0xe2,0xaa,0xbf,0x51,
0x66,0xa2,0x94,0xf5,0x12,0x9c,0x66,0x22,0x10,0x6b,0xf2,0xb7,0x30,0x92,0x2d,0xf2,
0x29,0xf0,0x3d,0x3b,0x14,0x43,0x68,0xa2,0xf1,0x9c,0x29,0x37,0xcb,0xce,0x38,0x20,
0x25,0x6d,0x7c,0x67,0xf3,0x7e,0x24,0x12,0x24,0x03,0x08,0x81,0x47,0xec,0xa5,0x9e,
0x97,0xf5,0x18,0xd7,0xcf,0xbb,0xd5,0xef,0x76,0x96,0xef,0xfd,0xce,0xdb,0x56,0x9d,
0x95,0xa0,0x42,0xf9,0x97,0x58,0xe1,0xd7,0x31,0x22,0xd3,0x5f,0x59,0xe6,0x3e,0x6e,
0x22,0x00,0xea,0x43,0x84,0xb6,0x25,0xdb,0xd9,0xf3,0x08,0x56,0x68,0xc0,0x64,0x6b,
0x1d,0x7c,0xec,0xb6,0x93,0xa2,0x62,0x57,0x6e,0x2e,0xd8,0xe7,0x58,0x8f,0xc4,0x31,
0x49,0x26,0xdd,0xde,0x29,0x35,0x87,0xf5,0x30,0x71,0x70,0x5b,0x14,0x3c,0x69,0xbd,
0x89,0x12,0x7d,0xeb,0x2e,0xa3,0xfe,0xd8,0x7f,0x9e,0x82,0x5a,0x52,0x0a,0x2b,0xc1,
0x43,0x2b,0xd9,0x30,0x88,0x9f,0xc8,0x10,0xfb,0x89,0x8d,0xe6,0xa1,0x85,0x75,0x33,
0x7e,0x6c,0x9e,0xdb,0x73,0x13,0x64,0x62,0x69,0xa5,0x2f,0x7d,0xca,0x96,0x6d,0x9f,
0xf8,0x04,0x4d,0x30,0x92,0x3d,0x6e,0x21,0x14,0x21,0xc9,0x3d,0xe0,0xc3,0xfd,0x8a,
0x6b,0x9d,0x4a,0xfd,0xd1,0xa1,0x9d,0x99,0x43,0x77,0x3f,0xb0,0xda };

static const struct CONST_BLOB {
    const BYTE *pb;
    DWORD       cb;
} msRootCerts[] = {
    { authenticode, sizeof(authenticode) },
    { rootauthority, sizeof(rootauthority) },
    { rootcertauthority, sizeof(rootcertauthority) },
};

static void add_ms_root_certs(HCERTSTORE to)
{
    DWORD i;

    TRACE("\n");

    for (i = 0; i < sizeof(msRootCerts) / sizeof(msRootCerts[0]); i++)
        if (!CertAddEncodedCertificateToStore(to, X509_ASN_ENCODING,
         msRootCerts[i].pb, msRootCerts[i].cb, CERT_STORE_ADD_NEW, NULL))
            WARN("adding root cert %d failed: %08x\n", i, GetLastError());
}

/* Reads certificates from the list of known locations into store.  Stops when
 * any location contains any certificates, to prevent spending unnecessary time
 * adding redundant certificates, e.g. when both a certificate bundle and
 * individual certificates exist in the same directory.
 */
static void read_trusted_roots_from_known_locations(HCERTSTORE store)
{
    HCERTSTORE from = CertOpenStore(CERT_STORE_PROV_MEMORY,
     X509_ASN_ENCODING, 0, CERT_STORE_CREATE_NEW_FLAG, NULL);

    if (from)
    {
        DWORD i;
        BOOL ret = FALSE;

#ifdef HAVE_SECURITY_SECURITY_H
        OSStatus status;
        CFArrayRef rootCerts;

        status = SecTrustCopyAnchorCertificates(&rootCerts);
        if (status == noErr)
        {
            int i;
            for (i = 0; i < CFArrayGetCount(rootCerts); i++)
            {
                SecCertificateRef cert = (SecCertificateRef)CFArrayGetValueAtIndex(rootCerts, i);
                CFDataRef certData;
                if ((status = SecKeychainItemExport(cert, kSecFormatX509Cert, 0, NULL, &certData)) == noErr)
                {
                    if (CertAddEncodedCertificateToStore(store, X509_ASN_ENCODING,
                            CFDataGetBytePtr(certData), CFDataGetLength(certData),
                            CERT_STORE_ADD_NEW, NULL))
                        ret = TRUE;
                    else
                        WARN("adding root cert %d failed: %08x\n", i, GetLastError());
                    CFRelease(certData);
                }
                else
                    WARN("could not export certificate %d to X509 format: 0x%08x\n", i, (unsigned int)status);
            }
            CFRelease(rootCerts);
        }
#endif

        for (i = 0; !ret &&
         i < sizeof(CRYPT_knownLocations) / sizeof(CRYPT_knownLocations[0]);
         i++)
            ret = import_certs_from_path(CRYPT_knownLocations[i], from, TRUE);
        check_and_store_certs(from, store);
    }
    CertCloseStore(from, 0);
}

static HCERTSTORE create_root_store(void)
{
    HCERTSTORE root = NULL;
    HCERTSTORE memStore = CertOpenStore(CERT_STORE_PROV_MEMORY,
     X509_ASN_ENCODING, 0, CERT_STORE_CREATE_NEW_FLAG, NULL);

    if (memStore)
    {
        CERT_STORE_PROV_INFO provInfo = {
         sizeof(CERT_STORE_PROV_INFO),
         sizeof(rootProvFuncs) / sizeof(rootProvFuncs[0]),
         rootProvFuncs,
         NULL,
         0,
         NULL
        };

        read_trusted_roots_from_known_locations(memStore);
        add_ms_root_certs(memStore);
        root = CRYPT_ProvCreateStore(0, memStore, &provInfo);
    }
    TRACE("returning %p\n", root);
    return root;
}

static WINECRYPT_CERTSTORE *CRYPT_rootStore;

WINECRYPT_CERTSTORE *CRYPT_RootOpenStore(HCRYPTPROV hCryptProv, DWORD dwFlags)
{
    TRACE("(%ld, %08x)\n", hCryptProv, dwFlags);

    if (dwFlags & CERT_STORE_DELETE_FLAG)
    {
        WARN("root store can't be deleted\n");
        SetLastError(ERROR_ACCESS_DENIED);
        return NULL;
    }
    if (!CRYPT_rootStore)
    {
        HCERTSTORE root = create_root_store();

        InterlockedCompareExchangePointer((PVOID *)&CRYPT_rootStore, root,
         NULL);
        if (CRYPT_rootStore != root)
            CertCloseStore(root, 0);
    }
    CRYPT_rootStore->vtbl->addref(CRYPT_rootStore);
    return CRYPT_rootStore;
}

void root_store_free(void)
{
    CertCloseStore(CRYPT_rootStore, 0);
}
