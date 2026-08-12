#ifndef CA_BUNDLE_H
#define CA_BUNDLE_H

/* PEM-encoded CA certificate bundle, null-terminated (mbedtls_x509_crt_parse
 * expects that for its PEM path) -- see ca_bundle.c for provenance. */
extern const unsigned char ca_bundle_pem[];
extern const unsigned int ca_bundle_pem_len;

#endif /* CA_BUNDLE_H */
