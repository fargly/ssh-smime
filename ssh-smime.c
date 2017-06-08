/*
 * ssh-smime - S/MIME file encryption with SSH public keys.
 *
 * Copyright John Eaglesham, 2013-2014
 *
 * Based on the OpenSSL Simple S/MIME encrypt example.
 *
 * This utility is licensed under the same terms as OpenSSL itself.
 */

 /***************************************************************
 * Post Fork Changes
 * -Generalize RSA SSH Public Key Extraction for Robustness

 ***************************************************************/



#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <openssl/pem.h>
//#if ! defined(sun)
//#include <err.h>
//#endif
#include <openssl/err.h>
#include <openssl/x509v3.h>

#ifdef USE_OPENSSL_CMS
#include <openssl/cms.h>
#else
#include <openssl/pkcs7.h>
#endif


#define SSH_MAX_PUBKEY_BYTES 8192

extern char *optarg;
extern int optind;

BIO *in_bio, *out_bio;


/*********************************************************
** PROTOTYPES ********************************************
*********************************************************/
void die_usage(void);
char **parse_opts(int, char **);
X509 *gen_temp_cert(RSA *);
int base64_decode(char *, char *);
void *read_bytes(char *, int, int, int *, void *);
RSA *parse_ssh_pubkey(char *, char *, int);
RSA *read_ssh_pubkey(char *);
STACK_OF(X509) *create_cert_stack(char **);
int extractRsaCrypto(char *, char *, int);
X509 *extractTransformRsaPublicKey(char *);




void die_usage(void) {
    fprintf(stderr, "usage: ssh-smime [-h] [-i file] [-o file] ssh-pubkey1 [ssh-pubkey2] [...]\n\n");
#if USE_OPENSSL_CMS
    fprintf(stderr, "CMS encrypt input using ssh keys for recipients.\n\n");
#else
    fprintf(stderr, "S/MIME encrypt input using ssh keys for recipients.\n\n");
#endif
    fprintf(stderr, "If not specified, input and output default to stdin and stdout.\n\n");
    fprintf(stderr, "Decryption can be done via the openssl command:\n");
#if USE_OPENSSL_CMS
    fprintf(stderr, "openssl cms -decrypt -inform PEM -in encrypted -inkey ~/.ssh/id_rsa -out decrypted\n");
#else
    fprintf(stderr, "openssl smime -decrypt -in encrypted -inkey ~/.ssh/id_rsa -out decrypted\n");
#endif
    exit(2);
}

/*
 * Returns a list of public key files to read.
 */
char **parse_opts(int argc, char **argv)
{
    int ch;

    while ((ch = getopt(argc, argv, "hi:o:")) != -1) {
        switch (ch) {
            case 'i':
                in_bio = BIO_new_file(optarg, "r");
                if (!in_bio) //err(1, "Unable to open file %s for reading: ", optarg);
                  fprintf(stderr, "Unable to open file %s for reading: ", optarg);
                break;
            case 'o':
                out_bio = BIO_new_file(optarg, "w");
                if (!out_bio) //err(1, "Unable to open file %s for writing: ", optarg);
                  fprintf(stderr, "Unable to open file %s for writing: ", optarg);
                break;
            case 'h':
            case '?':
            default:
                die_usage();
            }
    }
    argc -= optind;
    argv += optind;

    // User didn't specify any recipients.
    if (!argc) die_usage();

    if (!in_bio) in_bio = BIO_new_fp(stdin, BIO_NOCLOSE);
    if (!out_bio) out_bio = BIO_new_fp(stdout, BIO_NOCLOSE);

    return argv;
}

// Boilerplate cert creation. Apparnetly openssl smime is not that picky about
// what the certs look like, so we don't even bother signing this.
X509 *gen_temp_cert(RSA *key)
{
    X509 *cert;
    EVP_PKEY *pk;
    X509_NAME *name = NULL;

    if ((pk = EVP_PKEY_new()) == NULL) //errx(1, "Failed to allocate pubkey");
      fprintf(stderr, "Failed to allocate pubkey");

    if ((cert = X509_new()) == NULL) //errx(1, "Failed to allocate cert");
      fprintf(stderr, "Failed to allocate cert");

    if (!EVP_PKEY_assign_RSA(pk, key)) //errx(1, "Failed to assign key");
      fprintf(stderr, "Failed to assign key");

    X509_set_version(cert, 3);
    ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
    X509_gmtime_adj(X509_get_notBefore(cert), 0);
    X509_gmtime_adj(X509_get_notAfter(cert), (long)60*60);
    X509_set_pubkey(cert, pk);

    name = X509_get_subject_name(cert);

    /* This function creates and adds the entry, working out the
     * correct string type and performing checks on its length.
     * Normally we'd check the return value for errors...
     */
    X509_NAME_add_entry_by_txt(name, "C", MBSTRING_ASC, (unsigned char *)"US", -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (unsigned char *)"ssh-smime", -1, -1,
                               0);

    X509_set_issuer_name(cert, name);

    EVP_PKEY_free(pk);

    return cert;
}

int base64_decode(char *key, char *inbuf)
{
    BIO *bio, *b64;
    int inlen;

    b64 = BIO_new(BIO_f_base64());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    bio = BIO_new_mem_buf(key, -1);
    bio = BIO_push(b64, bio);
    inlen = BIO_read(bio, inbuf, SSH_MAX_PUBKEY_BYTES);

    BIO_free_all(bio);

    return inlen;
}

// Silly buffer management routine to insure we always walk forward in the
// buffer and never walk past the end.
void *read_bytes(char *blob, int blobsize, int amount, int *offset, void *dest)
{
    void *r;
    if (*offset + amount > blobsize) //errx(1, "Attempt to read past buffer");
      fprintf(stderr, "Attempt to read past buffer");
    memcpy(dest, blob + *offset, amount);
    r = blob + *offset;
    *offset += amount;
    return r;
}

RSA *parse_ssh_pubkey(char *file, char *key, int keysize)
{
    unsigned long i;
    char buf[SSH_MAX_PUBKEY_BYTES];
    int offset = 0;
    RSA *rsa = RSA_new();

    if (!rsa) //errx(1, "Could not allocate RSA");
      fprintf(stderr, "Could not allocate RSA");

    /*
     * The documentation states e and n will be allocated on a call to
     * BN_bin2bn if they are NULL. This doesn't seem to work, so allocate
     * them now.
     */
    rsa->e = BN_new();
    if (!rsa->e) //errx(1, "Could not allocate BIGNUM");
      fprintf(stderr, "Could not allocate BIGNUM");
    rsa->n = BN_new();
    if (!rsa->n) //errx(1, "Could not allocate BUGNUM");
      fprintf(stderr, "Could not allocate BIGNUM");

    // Read 4 bytes, the length of the key type name.
    read_bytes(key, keysize, 4, &offset, &i);
    i = ntohl(i);
    // Now read that many more bytes to read the key name itself.
    read_bytes(key, keysize, i, &offset, &buf);
    buf[i] = '\0';

    if (strcmp(buf, "ssh-rsa")) //errx(1, "%s does not appear to contain an SSH RSA public key", file);
      fprintf(stderr, "%s does not appear to contain an SSH RSA public key", file);

    // Same as above, for e.
    read_bytes(key, keysize, 4, &offset, &i);
    i = ntohl(i);
    read_bytes(key, keysize, i, &offset, &buf);
    if (BN_bin2bn((unsigned char *)buf, i, rsa->e) == NULL) //errx(1, "buffer_get_bignum2_ret: BN_bin2bn failed");
      fprintf(stderr, "buffer_get_bignum2_ret: BN_bin2bn failed");

    // Same as above, for n.
    read_bytes(key, keysize, 4, &offset, &i);
    i = ntohl(i);
    read_bytes(key, keysize, i, &offset, &buf);
    if (BN_bin2bn((unsigned char *)buf, i, rsa->n) == NULL) //errx(1, "buffer_get_bignum2_ret: BN_bin2bn failed");
      fprintf(stderr, "buffer_get_bignum2_ret: BN_bin2bn failed");

    return rsa;
}

RSA *read_ssh_pubkey(char *file)
{
    // Limit taken from SSH keygen
    char line[SSH_MAX_PUBKEY_BYTES + 1];
    //char *key;
    //char *comment;
    char decoded_key[SSH_MAX_PUBKEY_BYTES * 2];
    int decoded_size;
    char out_key[SSH_MAX_PUBKEY_BYTES * 2];
    int ret = 0;

    FILE *f = fopen(file, "r");
    if (!f) //err(1, "Failed to open file %s: ", file);
      fprintf(stderr, "Failed to open file %s: ", file);
    fread(line, SSH_MAX_PUBKEY_BYTES, 1, f);
    if (ferror(f)) //err(1, "Failed to read public key file %s: ", file);
      fprintf(stderr, "Failed to read public key file %s: ", file);
    fclose(f);
    line[SSH_MAX_PUBKEY_BYTES] = '\0';

    ret =  extractRsaCrypto(line, out_key, (SSH_MAX_PUBKEY_BYTES * 2));
    //decoded_size = base64_decode(key, decoded_key);
    decoded_size = base64_decode(out_key, decoded_key);
    return parse_ssh_pubkey(file, decoded_key, decoded_size);
}

STACK_OF(X509) *create_cert_stack(char **recipients)
{
    X509 *cert;
    STACK_OF(X509) *cert_stack = sk_X509_new_null();
    int i = 0;

    if (!cert_stack) //err(1, "Failed to allocate certificate stack.");
      fprintf(stderr, "Failed to allocate certificate stack.");

    while(recipients[i]) {
        cert = gen_temp_cert(read_ssh_pubkey(recipients[i++]));
        //cert = extractTransformRsaPublicKey(recipients[i++]);
        if (!sk_X509_push(cert_stack, cert)) {
            //err(1, "Failed to add certificate to stack");
            fprintf(stderr, "Failed to add certificate to stack");
        }
    }
    return cert_stack;
}

int main(int argc, char **argv)
{
    STACK_OF(X509) *recips = NULL;
    int ret = 1;

#ifdef USE_OPENSSL_CMS
    CMS_ContentInfo *crypt = NULL;
    int flags = CMS_BINARY;
#else
    PKCS7 *crypt = NULL;
    int flags = PKCS7_BINARY;
#endif

    recips = create_cert_stack(parse_opts(argc, argv));

    OpenSSL_add_all_algorithms();
    ERR_load_crypto_strings();

    /* encrypt content */
#ifdef USE_OPENSSL_CMS
    crypt = CMS_encrypt(recips, in_bio, EVP_aes_256_cbc(), flags);
#else
    crypt = PKCS7_encrypt(recips, in_bio, EVP_aes_256_cbc(), flags);
#endif

    if (!crypt)
        goto err;

    /* Write output in the appropriate format */
#ifdef USE_OPENSSL_CMS
    if (!PEM_write_bio_CMS_stream(out_bio, crypt, in_bio, flags))
#else
    if (!SMIME_write_PKCS7(out_bio, crypt, in_bio, flags))
#endif
        goto err;

    ret = 0;

    err:

    if (ret) {
        fprintf(stderr, "Error Encrypting Data\n");
        ERR_print_errors_fp(stderr);
    }

    if (crypt)
#ifdef USE_OPENSSL_CMS
        CMS_ContentInfo_free(crypt);
#else
        PKCS7_free(crypt);
#endif

    sk_X509_pop_free(recips, X509_free);

    BIO_free(in_bio);
    BIO_free(out_bio);

    ERR_free_strings();
    EVP_cleanup();

    return ret;
}



/******************************************************************************
 * IN: IN is an RSA SSH Public Key
 *     OUT is Crypto Portion of Public Key
 *     outSize is int size of OUT
 * OUT: int return code
 * Fn: int extractRsaCrypto(char *IN, char *OUT, int outSize)
 * More robustly handle parsing for all types of RSA SSH Public Keys
*******************************************************************************/
int extractRsaCrypto(char *IN, char *OUT, int outSize)
{
  char INcopy[SSH_MAX_PUBKEY_BYTES * 2];
  char *token;

  sprintf(INcopy, "%s", IN);
  token = strtok(INcopy, " ");
  while (token != NULL )
  {
    if (strcmp(token, "ssh-rsa")==0)
    {
      if (token != NULL) {
        token = strtok(NULL, " ");
        sprintf(OUT, "%s", token);
        return 0;
      }
      else return -1;
    }
    token = strtok(NULL, " ");
  }

  return -1;
}

/******************************************************************************
 * IN: rsaSshPubKeyString is an RSA SSH Public Key
 * OUT: X509 Certificate
 * Fn: X509 extractTransformRsaPublicKey(char *rsaSshPubKeyString)
 * Based on John Eaglesham's ssh-smime ETL function (BSD-style License)
*******************************************************************************/
X509 *extractTransformRsaPublicKey(char *rsaSshPubKeyString)
{
  X509 *cert = X509_new();
  char decoded_key[SSH_MAX_PUBKEY_BYTES * 2];
  char binary_rsa_key[SSH_MAX_PUBKEY_BYTES * 2];
  int ret;
  int decoded_size;
  RSA *key = RSA_new();
 
  // Extract and Decode Crypto String
  ret = extractRsaCrypto(rsaSshPubKeyString, decoded_key, (SSH_MAX_PUBKEY_BYTES * 2));
  if (ret != 0) //errx(1, "Failed to Extract/Transform RSA Key.");
      fprintf(stderr, "Failed to Extract/Transform RSA Key.");

  decoded_size = base64_decode(decoded_key, binary_rsa_key);
  key = parse_ssh_pubkey("Configured RSA SSH Public Key", binary_rsa_key, decoded_size);
  cert = gen_temp_cert(key);

  return cert;
}


// EOF
